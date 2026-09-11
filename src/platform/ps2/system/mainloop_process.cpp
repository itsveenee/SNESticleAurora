/* mainloop_process.cpp
 *
 * Hosts MainLoopProcess() -- the per-frame driver that polls input,
 * runs the SNES core, drives netplay/movie clip, kicks SRAM bookkeeping
 * and finally calls into MainLoopRender().
 *
 * Owns the file-static _iframetex flip flag used to alternate between
 * the two CRenderSurface entries in _fbTexture[].
 *
 * Extracted from mainloop.cpp during the Batch 3 split. Behaviour is
 * unchanged.
 */

#include <stdio.h>
#include <libpad.h>

#include "mainloop_debug.h"
#include "mainloop_shared.h"
#include "mainloop_input.h"
#include "mainloop_ui.h" /* AURORA_FCEUMM_FDS_V0_6_DRIVE_SYNC */
#include "mainloop_state.h"
#include "mainloop_exec.h"
#include "mainloop_safe_frameskip.h" /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */
#include "snppurender.h"
#include "mainloop_iop.h"
#include "sega/picodrive/picodrive_bridge.h"
/* AURORA_ALLCORES_PERF_V5_20260824 */
#include "nes/quicknes/quicknes_bridge.h"
#include "nes/fceumm/fceumm_fds_bridge.h" /* AURORA_FCEUMM_FDS_V0_5_PROCESS */
/* AURORA_PCE_EXPERIMENTAL_V1 */
#include "pce/beetle/pce_bridge.h"
#include "gb/system/gambattesystem.h" /* AURORA_GAMBATTE_MAINLOOP_PRESENT_V2R6_20260908 */
#include "gba/system/gpspsystem.h" /* AURORA_GPSP_GBA_V14_EXPLICIT_UPLOAD_20260911 */
/* AURORA_PS2_PERF_V4_20260824 */
#include "gskit_backend.h"

#include "types.h"
#include "console.h"
#include "input.h"
#include "snes.h"
#include "snio.h" /* AURORA_FAMICOM_MIC_CFG41_20260828: NES/FDS microphone carrier */
#include "rendersurface.h"
#include "mixbuffer.h"
#include "prof.h"
#include "emusys.h"
#include "emumovie.h"

extern "C" {
#include "gpprim.h"
};

extern "C" {
#include "netplay_ee.h"
};

extern "C" {
#include "audio.h"
};

static Uint32 _iframetex=0;

/* AURORA_GAMEPLAY_HEADROOM_TRANSITION
 * Host-audio warmup state only; no emulated/save-state state. */
static Bool _AudioGameplayWasActive = FALSE;
static Emu::System *_AudioGameplaySystem = NULL;
static Bool _SnesMouseWasActive = FALSE;
static Bool _UsbMouseGameplayWasActive = FALSE;


