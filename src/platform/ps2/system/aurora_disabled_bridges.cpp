/* AURORA_CORE_FLAGS_FINAL_20261104: lightweight API stubs only when archive excluded. */
#include <stddef.h>
#include "platform/ps2/system/aurora_core_build_config.h"
#if AURORA_NO_QUICKNES
#include "nes/quicknes/quicknes_bridge.h"
static bool s_AuroraShadowQuicknesLightGunEnabled = false;
bool QuicknesBridge_Init(void) { return false; }
void QuicknesBridge_Shutdown(void) { (void)0; }
bool QuicknesBridge_LoadGame(const void *pData, size_t nBytes, const char *pName) { return false; }
int QuicknesBridge_LastUnsupportedMapper(void) { return 0; }
void QuicknesBridge_UnloadGame(void) { (void)0; }
void QuicknesBridge_Reset(void) { (void)0; }
void QuicknesBridge_SoftReset(void) { (void)0; }
void QuicknesBridge_SetDutySwap(bool enabled) { (void)0; }
bool QuicknesBridge_SetPalette(const Uint8 *rgb192) { return false; }
void QuicknesBridge_SetTurboSpeed(unsigned speedShift) { (void)0; }
void QuicknesBridge_SetSkipVideo(bool skip) { (void)0; }
void QuicknesBridge_InvalidateGsResources(void) { (void)0; }
bool QuicknesBridge_CanDirectGsVideo(void) { return false; }
bool QuicknesBridge_DrawDirectGs(Uint32 auroraOutBaseTBP,
                                 Int32 logicalY,
                                 Float32 intensity) { return false; }
void QuicknesBridge_RunFrame(Emu::SysInputT *pInput,
                             CRenderSurface *pTarget,
                             CMixBuffer *pMixBuf) { (void)0; }
int QuicknesBridge_GetStateSize(void) { return 0; }
int QuicknesBridge_SaveState(void *pData, int nBytes) { return 0; }
bool QuicknesBridge_LoadState(const void *pData, int nBytes) { return false; }
int QuicknesBridge_GetSRAMBytes(void) { return 0; }
uint8_t * QuicknesBridge_GetSRAMData(void) { return NULL; }
bool QuicknesBridge_UsesN163InternalSave(void) { return false; }
bool QuicknesBridge_CommitSRAMData(void) { return false; }
bool QuicknesBridge_IsArkanoidVaus(void) { return false; }
bool QuicknesBridge_TurboFileEnabled(void) { return false; }
int QuicknesBridge_GetTurboFileBytes(void) { return 0; }
uint8_t * QuicknesBridge_GetTurboFileData(void) { return NULL; }
bool QuicknesBridge_TurboFileDirty(void) { return false; }
void QuicknesBridge_ClearTurboFileDirty(void) { (void)0; }
void QuicknesBridge_SetLightGunEnabled(bool enabled) { s_AuroraShadowQuicknesLightGunEnabled = enabled; }
bool QuicknesBridge_GetLightGunEnabled(void) { return s_AuroraShadowQuicknesLightGunEnabled; }
bool QuicknesBridge_LightGunActive(void) { return false; }
void QuicknesBridge_GetLightGunCursor(Int32 *x, Int32 *y) { if (x) *x = 0; if (y) *y = 0; }
void QuicknesBridge_DrawLightGunCursor(Int32 logicalY) { (void)0; }
bool QuicknesBridge_BattleBoxEnabled(void) { return false; }
int QuicknesBridge_GetBattleBoxBytes(void) { return 0; }
uint8_t * QuicknesBridge_GetBattleBoxData(void) { return NULL; }
bool QuicknesBridge_BattleBoxDirty(void) { return false; }
void QuicknesBridge_ClearBattleBoxDirty(void) { (void)0; }
unsigned QuicknesBridge_GetSampleRate(void) { return 0; }
#endif

