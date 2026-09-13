#ifndef _PCE_BRIDGE_H
#define _PCE_BRIDGE_H
/* AURORA_PCE_EXPERIMENTAL_V1 */
#include <stddef.h>
#include "types.h"
namespace Emu { struct SysInputT; }
class CRenderSurface;
class CMixBuffer;
bool PceBridge_Init(void);
void PceBridge_Shutdown(void);
bool PceBridge_LoadGame(const void *pData, size_t nBytes, size_t nCapacity, const char *pName);
/* AURORA_CD_SRAM_NOTICES_20260824 -- path-only PC Engine CD CUE. */
bool PceBridge_LoadDisc(const char *pPath, const char *pSystemPath);
/* AURORA_V4_11_CD_REALTIME_PACING_PCE_TOC_OFFSETS_20260830 */
bool PceBridge_IsDiscLoaded(void);
/* AURORA_CD_STATE_V1_SAFE_20260903: stable identity path for frontend CD states. */
const char *PceBridge_GetDiscPath(void);
/* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901 */
bool PceBridge_QuiesceDiscIO(void);
bool PceBridge_DiscIOPaused(void); /* AURORA_SSF2_PCE_MENU_FIX_V2_20260913_PCE_ACK_POLL_BRIDGE */
void PceBridge_ResumeDiscIO(void);
void PceBridge_UnloadGame(void);
void PceBridge_Reset(void);
void PceBridge_SoftReset(void);
void PceBridge_SetSkipVideo(bool skip); /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */
/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830 */
void PceBridge_SetCdAudioSafeWindow(bool allowed);
bool PceBridge_ConsumeCdAudioRefillRequest(void);
/* AURORA_CD_MUSIC_REDBOOK_V3_20260830
 * Red Book/CDDA only; PCE ADPCM/PSG are unaffected. */
void PceBridge_SetCdMusicEnabled(bool enabled);
bool PceBridge_GetCdMusicEnabled(void);
void PceBridge_RunFrame(Emu::SysInputT *pInput, CRenderSurface *pTarget, CMixBuffer *pMixBuf);
int PceBridge_GetStateSize(void);
int PceBridge_SaveState(void *pData, int nBytes);
bool PceBridge_LoadState(const void *pData, int nBytes);
int PceBridge_GetSRAMBytes(void);
Uint8 *PceBridge_GetSRAMData(void);
void PceBridge_InvalidateGsResources(void);
bool PceBridge_CanDirectGsVideo(void);
/* AURORA_PCE_KRAZY_RUNTIME_DIAG_V11R3_20260830 */
void PceBridge_GetVideoDebug(unsigned *w, unsigned *h, unsigned *pitchPixels,
                             int *fbw, int *nativeClass);
/* AURORA_PCE_NATIVE_GS_RASTER_V5_20260830: native GS framebuffer width for the current PCE dot-clock mode. */
/* AURORA_PCE_FIXED512_DBX0_CUMULATIVE_V8_20260830
 * Returns current native PCE source width/class; framebuffer itself stays 512. */
int PceBridge_GetNative240pRasterWidth(void);
bool PceBridge_DrawDirectGs(Uint32 auroraOutBaseTBP, Float32 intensity);
unsigned PceBridge_GetSampleRate(void);
#endif

/* AURORA_V4_11_CD_REALTIME_PACING_PCE_TOC_OFFSETS_20260830 */
