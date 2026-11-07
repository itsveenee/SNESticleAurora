#pragma once

#include "types.h"

/* AURORA_SAFE_FRAMESKIP_ONOFF_CATCHUP_V6_20260925
 * Safe Frameskip is host scheduling only; it is never part of emulated state.
 * Public semantics are now boolean.  GetLevel/SetLevel remain ABI-compatible
 * adapters: 0=OFF, every non-zero value=ON. */
Int32 MainLoopSafeFrameskipGetLevel(void);
void MainLoopSafeFrameskipSetLevel(Int32 level);
Bool MainLoopSafeFrameskipGetEnabled(void);
void MainLoopSafeFrameskipSetEnabled(Bool enabled);
/* AURORA_SAFE_FRAMESKIP_MENU_NEUTRAL_V7_20260926: host-only gameplay/UI scheduler transition. */
void MainLoopSafeFrameskipSetGameplayActive(Bool active);
/* AURORA_SAFE_FRAMESKIP_CRC_UNLIMITED_V21_2_20261001
 * Generic ROM identity API. CRC matching is resolved only when media is
 * attached; the per-frame scheduler uses only a cached owner pointer. */
/* No-Intro: 1=SNES, 2=NES/Famicom, 3=Game Boy Color.
 * Chamadas só durante transições/attach, nunca por frame. */
void AuroraRomIdentitySetNoIntroName(Uint32 system, const char *path);
Uint32 AuroraRomIdentityCRCForDetection(Uint32 crc);
Uint32 MainLoopSafeFrameskipRomCRC32(const void *pData, Uint32 nBytes);
void MainLoopSafeFrameskipSetRomIdentityCRC32(const void *pSystemOwner, Uint32 crc32);
/* AURORA_SAFE_FRAMESKIP_PICODRIVE_AUTO_V1: PicoDrive-style Auto decision once per host tick. */
Bool MainLoopSafeFrameskipTake(Bool allowed);
Bool MainLoopSafeFrameskipConsumePresentationSkip(void);
/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830
 * CDDA may request ONE host skip so synchronous storage refill happens
 * only inside the already-authorized Safe Frameskip mechanism. */
void MainLoopSafeFrameskipRequestCdAudioWindow(void);
/* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
void MainLoopSafeFrameskipCancelCdAudioWindow(void);

