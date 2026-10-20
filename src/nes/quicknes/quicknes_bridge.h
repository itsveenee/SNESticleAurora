/* SNESTICLE_QUICKNES_BRIDGE */
#ifndef _QUICKNES_BRIDGE_H
#define _QUICKNES_BRIDGE_H

/* AURORA_VOLUME_TFA_N163_V4_20260913 */

#include <stddef.h>
#include <stdint.h>

#include "types.h"
namespace Emu { struct SysInputT; }
class CRenderSurface;
class CMixBuffer;

/*
 * Native QuickNES snapshots are currently well below this bound.
 * Keep a fixed capacity so asking for the state size never needs to
 * serialize the machine merely to discover the length.
 */
enum { QUICKNES_STATE_CAPACITY = 64 * 1024 };

bool QuicknesBridge_Init(void);
void QuicknesBridge_Shutdown(void);
bool QuicknesBridge_LoadGame(const void *pData, size_t nBytes, const char *pName);
int QuicknesBridge_LastUnsupportedMapper(void); /* negative = other error */
void QuicknesBridge_UnloadGame(void);
void QuicknesBridge_Reset(void);
void QuicknesBridge_SoftReset(void);
void QuicknesBridge_SetDutySwap(bool enabled);
/* AURORA_FCEUMM_FDS_V4_TURBO_PAL_PERF_20260827: shared 192-byte NES RGB palette. */
bool QuicknesBridge_SetPalette(const Uint8 *rgb192);
/* AURORA_CONTROLLER_OPTIONS_V2 */
void QuicknesBridge_SetTurboSpeed(unsigned speedShift);
void QuicknesBridge_SetSkipVideo(bool skip); /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */
/* AURORA_ALLCORES_PERF_V5_20260824 */
void QuicknesBridge_InvalidateGsResources(void);
bool QuicknesBridge_CanDirectGsVideo(void);
bool QuicknesBridge_DrawDirectGs(Uint32 auroraOutBaseTBP,
                                 Int32 logicalY,
                                 Float32 intensity);
void QuicknesBridge_RunFrame(Emu::SysInputT *pInput,
                             CRenderSurface *pTarget,
                             CMixBuffer *pMixBuf);
int QuicknesBridge_GetStateSize(void);
int QuicknesBridge_SaveState(void *pData, int nBytes);
bool QuicknesBridge_LoadState(const void *pData, int nBytes);
int QuicknesBridge_GetSRAMBytes(void);
uint8_t *QuicknesBridge_GetSRAMData(void);
/* AURORA_VOLUME_TFA_N163_V4_20260913: N163 internal 128-byte battery RAM bridge. */
bool QuicknesBridge_UsesN163InternalSave(void);
bool QuicknesBridge_CommitSRAMData(void);
/* AURORA_QN_EXT_HOST_V2_20260828 */
bool QuicknesBridge_IsArkanoidVaus(void);
bool QuicknesBridge_TurboFileEnabled(void); /* AURORA_CD_AUDIO_STREAM_V3_NES_HEADER_20260829 */
int QuicknesBridge_GetTurboFileBytes(void);
uint8_t *QuicknesBridge_GetTurboFileData(void);
bool QuicknesBridge_TurboFileDirty(void);
void QuicknesBridge_ClearTurboFileDirty(void);
/* AURORA_QN_BATTLEBOX_V5_20260829 */
/* AURORA_QN_LIGHTGUN_CURSOR_V7_20260829 */
/* AURORA_PCE_SCALING_LIGHTGUN_TOGGLE_V2_20260830 */
void QuicknesBridge_SetLightGunEnabled(bool enabled);
bool QuicknesBridge_GetLightGunEnabled(void);
/* AURORA_V6_1G_QN_KRAZY_DEBUG_CONSUMER_CURE_20260831: QN79 debug API retired. */
bool QuicknesBridge_LightGunActive(void);
void QuicknesBridge_GetLightGunCursor(Int32 *x, Int32 *y);
void QuicknesBridge_DrawLightGunCursor(Int32 logicalY);

bool QuicknesBridge_BattleBoxEnabled(void);
int QuicknesBridge_GetBattleBoxBytes(void);
uint8_t *QuicknesBridge_GetBattleBoxData(void);
bool QuicknesBridge_BattleBoxDirty(void);
void QuicknesBridge_ClearBattleBoxDirty(void);
unsigned QuicknesBridge_GetSampleRate(void);

#endif
