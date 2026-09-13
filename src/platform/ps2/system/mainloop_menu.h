#pragma once

#include "types.h"

enum MainLoopMemCardFormatActionE
{
	MAINLOOP_MEMCARDFORMAT_STATE_SAVE,
	MAINLOOP_MEMCARDFORMAT_SRAM_SAVE,
	MAINLOOP_MEMCARDFORMAT_BROWSE
};

int _MainLoopMenuEvent(Uint32 Type, Uint32 Parm1, void *Parm2);
int _MainLoopStateMenuEvent(Uint32 Type, Uint32 Parm1, void *Parm2);
int _MainLoopStateBrowserEvent(Uint32 Type, Uint32 Parm1, void *Parm2);
int _MainLoopStateDeviceMenuEvent(Uint32 Type, Uint32 Parm1, void *Parm2);
int _MainLoopStateConfirmMenuEvent(Uint32 Type, Uint32 Parm1, void *Parm2);
int _MainLoopMemCardFormatMenuEvent(Uint32 Type, Uint32 Parm1, void *Parm2);
void _MainLoopStateMenuRefresh();
void _MainLoopStateDevicePromptOpen();
void _MainLoopStateDevicePromptCancel();
void _MainLoopStateConfirmPromptOpen(Bool bSave);
void _MainLoopStateConfirmPromptCancel();
void _MainLoopStateConfirmPromptInput(Uint32 buttons, Uint32 trigger);
/* AURORA_AUDIO_UI_SOFT_TRANSITION_V2_20260901 */
Bool _MainLoopStateConfirmPromptConsumeExecuted();
void _MainLoopMemCardFormatPromptOpen(
	Int32 iPort,
	MainLoopMemCardFormatActionE eAction
);
void _MainLoopMemCardFormatPromptCancel();
int _MainLoopLogEvent(Uint32 Type, Uint32 Parm1, void *Parm2);
extern const char *_MainLoopMenuEntries[];
extern char *_MainLoopStateMenuEntries[];
void _MainLoopStateBrowserReturn(void);
extern char *_MainLoop_pInstallFiles[];

/* AURORA_AUDIO_UI_SOFT_TRANSITION_V1_20260901
 * Audio transition helpers. HardCut/ResumeGame destroy the audsrv queue and
 * are reserved for real timeline discontinuities. UiMute/UiResume keep the
 * IOP/SPU2 service running for menu/prompt transitions. */
void MainLoopAudioHardCut(void);
void MainLoopAudioResumeGame(void);
void MainLoopAudioUiMute(void);
void MainLoopAudioUiResume(void);

/* AURORA_FINAL_V1_1_UI_CD_STORAGE_BARRIER_20260901
 * Shared by normal menu and isolated state-storage actions. */
Bool MainLoopCdUiQuiesce(void);
Bool MainLoopCdUiReady(void); /* AURORA_SSF2_PCE_MENU_FIX_V2_20260913_PCE_MENU_ASYNC */
void MainLoopCdUiResume(void);

/* AURORA_FINAL_V1_3_NORMAL_MENU_BGM_SESSION_20260901 */
Bool MainLoopNormalMenuBgmSessionActive(void);

/* AURORA_V4_16_SAFE_GAME_SWITCH_FLUSH_20260830 */
Bool MainLoopSramSaveBusy(void);

/* AURORA_V4_16_SAFE_GAME_SWITCH_FLUSH_20260830 */