#if AURORA_NO_FCEUMM
#include "nes/fceumm/fceumm_fds_bridge.h"
bool FceummFdsBridge_Init(void) { return false; }
void FceummFdsBridge_Shutdown(void) { (void)0; }
bool FceummFdsBridge_LoadDisk(const char *path, const char *systemPath,
                              unsigned totalSides) { return false; }
bool FceummFdsBridge_LoadDiskMemory(const void *data, Uint32 bytes,
                                    const char *contentName,
                                    const char *systemPath,
                                    unsigned totalSides) { return false; }
void FceummFdsBridge_UnloadGame(void) { (void)0; }
void FceummFdsBridge_Reset(void) { (void)0; }
void FceummFdsBridge_SoftReset(void) { (void)0; }
void FceummFdsBridge_SetSkipVideo(bool skip) { (void)0; }
bool FceummFdsBridge_SetPalette(const Uint8 *rgb192) { return false; }
void FceummFdsBridge_InvalidateGsResources(void) { (void)0; }
bool FceummFdsBridge_CanDirectGsVideo(void) { return false; }
bool FceummFdsBridge_DrawDirectGs(Uint32 auroraOutBaseTBP,
                                  Int32 logicalY,
                                  Float32 intensity) { return false; }
void FceummFdsBridge_RunFrame(Emu::SysInputT *input,
                              CRenderSurface *target,
                              CMixBuffer *mix) { (void)0; }
int FceummFdsBridge_GetStateSize(void) { return 0; }
int FceummFdsBridge_SaveState(void *data, int bytes) { return 0; }
bool FceummFdsBridge_LoadState(const void *data, int bytes) { return false; }
bool FceummFdsBridge_BeginSideSwap(void) { return false; }
bool FceummFdsBridge_IsSideSwapPending(void) { return false; }
unsigned FceummFdsBridge_GetSelectedSide(void) { return 0; }
bool FceummFdsBridge_IsDiskInserted(void) { return false; }
bool FceummFdsBridge_ConsumeDriveStateChange(unsigned *selectedSide,
                                              bool *inserted) { return false; }
void FceummFdsBridge_GetDriveState(unsigned *selectedSide,
                                   bool *inserted,
                                   unsigned *swapFramesRemaining,
                                   unsigned *swapTargetSide) { if (selectedSide) *selectedSide = 0; if (inserted) *inserted = 0; if (swapFramesRemaining) *swapFramesRemaining = 0; if (swapTargetSide) *swapTargetSide = 0; }
bool FceummFdsBridge_SetDriveState(unsigned selectedSide,
                                   bool inserted,
                                   unsigned swapFramesRemaining,
                                   unsigned swapTargetSide) { return false; }
unsigned FceummFdsBridge_GetSampleRate(void) { return 0; }
#endif

#if AURORA_NO_PICODRIVE
#include "sega/picodrive/picodrive_bridge.h"
static bool s_AuroraShadowPicodrive6Button = false;
static int s_AuroraShadowPicodriveRenderingMode = 0;
static bool s_AuroraShadowPicodriveSmsColorBorder = false;
static bool s_AuroraShadowPicodriveGgZoom = false;
static bool s_AuroraShadowPicodriveSmsFm = false;
static int s_AuroraShadowPicodriveAudioRate = 32000;
static bool s_AuroraShadowPicodriveCdMusicEnabled = false;
bool PicoDriveBridge_Init(void) { return false; }
void PicoDriveBridge_Shutdown(void) { (void)0; }
size_t PicoDriveBridge_RequiredRomCapacity(size_t nBytes) { return 0; }
size_t PicoDriveBridge_CompactMegaDriveCapacity(size_t nBytes) { return 0; }
bool PicoDriveBridge_LoadGame(const void *pData, size_t nBytes,
                              size_t nCapacity, const char *pName) { return false; }
