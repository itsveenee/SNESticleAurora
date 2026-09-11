/* AURORA_V13_UNIFIED_GBC_AUDIO_32X_FRAMESKIP_20260910 */
/* mainloop_render.cpp
 *
 * Hosts MainLoopRender() and the file-static state it needs
 * (_uVblankCycle and the screen-size #defines used to clear / blit the
 * SNES output texture).
 *
 * Extracted from mainloop.cpp during the Batch 3 split. No logic,
 * literal, or attribute change.
 */

#include <stdio.h>

#include "mainloop_debug.h"
#include "mainloop_shared.h"
#include "mainloop_ui.h"
#include "mainloop_menu.h" /* AURORA_FINAL_V1_5_RENDER_MENU_API_INCLUDE_20260901 */
#include "mainloop_bgm.h"
#include "mainloop_safe_frameskip.h" /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */
#include "sega/picodrive/picodrive_bridge.h"
/* AURORA_ALLCORES_PERF_V5_20260824 */
#include "nes/quicknes/quicknes_bridge.h"
/* AURORA_FCEUMM_FDS_PERF_DIRECT_T8_V3_20260827 */
#include "nes/fceumm/fceumm_fds_bridge.h"
#include "pce/beetle/pce_bridge.h"
#include "gb/system/gambattesystem.h" /* AURORA_GAMBATTE_SQUARE_ASPECT_V13_20260911 */

/* AURORA_PS2_PERF_V4_20260824 */
#include "types.h"
#include "console.h"
#include "snes.h"
#include "rendersurface.h"
#include "texture.h"
#include "font.h"
#include "poly.h"
#include "prof.h"
#include "snstate.h"
#include "snppublend_gs.h"
#include "common/debug/dbgterm.h"

/* AURORA_PCE_RELEASE_CLEANUP_FINAL_V15_20260830
 * Hidden release diagnostics. Keep the probes available for future
 * PCE/QuickNES investigations without drawing anything by default. */
#ifndef AURORA_RUNTIME_DIAG_OVERLAY
#define AURORA_RUNTIME_DIAG_OVERLAY 0
#endif

#include "mainloop_iop.h"

extern "C" {
#include "hw.h"
#include "gs.h"
#include "gpfifo.h"
#include "gpprim.h"
#include "gskit_backend.h"
#include "audio.h"
};

extern "C" {
#include "mcsave_ee.h"
};


/* Same MAINLOOP_SCREENWIDTH / HEIGHT pair as mainloop_init.cpp. The
   render path uses these to size the output blit; the init path uses
   them to size the GS framebuffer. Three other historical layouts are
   kept commented out in mainloop_init.cpp for reference. */
#define MAINLOOP_SCREENWIDTH 256
#define MAINLOOP_SCREENHEIGHT 240


static Uint32 _uVblankCycle;

/* AURORA_SAFE_FRAMESKIP_PICODRIVE_AUTO_V1
 * Global adaptation of standalone PicoDrive's Auto Frameskip scheduler.
 *
 * Standalone PicoDrive keeps an ideal timestamp (timestamp_aim), compares it
 * with the current clock before each frame, skips video when it is more than
 * one target frame late, permits up to max_skip consecutive skips (default 4),
 * and trims timing debt beyond roughly three frames.
 *
 * Aurora's host loop is a presentation loop, so its target_frametime is the
 * calibrated GS VBlank period. The decision is made once per host tick and is
 * then shared by every core. A skipped tick still executes core CPU/audio but
 * MainLoopRender consumes the one-shot below and does not present or wait for
 * VBlank, which is what makes real catch-up possible.
 *
 * Menu value: 0=Off, 1..9=max_skip. Aurora default=1.
 * AURORA_FAMICOM_MIC_CFG41_20260828
 */
static Int32  s_SafeFrameskipLevel = 1;
static Bool   s_SafeFrameskipSkipPresentation = FALSE;
static Uint32 s_SafeFrameskipAim = 0;
static Uint32 s_SafeFrameskipConsecutive = 0;
static Uint32 s_SafeFrameskipLastFlip = 0;
static Uint32 s_SafeFrameskipPeriod = 0;
static Uint32 s_SafeFrameskipSamples[3] = { 0, 0, 0 };
static Uint32 s_SafeFrameskipSampleCount = 0;
static Uint32 s_SafeFrameskipSamplePos = 0;
/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830
 * One-shot request raised only by a CDDA cache/hunk miss. */
static Bool s_SafeFrameskipCdAudioWindowRequested = FALSE;

static Uint32 _MainLoopSafeFrameskipMedian3(Uint32 a, Uint32 b, Uint32 c)
{
    if (a > b) { const Uint32 x = a; a = b; b = x; }
    if (b > c) { const Uint32 x = b; b = c; c = x; }
    if (a > b) { const Uint32 x = a; a = b; b = x; }
    return b;
}

