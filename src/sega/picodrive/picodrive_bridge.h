#ifndef _PICODRIVE_BRIDGE_H
#define _PICODRIVE_BRIDGE_H

#include <stddef.h>
#include "types.h"

namespace Emu { struct SysInputT; }
class CRenderSurface;
class CMixBuffer;

bool PicoDriveBridge_Init(void);
void PicoDriveBridge_Shutdown(void);
/* AURORA_DYNAMIC_ROM_BUFFER_V1_20260823 */
size_t PicoDriveBridge_RequiredRomCapacity(size_t nBytes);
/* AURORA_LARGE_MD_COMPACT_V2_20260914: compact backing for validated plain MD >4 MiB. */
size_t PicoDriveBridge_CompactMegaDriveCapacity(size_t nBytes);
bool PicoDriveBridge_LoadGame(const void *pData, size_t nBytes,
                              size_t nCapacity, const char *pName);
/* AURORA_SUPER_MAGIC_DRIVE_V1_20260902 */
bool PicoDriveBridge_LoadSuperMagicDrive(const char *pBiosPath, const char *pDiskPath);
bool PicoDriveBridge_SmdInsertCartridge(const void *pData, size_t nBytes, size_t nCapacity, const char *pName);
void PicoDriveBridge_SmdEjectCartridge(void);
bool PicoDriveBridge_SmdSwapDisk(const char *pPath);
void PicoDriveBridge_SmdPowerCycle(void);
bool PicoDriveBridge_IsSuperMagicDrive(void);
bool PicoDriveBridge_IsSuperMagicDriveFirmwareMode(void);
bool PicoDriveBridge_SmdHasCartridge(void);
bool PicoDriveBridge_SmdHasDisk(void);
const char *PicoDriveBridge_SmdDiskPath(void);
const char *PicoDriveBridge_SmdLastError(void);
/* AURORA_CD_SRAM_NOTICES_20260824: CUE stays on storage; only its tracks stream through the core. */
int  PicoDriveBridge_ProbeSegaCd(const char *pPath);
bool PicoDriveBridge_LoadDisc(const char *pPath, const char *pSystemPath);
void PicoDriveBridge_UnloadGame(void);
void PicoDriveBridge_Reset(void);
void PicoDriveBridge_SoftReset(void);

void PicoDriveBridge_Set6Button(bool enabled);
bool PicoDriveBridge_Get6Button(void);
void PicoDriveBridge_SetRenderingMode(int mode);
int  PicoDriveBridge_GetRenderingMode(void);
void PicoDriveBridge_SetSmsColorBorder(bool enabled);
bool PicoDriveBridge_GetSmsColorBorder(void);
void PicoDriveBridge_SetGgZoom(bool enabled); /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */
bool PicoDriveBridge_GetGgZoom(void);
void PicoDriveBridge_SetSmsFm(bool enabled);
bool PicoDriveBridge_GetSmsFm(void);
void PicoDriveBridge_SetAudioRate(int hz);
int  PicoDriveBridge_GetAudioRate(void);
/* AURORA_PD_HOST_CADENCE_V1_20260821 */
int  PicoDriveBridge_GetNominalFrameRate(void);
/* AURORA_PD_MEGA_FIX_20260820 */
bool PicoDriveBridge_IsMasterSystem(void);
bool PicoDriveBridge_Is8Bit(void);
/* AURORA_V6_SMS_PHYSICAL_PAUSE_BRIDGE_20260828 */
void PicoDriveBridge_QueueMasterSystemPause(void);

/* Aurora Region Select values are passed straight in:
 * Off/Auto=0, NTSC-U, NTSC-J, PAL. */
void PicoDriveBridge_SetRegion(int auroraRegion);

void PicoDriveBridge_SetMouseInput(bool active, int dx, int dy, unsigned buttons);
/* AURORA_PD_SKIP_DISCARDED_VIDEO_V2_H_20260821 */
void PicoDriveBridge_SetSkipVideo(bool skip);
/* AURORA_V4_11_CD_REALTIME_PACING_PCE_TOC_OFFSETS_20260830 */
bool PicoDriveBridge_IsSegaCD(void);
/* AURORA_CD_STATE_V1_SAFE_20260903: stable identity path for frontend CD states. */
const char *PicoDriveBridge_GetDiscPath(void);
/* AURORA_V4_17_SAFE_CD_GAME_SWITCH_QUIESCE_20260830
 * Bounded preflight before destroying a live Sega CD core. */
bool PicoDriveBridge_PrepareGameSwitch(void);
/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830
 * Separate from SetSkipVideo(): cadence discard is NOT an I/O window. */
void PicoDriveBridge_SetCdAudioSafeWindow(bool allowed);
bool PicoDriveBridge_ConsumeCdAudioRefillRequest(void);
/* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830 */
bool PicoDriveBridge_PrefetchCdAudio(void);
/* AURORA_CD_MUSIC_REDBOOK_V3_20260830
 * Red Book/CDDA only. Does not alter Sega CD PCM, FM or PSG. */
void PicoDriveBridge_SetCdMusicEnabled(bool enabled);
bool PicoDriveBridge_GetCdMusicEnabled(void);

void PicoDriveBridge_RunFrame(Emu::SysInputT *pInput,
                              CRenderSurface *pTarget,
                              CMixBuffer *pMixBuf);

int PicoDriveBridge_GetStateSize(void);
int PicoDriveBridge_SaveState(void *pData, int nBytes);
bool PicoDriveBridge_LoadState(const void *pData, int nBytes);

int PicoDriveBridge_GetSRAMBytes(void);
Uint8 *PicoDriveBridge_GetSRAMData(void);
unsigned PicoDriveBridge_GetSampleRate(void);

/* AURORA_GS_VRAM_EPOCH_V4_2
 * A new gsKit VRAM epoch invalidates direct-T8 CLUT residency. */
void PicoDriveBridge_InvalidateGsResources(void);

/* AURORA_PD_NATIVE320_DIRECT_T8_V1 */
bool PicoDriveBridge_IsMegaDrive(void);
/* AURORA_SEGA_CD_32X_MD_SCALING_V2R1_20260828
 * Video geometry only: Sega/Mega CD and 32X use the same VDP H32/H40
 * presentation policy as cartridge Mega Drive, without reclassifying them
 * for SRAM/input/core behavior. */
bool PicoDriveBridge_IsMegaDriveVideo(void);
bool PicoDriveBridge_CanDirectGsVideo(void);
bool PicoDriveBridge_DrawDirectGs(Uint32 auroraOutBaseTBP, Float32 intensity);

#endif

/* AURORA_V4_11_CD_REALTIME_PACING_PCE_TOC_OFFSETS_20260830 */
