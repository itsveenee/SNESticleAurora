/* AURORA_FINAL_V1_7_D88_DUAL_SRAM_PERSIST_20260901 */
/* mainloop_menu_runtime.cpp
 *
 * Hosts the runtime menu helpers used by MainLoopRender() and the input
 * path:
 *
 *   - _MenuEnable() : toggle the in-game menu, flushing SRAM to memcard
 *                     when the menu is brought up.
 *   - _MenuDraw()   : per-frame menu overlay (called from
 *                     MainLoopRender()).
 *
 * _MenuDraw() used to be a file-static helper inside mainloop.cpp.
 * After the Batch 3 split it has external linkage so MainLoopRender()
 * (now in mainloop_render.cpp) can still reach it through the
 * declaration in mainloop_shared.h.
 *
 * Extracted from mainloop.cpp during the Batch 3 split. Behaviour and
 * the surrounding `#if MAINLOOP_MEMCARD` / `#if 0` gating are unchanged.
 */

#include <stdio.h>
#include <string.h>
#include <libcdvd.h> /* AURORA_PS2_RTC_CLOCK_DIRECT_MENU_V1_5_20260905 */
#include <osd_config.h> /* PS2 configured local time */

#include "mainloop_debug.h"
#include "mainloop_shared.h"
#include "mainloop_menu.h"
#include "mainloop_state.h"
#include "mainloop_ui.h"
#include "mainloop_iop.h"

#include "types.h"
#include "console.h"
#include "font.h"
#include "poly.h"
#include "memcard.h"
#include "uiScreen.h"
#include "mainloop_bgm.h"
#include "pce/beetle/pce_bridge.h" /* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901 */
#include "sega/picodrive/picodrive_bridge.h" /* AURORA_SEGACD_MENU_IO_QUIESCE_V1_20260901 */
#include "audmixbuffer.h"

extern "C" {
#include "audio.h"
};


/* The L2+R2 path must return control immediately. The old implementation did
   all memory-card work plus a fixed 60-frame success modal before setting
   _bMenu, which made the shortcut look frozen. Schedule the write a couple of
   already-visible menu frames later; BgmIO keeps the tracker alive during the
   still-synchronous device operation. */
static Bool s_sramSavePending = FALSE;
static Bool s_sramSaveActive = FALSE;
static Int32 s_sramSaveDelay = 0;

/* AURORA_FINAL_V1_3_NORMAL_MENU_BGM_SESSION_20260901
 * _bMenu is also used by isolated quick-state/device/format prompts, so it
 * cannot identify a real pause-menu BGM session. Only _MenuEnable(TRUE)
 * owns this flag. */
static Bool s_NormalMenuBgmSession = FALSE;

Bool MainLoopNormalMenuBgmSessionActive(void)
{
    return s_NormalMenuBgmSession;
}

/* AURORA_V4_16_SAFE_GAME_SWITCH_FLUSH_20260830 */
Bool MainLoopSramSaveBusy(void)
{
    return (s_sramSavePending || s_sramSaveActive) ? TRUE : FALSE;
}

static void _MenuSavePendingSRAM(void)
{
	Bool bSaved;

	if (!s_sramSavePending)
		return;
	s_sramSavePending = FALSE;
	s_sramSaveActive = TRUE;

	BgmIOBegin();
	#if MAINLOOP_MEMCARD
	if (MainLoopSramNeedsMemoryCardPreflight() &&
	    MemCardGetStatus(0) == MEMCARD_STATUS_UNFORMATTED)
	{
		BgmIOEnd();
		s_sramSaveActive = FALSE;
		_MainLoopMemCardFormatPromptOpen(
			0,
			MAINLOOP_MEMCARDFORMAT_SRAM_SAVE
		);
		return;
	}
	#endif

	bSaved = _MainLoopSaveSRAM(TRUE);

	/* Second-stage check: AUTO may have seen mass0 as mounted, failed the
	   actual USB write, and then discovered an unformatted MC fallback. */
	#if MAINLOOP_MEMCARD
	if (!bSaved &&
	    MainLoopSramGetDevice() != MAINLOOP_SRAMDEVICE_USB &&
	    MemCardGetStatus(0) == MEMCARD_STATUS_UNFORMATTED)
	{
		BgmIOEnd();
		s_sramSaveActive = FALSE;
		_MainLoopMemCardFormatPromptOpen(
			0, MAINLOOP_MEMCARDFORMAT_SRAM_SAVE);
		return;
	}
	#endif

	BgmIOEnd();
	s_sramSaveActive = FALSE;
	MainLoopStatusPrintf(
		bSaved ? 90 : 180,
		bSaved ? "SRAM saved." : "Error saving SRAM!"
	);
}