static void _MainLoopSafeFrameskipResetTiming(void)
{
    s_SafeFrameskipAim = 0;
    s_SafeFrameskipConsecutive = 0;
    s_SafeFrameskipSkipPresentation = FALSE;
}

static void _MainLoopSafeFrameskipLearn(Uint32 delta, Bool gameplay)
{
    if (delta == 0)
        return;

    /* AURORA_V13_UNIFIED_GBC_AUDIO_32X_FRAMESKIP_20260910
     * Do not let one transition/first-frame timing sample permanently poison
     * Auto. Before a period is established, acquire three mutually coherent
     * VBlank intervals. A 25% disagreement restarts acquisition from the new
     * sample instead of rejecting every later healthy sample forever. */
    if (s_SafeFrameskipPeriod == 0)
    {
        if (s_SafeFrameskipSampleCount != 0)
        {
            Uint32 lastPos = (s_SafeFrameskipSamplePos + 2u) % 3u;
            Uint32 ref = s_SafeFrameskipSamples[lastPos];
            if ((Uint64)delta * 4u < (Uint64)ref * 3u ||
                (Uint64)delta * 4u > (Uint64)ref * 5u)
            {
                s_SafeFrameskipSamples[0] = delta;
                s_SafeFrameskipSamples[1] = 0;
                s_SafeFrameskipSamples[2] = 0;
                s_SafeFrameskipSamplePos = 1u;
                s_SafeFrameskipSampleCount = 1u;
                return;
            }
        }

        s_SafeFrameskipSamples[s_SafeFrameskipSamplePos] = delta;
        s_SafeFrameskipSamplePos = (s_SafeFrameskipSamplePos + 1u) % 3u;
        if (s_SafeFrameskipSampleCount < 3u)
            ++s_SafeFrameskipSampleCount;

        if (s_SafeFrameskipSampleCount < 3u)
            return;

        s_SafeFrameskipPeriod = _MainLoopSafeFrameskipMedian3(
            s_SafeFrameskipSamples[0],
            s_SafeFrameskipSamples[1],
            s_SafeFrameskipSamples[2]);
        _MainLoopSafeFrameskipResetTiming();
        return;
    }

    if (gameplay)
    {
        /* Only a healthy single-VBlank presentation may refine timing. */
        if ((Uint64)delta * 4u < (Uint64)s_SafeFrameskipPeriod * 3u ||
            (Uint64)delta * 4u > (Uint64)s_SafeFrameskipPeriod * 5u)
            return;
    }
    else
    {
        if ((Uint64)delta * 3u < (Uint64)s_SafeFrameskipPeriod * 2u ||
            (Uint64)delta * 2u > (Uint64)s_SafeFrameskipPeriod * 3u)
            return;
    }

    s_SafeFrameskipSamples[s_SafeFrameskipSamplePos] = delta;
    s_SafeFrameskipSamplePos = (s_SafeFrameskipSamplePos + 1u) % 3u;
    s_SafeFrameskipPeriod = _MainLoopSafeFrameskipMedian3(
        s_SafeFrameskipSamples[0],
        s_SafeFrameskipSamples[1],
        s_SafeFrameskipSamples[2]);
}

Int32 MainLoopSafeFrameskipGetLevel(void)
{
    return s_SafeFrameskipLevel;
}

void MainLoopSafeFrameskipSetLevel(Int32 level)
{
    if (level < 0) level = 0;
    if (level > 9) level = 9;
    s_SafeFrameskipLevel = level;
    /* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830 */
    s_SafeFrameskipCdAudioWindowRequested = FALSE;
    _MainLoopSafeFrameskipResetTiming();
}

Bool MainLoopSafeFrameskipGetEnabled(void)
{
    return s_SafeFrameskipLevel > 0 ? TRUE : FALSE;
}

void MainLoopSafeFrameskipSetEnabled(Bool enabled)
{
    if (!enabled)
        MainLoopSafeFrameskipSetLevel(0);
    else if (s_SafeFrameskipLevel <= 0)
        MainLoopSafeFrameskipSetLevel(1);
}

/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830
 *
 * A CDDA miss is forbidden from synchronously touching storage on a frame
 * that is going to be presented. The core returns silence for that span and
 * asks for one Safe Frameskip window on the next host tick.
 */
void MainLoopSafeFrameskipRequestCdAudioWindow(void)
{
    if (s_SafeFrameskipLevel > 0)
        s_SafeFrameskipCdAudioWindowRequested = TRUE;
}

/* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
void MainLoopSafeFrameskipCancelCdAudioWindow(void)
{
    s_SafeFrameskipCdAudioWindowRequested = FALSE;
}

Bool MainLoopSafeFrameskipTake(Bool allowed)
{
    Uint32 now;
    Int32 diff;
    const Int32 target = (Int32)s_SafeFrameskipPeriod;
    Bool skip = FALSE;

    /* Exactly one presentation one-shot is produced by each host decision. */
    s_SafeFrameskipSkipPresentation = FALSE;

    /* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830
     * The presented frame that discovered the miss stays untouched.
     * This NEXT tick is the only place where blocking CDDA refill may run. */
    if (!allowed || s_SafeFrameskipLevel <= 0)
    {
        s_SafeFrameskipCdAudioWindowRequested = FALSE;
        _MainLoopSafeFrameskipResetTiming();
        return FALSE;
    }

    if (s_SafeFrameskipCdAudioWindowRequested)
    {
        s_SafeFrameskipCdAudioWindowRequested = FALSE;

        /* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830
         * CDDA may spend Safe Frameskip, but never bypass max_skip. */
        if (s_SafeFrameskipConsecutive >=
            (Uint32)s_SafeFrameskipLevel)
        {
            /* AURORA_SAFE_FRAMESKIP_UNSTICK_V1_20260907
             * max_skip means the next frame MUST be presented. Drop transient
             * host timing debt completely instead of rebasing to a timestamp
             * captured before that recovery frame executes. The learned
             * VBlank period is intentionally preserved by ResetTiming().
             * The next eligible host tick will establish a fresh aim after
             * the forced presentation has actually completed. */
            _MainLoopSafeFrameskipResetTiming();
            return FALSE;
        }

        if (s_SafeFrameskipSampleCount >= 3u && target > 0)
        {
            now = ProfCtrGetCycle();
            if (s_SafeFrameskipAim == 0)
                s_SafeFrameskipAim = now;

            ++s_SafeFrameskipConsecutive;
            s_SafeFrameskipAim += s_SafeFrameskipPeriod;
        }
        else
        {
            ++s_SafeFrameskipConsecutive;
        }

        s_SafeFrameskipSkipPresentation = TRUE;
        return TRUE;
    }

    if (s_SafeFrameskipSampleCount < 3u || target <= 0)
    {
        _MainLoopSafeFrameskipResetTiming();
        return FALSE;
    }

    now = ProfCtrGetCycle();

    /* PicoDrive reset_timing equivalent. First eligible tick establishes aim. */
    if (s_SafeFrameskipAim == 0)
    {
        s_SafeFrameskipAim = now;
        s_SafeFrameskipConsecutive = 0;
    }

    /* PicoDrive: diff = timestamp_aim - timestamp. Uint32 subtraction then
     * Int32 interpretation intentionally preserves short counter wraparound. */
    diff = (Int32)(s_SafeFrameskipAim - now);

    /* PicoDrive Auto: if more than one target frame late, discard video,
     * limited by max_skip. Here the menu level IS max_skip. */
    /* AURORA_V13: tolerate ordinary scheduling jitter; catch up only
     * once host debt exceeds 1.25 learned VBlank periods. */
    if ((Int64)diff * 4 < -(Int64)target * 5)
    {
        if (s_SafeFrameskipConsecutive < (Uint32)s_SafeFrameskipLevel)
        {
            ++s_SafeFrameskipConsecutive;
            skip = TRUE;
        }
        else
        {
            /* AURORA_SAFE_FRAMESKIP_UNSTICK_V1_20260907
             *
             * We have spent max_skip consecutive catch-up frames, so this
             * frame is a hard presentation boundary. The previous rebase used
             * `now` BEFORE the recovery frame and then advanced aim again at
             * the bottom of this function. Any latency in that forced frame
             * could instantly recreate the old debt and latch Auto into
             * repeated skip bursts.
             *
             * Reset only transient scheduler state. The calibrated VBlank
             * samples/period stay intact; the next eligible tick seeds aim
             * from the real post-presentation host time. This is the same
             * debt-clearing effect that manually toggling Safe Frameskip
             * Off/On had, but it now happens automatically at max_skip. */
            _MainLoopSafeFrameskipResetTiming();
            return FALSE;
        }
    }
    else
    {
        s_SafeFrameskipConsecutive = 0;
    }

    /* PicoDrive: don't go in debt too much. Keep the ideal clock no more than
     * roughly three target frames behind before advancing this frame's aim. */
    while (diff < -(target * 3))
    {
        s_SafeFrameskipAim += s_SafeFrameskipPeriod;
        diff = (Int32)(s_SafeFrameskipAim - now);
    }

    /* PicoDrive advances timestamp_aim once for every emulated host tick,
     * whether that tick is shown or skipped. */
    s_SafeFrameskipAim += s_SafeFrameskipPeriod;
    s_SafeFrameskipSkipPresentation = skip;
    return skip;
}

Bool MainLoopSafeFrameskipConsumePresentationSkip(void)
{
    const Bool skip = s_SafeFrameskipSkipPresentation;
    s_SafeFrameskipSkipPresentation = FALSE;
    return skip;
}

