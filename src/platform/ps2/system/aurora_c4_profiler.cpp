
/* AURORA_C4_PROFILER_V8_20261003
 * AURORA_C4_AUDIO_COST_V8_2_20261004
 * Diagnostic-only audio EE subsets; PCM/timing unchanged.
 * Unified profiler for DKC / Magical Pop'n / Speedy Gonzales / Top Gear.
 * This TU exists only in AURORA_C4_PROFILER=1 builds.
 */
#include "aurora_c4_profiler.h"
#include "mainloop_safe_frameskip.h"

#if AURORA_C4_PROFILER

#include <stdio.h>
#include <string.h>
#include <libcdvd.h>
#include <osd_config.h>

#include "prof.h"
#include "sndbglog.h"
#include "mainloop_bgm.h"
#include "mainloop_ui.h"
#include "gskit_backend.h"
#include "aurora_snes_cost_profiler.h"

#if !SNDBG_LOG
#error AURORA_C4_PROFILER requires SNDBG_LOG=1
#endif

namespace
{
enum {
    C4_WORST_COUNT = 8,
    C4_MODE_COUNT = 8,
    C4_MENU_WRITE_DELAY = 1,
    C4_PPU_REG_COUNT = 64,
    C4_HDMA_CHANNELS = 8,
    C4_HDMA_MODES = 8
};

struct DebugBaseT
{
    Uint32 oamWrites, vramWrites, cgramWrites;
    Uint32 objEnabledLines, objRefs, objTiles;
    Uint32 objCacheHits, objCacheMisses, objCacheRefreshes;
    Uint32 bgCacheHits, bgCacheMisses, bgCacheRefreshes;
    Uint32 chrCacheInvalidations, audioSamples;
    Uint32 objRangeLines, objTimeLines;
    Uint32 ppuSyncCalls, ppuRenderLines;
    Uint32 ppuQueuedWrites, ppuAppliedWrites, ppuQueueFull;
    Uint32 dmaStarts, dmaReadBytes, dmaOamBytes, dmaVramBytes;
    Uint32 dmaCgramBytes, dmaOtherBytes, dmaWraps;
    Uint32 dmaModes[8];
    Uint32 hdmaScrollBytes, hdmaCgramBytes, hdmaWindowColorBytes, hdmaOtherBytes;
    Uint32 hdmaLines, hdmaActiveChannels, hdmaTransferChannels;
    Uint32 bgActiveLayers, bgMapReloads, bgChrRows;
};

struct CoreFrameT
{
    Uint32 frameNo;
    Bool videoTarget;
    Uint32 coreCycles;

    /* V8: exclusive top-level buckets from AuroraSnesCostProfiler. */
    Bool exclusiveValid;
    Uint32 exclusiveTotal;
    Uint32 exclusiveCpu;
    Uint32 exclusivePpu;
    Uint32 exclusiveRaster;
    Uint32 exclusiveSpc;
    Uint32 exclusiveDsp;

    /* Legacy/inclusive diagnostics retained for continuity with V7. */
    Uint32 cpuCycles, ppuCycles, gsuCycles, mdmaCycles, hdmaCycles;
    Uint32 spcCycles, dspCycles, blendCycles, ppuSyncCycles, mode7Cycles;
    Uint32 bgInfoCycles, bgOffsetCycles, bgMapCycles, bgChrCycles;
    Uint32 bgMainCycles, bgSubCycles, colorMathCycles;
    Uint32 objUpdateCycles, objFetchCycles, objDrawCycles;
    Uint32 hdmaDataCycles, hdmaTableCycles;

    Uint32 oamWrites, vramWrites, cgramWrites;
    Uint32 objEnabledLines, objRefs, objTiles, objPotentialSlivers;
    Uint32 objMaxSelectedLine, objMaxPotentialLine, objMaxFetchedLine;
    Uint32 objCacheHits, objCacheMisses, objCacheRefreshes;
    Uint32 bgCacheHits, bgCacheMisses, bgCacheRefreshes;
    Uint32 chrCacheInvalidations, audioSamples;
    Uint32 objRangeLines, objTimeLines;
    Uint8 lastObjOBSEL, lastObjTM, lastObjTS;
    Uint16 lastObjPriority;

    Uint32 ppuSyncCalls, ppuRenderLines;
    Uint32 ppuQueuedWrites, ppuAppliedWrites, ppuQueueFull;

    Uint32 dmaStarts, dmaReadBytes, dmaOamBytes, dmaVramBytes;
    Uint32 dmaCgramBytes, dmaOtherBytes, dmaWraps;
    Uint32 dmaModes[8];

    Uint32 hdmaScrollBytes, hdmaCgramBytes, hdmaWindowColorBytes, hdmaOtherBytes;
    Uint32 hdmaLines, hdmaActiveChannels, hdmaTransferChannels;
    Uint32 bgActiveLayers, bgMapReloads, bgChrRows;

    Uint32 modeLines[C4_MODE_COUNT];
    Uint32 mosaicLines, windowLines, colorMathLines;
    Uint32 hiresMode56Lines, pseudoHiresLines;
    Uint32 hirqCount;
    Int32 hirqMinLine, hirqMaxLine;

    /* V8 causal attribution. */
    Uint32 syncReasonCalls[AURORA_C4_SYNC_REASON_COUNT];
    Uint32 syncReasonCycles[AURORA_C4_SYNC_REASON_COUNT];
    Uint32 rasterCatchupCalls;
    Uint32 rasterCatchupCycles;
    Uint32 rasterCatchupHdmaRuns;

    Uint32 ppuWrites[AURORA_C4_PPUWRITE_SOURCE_COUNT][C4_PPU_REG_COUNT];

    Uint32 hdmaChActive[C4_HDMA_CHANNELS];
    Uint32 hdmaChTransfers[C4_HDMA_CHANNELS];
    Uint32 hdmaChBytes[C4_HDMA_CHANNELS];
    Uint32 hdmaChModeTransfers[C4_HDMA_CHANNELS][C4_HDMA_MODES];
    Uint32 hdmaChIndirectTransfers[C4_HDMA_CHANNELS];
    Uint32 hdmaChReverseTransfers[C4_HDMA_CHANNELS];
    Uint8 hdmaChLastDMAP[C4_HDMA_CHANNELS];
    Uint8 hdmaChLastBBAD[C4_HDMA_CHANNELS];
};

struct HostFrameT
{
    CoreFrameT core;
    Uint32 hostTotalCycles, hostWorkCycles;
    Uint32 preCoreCycles, coreWrapperCycles, betweenCoreRenderCycles;
    Uint32 renderCycles, renderWorkCycles, postRenderCycles;
    Uint32 gpFlushCycles, vblankCycles, postAudioCycles, skipAudioCycles;
    Bool frameskipEnabled, frameskipAllowed, frameskipSkipped;
    AuroraC4GsDiagT gs;
    AuroraC4AudioDiagT audio;
};

struct WorstT
{
    Bool valid, fsSkipped, videoSuppressed;
    Uint32 frameNo, work, host, core, cpu, ppu;
    Uint32 objFetch, objDraw, spc, dsp, mdma, hdma, ppuSync;
    Uint32 gsSync, audioPlay, objRefs, objPotential, objFetched;
    Uint32 ppuSyncCalls, hirqCount;

    Uint32 syncReadCalls, syncWriteCalls, syncMdmaCalls, syncFrameCalls;
    Uint32 rasterCatchupCalls;
    Uint32 ppuCpuWrites, ppuHdmaWrites, ppuMdmaWrites;
};

struct StatsT
{
    Uint32 frames, videoTargetFrames, videoSuppressedFrames, hostOverBudgetFrames;
    Uint64 hostTotalCycles, hostWorkCycles, preCoreCycles, coreWrapperCycles;
    Uint64 betweenCoreRenderCycles, renderCycles, renderWorkCycles, postRenderCycles;
    Uint64 gpFlushCycles, vblankCycles, postAudioCycles, skipAudioCycles;
    Uint32 hostWorkMin, hostWorkMax;

    Uint32 exclusiveValidFrames;
    Uint64 exclusiveTotal, exclusiveCpu, exclusivePpu, exclusiveRaster;
    Uint64 exclusiveSpc, exclusiveDsp;

    Uint64 coreCycles, cpuCycles, ppuCycles, gsuCycles, mdmaCycles, hdmaCycles;
    Uint64 spcCycles, dspCycles, blendCycles, ppuSyncCycles, mode7Cycles;
    Uint64 bgInfoCycles, bgOffsetCycles, bgMapCycles, bgChrCycles;
    Uint64 bgMainCycles, bgSubCycles, colorMathCycles;
    Uint64 objUpdateCycles, objFetchCycles, objDrawCycles;
    Uint64 hdmaDataCycles, hdmaTableCycles;

    Uint64 oamWrites, vramWrites, cgramWrites;
    Uint64 objEnabledLines, objRefs, objTiles, objPotentialSlivers;
    Uint32 objMaxSelectedLine, objMaxPotentialLine, objMaxFetchedLine;
    Uint64 objCacheHits, objCacheMisses, objCacheRefreshes;
    Uint64 bgCacheHits, bgCacheMisses, bgCacheRefreshes;
    Uint64 chrCacheInvalidations, audioSamples, objRangeLines, objTimeLines;
    Uint8 lastObjOBSEL, lastObjTM, lastObjTS;
    Uint16 lastObjPriority;

    Uint64 ppuSyncCalls, ppuRenderLines;
    Uint64 ppuQueuedWrites, ppuAppliedWrites, ppuQueueFull;
    Uint64 dmaStarts, dmaReadBytes, dmaOamBytes, dmaVramBytes;
    Uint64 dmaCgramBytes, dmaOtherBytes, dmaWraps, dmaModes[8];

    Uint64 hdmaScrollBytes, hdmaCgramBytes, hdmaWindowColorBytes, hdmaOtherBytes;
    Uint64 hdmaLines, hdmaActiveChannels, hdmaTransferChannels;
    Uint64 bgActiveLayers, bgMapReloads, bgChrRows;

    Uint64 modeLines[C4_MODE_COUNT];
    Uint64 mosaicLines, windowLines, colorMathLines, hiresMode56Lines, pseudoHiresLines;
    Uint64 hirqCount;
    Int32 hirqMinLine, hirqMaxLine;

    Uint64 syncReasonCalls[AURORA_C4_SYNC_REASON_COUNT];
    Uint64 syncReasonCycles[AURORA_C4_SYNC_REASON_COUNT];
    Uint64 rasterCatchupCalls, rasterCatchupCycles, rasterCatchupHdmaRuns;

    Uint64 ppuWrites[AURORA_C4_PPUWRITE_SOURCE_COUNT][C4_PPU_REG_COUNT];

    Uint64 hdmaChActive[C4_HDMA_CHANNELS];
    Uint64 hdmaChTransfers[C4_HDMA_CHANNELS];
    Uint64 hdmaChBytes[C4_HDMA_CHANNELS];
    Uint64 hdmaChModeTransfers[C4_HDMA_CHANNELS][C4_HDMA_MODES];
    Uint64 hdmaChIndirectTransfers[C4_HDMA_CHANNELS];
    Uint64 hdmaChReverseTransfers[C4_HDMA_CHANNELS];
    Uint8 hdmaChLastDMAP[C4_HDMA_CHANNELS];
    Uint8 hdmaChLastBBAD[C4_HDMA_CHANNELS];

    Uint64 gsFrames, gsLines, gsSyncCalls, gsSyncCycles, gsCopyCycles;
    Uint64 gsKickCycles, gsCopyBytes, gsPaletteUploads, gsIntensityLines, gsDirectMainLines;