void _MenuRuntimeUpdate(void)
{
	if (!_bMenu || !s_sramSavePending)
		return;
	/* PCE-CD: don't overlap the final pre-menu async read with SRAM I/O. */
	if (!MainLoopCdUiReady())
		return;
	if (s_sramSaveDelay > 0)
	{
		s_sramSaveDelay--;
		return;
	}
	_MenuSavePendingSRAM();
}


/* AURORA_FINAL_AUDIO_VIDEO_MD_V1
 * A transition must discard THREE places where an old sample can survive:
 *   1) AudMixBuffer's local EE frame accumulator,
 *   2) the EE async FIFO,
 *   3) audsrv's IOP/SPU2 queue.
 * Aud_Clearbuff() performs (2)+(3); Reset() performs (1). */
void MainLoopAudioHardCut(void)
{
    if (_AudMix)
        _AudMix->Reset();

    if (_MainLoop_bAudioReady)
    {
        Aud_Setvol(0);
        Aud_Clearbuff();
    }
}

/* AURORA_AUDIO_UI_SOFT_TRANSITION_V1_20260901
 * UI entry must not stop audsrv. Reset only EE-side producer state, discard
 * staged gameplay PCM and mute while the already-playing IOP ring drains. */
void MainLoopAudioUiMute(void)
{
    if (_AudMix)
        _AudMix->Reset();

    if (_MainLoop_bAudioReady)
    {
        Aud_AsyncDiscardPending();
        Aud_Setvol(0);
    }
}

void MainLoopAudioUiResume(void)
{
    if (_MainLoop_bAudioReady)
    {
        /* Normally already playing. If an unrelated path stopped audsrv,
           wake it while still muted, then restore full scale. */
        Aud_Play();
        Aud_Setvol(0x3FFF);
    }
}

void MainLoopAudioResumeGame(void)
{
    /* Clear once more before waking audsrv. This is intentional: closing a
       modal must never replay a sample generated before the modal opened. */
    MainLoopAudioHardCut();

    if (_MainLoop_bAudioReady)
    {
        Aud_Setvol(0x3FFF);
        Aud_Play();
    }
}

/* AURORA_FINAL_V1_1_UI_CD_STORAGE_BARRIER_20260901
 *
 * One frontend owner for CD transport quiescence. PCE CD and Sega CD are
 * intentionally different cores and keep their native barriers; this helper
 * only owns their UI lifetime and prevents duplicate/unconditional resumes.
 */
static Bool s_CdUiPceHeld = FALSE;
static Bool s_CdUiSegaHeld = FALSE;

Bool MainLoopCdUiQuiesce(void)
{
    if (s_CdUiPceHeld || s_CdUiSegaHeld)
        return TRUE;

    if (_pSystem == _pPce && PceBridge_IsDiscLoaded())
    {
        if (!PceBridge_QuiesceDiscIO())
            return FALSE;
        s_CdUiPceHeld = TRUE;
        return TRUE;
    }

    if (_pSystem == _pSega && PicoDriveBridge_IsSegaCD())
    {
        if (!PicoDriveBridge_PrepareGameSwitch())
            return FALSE;
        s_CdUiSegaHeld = TRUE;
        return TRUE;
    }

    return TRUE;
}

/* AURORA_SSF2_PCE_MENU_FIX_V2_20260913_PCE_MENU_ASYNC
 * PCE-CD pause completion is asynchronous. UI drawing/navigation is safe
 * immediately; filesystem work waits for this readiness poll. */
Bool MainLoopCdUiReady(void)
{
    if (s_CdUiPceHeld)
        return PceBridge_DiscIOPaused() ? TRUE : FALSE;
    return TRUE; /* Sega CD barrier remains synchronous. */
}

void MainLoopCdUiResume(void)
{
    /* Beetle keeps an explicit paused worker. PicoDrive private fileXio
       transport was closed by its native barrier and automatically reopens
       from the logical CDDA position on the next emulated frame. */
    if (s_CdUiPceHeld)
        PceBridge_ResumeDiscIO();

    s_CdUiPceHeld = FALSE;
    s_CdUiSegaHeld = FALSE;
}