static void _MainLoopSafeFrameskipAfterFlip(void)
{
    const Uint32 now = ProfCtrGetCycle();
    const Bool gameplay =
        (!_bMenu && _pSystem && !_MainLoop_BlackScreen) ? TRUE : FALSE;

    /* AURORA_FCEUMM_FDS_V14_FRAMESKIP_STABILITY_20260827
     * Menu/prompt/storage frames never retrain gameplay timing. Preserve the
     * learned gameplay VBlank period, but break the last-flip edge outside
     * gameplay so closing UI cannot alter the next skip cadence. */
    if (gameplay)
    {
        if (s_SafeFrameskipLastFlip != 0)
        {
            const Uint32 delta = now - s_SafeFrameskipLastFlip;
            _MainLoopSafeFrameskipLearn(delta, TRUE);
        }
        s_SafeFrameskipLastFlip = now;
    }
    else
    {
        s_SafeFrameskipLastFlip = 0;
    }

    if (s_SafeFrameskipLevel <= 0 || !gameplay)
        _MainLoopSafeFrameskipResetTiming();
}

void MainLoopRender()
{
	static Uint32 _iFrame=0;
        static int whichdrawbuf = 0;

        /* AURORA_SAFE_FRAMESKIP_PICODRIVE_AUTO_V1: standalone PicoDrive skip path has no
         * finalize/present/flip/VBlank wait. Core/audio already ran. */
        if (MainLoopSafeFrameskipConsumePresentationSkip())
        {
            if (!_bMenu && _pSystem && !_MainLoop_BlackScreen)
            {
                /* AURORA_CD_AUDIO_STREAM_V2_SKIP_AUDIO_CATCHUP_20260829
                 * The presentation/VBlank wait is already being skipped, so
                 * spend that recovered host time on at most one EXTRA
                 * nonblocking async-audio drain. No wait_audio(), no PCM drop.
                 * With no backlog the second call returns immediately. */
                Aud_BufferedAsyncStart();
                Aud_BufferedAsyncStart();
            }
            ++_iFrame;
            return;
        }

        /* AURORA_SEGA_UI_PRESENTATION_V3
         *
         * Keep two different host presentations:
         *
         *   UI / browser / prompts -> Aurora's normal 256-wide raster
         *   plain MD gameplay      -> native 320-wide raster in 240p
         *   SMS and other systems  -> normal 256-wide raster
         *
         * The important distinction from the old late-init path is that
         * the FIRST MD 320 raster is still selected in mainloop_load.cpp
         * before PicoDrive is initialised. These later switches happen
         * only when entering/leaving Aurora's internal UI.
         *
         * In 480i/1080i MainLoopEnsureGameplayRasterWidth() does not
         * rebuild the GS; those modes keep their normal 640 framebuffer.
         */
        {
            Int32 wantedRaster = 256;
            const Bool bMdVideo =
                (_pSystem == _pSega &&
                 PicoDriveBridge_IsMegaDriveVideo()) ? TRUE : FALSE;

            /* AURORA_SEGA_CD_32X_MD_SCALING_V2R1_20260828
             * Cartridge MD, Sega CD and 32X share H32/H40 presentation. */
            if (bMdVideo)
                wantedRaster = 320;


            if (_pSystem == _pPce && !_bMenu && !_MainLoop_BlackScreen)
            {
                /* AURORA_PCE_FIXED512_DBX0_CUMULATIVE_V8_20260830
                 * One aligned PCE framebuffer for 256/342/512 dot clocks. */
                wantedRaster = 512;
            }
            /* Menu/prompts use exact 256-source integer presentation on the
             * still-alive 320 framebuffer, not 256->320 resampling. */
            GSK_SetUi256On320Framebuffer(
                (_bMenu && bMdVideo) ? 1 : 0);

            if (_bMenu)
                GSK_SetNative240pPar(0);

            if (!MainLoopEnsureGameplayRasterWidth(wantedRaster))
            {
                printf("[video] warning: could not switch to %d-wide presentation\n",
                       (int)wantedRaster);
            }

            /* AURORA_PCE_ROOT512_KRAZY_LATCH_V12_20260830: clear window */
            if (_pSystem != _pPce || _bMenu || _MainLoop_BlackScreen)
                GSK_Clear240pVisibleWindow();

            /* AURORA_PCE_FIXED512_DBX0_CUMULATIVE_V8_20260830: clear window */
            if (_pSystem != _pPce || _bMenu || _MainLoop_BlackScreen)
                GSK_Clear240pVisibleWindow();
        }



        /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911
         * Both standalone GB/GBC and GBA LCDs use square source pixels.
         * Reuse v13's presentation-only policy: no core framebuffer
         * resampling, no effect on dynamic SGB, menus or other systems. */
        const Bool bSquareHandheldGameplay =
            (!_bMenu && !_MainLoop_BlackScreen &&
             ((_pSystem == _pGb && _pGb &&
               _pGb->UsesSquarePixelPresentation()) ||
              (_pSystem == _pGba))) ? TRUE : FALSE;
        GSK_SetGbSquarePixelPresentation(bSquareHandheldGameplay ? 1 : 0);

        /*
         * NES/SNES/8-bit-console 240p: keep the framebuffer strictly 256x240 (1 source
         * pixel = 1 framebuffer pixel) and correct horizontal size at
         * the PCRTC level instead. This avoids uneven pixel widths
         * caused by scaling 256 pixels into a smaller PolyRect.
         */
        static int s_native240pPar = -1;
        int native240pPar =
            (g_GskVideoMode == GSK_VIDMODE_240P &&
             (_pSystem == _pNes ||
              _pSystem == _pFds || /* AURORA_FCEUMM_FDS_V0_5_RENDER */
              _pSystem == _pSnes ||
              (_pSystem == _pSega &&
               (PicoDriveBridge_IsMegaDriveVideo() ||
                PicoDriveBridge_IsMasterSystem()))) &&
             !_bMenu) ? 1 : 0;

        if (native240pPar != s_native240pPar)
        {
            GSK_SetNative240pPar(native240pPar);
            s_native240pPar = native240pPar;
        }

        /* AURORA_MD_YOFFSET_MINUS9_V1
         * Plain MD only: effective Y = configured menu Y - 9. */
        {
            static int s_lastMdYBias = 9999;
            int mdYBias = 0;

            if (mdYBias != s_lastMdYBias)
            {
                GSK_SetGameplayYOffsetBias(mdYBias);
                s_lastMdYBias = mdYBias;
            }
        }



    /* Re-anchor FRAME_1 to gsKit's current draw buffer before any
       primitive runs this frame. The legacy GS_SetDrawFB used to do
       this implicitly per frame; gsKit_sync_flip only swaps the
       display buffer, not the draw buffer. Without this, prims drew
       to a stale (or, after the SNES blender ran, completely wrong)
       buffer and the visible framebuffer flickered black on every
       other frame. See gskit_backend.h for the longer rationale. */
    /* AURORA_GS_PARTIAL_GAMEPLAY_CLEAR_V1
     * Only enable the reduced clear when this frame is guaranteed to draw the
     * normal game output texture. Menu, boot and black-screen frames retain
     * the complete physical clear. */
    /* AURORA_PD_DIRECT_MD_SKIP_CLEAR_V3_RENDER_20260821
     * DrawDirectGs() for plain MD always emits an opaque backdrop over
     * the entire transformed 256x240 canvas, which is the entire active
     * framebuffer in 240p/480i/1080i. Avoid clearing pixels that are
     * guaranteed to be replaced later in this same frame. */
    /* AURORA_PD_DIRECT_8BIT_SKIP_CLEAR_V4_20260821 */
    /* AURORA_PCE_EXPERIMENTAL_V13_NATIVE_PAR_REBUILD
     *
     * PCE base geometry is 256x240 with a narrower-than-4:3 presentation.
     * Use the same native-240p PCRTC technique already used by Aurora's
     * NES/SNES path: change scanout magnification, NOT framebuffer sampling.
     * Every one of the 256 source columns is still scanned exactly once.
     *
     * Keep menu/black-screen presentation untouched. The loaded system remains
     * _pPce while Aurora's menu is open, so explicitly turn the PCE-only PAR
     * correction back off there. GSK_SetNative240pPar internally affects only
     * the 240p display mode.
     */
    if (_pSystem == _pPce)
    {
        /* AURORA_PCE_CRT_OVERSCAN_352_V9_20260830
         * PCE has explicit 256/352/512 PCRTC profiles in gskit_backend.
         * Do not feed the fixed 512 framebuffer through NES/SNES PAR math. */
        GSK_SetNative240pPar(0);
    }

    GSK_SetGameplaySkipClear(
        (!_bMenu && !_MainLoop_BlackScreen &&
         (((_pSystem == _pSega) &&
           PicoDriveBridge_CanDirectGsVideo() &&
           (PicoDriveBridge_IsMegaDriveVideo() ||
            g_GskVideoMode == GSK_VIDMODE_240P)) ||
          ((_pSystem == _pPce) &&
           PceBridge_CanDirectGsVideo()))) ? TRUE : FALSE);
    /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911
     * Interlaced 2x2 handheld presentation leaves physical side bars, so use
     * the existing complete framebuffer clear instead of full-width fast-clear. */
    /* AURORA_GAMBATTE_SQUARE_CLEAR_V13_20260911: retained and extended by GBA V14. */
    GSK_SetGameplayFastClear(
        (!_bMenu && _pSystem && !_MainLoop_BlackScreen &&
         !(GSK_GetActiveVideoMode() != GSK_VIDMODE_240P &&
           bSquareHandheldGameplay)) ? TRUE : FALSE);
    GSK_ResetFrame();

    // render frame
    GPPrimDisableZBuf();

    /* Per-frame full-screen clear to black.
     *
     * MainLoopRender historically NEVER cleared the framebuffer: it
     * relied on the full-screen _OutTex blit below to repaint every
     * pixel.  But that blit is (a) skipped entirely when
     * _MainLoop_BlackScreen is set (boot log + menus) and (b) even when
     * drawn it starts at dy=8, so the top rows are never touched.  With
     * DoubleBuffering=ON each draw goes to the alternate buffer, so any
     * row we don't repaint shows stale content from two frames ago --
     * which appears as a fixed-position horizontal "faixa"/stripe
     * through the text (worst in the log and menus, where nothing
     * covers the background).  Clearing to black first costs a single
     * sprite and removes the band entirely.  GSK_ResetFrame now clears
     * the complete PHYSICAL framebuffer, including any overscan borders.
     * Keep the Poly state reset here, but do not queue a duplicate
     * logical-canvas clear. */
    PolyTexture(NULL);
    PolyBlend(FALSE);
    PolyColor4f(0.0f, 0.0f, 0.0f, 1.0f);

	if (!_MainLoop_BlackScreen)
	{
//		Float32 fDestColor = (_bMenu || _MainLoop_ModalCount) ? 0.10f : 0.80f;
		Float32 fDestColor = 0.10f;
		
		if  (!_bMenu && !_MainLoop_ModalCount)
		{
			fDestColor = _MainLoop_fOutputIntensity;
		}

		static Float32 fColor=0.0f;
		Float32 dx = 0.0f;
		Float32 dy = 8.0f;

		if (fColor < fDestColor) 
		{
			fColor+=0.06f;
			if (fColor > fDestColor) 
			{
				fColor = fDestColor;
			}
		} 

		if (fColor > fDestColor) 
		{
			fColor-=0.06f;
			if (fColor < fDestColor) 
			{
				fColor = fDestColor;
			}
		}


        /* AURORA_PD_DIRECT_8BIT_GS_V1_20260821 */
        if (_pSystem == _pNes &&
            QuicknesBridge_CanDirectGsVideo())
        {
            QuicknesBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP,
                g_GskVideoMode == GSK_VIDMODE_240P ? 2 : 4,
                fColor);
        }
        else if (_pSystem == _pFds &&
                 FceummFdsBridge_CanDirectGsVideo())
        {
            /* AURORA_FCEUMM_FDS_PERF_DIRECT_T8_V3_20260827: native FCEUmm XBuf -> GS T8 + CLUT. */
            FceummFdsBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP,
                g_GskVideoMode == GSK_VIDMODE_240P ? 2 : 4,
                fColor);
        }
        else if (_pSystem == _pPce &&
                 PceBridge_CanDirectGsVideo())
        {
            /* AURORA_PCE_EXPERIMENTAL_V10_DIRECT_GS */
            PceBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP, fColor);
        }
        else if (_pSystem == _pSega &&
                 PicoDriveBridge_CanDirectGsVideo())
        {
            /* AURORA_PD_DIRECT_T8_RENDER */
            PicoDriveBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP, fColor);
        }
        else
        {
            /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911
             * Scope the 480i/1080i 2x2 transform to the handheld game image;
             * overlays/status/modal geometry immediately returns to normal. */
            /* AURORA_GAMBATTE_DRAW_SCOPE_V13_20260911: retained and extended by GBA V14. */
            const Bool bHandheldSquareDraw =
                (GSK_GetActiveVideoMode() != GSK_VIDMODE_240P &&
                 bSquareHandheldGameplay) ? TRUE : FALSE;
            if (bHandheldSquareDraw)
                GSK_SetGbSquarePixelDraw(1);

            PolyBlend(FALSE);
            PolyTexture(&_OutTex);
            PolyUV(0,0,256,240);
    		PolyColor4f(fColor, fColor, fColor, 1.0f);
    
    
                    if (g_GskVideoMode == GSK_VIDMODE_240P &&
                        (_pSystem == _pNes || _pSystem == _pFds)) /* AURORA_FCEUMM_FDS_V0_5_RENDER */
            {
    /*
     * InfoNES 240p overscan compensation.
     *
     * Keep the NES framebuffer at its native 256x240 size and
     * preserve a 1:1 pixel mapping. Only reposition the image
     * to compensate for CRT overscan.
     */
    PolyRect(0.0f, 2.0f, 256.0f, 240.0f);
            }
            else if (g_GskVideoMode == GSK_VIDMODE_240P && _pSystem == _pSega)
            {
    /* AURORA_SEGA_NATIVE_240P_V1
     * PicoDriveBridge already composes MD/SMS/GG into an exact 256x240 logical
     * raster. Present that raster 1:1; PCRTC handles the CRT pixel aspect. */
    PolyRect(0.0f, 0.0f, 256.0f, 240.0f);
            }
            else if (_pSystem == _pSnes)
            {
                /* AURORA_GB_HOTFIX_R13F_STANDALONE_BOOT_AUDIO_Y_20260909_Y_SCOPE:
                 * GB standalone positioning is done in GambatteSystem.
                 * Native SNES/SGB outer presentation remains at Y=8. */
                PolyRect(0.0f, 8.0f, 256.0f, 240.0f);
            }
            else if (_pSystem == _pGba)
            {
                /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911 */
                PolyRect(0.0f, 0.0f, 256.0f, 240.0f);
            }
            else
            {
    PolyRect(0.0f, 4.0f, 256.0f, 240.0f);
            }
    
            PolyBlend(TRUE);

            if (bHandheldSquareDraw)
                GSK_SetGbSquarePixelDraw(0);
        }

        /* AURORA_QN_LIGHTGUN_OVERLAY_V7_20260829
         * Post-frame overlay: never enters the QuickNES sensor framebuffer. */
        if (_pSystem == _pNes && QuicknesBridge_LightGunActive())
            QuicknesBridge_DrawLightGunCursor(
                g_GskVideoMode == GSK_VIDMODE_240P ? 2 : 4);

        //PolyTexture(NULL);
        //PolyRect(dx,dy,128,120);
    }


    #if AURORA_RUNTIME_DIAG_OVERLAY
    /* AURORA_PCE_KRAZY_RUNTIME_DIAG_V11R3_20260830
     * Hidden diagnostics retained for future PCE/QuickNES investigations. */
    if (!_bMenu && _pSystem == _pPce)
    {
        unsigned vw=0, vh=0, pp=0;
        int pfb=0, nativeClass=0;
        int gfb=0, win=0, dw=0, mh=0, sx=0, ovs=0, ws=0;

        PceBridge_GetVideoDebug(&vw, &vh, &pp, &pfb, &nativeClass);
        GSK_GetPceDebugState(&gfb, &win, &dw, &mh, &sx, &ovs, &ws);

        FontSelect(2);
        FontColor4f(1.0f, 1.0f, 0.0f, 1.0f);
        FontPrintf(24, 20, "PCE W%u H%u P%u FB%d N%d", vw, vh, pp, pfb, nativeClass);
        FontPrintf(24, 32, "GFB%d WIN%d DW%d M%d O%d W%d", gfb, win, dw, mh+1, ovs, ws);
    }
    /* AURORA_V6_1G_QN_KRAZY_DEBUG_CONSUMER_CURE_20260831
     * Retired obsolete QuickNES mapper-79 debug overlay; PCE diagnostics above remain. */
    #endif /* AURORA_RUNTIME_DIAG_OVERLAY */

    if (!_bMenu)
    {	
	
		if (s_pMovieClip->IsPlaying())
		{
	        FontSelect(2);
	        FontColor4f(0.5, 0.5f, 0.5f, 1.0f);
	        FontPrintf(240,220, ">");
		}

		if (s_pMovieClip->IsRecording())
		{
	        FontSelect(2);
	        FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
	        FontPrintf(240,220, "O");
		}


		switch (_MainLoop_uDebugDisplay)
        {
		case 0:
/*	        FontSelect(2);
	        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
	        FontPrintf(40,170, "%08X", InputGetPadData(0));
  */

//		        FontSelect(2);
//		        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
//		        FontPrintf(40,190, "%3d", xpadGetFrameCount(0,0));
			break;
		case 1:
		/*
	        FontSelect(2);
	        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
	        FontPrintf(40,190, "%3d %3d", NetInput.InputSize[0], NetInput.OutputSize[0]);
	        FontPrintf(40,200, "%3d %3d", NetInput.InputSize[1], NetInput.OutputSize[1]);
	        FontPrintf(40,210, "%3d %3d", NetInput.InputSize[2], NetInput.OutputSize[2]);
	        FontPrintf(40,220, "%3d %3d", NetInput.InputSize[3], NetInput.OutputSize[3]);
			*/
			break;
		case 2:
	        FontSelect(2);
	        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
	        FontPrintf(40,170, "%08X", _uInputFrame);
	        FontPrintf(40,180, "%08X", _uInputChecksum[0]);
	        FontPrintf(40,190, "%08X", _uInputChecksum[1]);
	        FontPrintf(40,200, "%08X", _uInputChecksum[2]);
	        FontPrintf(40,210, "%08X", _uInputChecksum[3]);
	        FontPrintf(40,220, "%08X", _uInputChecksum[4]);
			break;
		case 3:
			FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
			FontPrintf(195, 210, "%8d", _uVblankCycle / 1024);
			break;
        }

        FontSelect(2);
		FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
		{

/*
		FontPrintf(15, 180, "%08X %08X Y", (Int32)(_ColorCalib.y_mul * 0x10000), (Int32)(_ColorCalib.y_add * 0x10000));
		FontPrintf(15, 190, "%08X %08X I", (Int32)(_ColorCalib.i_mul * 0x10000), (Int32)(_ColorCalib.i_add * 0x10000));
		FontPrintf(15, 200, "%08X %08X Q", (Int32)(_ColorCalib.q_mul * 0x10000), (Int32)(_ColorCalib.q_add * 0x10000));
  */

		/*
		FontPrintf(195, 180, "%6.3f %6.3f", _ColorCalib.y_mul, _ColorCalib.y_add);
		FontPrintf(195, 190, "%6.3f %6.3f", _ColorCalib.i_mul, _ColorCalib.i_add);
		FontPrintf(195, 200, "%6.3f %6.3f", _ColorCalib.q_mul, _ColorCalib.q_add);
		*/
		}

//			FontPrintf(195, 210, "%8d", _AudMix.GetLastOutput());
    }


	/* Keep menu audio alive even while a modal overlays the UI. Previously
	   BgmUpdate lived only in the non-modal branch below, so every fixed-time
	   message starved audsrv regardless of whether any I/O was happening. */
	if (_bMenu)
	{
		/* AURORA_FINAL_V1_3_NORMAL_MENU_BGM_SESSION_20260901
		 * _bMenu alone is insufficient: isolated quick-state/device/format
		 * prompts also pause the core without acquiring the CD transport.
		 * BGM may scan/open storage, so it is legal only for a session that
		 * successfully entered through _MenuEnable(TRUE). */
		if (MainLoopNormalMenuBgmSessionActive() &&
		    _MainLoop_pScreen != (CScreen *)_MainLoop_pStateConfirmScreen)
			BgmUpdate();
		/* Draw the live menu first, then place modal/status text on top. The
		   previous order painted _MenuDraw after the status and hid it. */
		_MenuDraw();
	}

	if (_MainLoop_ModalCount > 0)
	{
		FontSelect(0);
		{
			const Int32 textW = FontGetStrWidth(_MainLoop_ModalStr);
			const Int32 textX = 128 - textW / 2;

			/* AURORA_GB_HOTFIX_R13E_20260909_MODAL_BOX
			 * Opaque black backing only behind the centered modal/error text. */
			PolyTexture(NULL);
			PolyBlend(FALSE);
			PolyColor4f(0.0f, 0.0f, 0.0f, 1.0f);
			PolyRect((Float32)(textX - 4), 96.0f,
			         (Float32)(textW + 8), 16.0f);
			PolyBlend(TRUE);

			FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
			FontPrintf(textX, 100, _MainLoop_ModalStr);
		}

		_MainLoop_ModalCount--;
	}
	else
	{
		if (_MainLoop_StatusCount > 0)
		{
			FontSelect(0);
			/* AURORA_V15_STATUS_SHADOW_REVERT_20260824
			 * Revert V15 black double-draw: transient status text is plain cyan again. */
			FontColor4f(0.0, 0.8f, 0.8f, 1.0f);
			FontPrintf(20, 200, _MainLoop_StatusStr);
			_MainLoop_StatusCount--;
		}
	}

	#if CODE_DEBUG
	if (_MainLoop_bMCSaveReady && MCSave_WriteSync(FALSE, NULL))
	{
		FontSelect(1);
		FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
		if (_iFrame & 4)
			FontPrintf(235,216, "#");
	}
	#endif



    PROF_ENTER("GPFlush");
    GPFifoFlush();
    PROF_LEAVE("GPFlush");

    /* gsKit_sync_flip waits for vsync, swaps the display buffer
       and resets gsKit's draw queue for the next frame. The
       legacy WaitForNextVRstart / GS_SetCrtFB / GS_SetDrawFB
       block is now subsumed by this single call. */
    PROF_ENTER("WaitVBlank");
    if ( (_iFrame&15)==0)   _uVblankCycle = ProfCtrGetCycle();
    GSK_SyncFlip();
    if ( (_iFrame&15)==0)   _uVblankCycle = ProfCtrGetCycle() - _uVblankCycle;
    _MainLoopSafeFrameskipAfterFlip();
    PROF_LEAVE("WaitVBlank");

    /* AURORA_AUDIO_FAILSOFT_POSTVBLANK_V1
     * The frame is already presented. Keep the normal gameplay audio drain
     * here so synchronous audsrv RPC latency is moved out of the pre-render
     * deadline. Aud_BufferedAsyncStart uses only wait=0 drains. */
    if (!_bMenu && _pSystem && !_MainLoop_BlackScreen)
        Aud_BufferedAsyncStart();

    /* whichdrawbuf is now decorative - gsKit owns the active
       framebuffer index via gsGlobal->ActiveBuffer. Keep it
       alive so the diff against the original is small. */
    whichdrawbuf ^= 1;
    (void)whichdrawbuf;

    _iFrame++;
}


/* AURORA_PCE_KRAZY_RUNTIME_DIAG_V11R3_20260830 */

/* AURORA_PCE_ROOT512_KRAZY_LATCH_V12_20260830 */

/* AURORA_PCE_RELEASE_CLEANUP_FINAL_V15_20260830 */