bool PicoDriveBridge_LoadSuperMagicDrive(const char *pBiosPath, const char *pDiskPath) { return false; }
bool PicoDriveBridge_SmdInsertCartridge(const void *pData, size_t nBytes, size_t nCapacity, const char *pName) { return false; }
void PicoDriveBridge_SmdEjectCartridge(void) { (void)0; }
bool PicoDriveBridge_SmdSwapDisk(const char *pPath) { return false; }
void PicoDriveBridge_SmdPowerCycle(void) { (void)0; }
bool PicoDriveBridge_IsSuperMagicDrive(void) { return false; }
bool PicoDriveBridge_IsSuperMagicDriveFirmwareMode(void) { return false; }
bool PicoDriveBridge_SmdHasCartridge(void) { return false; }
bool PicoDriveBridge_SmdHasDisk(void) { return false; }
const char * PicoDriveBridge_SmdDiskPath(void) { return ""; }
const char * PicoDriveBridge_SmdLastError(void) { return ""; }
int PicoDriveBridge_ProbeSegaCd(const char *pPath) { return -1; }
bool PicoDriveBridge_LoadDisc(const char *pPath, const char *pSystemPath) { return false; }
void PicoDriveBridge_UnloadGame(void) { (void)0; }
void PicoDriveBridge_Reset(void) { (void)0; }
void PicoDriveBridge_SoftReset(void) { (void)0; }
void PicoDriveBridge_Set6Button(bool enabled) { s_AuroraShadowPicodrive6Button = enabled; }
bool PicoDriveBridge_Get6Button(void) { return s_AuroraShadowPicodrive6Button; }
void PicoDriveBridge_SetRenderingMode(int mode) { s_AuroraShadowPicodriveRenderingMode = mode; }
int PicoDriveBridge_GetRenderingMode(void) { return s_AuroraShadowPicodriveRenderingMode; }
void PicoDriveBridge_SetSmsColorBorder(bool enabled) { s_AuroraShadowPicodriveSmsColorBorder = enabled; }
bool PicoDriveBridge_GetSmsColorBorder(void) { return s_AuroraShadowPicodriveSmsColorBorder; }
void PicoDriveBridge_SetGgZoom(bool enabled) { s_AuroraShadowPicodriveGgZoom = enabled; }
bool PicoDriveBridge_GetGgZoom(void) { return s_AuroraShadowPicodriveGgZoom; }
void PicoDriveBridge_SetSmsFm(bool enabled) { s_AuroraShadowPicodriveSmsFm = enabled; }
bool PicoDriveBridge_GetSmsFm(void) { return s_AuroraShadowPicodriveSmsFm; }
void PicoDriveBridge_SetAudioRate(int hz) { s_AuroraShadowPicodriveAudioRate = hz; }
int PicoDriveBridge_GetAudioRate(void) { return s_AuroraShadowPicodriveAudioRate; }
int PicoDriveBridge_GetNominalFrameRate(void) { return 0; }
bool PicoDriveBridge_IsMasterSystem(void) { return false; }
bool PicoDriveBridge_Is8Bit(void) { return false; }
void PicoDriveBridge_QueueMasterSystemPause(void) { (void)0; }
void PicoDriveBridge_SetRegion(int auroraRegion) { (void)0; }
void PicoDriveBridge_SetMouseInput(bool active, int dx, int dy, unsigned buttons) { (void)0; }
void PicoDriveBridge_SetSkipVideo(bool skip) { (void)0; }
bool PicoDriveBridge_IsSegaCD(void) { return false; }
const char * PicoDriveBridge_GetDiscPath(void) { return ""; }
bool PicoDriveBridge_PrepareGameSwitch(void) { return false; }
void PicoDriveBridge_SetCdAudioSafeWindow(bool allowed) { (void)0; }
bool PicoDriveBridge_ConsumeCdAudioRefillRequest(void) { return false; }
bool PicoDriveBridge_PrefetchCdAudio(void) { return false; }
void PicoDriveBridge_SetCdMusicEnabled(bool enabled) { s_AuroraShadowPicodriveCdMusicEnabled = enabled; }
bool PicoDriveBridge_GetCdMusicEnabled(void) { return s_AuroraShadowPicodriveCdMusicEnabled; }
void PicoDriveBridge_RunFrame(Emu::SysInputT *pInput,
                              CRenderSurface *pTarget,
                              CMixBuffer *pMixBuf) { (void)0; }