    Uint64 audioAvailableCycles, audioPlayCycles, audioEnqueuedFrames;
    Uint64 audioRequestedFrames, audioSentFrames, audioDroppedFrames;
    Uint64 audioQueueSum, audioQueueObs;
    Uint64 audioAvailableCalls, audioPlayCalls, audioDrainCalls;
    Uint64 audioAsyncStartCalls, audioShortWrites;
    Uint32 audioQueueMin, audioQueueMax;
    Uint64 audioResampleCycles, audioResampleInFrames, audioResampleOutFrames;
    Uint64 audioPackCycles, audioPackFrames;
    Uint64 audioAvailableFramesSum, audioAvailableFrameObs;
    Uint64 audioResampleCalls, audioPackCalls;
    Uint32 audioAvailableFramesMin, audioAvailableFramesMax;

    Uint32 fsDecisions, fsEnabledDecisions, fsAllowedDecisions, fsSkipped;
    Uint32 fsCurrentStreak, fsMaxStreak;

    WorstT worst[C4_WORST_COUNT];
};

struct StampT { Uint32 year, month, day, hour, minute, second; Bool valid; };
struct SnapshotT
{
    StatsT stats;
    StampT stamp;
    Uint32 crc32, serial, clockPairMin, refreshNum, refreshDen, hostBudgetCycles;
    Char title[64];
};

static StatsT s_live;
static SnapshotT s_pending;
static HostFrameT s_tick;
static DebugBaseT s_base;
static Uint32 s_crc32=0, s_serial=0, s_clockPairMin=0;
static Uint32 s_refreshNum=0, s_refreshDen=0, s_hostBudgetCycles=0;
static Char s_title[64]={0};
static Bool s_coreOpen=FALSE, s_tickOpen=FALSE, s_coreSpanOpen=FALSE, s_renderSpanOpen=FALSE;
static Uint32 s_coreStart=0, s_tickStart=0, s_coreSpanStart=0, s_coreSpanEnd=0;
static Uint32 s_renderSpanStart=0, s_renderSpanEnd=0;
static Bool s_writePending=FALSE;
static Int32 s_writeDelay=0;

/* AURORA_C4_CHR_SPLIT_PROFILER_V8_3_20261004 */
struct ChrKernelStatT
{
    Uint64 calls;
    Uint64 tiles;
    Uint64 cycles;
};
static ChrKernelStatT s_chrFrame[AURORA_C4_CHR_KERNEL_COUNT];
static ChrKernelStatT s_chrLive[AURORA_C4_CHR_KERNEL_COUNT];
static ChrKernelStatT s_chrPending[AURORA_C4_CHR_KERNEL_COUNT];

/* AURORA_C4_CHR4_DATA_MIX_DIAG_V8_3_MIX1_20261004 */
struct Chr4MixStatT
{
    Uint64 flip[4];
    Uint64 transparent[4];
};
static Chr4MixStatT s_chr4MixFrame;
static Chr4MixStatT s_chr4MixLive;
static Chr4MixStatT s_chr4MixPending;

static Uint32 Delta32(Uint32 now, Uint32 before) { return (Uint32)(now-before); }
static Uint64 Avg64(Uint64 total, Uint64 n) { return n ? (total+n/2u)/n : 0u; }
static Uint32 Pct10(Uint64 part, Uint64 total)
{
    if (!total) return 0;
    Uint64 v=(part*1000u+total/2u)/total;
    return (Uint32)(v>99999u ? 99999u : v);
}
static Uint32 Bcd(Uint8 v) { return (((v>>4)&15u)*10u)+(v&15u); }

static StampT CaptureStamp(void)
{
    StampT s; memset(&s,0,sizeof(s)); sceCdCLOCK rtc;
    if (!sceCdReadClock(&rtc)) return s;
    configConvertToLocalTime(&rtc);
    s.year=2000u+Bcd(rtc.year); s.month=Bcd(rtc.month); s.day=Bcd(rtc.day);
    s.hour=Bcd(rtc.hour); s.minute=Bcd(rtc.minute); s.second=Bcd(rtc.second); s.valid=TRUE;
    return s;
}
static Uint32 CalibrateClockPair(void)
{
    Uint32 best=0xFFFFFFFFu;
    for (Int32 i=0;i<32;i++){ Uint32 a=ProfCtrGetCycle(),b=ProfCtrGetCycle(),d=b-a; if(d<best)best=d; }
    return best==0xFFFFFFFFu?0u:best;
}
/* AURORA_C4_ADD_SMK_TARGET_V8_3_MIX1_1_20261004: SMK joins the existing C4 target gate; schema unchanged. */
static const char *FamilyName(Uint32 c)
{
    c = AuroraRomIdentityCRCForDetection(c);
    switch(c){
    case 0x7A33E836u: case 0x17657DB6u: case 0xC946DCA0u: case 0x3EAA5697u:
    case 0x762AF827u: case 0x50F2D1BCu: case 0x3ADEF543u:
    case 0xCAF432AAu: case 0xECA2C2D1u: return "Donkey Kong Country";
    case 0xC49D28A4u:return "Magical Pop'n";
    case 0xE2DBAD76u:case 0xCB0653D0u:case 0xD0DE3012u:case 0xEFE9E087u:
    case 0x9DDE5CEAu:case 0xE44F42BAu:return "Speedy Gonzales";
    case 0xB0150052u:case 0xD34C49B7u:case 0xE5A57B12u:case 0x19369514u:return "Top Gear";
    case 0xCD80DB86u:case 0xC8002453u:case 0x56410E5Eu:return "Super Mario Kart";
    default:return "Unknown";
    }
}
static const char *VariantName(Uint32 c)
{
    c = AuroraRomIdentityCRCForDetection(c);
    switch(c){
    case 0x7A33E836u:return "Europe EnFrDe";
    case 0x17657DB6u:return "Europe EnFrDe Rev 1";
    case 0xC946DCA0u:return "USA";
    case 0x3EAA5697u:return "USA Rev 1";
    case 0x762AF827u:return "USA Rev 2";
    case 0x50F2D1BCu:return "Japan / Super Donkey Kong";
    case 0x3ADEF543u:return "Japan Rev 1 / Super Donkey Kong";
    case 0xCAF432AAu:return "USA-Europe Rev 2 official rerelease";
    case 0xECA2C2D1u:return "Japan Rev 1 official rerelease";
    case 0xC49D28A4u:return "Japan";
    case 0xE2DBAD76u:return "USA";
    case 0xCB0653D0u:return "USA Rev 1";
    case 0xD0DE3012u:return "Europe Proto 04";
    case 0xEFE9E087u:return "Europe Proto 14";
    case 0x9DDE5CEAu:return "USA Beta 1";
    case 0xE44F42BAu:return "USA Beta 2";
    case 0xB0150052u:return "Europe";
    case 0xD34C49B7u:return "USA";
    case 0xE5A57B12u:return "Japan / Top Racer";
    case 0x19369514u:return "World / Top Racer Piko";
    case 0xCD80DB86u:return "USA";
    case 0xC8002453u:return "Japan";
    case 0x56410E5Eu:return "Europe";
    default:return "Unknown";
    }
}
static const char *FileKey(Uint32 c)
{
    c = AuroraRomIdentityCRCForDetection(c);
    switch(c){
    case 0x7A33E836u:case 0x17657DB6u:case 0xC946DCA0u:case 0x3EAA5697u:
    case 0x762AF827u:case 0x50F2D1BCu:case 0x3ADEF543u:case 0xCAF432AAu:case 0xECA2C2D1u:return "DKC";
    case 0xC49D28A4u:return "MAGICAL";
    case 0xE2DBAD76u:case 0xCB0653D0u:case 0xD0DE3012u:case 0xEFE9E087u:case 0x9DDE5CEAu:case 0xE44F42BAu:return "SPEEDY";
    case 0xB0150052u:case 0xD34C49B7u:case 0xE5A57B12u:case 0x19369514u:return "TOPGEAR";
    case 0xCD80DB86u:case 0xC8002453u:case 0x56410E5Eu:return "SMK";
    default:return "C4";
    }
}
static Bool IsC4CRC(Uint32 c)
{
    c = AuroraRomIdentityCRCForDetection(c);
    switch(c){
    case 0x7A33E836u:case 0x17657DB6u:case 0xC946DCA0u:case 0x3EAA5697u:
    case 0x762AF827u:case 0x50F2D1BCu:case 0x3ADEF543u:case 0xCAF432AAu:case 0xECA2C2D1u:
    case 0xC49D28A4u:case 0xE2DBAD76u:case 0xCB0653D0u:case 0xD0DE3012u:case 0xEFE9E087u:
    case 0x9DDE5CEAu:case 0xE44F42BAu:case 0xB0150052u:case 0xD34C49B7u:case 0xE5A57B12u:
    case 0x19369514u:
    case 0xCD80DB86u:case 0xC8002453u:case 0x56410E5Eu:return TRUE;
    default:return FALSE;
    }
}
static const char *SyncReasonName(Int32 i)
{
    static const char *kNames[AURORA_C4_SYNC_REASON_COUNT] =
    {
        "ppu_read",
        "ppu_write_queue_full",
        "ppu_write_direct",
        "mdma_pre",
        "midframe",
        "everyline",
        "visible_end",
        "frame_final"
    };
    return (i >= 0 && i < AURORA_C4_SYNC_REASON_COUNT) ? kNames[i] : "unknown";
}
static const char *PpuWriteSourceName(Int32 i)
{
    static const char *kNames[AURORA_C4_PPUWRITE_SOURCE_COUNT] =
    {
        "CPU", "HDMA", "MDMA"
    };
    return (i >= 0 && i < AURORA_C4_PPUWRITE_SOURCE_COUNT) ? kNames[i] : "UNKNOWN";
}
static void ResetLive(void)
{
    memset(&s_live,0,sizeof(s_live));
    s_live.hostWorkMin=0xFFFFFFFFu; s_live.hirqMinLine=0x7FFFFFFF; s_live.hirqMaxLine=-1;
    s_live.audioQueueMin=0xFFFFFFFFu;
    s_live.audioAvailableFramesMin=0xFFFFFFFFu;
    memset(&s_tick,0,sizeof(s_tick));
    s_coreOpen=s_tickOpen=s_coreSpanOpen=s_renderSpanOpen=FALSE;
}
static void CaptureBase(void)
{
    s_base.oamWrites=g_DbgOAMWrites; s_base.vramWrites=g_DbgVRAMWrites; s_base.cgramWrites=g_DbgCGRAMWrites;
    s_base.objEnabledLines=g_DbgObjEnabledLines; s_base.objRefs=g_DbgObjOamRefs; s_base.objTiles=g_DbgObjTiles;
    s_base.objCacheHits=g_DbgObjCacheHits; s_base.objCacheMisses=g_DbgObjCacheMisses; s_base.objCacheRefreshes=g_DbgObjCacheRefreshes;
    s_base.bgCacheHits=g_DbgBGCacheHits; s_base.bgCacheMisses=g_DbgBGCacheMisses; s_base.bgCacheRefreshes=g_DbgBGCacheRefreshes;
    s_base.chrCacheInvalidations=g_DbgChrCacheInvalidations; s_base.audioSamples=g_DbgAudioSamples;
    s_base.objRangeLines=g_DbgObjRangeLimitLines; s_base.objTimeLines=g_DbgObjLimitLines;
    s_base.ppuSyncCalls=g_DbgPPUSyncCalls; s_base.ppuRenderLines=g_DbgPPURenderLines;
    s_base.ppuQueuedWrites=g_DbgPPUQueuedWrites; s_base.ppuAppliedWrites=g_DbgPPUAppliedWrites; s_base.ppuQueueFull=g_DbgPPUQueueFull;
    s_base.dmaStarts=g_DbgDMAStarts; s_base.dmaReadBytes=g_DbgDMAReadBytes; s_base.dmaOamBytes=g_DbgDMAOAMBytes;
    s_base.dmaVramBytes=g_DbgDMAVRAMBytes; s_base.dmaCgramBytes=g_DbgDMACGRAMBytes; s_base.dmaOtherBytes=g_DbgDMAOtherBytes;
    s_base.dmaWraps=g_DbgDMAWraps; for(Int32 i=0;i<8;i++)s_base.dmaModes[i]=g_DbgDMAModes[i];
    s_base.hdmaScrollBytes=g_DbgHDMAScrollBytes; s_base.hdmaCgramBytes=g_DbgHDMACGRAMBytes;
    s_base.hdmaWindowColorBytes=g_DbgHDMAWindowColorBytes; s_base.hdmaOtherBytes=g_DbgHDMAOtherBytes;
    s_base.hdmaLines=g_DbgHDMALines; s_base.hdmaActiveChannels=g_DbgHDMAActiveChannels; s_base.hdmaTransferChannels=g_DbgHDMATransferChannels;
    s_base.bgActiveLayers=g_DbgBGActiveLayers; s_base.bgMapReloads=g_DbgBGMapReloads; s_base.bgChrRows=g_DbgBGChrRows;
}
static void FillDelta(CoreFrameT &f)
{
#define D(field,global) f.field=Delta32(global,s_base.field)
    D(oamWrites,g_DbgOAMWrites); D(vramWrites,g_DbgVRAMWrites); D(cgramWrites,g_DbgCGRAMWrites);
    D(objEnabledLines,g_DbgObjEnabledLines); D(objRefs,g_DbgObjOamRefs); D(objTiles,g_DbgObjTiles);
    D(objCacheHits,g_DbgObjCacheHits); D(objCacheMisses,g_DbgObjCacheMisses); D(objCacheRefreshes,g_DbgObjCacheRefreshes);
    D(bgCacheHits,g_DbgBGCacheHits); D(bgCacheMisses,g_DbgBGCacheMisses); D(bgCacheRefreshes,g_DbgBGCacheRefreshes);
    D(chrCacheInvalidations,g_DbgChrCacheInvalidations); D(audioSamples,g_DbgAudioSamples);
    D(objRangeLines,g_DbgObjRangeLimitLines); D(objTimeLines,g_DbgObjLimitLines);
    D(ppuSyncCalls,g_DbgPPUSyncCalls); D(ppuRenderLines,g_DbgPPURenderLines);
    D(ppuQueuedWrites,g_DbgPPUQueuedWrites); D(ppuAppliedWrites,g_DbgPPUAppliedWrites); D(ppuQueueFull,g_DbgPPUQueueFull);
    D(dmaStarts,g_DbgDMAStarts); D(dmaReadBytes,g_DbgDMAReadBytes); D(dmaOamBytes,g_DbgDMAOAMBytes);
    D(dmaVramBytes,g_DbgDMAVRAMBytes); D(dmaCgramBytes,g_DbgDMACGRAMBytes); D(dmaOtherBytes,g_DbgDMAOtherBytes); D(dmaWraps,g_DbgDMAWraps);
    D(hdmaScrollBytes,g_DbgHDMAScrollBytes); D(hdmaCgramBytes,g_DbgHDMACGRAMBytes);
    D(hdmaWindowColorBytes,g_DbgHDMAWindowColorBytes); D(hdmaOtherBytes,g_DbgHDMAOtherBytes);
    D(hdmaLines,g_DbgHDMALines); D(hdmaActiveChannels,g_DbgHDMAActiveChannels); D(hdmaTransferChannels,g_DbgHDMATransferChannels);
    D(bgActiveLayers,g_DbgBGActiveLayers); D(bgMapReloads,g_DbgBGMapReloads); D(bgChrRows,g_DbgBGChrRows);
#undef D
    for(Int32 i=0;i<8;i++)f.dmaModes[i]=Delta32(g_DbgDMAModes[i],s_base.dmaModes[i]);
    f.lastObjOBSEL=g_DbgObjOBSEL; f.lastObjTM=g_DbgObjTM; f.lastObjTS=g_DbgObjTS; f.lastObjPriority=g_DbgObjPriority;
}

static Uint32 SumPpuWrites(const CoreFrameT &c, Int32 source)
{
    Uint32 total = 0;
    if (source < 0 || source >= AURORA_C4_PPUWRITE_SOURCE_COUNT)
        return 0;
    for (Int32 i=0;i<C4_PPU_REG_COUNT;i++)
        total += c.ppuWrites[source][i];
    return total;
}
static void InsertWorst(const HostFrameT &f)
{
    WorstT w; memset(&w,0,sizeof(w)); w.valid=TRUE;
    w.frameNo=f.core.frameNo; w.work=f.hostWorkCycles; w.host=f.hostTotalCycles; w.core=f.core.coreCycles;
    w.cpu=f.core.exclusiveCpu; w.ppu=f.core.exclusivePpu; w.objFetch=f.core.objFetchCycles; w.objDraw=f.core.objDrawCycles;
    w.spc=f.core.exclusiveSpc; w.dsp=f.core.exclusiveDsp; w.mdma=f.core.mdmaCycles; w.hdma=f.core.hdmaCycles;
    w.ppuSync=f.core.ppuSyncCycles; w.gsSync=f.gs.syncCycles;
    w.audioPlay=(Uint32)(f.audio.playCycles>0xFFFFFFFFull?0xFFFFFFFFu:f.audio.playCycles);
    w.objRefs=f.core.objRefs; w.objPotential=f.core.objPotentialSlivers; w.objFetched=f.core.objTiles;
    w.ppuSyncCalls=f.core.ppuSyncCalls; w.hirqCount=f.core.hirqCount;
    w.syncReadCalls=f.core.syncReasonCalls[AURORA_C4_SYNC_PPU_READ];
    w.syncWriteCalls=f.core.syncReasonCalls[AURORA_C4_SYNC_PPU_WRITE_QUEUE_FULL]+
        f.core.syncReasonCalls[AURORA_C4_SYNC_PPU_WRITE_DIRECT];
    w.syncMdmaCalls=f.core.syncReasonCalls[AURORA_C4_SYNC_MDMA_PRE];
    w.syncFrameCalls=f.core.syncReasonCalls[AURORA_C4_SYNC_VISIBLE_END]+
        f.core.syncReasonCalls[AURORA_C4_SYNC_FRAME_FINAL]+
        f.core.syncReasonCalls[AURORA_C4_SYNC_MIDFRAME]+
        f.core.syncReasonCalls[AURORA_C4_SYNC_EVERYLINE];
    w.rasterCatchupCalls=f.core.rasterCatchupCalls;
    w.ppuCpuWrites=SumPpuWrites(f.core,AURORA_C4_PPUWRITE_CPU);
    w.ppuHdmaWrites=SumPpuWrites(f.core,AURORA_C4_PPUWRITE_HDMA);
    w.ppuMdmaWrites=SumPpuWrites(f.core,AURORA_C4_PPUWRITE_MDMA);
    w.fsSkipped=f.frameskipSkipped; w.videoSuppressed=f.core.videoTarget?FALSE:TRUE;
    Int32 pos=C4_WORST_COUNT;
    for(Int32 i=0;i<C4_WORST_COUNT;i++) if(!s_live.worst[i].valid||w.work>s_live.worst[i].work){pos=i;break;}
    if(pos>=C4_WORST_COUNT)return;
    for(Int32 i=C4_WORST_COUNT-1;i>pos;i--)s_live.worst[i]=s_live.worst[i-1];
    s_live.worst[pos]=w;
}
static void Accumulate(const HostFrameT &f)
{
    const CoreFrameT &c=f.core; ++s_live.frames;
    if(c.videoTarget)++s_live.videoTargetFrames;else ++s_live.videoSuppressedFrames;
#define A(name) s_live.name+=f.name
    A(hostTotalCycles); A(hostWorkCycles); A(preCoreCycles); A(coreWrapperCycles);
    A(betweenCoreRenderCycles); A(renderCycles); A(renderWorkCycles); A(postRenderCycles);
    A(gpFlushCycles); A(vblankCycles); A(postAudioCycles); A(skipAudioCycles);
#undef A
    if(f.hostWorkCycles<s_live.hostWorkMin)s_live.hostWorkMin=f.hostWorkCycles;
    if(f.hostWorkCycles>s_live.hostWorkMax)s_live.hostWorkMax=f.hostWorkCycles;
    if(s_hostBudgetCycles&&f.hostWorkCycles>s_hostBudgetCycles)++s_live.hostOverBudgetFrames;
#define C(name) s_live.name+=c.name
    if(c.exclusiveValid)
    {
        ++s_live.exclusiveValidFrames;
        C(exclusiveTotal);C(exclusiveCpu);C(exclusivePpu);C(exclusiveRaster);C(exclusiveSpc);C(exclusiveDsp);
    }
    C(coreCycles);C(cpuCycles);C(ppuCycles);C(gsuCycles);C(mdmaCycles);C(hdmaCycles);C(spcCycles);C(dspCycles);
    C(blendCycles);C(ppuSyncCycles);C(mode7Cycles);C(bgInfoCycles);C(bgOffsetCycles);C(bgMapCycles);C(bgChrCycles);
    C(bgMainCycles);C(bgSubCycles);C(colorMathCycles);C(objUpdateCycles);C(objFetchCycles);C(objDrawCycles);
    C(hdmaDataCycles);C(hdmaTableCycles);C(oamWrites);C(vramWrites);C(cgramWrites);C(objEnabledLines);C(objRefs);
    C(objTiles);C(objPotentialSlivers);C(objCacheHits);C(objCacheMisses);C(objCacheRefreshes);C(bgCacheHits);
    C(bgCacheMisses);C(bgCacheRefreshes);C(chrCacheInvalidations);C(audioSamples);C(objRangeLines);C(objTimeLines);
    C(ppuSyncCalls);C(ppuRenderLines);C(ppuQueuedWrites);C(ppuAppliedWrites);C(ppuQueueFull);C(dmaStarts);
    C(dmaReadBytes);C(dmaOamBytes);C(dmaVramBytes);C(dmaCgramBytes);C(dmaOtherBytes);C(dmaWraps);
    C(hdmaScrollBytes);C(hdmaCgramBytes);C(hdmaWindowColorBytes);C(hdmaOtherBytes);C(hdmaLines);
    C(hdmaActiveChannels);C(hdmaTransferChannels);C(bgActiveLayers);C(bgMapReloads);C(bgChrRows);
    C(mosaicLines);C(windowLines);C(colorMathLines);C(hiresMode56Lines);C(pseudoHiresLines);C(hirqCount);
    C(rasterCatchupCalls);C(rasterCatchupCycles);C(rasterCatchupHdmaRuns);
#undef C
    for(Int32 i=0;i<8;i++){s_live.dmaModes[i]+=c.dmaModes[i];s_live.modeLines[i]+=c.modeLines[i];}
    for(Int32 i=0;i<AURORA_C4_SYNC_REASON_COUNT;i++)
    {
        s_live.syncReasonCalls[i]+=c.syncReasonCalls[i];
        s_live.syncReasonCycles[i]+=c.syncReasonCycles[i];
    }
    for(Int32 src=0;src<AURORA_C4_PPUWRITE_SOURCE_COUNT;src++)
        for(Int32 reg=0;reg<C4_PPU_REG_COUNT;reg++)
            s_live.ppuWrites[src][reg]+=c.ppuWrites[src][reg];
    for(Int32 ch=0;ch<C4_HDMA_CHANNELS;ch++)
    {
        s_live.hdmaChActive[ch]+=c.hdmaChActive[ch];
        s_live.hdmaChTransfers[ch]+=c.hdmaChTransfers[ch];
        s_live.hdmaChBytes[ch]+=c.hdmaChBytes[ch];
        s_live.hdmaChIndirectTransfers[ch]+=c.hdmaChIndirectTransfers[ch];
        s_live.hdmaChReverseTransfers[ch]+=c.hdmaChReverseTransfers[ch];
        s_live.hdmaChLastDMAP[ch]=c.hdmaChLastDMAP[ch];
        s_live.hdmaChLastBBAD[ch]=c.hdmaChLastBBAD[ch];
        for(Int32 mode=0;mode<C4_HDMA_MODES;mode++)
            s_live.hdmaChModeTransfers[ch][mode]+=c.hdmaChModeTransfers[ch][mode];
    }
    if(c.objMaxSelectedLine>s_live.objMaxSelectedLine)s_live.objMaxSelectedLine=c.objMaxSelectedLine;
    if(c.objMaxPotentialLine>s_live.objMaxPotentialLine)s_live.objMaxPotentialLine=c.objMaxPotentialLine;
    if(c.objMaxFetchedLine>s_live.objMaxFetchedLine)s_live.objMaxFetchedLine=c.objMaxFetchedLine;
    s_live.lastObjOBSEL=c.lastObjOBSEL;s_live.lastObjTM=c.lastObjTM;s_live.lastObjTS=c.lastObjTS;s_live.lastObjPriority=c.lastObjPriority;
    if(c.hirqCount){if(c.hirqMinLine<s_live.hirqMinLine)s_live.hirqMinLine=c.hirqMinLine;if(c.hirqMaxLine>s_live.hirqMaxLine)s_live.hirqMaxLine=c.hirqMaxLine;}

    s_live.gsFrames+=f.gs.frames;s_live.gsLines+=f.gs.lines;s_live.gsSyncCalls+=f.gs.syncCalls;s_live.gsSyncCycles+=f.gs.syncCycles;
    s_live.gsCopyCycles+=f.gs.copyCycles;s_live.gsKickCycles+=f.gs.kickCycles;s_live.gsCopyBytes+=f.gs.copyBytes;
    s_live.gsPaletteUploads+=f.gs.paletteUploads;s_live.gsIntensityLines+=f.gs.intensityLines;s_live.gsDirectMainLines+=f.gs.directMainLines;

    s_live.audioAvailableCycles+=f.audio.availableCycles;s_live.audioPlayCycles+=f.audio.playCycles;
    s_live.audioEnqueuedFrames+=f.audio.enqueuedFrames;s_live.audioRequestedFrames+=f.audio.requestedFrames;
    s_live.audioSentFrames+=f.audio.sentFrames;s_live.audioDroppedFrames+=f.audio.droppedFrames;
    s_live.audioQueueSum+=f.audio.queueSum;s_live.audioQueueObs+=f.audio.queueObs;
    s_live.audioAvailableCalls+=f.audio.availableCalls;s_live.audioPlayCalls+=f.audio.playCalls;
    s_live.audioDrainCalls+=f.audio.drainCalls;s_live.audioAsyncStartCalls+=f.audio.asyncStartCalls;
    s_live.audioShortWrites+=f.audio.shortWrites;
    if(f.audio.queueObs){if(f.audio.queueMin<s_live.audioQueueMin)s_live.audioQueueMin=f.audio.queueMin;if(f.audio.queueMax>s_live.audioQueueMax)s_live.audioQueueMax=f.audio.queueMax;}
    s_live.audioResampleCycles+=f.audio.resampleCycles;
    s_live.audioResampleInFrames+=f.audio.resampleInFrames;
    s_live.audioResampleOutFrames+=f.audio.resampleOutFrames;
    s_live.audioPackCycles+=f.audio.packCycles;
    s_live.audioPackFrames+=f.audio.packFrames;
    s_live.audioAvailableFramesSum+=f.audio.availableFramesSum;
    s_live.audioAvailableFrameObs+=f.audio.availableFrameObs;
    s_live.audioResampleCalls+=f.audio.resampleCalls;
    s_live.audioPackCalls+=f.audio.packCalls;
    if(f.audio.availableFrameObs){if(f.audio.availableFramesMin<s_live.audioAvailableFramesMin)s_live.audioAvailableFramesMin=f.audio.availableFramesMin;if(f.audio.availableFramesMax>s_live.audioAvailableFramesMax)s_live.audioAvailableFramesMax=f.audio.availableFramesMax;}
    InsertWorst(f);
}
static Bool CyclesLine(FILE *fp,const char *name,Uint64 cyc,Uint32 frames,Uint64 hostWork)
{
    Uint32 p=Pct10(cyc,hostWork);
    return fprintf(fp,"%-18s total=%llu avg_frame=%llu host_work_pct=%u.%u%%\n",name,
        (unsigned long long)cyc,(unsigned long long)Avg64(cyc,frames),(unsigned)(p/10u),(unsigned)(p%10u))>=0?TRUE:FALSE;
}
static Bool WriteDump(FILE *fp,const SnapshotT &snap)
{
    const StatsT &s=snap.stats; Uint32 f=s.frames?s.frames:1u;
    Uint32 qmin=s.audioQueueMin==0xFFFFFFFFu?0u:s.audioQueueMin;
    Uint32 avmin=s.audioAvailableFramesMin==0xFFFFFFFFu?0u:s.audioAvailableFramesMin;
    Int32 irqmin=s.hirqMinLine==0x7FFFFFFF?-1:s.hirqMinLine;
    Uint32 corePct=Pct10(s.coreCycles,s.hostWorkCycles);
    Uint32 overPct=s.frames?(s.hostOverBudgetFrames*1000u+s.frames/2u)/s.frames:0u;
    Uint32 skipPct=s.fsDecisions?(s.fsSkipped*1000u+s.fsDecisions/2u)/s.fsDecisions:0u;
    Uint64 exclusiveTracked=s.exclusiveCpu+s.exclusivePpu+s.exclusiveRaster+s.exclusiveSpc+s.exclusiveDsp;
    Uint64 exclusiveUntracked=s.exclusiveTotal>exclusiveTracked?s.exclusiveTotal-exclusiveTracked:0u;
    Uint64 audioRpcCycles=s.audioAvailableCycles+s.audioPlayCycles;
    Uint64 audioCoreExclusive=s.exclusiveSpc+s.exclusiveDsp;

    if(fprintf(fp,
"SNESticleAurora Cursed Four profiler v8.3-mix1\n"
"schema=c4-unified-v8.3-mix1\n"
"timestamp=%04u-%02u-%02u %02u:%02u:%02u\n"
"family=%s\nvariant=%s\ncrc32=%08X\nrom_title=%s\n"
"sample_frames=%u\nvideo_target_frames=%u\nvideo_suppressed_frames=%u\n"
"clock_pair_min_cycles=%u\nrefresh=%u/%u\nhost_budget_cycles=%u\n\n",
(unsigned)snap.stamp.year,(unsigned)snap.stamp.month,(unsigned)snap.stamp.day,(unsigned)snap.stamp.hour,(unsigned)snap.stamp.minute,(unsigned)snap.stamp.second,
FamilyName(snap.crc32),VariantName(snap.crc32),(unsigned)snap.crc32,snap.title,(unsigned)s.frames,
(unsigned)s.videoTargetFrames,(unsigned)s.videoSuppressedFrames,(unsigned)snap.clockPairMin,
(unsigned)snap.refreshNum,(unsigned)snap.refreshDen,(unsigned)snap.hostBudgetCycles)<0)return FALSE;

    if(fprintf(fp,
"[SUMMARY]\n"
"host_total_cycles=%llu avg=%llu\nhost_work_cycles=%llu avg=%llu min=%u max=%u\n"
"host_over_budget_frames=%u pct=%u.%u%%\ncore_cycles=%llu avg=%llu host_work_pct=%u.%u%%\n"
"vblank_cycles=%llu avg=%llu\nframeskip_skipped=%u video_suppressed=%u\n\n",
(unsigned long long)s.hostTotalCycles,(unsigned long long)Avg64(s.hostTotalCycles,f),
(unsigned long long)s.hostWorkCycles,(unsigned long long)Avg64(s.hostWorkCycles,f),
(unsigned)(s.hostWorkMin==0xFFFFFFFFu?0u:s.hostWorkMin),(unsigned)s.hostWorkMax,
(unsigned)s.hostOverBudgetFrames,(unsigned)(overPct/10u),(unsigned)(overPct%10u),
(unsigned long long)s.coreCycles,(unsigned long long)Avg64(s.coreCycles,f),(unsigned)(corePct/10u),(unsigned)(corePct%10u),
(unsigned long long)s.vblankCycles,(unsigned long long)Avg64(s.vblankCycles,f),(unsigned)s.fsSkipped,(unsigned)s.videoSuppressedFrames)<0)return FALSE;

    fprintf(fp,"[HOST]\n");
    if(!CyclesLine(fp,"pre_core",s.preCoreCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"core_wrapper",s.coreWrapperCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"between_core_render",s.betweenCoreRenderCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"render_total",s.renderCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"render_work",s.renderWorkCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"post_render",s.postRenderCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"gp_flush",s.gpFlushCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"post_vblank_audio",s.postAudioCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"skip_audio",s.skipAudioCycles,f,s.hostWorkCycles))return FALSE;
    fprintf(fp,"\n");

    fprintf(fp,"[CORE_EXCLUSIVE]\nvalid_frames=%u/%u\n",
        (unsigned)s.exclusiveValidFrames,(unsigned)s.frames);
    const Uint32 ef=s.exclusiveValidFrames?s.exclusiveValidFrames:1u;
    if(!CyclesLine(fp,"CPU_65816",s.exclusiveCpu,ef,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"PPU",s.exclusivePpu,ef,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"RASTER_DMA",s.exclusiveRaster,ef,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"SPC700",s.exclusiveSpc,ef,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"S_DSP",s.exclusiveDsp,ef,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"tracked",exclusiveTracked,ef,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"untracked_core",exclusiveUntracked,ef,s.hostWorkCycles))return FALSE;
    fprintf(fp,"cost_frame_total=%llu avg=%llu\n\n",
        (unsigned long long)s.exclusiveTotal,(unsigned long long)Avg64(s.exclusiveTotal,ef));

    fprintf(fp,"[CORE_LEGACY_INCLUSIVE]\n");
    if(!CyclesLine(fp,"legacy_CPU_ctr",s.cpuCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"PPU_inclusive",s.ppuCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"GSU_SuperFX",s.gsuCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"MDMA",s.mdmaCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"HDMA",s.hdmaCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"SPC700_inclusive",s.spcCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"S_DSP_mix",s.dspCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"PPU_blend",s.blendCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"PPU_sync",s.ppuSyncCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"Mode7",s.mode7Cycles,f,s.hostWorkCycles))return FALSE;
    fprintf(fp,"NOTE: this section preserves V7 diagnostic counters; use CORE_EXCLUSIVE for CPU/core attribution.\n\n");

    fprintf(fp,"[SYNC_PPU_REASONS]\nreason,calls,cycles,avg_call\n");
    for(Int32 i=0;i<AURORA_C4_SYNC_REASON_COUNT;i++)
        fprintf(fp,"%s,%llu,%llu,%llu\n",SyncReasonName(i),
            (unsigned long long)s.syncReasonCalls[i],
            (unsigned long long)s.syncReasonCycles[i],
            (unsigned long long)Avg64(s.syncReasonCycles[i],s.syncReasonCalls[i]));
    fprintf(fp,"raster_catchup calls=%llu cycles=%llu avg=%llu hdma_runs=%llu\n\n",
        (unsigned long long)s.rasterCatchupCalls,
        (unsigned long long)s.rasterCatchupCycles,
        (unsigned long long)Avg64(s.rasterCatchupCycles,s.rasterCatchupCalls),
        (unsigned long long)s.rasterCatchupHdmaRuns);

    fprintf(fp,"[PPU]\n");
    if(!CyclesLine(fp,"BG_info",s.bgInfoCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"BG_offset",s.bgOffsetCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"BG_map",s.bgMapCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"BG_chr",s.bgChrCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"BG_main",s.bgMainCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"BG_sub",s.bgSubCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"color_math",s.colorMathCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"OBJ_update",s.objUpdateCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"OBJ_fetch",s.objFetchCycles,f,s.hostWorkCycles))return FALSE;
    if(!CyclesLine(fp,"OBJ_draw",s.objDrawCycles,f,s.hostWorkCycles))return FALSE;
    fprintf(fp,
"ppu_render_lines=%llu\nppu_sync_calls=%llu avg_frame=%llu avg_cycles_per_call=%llu\n"
"ppu_writes queued/applied/full=%llu/%llu/%llu\n"
"bg_work active_layers/map_reloads/chr_rows=%llu/%llu/%llu\n"
"mode_lines 0/1/2/3/4/5/6/7=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n"
"effect_lines mosaic/window/color_math/hires56/pseudo_hires=%llu/%llu/%llu/%llu/%llu\n\n",
(unsigned long long)s.ppuRenderLines,(unsigned long long)s.ppuSyncCalls,(unsigned long long)Avg64(s.ppuSyncCalls,f),
(unsigned long long)Avg64(s.ppuSyncCycles,s.ppuSyncCalls),(unsigned long long)s.ppuQueuedWrites,(unsigned long long)s.ppuAppliedWrites,
(unsigned long long)s.ppuQueueFull,(unsigned long long)s.bgActiveLayers,(unsigned long long)s.bgMapReloads,(unsigned long long)s.bgChrRows,
(unsigned long long)s.modeLines[0],(unsigned long long)s.modeLines[1],(unsigned long long)s.modeLines[2],(unsigned long long)s.modeLines[3],
(unsigned long long)s.modeLines[4],(unsigned long long)s.modeLines[5],(unsigned long long)s.modeLines[6],(unsigned long long)s.modeLines[7],
(unsigned long long)s.mosaicLines,(unsigned long long)s.windowLines,(unsigned long long)s.colorMathLines,
(unsigned long long)s.hiresMode56Lines,(unsigned long long)s.pseudoHiresLines);

    fprintf(fp,"[PPU_WRITE_HIST]\nsource,reg,count,pct_source\n");
    for(Int32 src=0;src<AURORA_C4_PPUWRITE_SOURCE_COUNT;src++)
    {
        Uint64 sourceTotal=0;
        for(Int32 reg=0;reg<C4_PPU_REG_COUNT;reg++)sourceTotal+=s.ppuWrites[src][reg];
        fprintf(fp,"%s_TOTAL,----,%llu,100.0%%\n",PpuWriteSourceName(src),(unsigned long long)sourceTotal);
        for(Int32 reg=0;reg<C4_PPU_REG_COUNT;reg++)
        {
            Uint64 n=s.ppuWrites[src][reg];
            if(!n)continue;
            Uint32 p=Pct10(n,sourceTotal);
            fprintf(fp,"%s,$21%02X,%llu,%u.%u%%\n",PpuWriteSourceName(src),(unsigned)reg,
                (unsigned long long)n,(unsigned)(p/10u),(unsigned)(p%10u));
        }
    }
    fprintf(fp,"\n");

    Uint64 cacheTotal=s.objCacheHits+s.objCacheMisses; Uint32 hit=Pct10(s.objCacheHits,cacheTotal);
    fprintf(fp,
"[OBJ]\nenabled_lines=%llu\nselected_oam_refs=%llu avg_frame=%llu\npotential_slivers=%llu avg_frame=%llu\n"
"fetched_slivers=%llu avg_frame=%llu\nmax_line selected/potential/fetched=%u/%u/%u\n"
"cache hit/miss/refresh=%llu/%llu/%llu hit_pct=%u.%u%%\nrange_over_lines=%llu time_over_lines=%llu\n"
"cycles_per_fetched_sliver fetch=%llu draw=%llu\nlast_regs obsel/tm/ts/first=%02X/%02X/%02X/%u\n\n",
(unsigned long long)s.objEnabledLines,(unsigned long long)s.objRefs,(unsigned long long)Avg64(s.objRefs,f),
(unsigned long long)s.objPotentialSlivers,(unsigned long long)Avg64(s.objPotentialSlivers,f),
(unsigned long long)s.objTiles,(unsigned long long)Avg64(s.objTiles,f),
(unsigned)s.objMaxSelectedLine,(unsigned)s.objMaxPotentialLine,(unsigned)s.objMaxFetchedLine,
(unsigned long long)s.objCacheHits,(unsigned long long)s.objCacheMisses,(unsigned long long)s.objCacheRefreshes,
(unsigned)(hit/10u),(unsigned)(hit%10u),(unsigned long long)s.objRangeLines,(unsigned long long)s.objTimeLines,
(unsigned long long)Avg64(s.objFetchCycles,s.objTiles),(unsigned long long)Avg64(s.objDrawCycles,s.objTiles),
(unsigned)s.lastObjOBSEL,(unsigned)s.lastObjTM,(unsigned)s.lastObjTS,(unsigned)s.lastObjPriority);

    Uint64 bgct=s.bgCacheHits+s.bgCacheMisses;Uint32 bgh=Pct10(s.bgCacheHits,bgct);
    fprintf(fp,
"[CACHE]\nobj enabled=1 hit/miss/refresh=%llu/%llu/%llu hit_pct=%u.%u%%\n"
"bg enabled=%u hit/miss/refresh=%llu/%llu/%llu hit_pct=%u.%u%%\nchr_invalidated_tiles=%llu\n\n",
(unsigned long long)s.objCacheHits,(unsigned long long)s.objCacheMisses,(unsigned long long)s.objCacheRefreshes,
(unsigned)(hit/10u),(unsigned)(hit%10u),(unsigned)(bgct?1:0),
(unsigned long long)s.bgCacheHits,(unsigned long long)s.bgCacheMisses,(unsigned long long)s.bgCacheRefreshes,
(unsigned)(bgh/10u),(unsigned)(bgh%10u),(unsigned long long)s.chrCacheInvalidations);

    fprintf(fp,
"[DMA_HDMA_IRQ]\nmdma starts/read_bytes/wraps=%llu/%llu/%llu\nmdma target_bytes oam/vram/cgram/other=%llu/%llu/%llu/%llu\n"
"mdma modes0..7=%llu/%llu/%llu/%llu/%llu/%llu/%llu/%llu\n"
"hdma lines/active_channels/xfer_channels=%llu/%llu/%llu\nhdma bytes scroll/cgram/window_color/other=%llu/%llu/%llu/%llu\n"
"hdma cycles total/data/table=%llu/%llu/%llu\nhirq count=%llu line_min=%d line_max=%d\nports oam/vram/cgram writes=%llu/%llu/%llu\n\n",
(unsigned long long)s.dmaStarts,(unsigned long long)s.dmaReadBytes,(unsigned long long)s.dmaWraps,
(unsigned long long)s.dmaOamBytes,(unsigned long long)s.dmaVramBytes,(unsigned long long)s.dmaCgramBytes,(unsigned long long)s.dmaOtherBytes,
(unsigned long long)s.dmaModes[0],(unsigned long long)s.dmaModes[1],(unsigned long long)s.dmaModes[2],(unsigned long long)s.dmaModes[3],
(unsigned long long)s.dmaModes[4],(unsigned long long)s.dmaModes[5],(unsigned long long)s.dmaModes[6],(unsigned long long)s.dmaModes[7],
(unsigned long long)s.hdmaLines,(unsigned long long)s.hdmaActiveChannels,(unsigned long long)s.hdmaTransferChannels,
(unsigned long long)s.hdmaScrollBytes,(unsigned long long)s.hdmaCgramBytes,(unsigned long long)s.hdmaWindowColorBytes,(unsigned long long)s.hdmaOtherBytes,
(unsigned long long)s.hdmaCycles,(unsigned long long)s.hdmaDataCycles,(unsigned long long)s.hdmaTableCycles,
(unsigned long long)s.hirqCount,(int)irqmin,(int)s.hirqMaxLine,
(unsigned long long)s.oamWrites,(unsigned long long)s.vramWrites,(unsigned long long)s.cgramWrites);

    fprintf(fp,"[HDMA_CHANNELS]\nch,active_lines,transfers,bytes,indirect,reverse,last_dmap,last_bbad,mode0,mode1,mode2,mode3,mode4,mode5,mode6,mode7\n");
    for(Int32 ch=0;ch<C4_HDMA_CHANNELS;ch++)
        fprintf(fp,"%d,%llu,%llu,%llu,%llu,%llu,%02X,%02X,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            (int)ch,
            (unsigned long long)s.hdmaChActive[ch],
            (unsigned long long)s.hdmaChTransfers[ch],
            (unsigned long long)s.hdmaChBytes[ch],
            (unsigned long long)s.hdmaChIndirectTransfers[ch],
            (unsigned long long)s.hdmaChReverseTransfers[ch],
            (unsigned)s.hdmaChLastDMAP[ch],(unsigned)s.hdmaChLastBBAD[ch],
            (unsigned long long)s.hdmaChModeTransfers[ch][0],(unsigned long long)s.hdmaChModeTransfers[ch][1],
            (unsigned long long)s.hdmaChModeTransfers[ch][2],(unsigned long long)s.hdmaChModeTransfers[ch][3],
            (unsigned long long)s.hdmaChModeTransfers[ch][4],(unsigned long long)s.hdmaChModeTransfers[ch][5],
            (unsigned long long)s.hdmaChModeTransfers[ch][6],(unsigned long long)s.hdmaChModeTransfers[ch][7]);
    fprintf(fp,"\n");

    fprintf(fp,
"[GS]\nframes=%llu lines=%llu\nsync calls=%llu cycles=%llu avg_call=%llu\ncopy cycles=%llu bytes=%llu avg_line_cycles=%llu avg_line_bytes=%llu\n"
"kick cycles=%llu avg_line=%llu\npalette_uploads=%llu intensity_lines=%llu direct_main_lines=%llu\nfrontend_gp_flush_cycles=%llu avg_frame=%llu\n\n",
(unsigned long long)s.gsFrames,(unsigned long long)s.gsLines,(unsigned long long)s.gsSyncCalls,(unsigned long long)s.gsSyncCycles,
(unsigned long long)Avg64(s.gsSyncCycles,s.gsSyncCalls),(unsigned long long)s.gsCopyCycles,(unsigned long long)s.gsCopyBytes,
(unsigned long long)Avg64(s.gsCopyCycles,s.gsLines),(unsigned long long)Avg64(s.gsCopyBytes,s.gsLines),
(unsigned long long)s.gsKickCycles,(unsigned long long)Avg64(s.gsKickCycles,s.gsLines),(unsigned long long)s.gsPaletteUploads,
(unsigned long long)s.gsIntensityLines,(unsigned long long)s.gsDirectMainLines,(unsigned long long)s.gpFlushCycles,
(unsigned long long)Avg64(s.gpFlushCycles,f));

    {
        static const char *const names[AURORA_C4_CHR_KERNEL_COUNT]=
        {
            "CHR2_NOOFFSET",
            "CHR4_NOOFFSET",
            "CHR2_GENERIC_OFFSET",
            "CHR4_GENERIC_OFFSET"
        };
        Uint64 allCalls=0,allTiles=0,allCycles=0;

        fprintf(fp,
"[CHR_KERNELS]\n"
"kind,calls,tiles,cycles,avg_call,cycles_per_tile,bg_chr_pct\n");

        for(Uint32 k=0;k<AURORA_C4_CHR_KERNEL_COUNT;k++)
        {
            const ChrKernelStatT &v=s_chrPending[k];
            const Uint64 cpt10=v.tiles
                ? (v.cycles*10u+v.tiles/2u)/v.tiles : 0u;
            const Uint32 pct=Pct10(v.cycles,s.bgChrCycles);

            fprintf(fp,"%s,%llu,%llu,%llu,%llu,%llu.%llu,%u.%u%%\n",
                names[k],
                (unsigned long long)v.calls,
                (unsigned long long)v.tiles,
                (unsigned long long)v.cycles,
                (unsigned long long)Avg64(v.cycles,v.calls),
                (unsigned long long)(cpt10/10u),
                (unsigned long long)(cpt10%10u),
                (unsigned)(pct/10u),(unsigned)(pct%10u));

            allCalls+=v.calls;
            allTiles+=v.tiles;
            allCycles+=v.cycles;
        }

        {
            const Uint64 cpt10=allTiles
                ? (allCycles*10u+allTiles/2u)/allTiles : 0u;
            const Uint64 residual=s.bgChrCycles>allCycles
                ? s.bgChrCycles-allCycles : 0u;
            const Uint32 pct=Pct10(allCycles,s.bgChrCycles);

            fprintf(fp,
"TOTAL,%llu,%llu,%llu,%llu,%llu.%llu,%u.%u%%\n"
"bg_chr_total=%llu raw_kernel_coverage=%u.%u%% residual_cycles=%llu avg_residual_frame=%llu\n"
"NOTE raw kernel timers surround only the ASM call; wrapper/setup/dispatch/PROF and uninstrumented hires/CHR8 remain in residual.\n"
"NOTE bookkeeping happens after the end timestamp, but this diagnostic build adds some outer BG_chr overhead; compare kernel cycles/tile, not absolute BG_chr, against older profiler builds.\n\n",
                (unsigned long long)allCalls,
                (unsigned long long)allTiles,
                (unsigned long long)allCycles,
                (unsigned long long)Avg64(allCycles,allCalls),
                (unsigned long long)(cpt10/10u),
                (unsigned long long)(cpt10%10u),
                (unsigned)(pct/10u),(unsigned)(pct%10u),
                (unsigned long long)s.bgChrCycles,
                (unsigned)(pct/10u),(unsigned)(pct%10u),
                (unsigned long long)residual,
                (unsigned long long)Avg64(residual,f));
        }
    }

    {
        static const char *const flipName[4]={"NONE","H","V","HV"};
        Uint64 total=0,transparent=0;
        for(Uint32 k=0;k<4;k++)
        {
            total+=s_chr4MixPending.flip[k];
            transparent+=s_chr4MixPending.transparent[k];
        }

        fprintf(fp,
"[CHR4_DATA_MIX]\n"
"state,tiles,pct_tiles,transparent,pct_transparent_within_state\n");

        for(Uint32 k=0;k<4;k++)
        {
            const Uint64 n=s_chr4MixPending.flip[k];
            const Uint64 tr=s_chr4MixPending.transparent[k];
            const Uint32 pct=Pct10(n,total);
            const Uint32 tpct=Pct10(tr,n);
            fprintf(fp,"%s,%llu,%u.%u%%,%llu,%u.%u%%\n",
                flipName[k],
                (unsigned long long)n,
                (unsigned)(pct/10u),(unsigned)(pct%10u),
                (unsigned long long)tr,
                (unsigned)(tpct/10u),(unsigned)(tpct%10u));
        }

        {
            const Uint64 h=s_chr4MixPending.flip[1]+s_chr4MixPending.flip[3];
            const Uint64 v=s_chr4MixPending.flip[2]+s_chr4MixPending.flip[3];
            const Uint64 ht=s_chr4MixPending.transparent[1]+s_chr4MixPending.transparent[3];
            const Uint32 hp=Pct10(h,total);
            const Uint32 vp=Pct10(v,total);
            const Uint32 tp=Pct10(transparent,total);
            const Uint32 htp=Pct10(ht,h);

            fprintf(fp,
"TOTAL tiles=%llu transparent=%llu transparent_pct=%u.%u%% opaque=%llu\n"
"hflip=%llu pct=%u.%u%% hflip_transparent=%llu pct_within_h=%u.%u%%\n"
"vflip=%llu pct=%u.%u%%\n"
"NOTE data-mix scan runs after the raw CHR4 ASM timer closes. Ignore outer BG_chr/host totals from this diagnostic build; CHR4 cycles_per_tile remains the pre-scan raw timing.\n\n",
                (unsigned long long)total,
                (unsigned long long)transparent,
                (unsigned)(tp/10u),(unsigned)(tp%10u),
                (unsigned long long)(total-transparent),
                (unsigned long long)h,
                (unsigned)(hp/10u),(unsigned)(hp%10u),
                (unsigned long long)ht,
                (unsigned)(htp/10u),(unsigned)(htp%10u),
                (unsigned long long)v,
                (unsigned)(vp/10u),(unsigned)(vp%10u));
        }
    }

    fprintf(fp,
"[AUDIO]\ncore_spc_cycles=%llu avg_frame=%llu\ncore_dsp_mix_cycles=%llu avg_frame=%llu\n"
"core_audio_samples=%llu avg_frame=%llu\nasync enqueued/requested/sent/dropped=%llu/%llu/%llu/%llu\n"
"async starts=%llu drain_calls=%llu short_writes=%llu\navailable_rpc calls=%llu cycles=%llu avg=%llu\n"
"play_rpc calls=%llu cycles=%llu avg=%llu\nqueue min/max/avg=%u/%u/%llu\n"
"post_vblank_audio_cycles=%llu avg_frame=%llu\nskip_audio_cycles=%llu avg_skipped=%llu\n\n",
(unsigned long long)s.spcCycles,(unsigned long long)Avg64(s.spcCycles,f),(unsigned long long)s.dspCycles,(unsigned long long)Avg64(s.dspCycles,f),
(unsigned long long)s.audioSamples,(unsigned long long)Avg64(s.audioSamples,f),
(unsigned long long)s.audioEnqueuedFrames,(unsigned long long)s.audioRequestedFrames,(unsigned long long)s.audioSentFrames,(unsigned long long)s.audioDroppedFrames,
(unsigned long long)s.audioAsyncStartCalls,(unsigned long long)s.audioDrainCalls,(unsigned long long)s.audioShortWrites,
(unsigned long long)s.audioAvailableCalls,(unsigned long long)s.audioAvailableCycles,(unsigned long long)Avg64(s.audioAvailableCycles,s.audioAvailableCalls),
(unsigned long long)s.audioPlayCalls,(unsigned long long)s.audioPlayCycles,(unsigned long long)Avg64(s.audioPlayCycles,s.audioPlayCalls),
(unsigned)qmin,(unsigned)s.audioQueueMax,(unsigned long long)Avg64(s.audioQueueSum,s.audioQueueObs),
(unsigned long long)s.postAudioCycles,(unsigned long long)Avg64(s.postAudioCycles,f),
(unsigned long long)s.skipAudioCycles,(unsigned long long)Avg64(s.skipAudioCycles,s.fsSkipped));

    /* AURORA_C4_AUDIO_REPORT_SUBSET_NOTE_V8_2_2_20261004: wording-only clarification. */
    fprintf(fp,
"[AUDIO_EE_COST]\n"
"format snes_source_hz=32000 audsrv_sink_hz=48000 bits=16 channels=2 bytes_per_stereo_frame=4\n"
"core_audio_exclusive_spc_plus_dsp=%llu avg_frame=%llu\n"
"resample_32_to_48 calls=%llu cycles=%llu avg_frame=%llu avg_call=%llu in_frames=%llu out_frames=%llu\n"
"gain_pack_fifo calls=%llu cycles=%llu avg_frame=%llu avg_call=%llu frames=%llu\n"
"rpc_transport available_plus_play_cycles=%llu avg_frame=%llu host_work_pct=%u.%u%%\n"
"rpc_transport sent_frames=%llu sent_bytes=%llu\n"
"available_probe frames_min/max/avg=%u/%u/%llu observations=%llu\n"
"post_vblank_endpoint_cycles=%llu skip_endpoint_cycles=%llu\n"
"NOTE resample_32_to_48 and gain_pack_fifo are SUBSETS of the S_DSP/core_audio_exclusive scope; do not add either again.\n"
"NOTE rpc_transport is separate EE<->IOP transport measured after VBlank; it is not inside S_DSP.\n"
"NOTE probes measure EE cost and EE<->IOP RPC latency, not internal IOP CPU utilization.\n\n",
(unsigned long long)audioCoreExclusive,(unsigned long long)Avg64(audioCoreExclusive,f),
(unsigned long long)s.audioResampleCalls,(unsigned long long)s.audioResampleCycles,
(unsigned long long)Avg64(s.audioResampleCycles,f),(unsigned long long)Avg64(s.audioResampleCycles,s.audioResampleCalls),
(unsigned long long)s.audioResampleInFrames,(unsigned long long)s.audioResampleOutFrames,
(unsigned long long)s.audioPackCalls,(unsigned long long)s.audioPackCycles,
(unsigned long long)Avg64(s.audioPackCycles,f),(unsigned long long)Avg64(s.audioPackCycles,s.audioPackCalls),
(unsigned long long)s.audioPackFrames,
(unsigned long long)audioRpcCycles,(unsigned long long)Avg64(audioRpcCycles,f),
(unsigned)(Pct10(audioRpcCycles,s.hostWorkCycles)/10u),(unsigned)(Pct10(audioRpcCycles,s.hostWorkCycles)%10u),
(unsigned long long)s.audioSentFrames,(unsigned long long)(s.audioSentFrames*4u),
(unsigned)avmin,(unsigned)s.audioAvailableFramesMax,
(unsigned long long)Avg64(s.audioAvailableFramesSum,s.audioAvailableFrameObs),
(unsigned long long)s.audioAvailableFrameObs,
(unsigned long long)s.postAudioCycles,(unsigned long long)s.skipAudioCycles);

    if(fprintf(fp,
"[FRAMESKIP]\ndecisions=%u enabled=%u allowed=%u skipped=%u skip_pct=%u.%u%%\n"
"max_skip_streak=%u\ncore_video_target=%u core_video_suppressed=%u\nhost_over_budget=%u\n\n",
(unsigned)s.fsDecisions,(unsigned)s.fsEnabledDecisions,(unsigned)s.fsAllowedDecisions,(unsigned)s.fsSkipped,
(unsigned)(skipPct/10u),(unsigned)(skipPct%10u),(unsigned)s.fsMaxStreak,
(unsigned)s.videoTargetFrames,(unsigned)s.videoSuppressedFrames,(unsigned)s.hostOverBudgetFrames)<0)return FALSE;

    fprintf(fp,"[WORST_FRAMES]\nrank,frame,work,host,core,cpu_excl,ppu_excl,obj_fetch,obj_draw,spc_excl,dsp_excl,mdma,hdma,ppu_sync,gs_sync,audio_play,obj_refs,obj_potential,obj_fetched,ppu_sync_calls,hirq,sync_read,sync_write,sync_mdma,sync_frame,raster_catchup,ppu_cpu_w,ppu_hdma_w,ppu_mdma_w,fs_skip,video_suppressed\n");
    for(Int32 i=0;i<C4_WORST_COUNT;i++){const WorstT&w=s.worst[i];if(!w.valid)continue;
        fprintf(fp,"%d,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
        (int)(i+1),(unsigned)w.frameNo,(unsigned)w.work,(unsigned)w.host,(unsigned)w.core,(unsigned)w.cpu,(unsigned)w.ppu,
        (unsigned)w.objFetch,(unsigned)w.objDraw,(unsigned)w.spc,(unsigned)w.dsp,(unsigned)w.mdma,(unsigned)w.hdma,(unsigned)w.ppuSync,
        (unsigned)w.gsSync,(unsigned)w.audioPlay,(unsigned)w.objRefs,(unsigned)w.objPotential,(unsigned)w.objFetched,
        (unsigned)w.ppuSyncCalls,(unsigned)w.hirqCount,(unsigned)w.syncReadCalls,(unsigned)w.syncWriteCalls,
        (unsigned)w.syncMdmaCalls,(unsigned)w.syncFrameCalls,(unsigned)w.rasterCatchupCalls,
        (unsigned)w.ppuCpuWrites,(unsigned)w.ppuHdmaWrites,(unsigned)w.ppuMdmaWrites,
        (unsigned)(w.fsSkipped?1:0),(unsigned)(w.videoSuppressed?1:0));}

    fprintf(fp,
"\n[NOTES]\n"
"- Unified schema: the Cursed Four plus Super Mario Kart use the same probes and fields.\n"
"- V8 enables the existing exclusive AuroraSnesCostProfiler only in C4 diagnostic builds/runtime.\n"
"- CORE_EXCLUSIVE pauses a parent bucket while nested PPU/Raster/SPC/DSP scopes run; use it for CPU attribution. valid_frames reports rejected scope samples.\n"
"- CORE_LEGACY_INCLUSIVE preserves V7 counters for continuity and must not be summed as exclusive time.\n"
"- SyncPPU reason timers wrap existing calls only; no event is moved, merged or rescheduled.\n"
"- PPU_WRITE_HIST counts generated writes by source. A rare HDMA queue-full fallback can also appear as CPU; check ppuQueueFull.\n"
"- HDMA_CHANNELS deliberately counts work/config without per-channel cycle timers; total HDMA data/table cycles remain in DMA_HDMA_IRQ to keep probe overhead low.\n"
"- host_work excludes blocking VBlank wait and is the EE deadline-work metric.\n"
"- video_suppressed means the SNES core ran with NULL render target from Safe Frameskip.\n"
"- C4 V8.2 adds block-level audio EE subset timers; compare absolute timing with other V8.2 captures.\n"
"- Audio pitch invariant audited at patch time: S-DSP 32000 Hz -> AudMixBuffer 2:3 -> audsrv 48000 Hz.\n"
"- Audio resample and gain+pack timers are subsets of S_DSP; do not add them again to core_audio_exclusive.\n"
"- C4 V8.3 CHR_KERNELS times raw ASM calls only; use cycles_per_tile to choose the next CHR target.\n"
"- available/play timings measure EE-side synchronous RPC latency; they do not measure internal IOP CPU utilization.\n"
"- audio behavior/timing is not changed; RTC/filesystem/USB work still happens only after menu entry.\n");
    return ferror(fp)?FALSE:TRUE;
}
static Bool SavePending(void)
{
    Char path[192]; const StampT&t=s_pending.stamp;
    snprintf(path,sizeof(path),"mass0:/SNESticle/c4prof_%04u%02u%02u_%02u%02u%02u_%s_%08X_%03u.txt",
        (unsigned)t.year,(unsigned)t.month,(unsigned)t.day,(unsigned)t.hour,(unsigned)t.minute,(unsigned)t.second,
        FileKey(s_pending.crc32),(unsigned)s_pending.crc32,(unsigned)s_pending.serial);
    BgmIOBegin(); FILE *fp=fopen(path,"wb");
    if(!fp){snprintf(path,sizeof(path),"mass0:/C4PROF_%s_%08X_%03u.txt",FileKey(s_pending.crc32),(unsigned)s_pending.crc32,(unsigned)s_pending.serial);fp=fopen(path,"wb");}
    Bool ok=fp?WriteDump(fp,s_pending):FALSE;
    if(fp){if(fflush(fp)!=0)ok=FALSE;if(fclose(fp)!=0)ok=FALSE;}
    BgmIOEnd();
    if(!ok){MainLoopStatusPrintf(180,"C4 profiler: dump write failed.");return FALSE;}
    MainLoopStatusPrintf(180,"C4 profiler saved: %s",path);return TRUE;
}
} /* namespace */