void _MenuEnable(Bool bEnable)
{
	if (bEnable!=_bMenu)
	{
		if (bEnable)
		{
			/* AURORA_FINAL_V1_1_UI_CD_STORAGE_BARRIER_20260901
			 * Normal menu storage/BGM work must never overlap either CD core's
			 * private transport. Ownership is centralized so quick-state and the
			 * one-time state-device chooser can share the exact same rule. */
			if (!MainLoopCdUiQuiesce())
			{
				MainLoopStatusPrintf(
					120, "CD I/O busy; menu deferred.");
				return;
			}

			/* AURORA_FINAL_V1_3_NORMAL_MENU_BGM_SESSION_20260901
			 * Arm only after the CD transport is known idle. */
			s_NormalMenuBgmSession = TRUE;

			/* Publish the menu state before any storage RPC. MainLoopProcess
			   will render two frames, then run the pending save below. */
			_bMenu = TRUE;
			MainLoopAudioUiMute();
			BgmMenuEnter();

			/* Preserve a write performed in the <30-frame checksum window. */
			_MainLoopForceCheckSRAM();
			if (_MainLoopHasSRAM() &&
                (_MainLoop_SRAMUpdated ||
                 (_pSystem == _pSnes && _pSnes &&
                  _pSnes->IsSuperWildCard())))
			{
				s_sramSavePending = TRUE;
				s_sramSaveDelay = 2;
				MainLoopStatusPrintf(180, "Saving SRAM...");
			}
		}
		else
		{
			/* Normal input cannot close the menu again before the two-frame
			   delay expires. Clear defensively if another subsystem launches a
			   game directly while a save was queued for the previous ROM. */
			s_sramSavePending = FALSE;
			s_sramSaveDelay = 0;
			s_NormalMenuBgmSession = FALSE;
			_bMenu = FALSE;
			BgmStop();
			/* AURORA_FINAL_V1_1_UI_CD_STORAGE_BARRIER_20260901 */
			MainLoopCdUiResume();
			MainLoopAudioResumeGame();
		}
	}
}




void _MenuDraw()
{
	FontSelect(0);

	PolyTexture(NULL);
    PolyBlend(TRUE);

	// draw current screen
	if (_MainLoop_pScreen)
	{
		_MainLoop_pScreen->Draw();
	}

	/* Restore a distinct lower status area using the same dark teal as
	   the original iaddis title bars. Drawing it after the screen also
	   guarantees that a browser row can never paint over the footer. */
	const int footerY = 211;
	const int vy = 215;
	PolyTexture(NULL);
	PolyBlend(TRUE);
	PolyColor4f(0.0f, 0.2f, 0.2f, 0.9f);
	PolyRect(0, footerY, 256, 224 - footerY);

	FontSelect(2);
//	FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
//	FontColor4f(1.0, 0.5f, 0.5f, 1.0f);
	FontColor4f(0.2, 0.6f, 0.2f, 1.0f);

#if 0
	const VersionInfoT *pVersionInfo = VersionGetInfo();

	char VersionStr[256];
	
	sprintf(VersionStr, "%s v%d.%d.%d %s", 
		pVersionInfo->ApplicationName, 
		pVersionInfo->Version[0],
		pVersionInfo->Version[1],
		pVersionInfo->Version[2],
		pVersionInfo->BuildType
		);

	FontPuts(256 - 16 - FontGetStrWidth(VersionStr), vy, VersionStr);

//	FontPrintf(8, vy-16, "%d", CDVD_DiskReady(1));




	FontPrintf(8, vy, "%s%d.%d", 
		pVersionInfo->Compiler, 
		pVersionInfo->CompilerVersion[0],  
		pVersionInfo->CompilerVersion[1]
		);
#endif	

    /* Status bar (green): compiler version on the left and app version
       right-aligned. Network details already live on the Host settings
       screen, so the redundant IP field no longer consumes this row. */
    /* AURORA_PS2_RTC_CLOCK_DIRECT_MENU_V1_5_20260905
     * Replace the compiler footer directly at its real source.
     * The old string was "  GCC%d.%d" at x=8. The requested origin is the
     * position of G, so preserve the width of the two leading spaces. */
    {
        static Uint32 s_RtcPoll = 0;
        static Char s_RtcText[9] = "--:--:--";

        if ((s_RtcPoll++ & 31U) == 0U)
        {
            sceCdCLOCK rtc;
            if (sceCdReadClock(&rtc))
            {
                unsigned hour, minute, second;

                configConvertToLocalTime(&rtc);

                hour =
                    (unsigned)(((rtc.hour >> 4) & 0x0FU) * 10U +
                               (rtc.hour & 0x0FU));
                minute =
                    (unsigned)(((rtc.minute >> 4) & 0x0FU) * 10U +
                               (rtc.minute & 0x0FU));
                second =
                    (unsigned)(((rtc.second >> 4) & 0x0FU) * 10U +
                               (rtc.second & 0x0FU));

                snprintf(
                    s_RtcText, sizeof(s_RtcText),
                    "%02u:%02u:%02u",
                    hour, minute, second
                );
            }
        }

        FontPuts(8 + FontGetStrWidth("  "), vy, s_RtcText);
    }

#ifdef APP_VERSION
    static const char *_AppVersionStr =
        "SNESticle Aurora v" APP_VERSION;
#else
    static const char *_AppVersionStr = "SNESticle Aurora v1.0.4";
#endif
    FontPuts(256 - 16 - FontGetStrWidth(_AppVersionStr),
             vy, _AppVersionStr);



	FontSelect(0);
}

/* AURORA_V4_16_SAFE_GAME_SWITCH_FLUSH_20260830 */