int PicoDriveBridge_GetStateSize(void) { return 0; }
int PicoDriveBridge_SaveState(void *pData, int nBytes) { return 0; }
bool PicoDriveBridge_LoadState(const void *pData, int nBytes) { return false; }
int PicoDriveBridge_GetSRAMBytes(void) { return 0; }
Uint8 * PicoDriveBridge_GetSRAMData(void) { return NULL; }
unsigned PicoDriveBridge_GetSampleRate(void) { return 0; }
void PicoDriveBridge_InvalidateGsResources(void) { (void)0; }
bool PicoDriveBridge_IsMegaDrive(void) { return false; }
bool PicoDriveBridge_IsMegaDriveVideo(void) { return false; }
bool PicoDriveBridge_CanDirectGsVideo(void) { return false; }
bool PicoDriveBridge_DrawDirectGs(Uint32 auroraOutBaseTBP, Float32 intensity) { return false; }
#endif

#if AURORA_NO_PCE
#include "pce/beetle/pce_bridge.h"
static bool s_AuroraShadowPce6Button = false;
static bool s_AuroraShadowPceCdMusicEnabled = false;
void PceBridge_Set6Button(bool enabled) { s_AuroraShadowPce6Button = enabled; }
bool PceBridge_Get6Button(void) { return s_AuroraShadowPce6Button; }
bool PceBridge_Init(void) { return false; }
void PceBridge_Shutdown(void) { (void)0; }
bool PceBridge_LoadGame(const void *pData, size_t nBytes, size_t nCapacity, const char *pName) { return false; }
bool PceBridge_LoadDisc(const char *pPath, const char *pSystemPath) { return false; }
bool PceBridge_IsDiscLoaded(void) { return false; }
const char * PceBridge_GetDiscPath(void) { return ""; }
bool PceBridge_QuiesceDiscIO(void) { return false; }
bool PceBridge_DiscIOPaused(void) { return false; }
void PceBridge_ResumeDiscIO(void) { (void)0; }
void PceBridge_UnloadGame(void) { (void)0; }
void PceBridge_Reset(void) { (void)0; }
void PceBridge_SoftReset(void) { (void)0; }
void PceBridge_SetSkipVideo(bool skip) { (void)0; }
void PceBridge_SetCdAudioSafeWindow(bool allowed) { (void)0; }
bool PceBridge_ConsumeCdAudioRefillRequest(void) { return false; }
void PceBridge_SetCdMusicEnabled(bool enabled) { s_AuroraShadowPceCdMusicEnabled = enabled; }
bool PceBridge_GetCdMusicEnabled(void) { return s_AuroraShadowPceCdMusicEnabled; }
void PceBridge_RunFrame(Emu::SysInputT *pInput, CRenderSurface *pTarget, CMixBuffer *pMixBuf) { (void)0; }
int PceBridge_GetStateSize(void) { return 0; }
int PceBridge_SaveState(void *pData, int nBytes) { return 0; }
bool PceBridge_LoadState(const void *pData, int nBytes) { return false; }
int PceBridge_GetSRAMBytes(void) { return 0; }
Uint8 * PceBridge_GetSRAMData(void) { return NULL; }
void PceBridge_InvalidateGsResources(void) { (void)0; }
bool PceBridge_CanDirectGsVideo(void) { return false; }
void PceBridge_GetVideoDebug(unsigned *w, unsigned *h, unsigned *pitchPixels,
                             int *fbw, int *nativeClass) { if (w) *w = 0; if (h) *h = 0; if (pitchPixels) *pitchPixels = 0; if (fbw) *fbw = 0; if (nativeClass) *nativeClass = 0; }
