#pragma once

#include "types.h"

/* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2
 * Safe Frameskip is host scheduling only; it is never part of emulated state. */
Int32 MainLoopSafeFrameskipGetLevel(void);
void MainLoopSafeFrameskipSetLevel(Int32 level);
Bool MainLoopSafeFrameskipGetEnabled(void);
void MainLoopSafeFrameskipSetEnabled(Bool enabled);
/* AURORA_SAFE_FRAMESKIP_PICODRIVE_AUTO_V1: PicoDrive-style Auto decision once per host tick. */
Bool MainLoopSafeFrameskipTake(Bool allowed);
Bool MainLoopSafeFrameskipConsumePresentationSkip(void);
/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830
 * CDDA may request ONE host skip so synchronous storage refill happens
 * only inside the already-authorized Safe Frameskip mechanism. */
void MainLoopSafeFrameskipRequestCdAudioWindow(void);
/* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
void MainLoopSafeFrameskipCancelCdAudioWindow(void);