Bool MainLoopProcess()
{
    NetPlayRPCInputT NetInput;
    /* AURORA_PD_CADENCE_RESUME_FIRST_FRAME_V7_20260821 */
    Bool bGameplayJustStarted = FALSE;

    PROF_ENTER("Frame");

    /* AURORA_RUNTIME_LEAN_V1_FRONTEND_20260824
     * NetPlayRPCProcess() is a permanent empty compatibility stub now that
     * netplay runs directly on the EE. Do not pay a cross-TU call every frame. */

    PROF_ENTER("InputProcess");
    InputPoll();

    PROF_LEAVE("InputProcess");


	{
	    /* OR the digital pad bits with d-pad bits synthesised from each
	       pad's left analog stick. The synthesised bits only travel
	       through _MainLoopInputProcess (menu / screen-cycle / debug
	       triggers), so SNES gameplay still uses the strictly digital
	       _Input_PadData via _MainLoopInput. Result: the analog stick
	       drives menu navigation just like InfinityStation, without
	       leaking into the running game. */
	    Uint32 buttons =
	          InputGetPadData(0) | InputGetPadData(1)
	        | InputGetPadData(2) | InputGetPadData(3)
	        | InputGetPadDpadFromAnalog(0) | InputGetPadDpadFromAnalog(1)
	        | InputGetPadDpadFromAnalog(2) | InputGetPadDpadFromAnalog(3);

	    _MainLoopInputProcess(buttons);
	}

//	_MainLoopInputProcess(InputGetPadData(0));
//	_MainLoopInputProcess(InputGetPadData(1));

    {
        const Bool bGameplayNow =
            (!_bMenu && _pSystem && !_MainLoop_BlackScreen) ? TRUE : FALSE;

        bGameplayJustStarted =
            (bGameplayNow &&
             (!_AudioGameplayWasActive ||
              _AudioGameplaySystem != _pSystem)) ? TRUE : FALSE;

        if (bGameplayJustStarted)
        {
            /* Start SMS/GG autofire in its ON half whenever gameplay resumes. */
            MainLoopTurboRearmHostPhase();
            /* AURORA_FCEUMM_FDS_V14_AUDIO_BURST_STABILITY_20260827
             * FDS also produces roughly one 48 kHz frame of audio per host
             * tick. 1024 drains faster than production while preventing an
             * accumulated backlog from becoming one 4095-frame SIF RPC. */
            /* AURORA_V3_SAFE_AUDIO_BURST_20260828:
             * <=960 host frames/tick at PAL50 worst case for these cores. */
            Aud_SetAsyncBurstLimit(
                (_pSystem == _pSnes ||
                 _pSystem == _pFds ||
                 _pSystem == _pPce ||
                 _pSystem == _pSega ||
                 _pSystem == _pNes ||
                 _pSystem == _pGb) ? 1024 : 4095); /* AURORA_GAMBATTE_MAINLOOP_PRESENT_V2R6_20260908 */

            /* AURORA_AUDIO_SPLIT_VOLUMES_V36_20260823
             * Select which saved final gain the shared mixer will use. */
            AudMixSetSegaVolumeMode(_pSystem == _pSega ? 1 : 0);
            AudMixSetPceVolumeMode(_pSystem == _pPce ? 1 : 0);

            /* Fill host-audio safety headroom BEFORE the first cold frame. */
            Aud_PrepareGameplayHeadroom();
            _AudioGameplaySystem = _pSystem;
        }

        _AudioGameplayWasActive = bGameplayNow;
    }

    if (!_bMenu && _pSystem && !_MainLoop_BlackScreen)
    {
        CRenderSurface *pSurface;
        CMixBuffer *pMixBuffer = NULL;
        pSurface = _fbTexture[_iframetex];
	
		Emu::SysInputT Input;
		Bool bSnesMouse = FALSE;
		Int32 nSnesMouseX = 0;
		Int32 nSnesMouseY = 0;
		Uint32 uSnesMouseButtons = 0;
	
		Int32 iPad;

		/* AURORA_AUTO_SNES_MOUSE_V1_5
		 * Mouse replaces gameplay port 1 on SNES and non-8-bit Sega.
		 * Pads remain polled for menus and frontend hotkeys. */
		if ((_pSystem == _pSnes ||
		     (_pSystem == _pSega && !PicoDriveBridge_Is8Bit())) &&
		    InputSnesMouseShouldUse())
		{
			bSnesMouse = TRUE;
			InputGetMouseData(&nSnesMouseX, &nSnesMouseY, &uSnesMouseButtons);
		}

        /*
        if (_WavFile.IsOpen())
        {
            pMixBuffer = &_WavFile;
        } else
        {
            pMixBuffer = &_AudMix;
        } 
        */                        
        pMixBuffer = _AudMix;
//                pMixBuffer = NULL;

		// read inputs
		for (iPad=0; iPad < 5; iPad++)
		{
			if (bSnesMouse && iPad == 0)
			{
				Input.uPad[iPad] = EMUSYS_DEVICE_DISCONNECTED;
			}
			else if (InputIsPadConnected(iPad))
			{
				/* OR the digital pad bits with d-pad bits synthesised from
				   the left analog stick so the analog stick drives the SNES
				   d-pad in-game (LEFT/RIGHT/UP/DOWN). Digital and analog
				   inputs are merged: if both press the same direction the
				   result is identical to a single press, so users can use
				   whichever they prefer (or both). */
				Uint32 uAnalogDpad = InputGetPadDpadFromAnalog(iPad);

				/* AURORA_QN_ARKANOID_ANALOG_ONLY_V2_20260828
				 * Arkanoid (Japan) owns P1 left-stick X as an absolute Vaus
				 * paddle. Do not also synthesize LEFT/RIGHT from that stick;
				 * the physical D-pad from InputGetPadData() remains available. */
				if (iPad == 0 && _pSystem == _pNes &&
				    QuicknesBridge_IsArkanoidVaus())
				    uAnalogDpad = 0;

				const Uint32 uHostPad = InputGetPadData(iPad) | uAnalogDpad;
				Input.uPad[iPad] = _MainLoopInput(uHostPad);

				/* AURORA_FAMICOM_MIC_CFG41_20260828
				 * L2 is already consumed by _MainLoopInput(), so START can
				 * never leak to NES pad 1. Reuse the otherwise ignored NES
				 * carrier SNESIO_JOY_L on P1 so netplay/movie input keeps the
				 * microphone event in-band. The bridges convert it to the
				 * Famicom controller-II microphone at $4016 D2. */
				if (iPad == 0 && (_pSystem == _pNes || _pSystem == _pFds))
				{
					Input.uPad[0] &= (Uint16)~SNESIO_JOY_L;
					if ((uHostPad & (PAD_L2 | PAD_START)) ==
					    (PAD_L2 | PAD_START))
						Input.uPad[0] |= SNESIO_JOY_L;
				}
			} else
			{
				Input.uPad[iPad] = EMUSYS_DEVICE_DISCONNECTED;
			}
		}

		// send controller 1 + 2 inputs combined to 32-bits
		NetInput.InputSend = ((Uint32)Input.uPad[0]) | (((Uint32)Input.uPad[1])<<16);

        PROF_ENTER("NetPlayClientInput");
        NetPlayClientInput(&NetInput);
        PROF_LEAVE("NetPlayClientInput");

        if (NetInput.eGameState == NETPLAY_GAMESTATE_PLAY)
        {
            if ((_pSystem->GetFrame()+1) != NetInput.uFrame)
            {
				#if CODE_DEBUG
                printf("Not executing frame %d %d\n", NetInput.uFrame, _pSystem->GetFrame());
				#endif
                NetInput.eGameState = NETPLAY_GAMESTATE_PAUSE;
            }

			// we are connected, retrieve input data
	        Input.uPad[0] = (Uint16)NetInput.InputRecv[0];
	        Input.uPad[1] = (Uint16)NetInput.InputRecv[1];
	        Input.uPad[2] = (Uint16)NetInput.InputRecv[2];
	        Input.uPad[3] = (Uint16)NetInput.InputRecv[3];
			Input.uPad[4] = EMUSYS_DEVICE_DISCONNECTED;

			if (Input.uPad[2] == EMUSYS_DEVICE_DISCONNECTED)
			{
				// if controller 3 is disconnected, use controller 2 of first peer
				Input.uPad[2] = (Uint16)(NetInput.InputRecv[0]>>16);
			}

			if (Input.uPad[3] == EMUSYS_DEVICE_DISCONNECTED)
			{
				// if controller 4 is disconnected, use controller 2 of second peer
				Input.uPad[3] = (Uint16)(NetInput.InputRecv[1]>>16);
			}
		
        }  
		else
		{
		
            if (s_pMovieClip->IsPlaying())
            {
                if (!s_pMovieClip->PlayFrame(Input))
                {
                    s_pMovieClip->PlayEnd();
                    ConPrint("Movie: Play End\n");
                }
            }
	
		}

        if (NetInput.eGameState != NETPLAY_GAMESTATE_PAUSE)
        {
			Emu::System::ModeE eMode;

            #if MAINLOOP_HISTORY
            if (_nHistory < 16384 * 2)
            {
                _History[_nHistory++] = Input.uPad[0];
                _History[_nHistory++] = Input.uPad[1];
                _History[_nHistory++] = Input.uPad[2];
                _History[_nHistory++] = Input.uPad[3];
            }
            #endif

			_uInputFrame    = NetInput.uFrame;
			_uInputChecksum[0] += Input.uPad[0];
			_uInputChecksum[1] += Input.uPad[1];
			_uInputChecksum[2] += Input.uPad[2];
			_uInputChecksum[3] += Input.uPad[3];
			_uInputChecksum[4] += Input.uPad[4];

			eMode = (NetInput.eGameState == NETPLAY_GAMESTATE_IDLE) ? Emu::System::MODE_ACCURATENONDETERMINISTIC : Emu::System::MODE_INACCURATEDETERMINISTIC;

            /* Safe Frameskip is host-only. Never alter deterministic
             * movie/netplay execution; the Auto scheduler receives
             * allowed=FALSE and resets its host timing debt. */
            /* AURORA_V4_11_CD_REALTIME_PACING_PCE_TOC_OFFSETS_20260830
             *
             * Safe Frameskip catches up by skipping MainLoopRender()'s
             * GSK_SyncFlip()/VBlank wait. That intentionally advances the
             * emulated host clock faster than wall time. For streaming CDDA,
             * doing so makes the logical playhead outrun the async USB worker.
             *
             * CD systems therefore keep real-time host pacing. Their video
             * priority comes from failsoft asynchronous CDDA, not from running
             * emulation faster than the display clock.
             */
            const Bool bStreamingCdNeedsRealtimePacing =
                ((_pSystem == _pSega &&
                  PicoDriveBridge_IsSegaCD()) ||
                 (_pSystem == _pPce &&
                  PceBridge_IsDiscLoaded())) ? TRUE : FALSE;

            const Bool bSafeFrameskipAllowed =
                (NetInput.eGameState == NETPLAY_GAMESTATE_IDLE &&
                 !s_pMovieClip->IsPlaying() &&
                 !s_pMovieClip->IsRecording() &&
                 !bStreamingCdNeedsRealtimePacing) ? TRUE : FALSE;

            if (s_pMovieClip->IsRecording())
            {
                if (!s_pMovieClip->RecordFrame(Input))
                {
                    s_pMovieClip->RecordEnd();
                    ConPrint("Movie: Reached end of record buffer!\n");
                }
            }

            /* AURORA_SAFE_FRAMESKIP_PICODRIVE_AUTO_V1: one Auto decision per Aurora host tick,
             * shared by SNES/reference emulator/QuickNES/PicoDrive/PCE. */
            const Bool bSafeSkip =
                MainLoopSafeFrameskipTake(bSafeFrameskipAllowed);
            if (!bSafeSkip)
                GPPrimDisableZBuf();

            /* Phase 2 of the NES integration: dispatch ExecuteFrame
               through the polymorphic Emu::System* when the loaded
               system is the NES wrapper.  The SNES path stays on its
               bespoke _ExecuteSnes helper that does the PPU upload
               and CLUT bookkeeping.  NesSystem renders directly into
               the surface (Phase 2 = diagnostic test pattern) and we
               upload to the EE texture from here. */
            if (_pSystem == _pNes)
            {
                QuicknesBridge_SetSkipVideo(bSafeSkip ? true : false);
                PROF_ENTER("NesExecuteFrame");
                _pNes->ExecuteFrame(&Input, pSurface, pMixBuffer, eMode);
                PROF_LEAVE("NesExecuteFrame");
                if (!bSafeSkip && !QuicknesBridge_CanDirectGsVideo())
                {
                    PROF_ENTER("NesTexUpload");
                    TextureUpload(&_OutTex, pSurface->GetLinePtr(0));
                    PROF_LEAVE("NesTexUpload");
                }
            }
            else if (_pSystem == _pFds)
            {
                /* AURORA_FCEUMM_FDS_V0_5_PROCESS */
                /* AURORA_FCEUMM_FDS_V12_3B_BRIDGE_HOTPATH_FIX_20260827: bridge skip is one-shot; false is already the default. */
                if (bSafeSkip)
                    FceummFdsBridge_SetSkipVideo(true);
                PROF_ENTER("FdsExecuteFrame");
                _pFds->ExecuteFrame(&Input, pSurface, pMixBuffer, eMode);
                PROF_LEAVE("FdsExecuteFrame");
                {
                    /* AURORA_FCEUMM_FDS_V12_3B_BRIDGE_HOTPATH_FIX_20260827: drive state changes only on load/eject/insert/restore. */
                    unsigned fdsSide = 0;
                    bool fdsInserted = false;
                    if (FceummFdsBridge_ConsumeDriveStateChange(&fdsSide, &fdsInserted))
                    {
                        const Bool bWasInserted = _MainLoop_bDiskInserted;
                        _MainLoop_iDisk = (Int32)fdsSide;
                        _MainLoop_bDiskInserted = fdsInserted ? TRUE : FALSE;
                        if (!bWasInserted && _MainLoop_bDiskInserted)
                        {
                            MainLoopStatusPrintf(90,
                                "FDS: Disk %d Side %c inserted",
                                (int)(_MainLoop_iDisk >> 1) + 1,
                                (_MainLoop_iDisk & 1) ? 'B' : 'A');
                        }
                    }
                }
                /* AURORA_FCEUMM_FDS_PERF_DIRECT_T8_V3_20260827: normal FDS presentation is native T8/CLUT.
                 * Keep the historical RGBA upload only as a fallback. */
                if (!bSafeSkip && !FceummFdsBridge_CanDirectGsVideo())
                {
                    PROF_ENTER("FdsTexUpload");
                    TextureUpload(&_OutTex, pSurface->GetLinePtr(0));
                    PROF_LEAVE("FdsTexUpload");
                }
            }
            else if (_pSystem == _pSega)
            {
                /* AURORA_PD_HOST_CADENCE_EXEC_V1_20260821
                 *
                 * O core produz 50 fps em PAL e 60 fps em NTSC, enquanto o
                 * Aurora apresenta uma vez por VBlank do GS. Quando as famílias
                 * diferem, converta a cadência no frontend:
                 *
                 *   PAL 50 -> NTSC ~59.94: repete periodicamente um frame;
                 *   NTSC 60 -> PAL 50: executa periodicamente dois frames e
                 *                       apresenta apenas o mais novo.
                 *
                 * AURORA_PD_HOST_CADENCE_ALL_SEGA_V6_20260821
                 * O one-shot skip do V2 faz o primeiro frame de um burst 60->50
                 * avançar CPU+áudio sem reescrever vídeo. Assim o caminho RGBA
                 * também pode usar 0/2 ExecuteFrame(): num tick sem core frame,
                 * _OutTex simplesmente reapresenta a última imagem residente.
                 * Netplay e movies permanecem 1:1 para preservar determinismo.
                 */
                static Uint32 sPdHostNum = 0;
                static Uint32 sPdHostDen = 0;
                static Int32 sPdGameHz = 0;
                /* AURORA_PD_NTSC_5994_CLOCK_V1_20260822
                 * Phase is rational; all products stay below ~61 million. */
                static Uint32 sPdPhase = 0;
                /* AURORA_V4_4_BUILD_FIX_32X_VIDEO_FIRST_20260830
                 * Only 32X uses this recovery latch. */
                static Int32 sPd32xAudioProtectFrames = 0;

                Bool bDirectSega;
                Bool bIs32x;
                Bool b32xSacrificeAudio;
                Int32 executeFrames = 1;

                PicoDriveBridge_SetRegion((int)g_SnesForceRegion);
                PicoDriveBridge_SetMouseInput(
                    bSnesMouse ? true : false,
                    (int)nSnesMouseX,
                    (int)nSnesMouseY,
                    (unsigned)uSnesMouseButtons);

                bDirectSega =
                    PicoDriveBridge_CanDirectGsVideo() ? TRUE : FALSE;

                bIs32x = PicoDriveBridge_Is32X() ? TRUE : FALSE;
                if (!bIs32x)
                    sPd32xAudioProtectFrames = 0;
                else if (bSafeSkip)
                    sPd32xAudioProtectFrames = 2;

                b32xSacrificeAudio =
                    (bIs32x &&
                     (bSafeSkip || sPd32xAudioProtectFrames > 0))
                    ? TRUE : FALSE;

                if (bIs32x && !bSafeSkip &&
                    sPd32xAudioProtectFrames > 0)
                    --sPd32xAudioProtectFrames;

                if (NetInput.eGameState == NETPLAY_GAMESTATE_IDLE &&
                    /* AURORA_PD_HOST_CADENCE_ALL_SEGA_V6_GATE_20260821 */
                    !s_pMovieClip->IsPlaying() &&
                    !s_pMovieClip->IsRecording())
                {
                    Uint32 hostNum = 60000, hostDen = 1001;
                    Uint32 gameNum, gameDen;
                    Uint32 add, threshold;
                    Int32 gameHz;

                    GSK_GetRefreshRate(&hostNum, &hostDen);
                    if (hostNum == 0 || hostDen == 0)
                    {
                        hostNum = 60000;
                        hostDen = 1001;
                    }

                    gameHz = PicoDriveBridge_GetNominalFrameRate();

                    /* Match the PS2/core sound timebase above: PAL is 50/1;
                     * Aurora's NTSC PicoDrive presentation is 60000/1001.
                     * Do not collapse NTSC to integer 60 here. */
                    if (gameHz == 50)
                    {
                        gameNum = 50;
                        gameDen = 1;
                    }
                    else
                    {
                        gameNum = 60000;
                        gameDen = 1001;
                    }

                    add = gameNum * hostDen;
                    threshold = hostNum * gameDen;

                    if (add != threshold)
                    {
                        /* AURORA_PD_CADENCE_RESUME_FIRST_FRAME_V7_RESET_20260821 */
                        if (bGameplayJustStarted ||
                            _pSega->GetFrame() == 0 ||
                            hostNum != sPdHostNum ||
                            hostDen != sPdHostDen ||
                            gameHz != sPdGameHz)
                        {
                            sPdHostNum = hostNum;
                            sPdHostDen = hostDen;
                            sPdGameHz = gameHz;
                            sPdPhase =
                                (add < threshold && threshold > 0)
                                ? threshold - 1 : 0;
                        }

                        executeFrames = 0;
                        sPdPhase += add;

                        while (sPdPhase >= threshold &&
                               executeFrames < 2)
                        {
                            sPdPhase -= threshold;
                            ++executeFrames;
                        }
                    }
                    else
                    {
                        sPdHostNum = hostNum;
                        sPdHostDen = hostDen;
                        sPdGameHz = gameHz;
                        sPdPhase = 0;
                    }
                }
                else
                {
                    sPdHostNum = 0;
                    sPdHostDen = 0;
                    sPdGameHz = 0;
                    sPdPhase = 0;
                }

                /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830
                 * No CDDA storage work is permitted on this thread. */
                PROF_ENTER("SegaExecuteFrame");
                for (Int32 iPdFrame = 0;
                     iPdFrame < executeFrames;
                     ++iPdFrame)
                {
                    /* AURORA_PD_SKIP_DISCARDED_VIDEO_V2_EXEC_20260821
                     * In 60->50 conversion, the first of a two-frame burst is
                     * never displayed. CPU + audio still advance; only video
                     * rendering is skipped for that discarded frame. */
                    const Bool bCadenceDiscard =
                        (executeFrames > 1 &&
                         iPdFrame + 1 < executeFrames) ? TRUE : FALSE;
                    /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830
                     * Safe Frameskip is NEVER an I/O permission window now. */

                    /* 32X ONLY: preserve game/PWM timing, drop only produced
                     * audio when recovering video or on a hidden burst frame. */
                    if (bIs32x)
                        PicoDriveBridge_Set32xAudioSacrifice(
                            (b32xSacrificeAudio || bCadenceDiscard)
                            ? true : false);

                    PicoDriveBridge_SetSkipVideo(
                        (bCadenceDiscard || bSafeSkip) ? true : false);

                    /* Sempre passe pSurface como fallback. No caminho direto
                     * ela não é tocada; se a geometria mudar durante retro_run,
                     * o bridge ainda consegue produzir o RGBA de segurança. */
                    _pSega->ExecuteFrame(
                        &Input, pSurface, pMixBuffer, eMode);
                }
                PROF_LEAVE("SegaExecuteFrame");

                /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830
                 * CDDA underrun is audio loss only; it never requests a skip. */

                /* Releia depois de retro_run(): a geometria/PSM do core pode
                 * ter mudado durante o frame. */
                bDirectSega =
                    PicoDriveBridge_CanDirectGsVideo() ? TRUE : FALSE;

                if (!bSafeSkip && !bDirectSega && executeFrames > 0)
                {
                    PROF_ENTER("SegaTexUpload");
                    TextureUpload(&_OutTex, pSurface->GetLinePtr(0));
                    PROF_LEAVE("SegaTexUpload");
                }
            }
            else if (_pSystem == _pPce)
            {
                /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830 */
                PceBridge_SetSkipVideo(bSafeSkip ? true : false);
                PROF_ENTER("PceExecuteFrame");
                _pPce->ExecuteFrame(&Input, pSurface, pMixBuffer, eMode);
                PROF_LEAVE("PceExecuteFrame");
                /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830
                 * CDDA underrun cannot influence presentation timing. */

                /* AURORA_PCE_V10_DIRECT_PROCESS */
                if (!bSafeSkip && !PceBridge_CanDirectGsVideo())
                {
                    PROF_ENTER("PceTexUploadFallback");
                    TextureUpload(&_OutTex, pSurface->GetLinePtr(0));
                    PROF_LEAVE("PceTexUploadFallback");
                }
            }
            else if (_pSystem == _pGb)
            {
                PROF_ENTER("GbExecuteFrame");
                _pGb->ExecuteFrame(&Input,
                    bSafeSkip ? NULL : pSurface, pMixBuffer, eMode);
                PROF_LEAVE("GbExecuteFrame");

                if (!bSafeSkip)
                {
                    /* AURORA_GAMBATTE_MAINLOOP_PRESENT_V2R6_20260908
                     * r5 executed Gambatte via the generic/SNES branch, but
                     * SNES presentation never uploads a generic RGBA surface.
                     * Standalone GB is software video, so upload it explicitly. */
                    PROF_ENTER("GbTexUpload");
                    TextureUpload(&_OutTex, pSurface->GetLinePtr(0));
                    PROF_LEAVE("GbTexUpload");
                }
            }
            else if (_pSystem == _pGba)
            {
                /* AURORA_GPSP_GBA_V14_EXPLICIT_UPLOAD_20260911
                 * gpSP is software video exactly like standalone Gambatte.
                 * v1-v13 executed it through the generic/SNES fallthrough,
                 * which never TextureUpload'ed its CRenderSurface. CPU/audio
                 * therefore ran while _OutTex remained stale/white. */
                if (_AudMix)
                {
                    Uint32 uRateNum = 60, uRateDen = 1;
                    GSK_GetRefreshRate(&uRateNum, &uRateDen);
                    _AudMix->SetFrameRateRational(uRateNum, uRateDen);
                }

                PROF_ENTER("GbaExecuteFrame");
                _pGba->ExecuteFrame(&Input,
                    bSafeSkip ? NULL : pSurface, pMixBuffer, eMode);
                PROF_LEAVE("GbaExecuteFrame");

                if (!bSafeSkip)
                {
                    PROF_ENTER("GbaTexUpload");
                    TextureUpload(&_OutTex, pSurface->GetLinePtr(0));
                    PROF_LEAVE("GbaTexUpload");
                }
            }
            else
            {
                /* AURORA_MEGA_V2_SNES_AUDIO_CLOCK
                   Match produced PCM to the GS VBlank clock. Keep this
                   SNES-only so the QuickNES path is unchanged. */
                if (_AudMix)
                {
                    Uint32 uRateNum = 60, uRateDen = 1;
                    GSK_GetRefreshRate(&uRateNum, &uRateDen);
                    _AudMix->SetFrameRateRational(uRateNum, uRateDen);
                }
				if (bSnesMouse)
					Input.uPad[0] = EMUSYS_DEVICE_DISCONNECTED;
				if (bSnesMouse || _SnesMouseWasActive)
					_pSnes->SetMouseInput(
						bSnesMouse,
						nSnesMouseX,
						nSnesMouseY,
						uSnesMouseButtons);
				_SnesMouseWasActive = bSnesMouse;
				{
					_ExecuteSnes(bSafeSkip ? NULL : pSurface,
					             pMixBuffer, &Input, eMode);
				}
            }
		    _iframetex^=1;
        }

        /* AURORA_AUDIO_POSTVBLANK_ONLY_V10_20260823
         * Gameplay PCM is staged in the EE FIFO here, but the IOP RPC
         * drain is intentionally deferred to MainLoopRender(), after
         * GSK_SyncFlip(). This keeps synchronous audsrv latency out of
         * the emulation/render deadline. */
    }

	/* Deferred menu work runs only after _MenuEnable has returned. In the
	   L2+R2 path this leaves two complete frames for the menu/status to become
	   visible before a synchronous SRAM write begins. */
    /* AURORA_RUNTIME_LEAN_V1_FRONTEND_20260824
     * Deferred SRAM/menu work can only exist while the menu is active. */
    if (_bMenu)
        _MenuRuntimeUpdate();

	MainLoopRender();

	/* AURORA_MOUSE_EXPLICIT_V4: no SIF mouse RPC in Off/Controller,
	   menus, NES, SMS or GG. First resumed USB frame only drains stale motion. */
	{
		Bool bUsbGameplay =
			(!_bMenu &&
			 (_pSystem == _pSnes ||
			  (_pSystem == _pSega && !PicoDriveBridge_Is8Bit())) &&
			 !_MainLoop_BlackScreen &&
			 InputSnesMouseGetMode() == INPUT_SNES_MOUSE_USB) ? TRUE : FALSE;
		if (bUsbGameplay)
			InputMousePollPostFrame(_UsbMouseGameplayWasActive ? TRUE : FALSE);
		else if (_UsbMouseGameplayWasActive)
			InputMouseClearSnapshot();
		_UsbMouseGameplayWasActive = bUsbGameplay;
	}

    /* AURORA_SMS_GG_TURBO_HOST_CLOCK_V2
     * Exactly one tick per completed frontend frame; never one tick per
     * controller and never one tick per PicoDrive core frame. */
    MainLoopTurboAdvanceHostFrame();

    PROF_LEAVE("Frame");

    #if PROF_ENABLED
    ProfProcess();
    #endif

    return TRUE;
}


/* AURORA_V4_4_BUILD_FIX_32X_VIDEO_FIRST_20260830 */

/* AURORA_V4_11_CD_REALTIME_PACING_PCE_TOC_OFFSETS_20260830 */