int PceBridge_GetNative240pRasterWidth(void) { return 0; }
bool PceBridge_DrawDirectGs(Uint32 auroraOutBaseTBP, Float32 intensity) { return false; }
unsigned PceBridge_GetSampleRate(void) { return 0; }
#endif


#if AURORA_NO_PICODRIVE
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <stdlib.h>
/* Compact CUE classifier when PicoDrive is not linked. Only determines the
   Sega CD system signature, never loads a CD core. -1 = unreadable. */
int AuroraCoreFallbackProbeSegaCd(const char *cuePath)
{
    FILE *cue;
    FILE *track;
    char line[1024], fileName[768] = {0}, dataPath[1024] = {0};
    char dir[1024];
    int modeBytes = 0, indexMin = 0, indexSec = 0, indexFrame = 0;
    int haveIndex = 0, seenTrack = 0, lines = 0;
    unsigned char header[64];
    long offset;
    size_t len;
    if (!cuePath || !*cuePath || !(cue = fopen(cuePath, "rb"))) return -1;
    snprintf(dir, sizeof(dir), "%s", cuePath);
    {
        char *slash = strrchr(dir, '/');
        char *backslash = strrchr(dir, '\\');
        if (backslash && (!slash || backslash > slash)) slash = backslash;
        if (slash) slash[1] = 0;
        else dir[0] = 0;
    }
    while (lines++ < 256 && fgets(line, sizeof(line), cue))
    {
        char *s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (!strncasecmp(s, "FILE", 4) && (s[4] == ' ' || s[4] == '\t'))
        {
            char *begin, *end;
            s += 4;
            while (*s == ' ' || *s == '\t') ++s;
            if (*s == '"') { begin = ++s; end = strchr(begin, '"'); }
            else {
                begin = s; end = begin;
                while (*end && *end != ' ' && *end != '\t' && *end != '\r' && *end != '\n') ++end;
            }
            if (!end || end <= begin) continue;
            len = (size_t)(end - begin);
            if (len >= sizeof(fileName)) continue;
            memcpy(fileName, begin, len); fileName[len] = 0;
        }
        else if (!strncasecmp(s, "TRACK", 5) && (s[5] == ' ' || s[5] == '\t'))
        {
            /* The first data track identifies the disc. */
            if (seenTrack) break;
            if (strstr(s, "MODE1/2352") || strstr(s, "MODE2/2352")) modeBytes = 2352;
            else if (strstr(s, "MODE1/2048")) modeBytes = 2048;
            if (modeBytes && fileName[0])
            {
                snprintf(dataPath, sizeof(dataPath), "%s%s", dir, fileName);
                seenTrack = 1;
            }
        }
        else if (seenTrack && !strncasecmp(s, "INDEX 01", 8))
        {
            int mm, ss, ff;
            if (sscanf(s + 8, "%d:%d:%d", &mm, &ss, &ff) == 3 &&
                mm >= 0 && mm < 600 && ss >= 0 && ss < 60 && ff >= 0 && ff < 75)
            {
                indexMin = mm; indexSec = ss; indexFrame = ff;
                haveIndex = 1;
            }
        }
    }
    fclose(cue);
    if (!seenTrack || !dataPath[0]) return -1;
    track = fopen(dataPath, "rb");
    if (!track) return -1;
    offset = (long)(((indexMin * 60 + indexSec) * 75 + indexFrame) * modeBytes);
    if (haveIndex && fseek(track, offset, SEEK_SET) != 0) { fclose(track); return -1; }
    len = fread(header, 1, sizeof(header), track);
    fclose(track);
    if (len < 32) return -1;
    return (!strncasecmp((const char *)header, "SEGADISCSYSTEM", 14) ||
            !strncasecmp((const char *)header + 0x10, "SEGADISCSYSTEM", 14) ||
            (modeBytes == 2352 && len >= 40 &&
             !strncasecmp((const char *)header + 24, "SEGADISCSYSTEM", 14))) ? 1 : 0;
}
#endif