extern "C" {
Bool g_AuroraC4ProfilerActive=FALSE;
Uint32 g_AuroraC4PpuWritesFrame
    [AURORA_C4_PPUWRITE_SOURCE_COUNT][AURORA_C4_PPU_REG_COUNT];
Uint32 g_AuroraC4HdmaActiveFrame[AURORA_C4_HDMA_CHANNELS];
Uint32 g_AuroraC4HdmaTransferFrame[AURORA_C4_HDMA_CHANNELS];
Uint32 g_AuroraC4HdmaBytesFrame[AURORA_C4_HDMA_CHANNELS];
Uint32 g_AuroraC4HdmaModeFrame
    [AURORA_C4_HDMA_CHANNELS][AURORA_C4_HDMA_MODES];
Uint32 g_AuroraC4HdmaIndirectFrame[AURORA_C4_HDMA_CHANNELS];
Uint32 g_AuroraC4HdmaReverseFrame[AURORA_C4_HDMA_CHANNELS];
Uint8 g_AuroraC4HdmaLastDMAPFrame[AURORA_C4_HDMA_CHANNELS];
Uint8 g_AuroraC4HdmaLastBBADFrame[AURORA_C4_HDMA_CHANNELS];
void AuroraC4ProfilerConfigureGame(Uint32 crc32,const char *title)
{
    s_crc32=crc32;g_AuroraC4ProfilerActive=IsC4CRC(crc32);s_writePending=FALSE;s_writeDelay=0;
    s_refreshNum=s_refreshDen=s_hostBudgetCycles=0;s_clockPairMin=g_AuroraC4ProfilerActive?CalibrateClockPair():0u;
    AuroraSnesCostProfilerSetEnabled(g_AuroraC4ProfilerActive);
    memset(s_title,0,sizeof(s_title));if(g_AuroraC4ProfilerActive&&title)strncpy(s_title,title,sizeof(s_title)-1u);ResetLive();
    memset(s_chrFrame,0,sizeof(s_chrFrame));
    memset(s_chrLive,0,sizeof(s_chrLive));
    memset(s_chrPending,0,sizeof(s_chrPending));
    memset(&s_chr4MixFrame,0,sizeof(s_chr4MixFrame));
    memset(&s_chr4MixLive,0,sizeof(s_chr4MixLive));
    memset(&s_chr4MixPending,0,sizeof(s_chr4MixPending));
}
void AuroraC4ProfilerCoreFrameBegin(Uint32 frameNo,Bool videoTarget)
{
    if(!g_AuroraC4ProfilerActive||!s_tickOpen)return;
    memset(&s_tick.core,0,sizeof(s_tick.core));s_tick.core.frameNo=frameNo;s_tick.core.videoTarget=videoTarget;
    memset(s_chrFrame,0,sizeof(s_chrFrame));
    memset(&s_chr4MixFrame,0,sizeof(s_chr4MixFrame));
    memset(g_AuroraC4PpuWritesFrame,0,sizeof(g_AuroraC4PpuWritesFrame));
    memset(g_AuroraC4HdmaActiveFrame,0,sizeof(g_AuroraC4HdmaActiveFrame));
    memset(g_AuroraC4HdmaTransferFrame,0,sizeof(g_AuroraC4HdmaTransferFrame));
    memset(g_AuroraC4HdmaBytesFrame,0,sizeof(g_AuroraC4HdmaBytesFrame));
    memset(g_AuroraC4HdmaModeFrame,0,sizeof(g_AuroraC4HdmaModeFrame));
    memset(g_AuroraC4HdmaIndirectFrame,0,sizeof(g_AuroraC4HdmaIndirectFrame));
    memset(g_AuroraC4HdmaReverseFrame,0,sizeof(g_AuroraC4HdmaReverseFrame));
    memset(g_AuroraC4HdmaLastDMAPFrame,0,sizeof(g_AuroraC4HdmaLastDMAPFrame));
    memset(g_AuroraC4HdmaLastBBADFrame,0,sizeof(g_AuroraC4HdmaLastBBADFrame));
    s_tick.core.hirqMinLine=0x7FFFFFFF;s_tick.core.hirqMaxLine=-1;CaptureBase();s_coreStart=ProfCtrGetCycle();s_coreOpen=TRUE;
}
void AuroraC4ProfilerCoreFrameEnd(Uint32 frameNo)
{
    if(!g_AuroraC4ProfilerActive||!s_tickOpen||!s_coreOpen)return;
    CoreFrameT&f=s_tick.core;f.frameNo=frameNo;f.coreCycles=ProfCtrGetCycle()-s_coreStart;

    AuroraSnesCostFrameT cost;
    memset(&cost,0,sizeof(cost));
    if(AuroraSnesCostProfilerGetLastFrame(&cost))
    {
        f.exclusiveValid=TRUE;
        f.exclusiveTotal=cost.total;
        f.exclusiveCpu=cost.bucket[AURORA_SNES_COST_CPU];
        f.exclusivePpu=cost.bucket[AURORA_SNES_COST_PPU];
        f.exclusiveRaster=cost.bucket[AURORA_SNES_COST_RASTER];
        f.exclusiveSpc=cost.bucket[AURORA_SNES_COST_SPC];
        f.exclusiveDsp=cost.bucket[AURORA_SNES_COST_DSP];
    }

    f.cpuCycles=g_TmgCycCPU;f.ppuCycles=g_TmgCycPPU;f.gsuCycles=g_TmgCycGSU;f.mdmaCycles=g_TmgCycMDMA;
    f.hdmaCycles=g_TmgCycHDMA;f.spcCycles=g_TmgCycAPU;f.dspCycles=g_TmgCycMix;f.blendCycles=g_TmgCycBlend;
    f.ppuSyncCycles=g_TmgCycPPUSync;f.mode7Cycles=g_TmgCycM7;f.bgInfoCycles=g_TmgCycBGInfo;f.bgOffsetCycles=g_TmgCycBGOffset;
    f.bgMapCycles=g_TmgCycBGMap;f.bgChrCycles=g_TmgCycBGChr;f.bgMainCycles=g_TmgCycBGMain;f.bgSubCycles=g_TmgCycBGSub;
    f.colorMathCycles=g_TmgCycColorMath;f.objUpdateCycles=g_TmgCycObjUpdate;f.objFetchCycles=g_TmgCycObjFetch;f.objDrawCycles=g_TmgCycObjDraw;
    f.hdmaDataCycles=g_TmgCycHDMAData;f.hdmaTableCycles=g_TmgCycHDMATable;
    memcpy(f.ppuWrites,g_AuroraC4PpuWritesFrame,sizeof(f.ppuWrites));
    for(Int32 ch=0;ch<C4_HDMA_CHANNELS;ch++)
    {
        f.hdmaChActive[ch]=g_AuroraC4HdmaActiveFrame[ch];
        f.hdmaChTransfers[ch]=g_AuroraC4HdmaTransferFrame[ch];
        f.hdmaChBytes[ch]=g_AuroraC4HdmaBytesFrame[ch];
        f.hdmaChIndirectTransfers[ch]=g_AuroraC4HdmaIndirectFrame[ch];
        f.hdmaChReverseTransfers[ch]=g_AuroraC4HdmaReverseFrame[ch];
        f.hdmaChLastDMAP[ch]=g_AuroraC4HdmaLastDMAPFrame[ch];
        f.hdmaChLastBBAD[ch]=g_AuroraC4HdmaLastBBADFrame[ch];
        for(Int32 mode=0;mode<C4_HDMA_MODES;mode++)
            f.hdmaChModeTransfers[ch][mode]=g_AuroraC4HdmaModeFrame[ch][mode];
    }
    FillDelta(f);s_coreOpen=FALSE;
}
void AuroraC4ProfilerHostTickBegin(Bool gameplay)
{
    if(!g_AuroraC4ProfilerActive||!gameplay){s_tickOpen=s_coreOpen=s_coreSpanOpen=s_renderSpanOpen=FALSE;return;}
    memset(&s_tick,0,sizeof(s_tick));s_tickStart=ProfCtrGetCycle();s_tickOpen=TRUE;s_coreSpanEnd=s_renderSpanEnd=0;
    AuroraC4AudioDiagReset();AuroraC4GsDiagReset();
    if(!s_refreshNum||!s_refreshDen){Uint32 n=60,d=1;GSK_GetRefreshRate(&n,&d);if(n&&d){s_refreshNum=n;s_refreshDen=d;s_hostBudgetCycles=(Uint32)(((Uint64)147456000u*d+n/2u)/n);}}
}
void AuroraC4ProfilerHostCoreBegin(void){if(!s_tickOpen)return;Uint32 n=ProfCtrGetCycle();s_tick.preCoreCycles=n-s_tickStart;s_coreSpanStart=n;s_coreSpanOpen=TRUE;}
void AuroraC4ProfilerHostCoreEnd(void){if(!s_tickOpen||!s_coreSpanOpen)return;s_coreSpanEnd=ProfCtrGetCycle();s_tick.coreWrapperCycles=s_coreSpanEnd-s_coreSpanStart;s_coreSpanOpen=FALSE;}
void AuroraC4ProfilerHostRenderBegin(void){if(!s_tickOpen)return;Uint32 n=ProfCtrGetCycle();if(s_coreSpanEnd)s_tick.betweenCoreRenderCycles=n-s_coreSpanEnd;s_renderSpanStart=n;s_renderSpanOpen=TRUE;}
void AuroraC4ProfilerHostRenderEnd(void){if(!s_tickOpen||!s_renderSpanOpen)return;s_renderSpanEnd=ProfCtrGetCycle();s_tick.renderCycles=s_renderSpanEnd-s_renderSpanStart;s_renderSpanOpen=FALSE;}
void AuroraC4ProfilerHostTickEnd(void)
{
    if(!s_tickOpen)
        return;

    Uint32 n=ProfCtrGetCycle();
    s_tick.hostTotalCycles=n-s_tickStart;
    if(s_renderSpanEnd)
        s_tick.postRenderCycles=n-s_renderSpanEnd;
    s_tick.hostWorkCycles=s_tick.hostTotalCycles>s_tick.vblankCycles
        ? s_tick.hostTotalCycles-s_tick.vblankCycles : 0u;
    s_tick.renderWorkCycles=s_tick.renderCycles>s_tick.vblankCycles
        ? s_tick.renderCycles-s_tick.vblankCycles : 0u;
    AuroraC4GsDiagRead(&s_tick.gs);
    AuroraC4AudioDiagRead(&s_tick.audio);
    if(s_tick.core.coreCycles)
    {
        for(Uint32 k=0;k<AURORA_C4_CHR_KERNEL_COUNT;k++)
        {
            s_chrLive[k].calls+=s_chrFrame[k].calls;
            s_chrLive[k].tiles+=s_chrFrame[k].tiles;
            s_chrLive[k].cycles+=s_chrFrame[k].cycles;
        }
        for(Uint32 k=0;k<4;k++)
        {
            s_chr4MixLive.flip[k]+=s_chr4MixFrame.flip[k];
            s_chr4MixLive.transparent[k]+=s_chr4MixFrame.transparent[k];
        }
        Accumulate(s_tick);
    }
    s_tickOpen=s_coreOpen=s_coreSpanOpen=s_renderSpanOpen=FALSE;
}
void AuroraC4ProfilerGpFlush(Uint32 c){if(s_tickOpen)s_tick.gpFlushCycles+=c;}
void AuroraC4ProfilerVBlank(Uint32 c){if(s_tickOpen)s_tick.vblankCycles+=c;}
void AuroraC4ProfilerPostAudio(Uint32 c,Bool skipped){if(!s_tickOpen)return;if(skipped)s_tick.skipAudioCycles+=c;else s_tick.postAudioCycles+=c;}
void AuroraC4ProfilerFrameskipDecision(Bool enabled,Bool allowed,Bool skipped)
{
    if(!s_tickOpen)
        return;
    s_tick.frameskipEnabled=enabled;
    s_tick.frameskipAllowed=allowed;
    s_tick.frameskipSkipped=skipped;
    ++s_live.fsDecisions;
    if(enabled) ++s_live.fsEnabledDecisions;
    if(allowed) ++s_live.fsAllowedDecisions;
    if(skipped)
    {
        ++s_live.fsSkipped;
        ++s_live.fsCurrentStreak;
        if(s_live.fsCurrentStreak>s_live.fsMaxStreak)
            s_live.fsMaxStreak=s_live.fsCurrentStreak;
    }
    else
    {
        s_live.fsCurrentStreak=0;
    }
}
void AuroraC4ProfilerPpuLine(Uint8 mode,Bool mosaic,Bool windowed,Bool colorMath,Bool hires56,Bool pseudo)
{
    if(!s_coreOpen)
        return;
    CoreFrameT &f=s_tick.core;
    ++f.modeLines[mode&7u];
    if(mosaic) ++f.mosaicLines;
    if(windowed) ++f.windowLines;
    if(colorMath) ++f.colorMathLines;
    if(hires56) ++f.hiresMode56Lines;
    if(pseudo) ++f.pseudoHiresLines;
}
void AuroraC4ProfilerObjLine(Uint32 selected,Uint32 potential,Uint32 fetched)
{
    if(!s_coreOpen)
        return;
    CoreFrameT &f=s_tick.core;
    f.objPotentialSlivers+=potential;
    if(selected>f.objMaxSelectedLine) f.objMaxSelectedLine=selected;
    if(potential>f.objMaxPotentialLine) f.objMaxPotentialLine=potential;
    if(fetched>f.objMaxFetchedLine) f.objMaxFetchedLine=fetched;
}
void AuroraC4ProfilerHirq(Int32 line){if(!s_coreOpen)return;CoreFrameT&f=s_tick.core;++f.hirqCount;if(line<f.hirqMinLine)f.hirqMinLine=line;if(line>f.hirqMaxLine)f.hirqMaxLine=line;}

void AuroraC4ProfilerSyncPpu(Uint32 reason,Uint32 cycles)
{
    if(!s_coreOpen||reason>=AURORA_C4_SYNC_REASON_COUNT)return;
    ++s_tick.core.syncReasonCalls[reason];
    s_tick.core.syncReasonCycles[reason]+=cycles;
}
void AuroraC4ProfilerRasterCatchup(Uint32 cycles,Bool hdmaRan)
{
    if(!s_coreOpen)return;
    ++s_tick.core.rasterCatchupCalls;
    s_tick.core.rasterCatchupCycles+=cycles;
    if(hdmaRan)++s_tick.core.rasterCatchupHdmaRuns;
}
void AuroraC4ProfilerChrKernel(Uint32 kind,Uint32 cycles,Uint32 tiles)
{
    if(!s_coreOpen||kind>=AURORA_C4_CHR_KERNEL_COUNT)
        return;
    ChrKernelStatT &v=s_chrFrame[kind];
    ++v.calls;
    v.tiles+=tiles;
    v.cycles+=cycles;
}

void AuroraC4ProfilerChr4Mix(
    Uint32 flip0,Uint32 flipH,Uint32 flipV,Uint32 flipHV,
    Uint32 trans0,Uint32 transH,Uint32 transV,Uint32 transHV)
{
    if(!s_coreOpen)
        return;
    const Uint32 f[4]={flip0,flipH,flipV,flipHV};
    const Uint32 t[4]={trans0,transH,transV,transHV};
    for(Uint32 k=0;k<4;k++)
    {
        s_chr4MixFrame.flip[k]+=f[k];
        s_chr4MixFrame.transparent[k]+=t[k];
    }
}

/* AURORA_C4_CHR4_DATA_MIX_DIAG_V8_3_MIX1_20261004: scan/recording is after the raw CHR4 timer. */
/* AURORA_C4_CHR_SPLIT_PROFILER_V8_3_20261004: recorder above intentionally runs after the raw timer closes. */
void AuroraC4ProfilerMenuOpen(void)
{
    if(!g_AuroraC4ProfilerActive||s_writePending||!s_live.frames){s_tickOpen=s_coreOpen=FALSE;return;}
    s_tickOpen=s_coreOpen=s_coreSpanOpen=s_renderSpanOpen=FALSE;memset(&s_pending,0,sizeof(s_pending));s_pending.stats=s_live;
    memcpy(s_chrPending,s_chrLive,sizeof(s_chrPending));
    memset(s_chrLive,0,sizeof(s_chrLive));
    s_chr4MixPending=s_chr4MixLive;
    memset(&s_chr4MixLive,0,sizeof(s_chr4MixLive));
    s_pending.stamp=CaptureStamp();s_pending.crc32=s_crc32;s_pending.serial=++s_serial;s_pending.clockPairMin=s_clockPairMin;
    s_pending.refreshNum=s_refreshNum;s_pending.refreshDen=s_refreshDen;s_pending.hostBudgetCycles=s_hostBudgetCycles;
    strncpy(s_pending.title,s_title,sizeof(s_pending.title)-1u);ResetLive();s_writePending=TRUE;s_writeDelay=C4_MENU_WRITE_DELAY;
    MainLoopStatusPrintf(90,"C4 profiler: snapshot captured.");
}
void AuroraC4ProfilerMenuUpdate(void){if(!s_writePending)return;if(s_writeDelay>0){--s_writeDelay;return;}(void)SavePending();s_writePending=FALSE;}
void AuroraC4ProfilerCancelPending(void){s_writePending=FALSE;s_writeDelay=0;}
} /* extern C */
#endif /* AURORA_C4_PROFILER */
