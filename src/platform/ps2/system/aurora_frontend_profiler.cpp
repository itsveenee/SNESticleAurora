/* AURORA_FRONTEND_PROFILER_V4_6_FS_RECOVERY_AUDIO_20261004
 *
 * All-core Aurora/frontend diagnostic profiler.
 * This TU exists only when AURORA_FRONTEND_PROFILER=1.
 *
 * v4.4 fixes v3 attribution:
 * - InputPoll, snapshot/prepare, and frontend input logic are distinct;
 * - audio drain is actually instrumented;
 * - process work around the core and post-render work are separated;
 * - frontend spikes are reported separately instead of poisoning the
 *   continuous-cost baseline.
 */
#include "platform/ps2/system/aurora_snes_cost_profiler.h" /* AURORA_SNES_DEEP_FP_COST_INCLUDE_V3_14_6_20261006 */
#include "aurora_frontend_profiler.h"
#include "aurora_external_profiler.h" /* AURORA_MULTICORE_DEEP_PROFILER_V3_48_20261008 */

#if AURORA_FRONTEND_PROFILER

#include <stdio.h>
#include <string.h>
#include <libcdvd.h>
#include <osd_config.h>

#include "prof.h"
#include "mainloop_bgm.h"
#include "mainloop_ui.h"
#include "mainloop_shared.h" /* AURORA_PROFILER_ROM_IDENTITY_V3_1_20261005 */

namespace
{

/* Same NTSC host-frame budget used by the C4 diagnostics.
 * Report classification only: this mutates no timing or scheduler state. */
static const Uint32 AURORA_FP_FRONTEND_SPIKE_BUDGET = 2460058u;

enum SafeFrameskipClassE
{
    AURORA_FP_FS_UNKNOWN = 0,
    AURORA_FP_FS_OFF,
    AURORA_FP_FS_ON_BLOCKED,
    AURORA_FP_FS_ON_PRESENTED,
    AURORA_FP_FS_ON_SKIPPED,
    AURORA_FP_FS_CLASS_COUNT
};

enum SpanE
{
    SPAN_INPUT_POLL = 0,
    SPAN_INPUT_SNAPSHOT,
    SPAN_INPUT_LOGIC,
    SPAN_PROCESS_PRE_CORE,
    SPAN_CORE,
    SPAN_PROCESS_POST_CORE,
    SPAN_UPLOAD,
    SPAN_RENDER,
    SPAN_RENDER_SETUP,
    SPAN_GAME_DRAW,
    SPAN_UI,
    SPAN_GP_FLUSH,
    SPAN_VBLANK,
    SPAN_AUDIO_DRAIN,
    SPAN_AUDIO_CONVERT,
    SPAN_AUDIO_FLUSH,
    /* AURORA_SNES_CORE_PROFILER_V3_6_20261005: nested drill-down spans. */
    SPAN_SNES_MDMA,
    SPAN_SNES_HDMA_SETUP,
    SPAN_SNES_HDMA_DATA,
    SPAN_SNES_SPC,
    SPAN_SNES_DSP_TOTAL,
    SPAN_SNES_DSP_SYNC,
    SPAN_SNES_DSP_NOISE,
    SPAN_SNES_DSP_VOICES,
    /* AURORA_SNES_DSP_VOICES_PROFILER_V3_5_1_20261005 */
    SPAN_SNES_DSP_ENVELOPE,
    SPAN_SNES_DSP_SAMPLE_NORMAL,
    SPAN_SNES_DSP_SAMPLE_PMON,
    SPAN_SNES_DSP_BRR_DECODE,
    SPAN_SNES_DSP_PITCHMOD_FEEDER,
    SPAN_SNES_DSP_MIX_DRY,
    SPAN_SNES_DSP_MIX_ECHO,
    SPAN_SNES_DSP_ECHO,
    SPAN_SNES_DSP_FINAL,
    SPAN_POST_RENDER,
    SPAN_COUNT
};

struct FrameT
{
    Uint32 start;
    Uint32 state;
    Uint32 core;
    Uint32 cycles[SPAN_COUNT];
    Uint32 spanStart[SPAN_COUNT];
    Bool spanOpen[SPAN_COUNT];
    Bool fsSeen;
    Bool fsEnabled;
    Bool fsAllowed;
    Bool fsSkipped;

    /* AURORA_V45_VBLANK_CADENCE_PROFILER_ONLY_20261011: per-host-tick observational timestamps only. */
    Uint32 v45PreFlipWork;
    Uint32 v45FlipStamp;
    Bool v45Presented;

    Bool fsDetailSeen;
    Uint32 fsPeriod;
    Int32 fsDiff;
    Uint32 fsLastPresentedWork;
    Bool fsMeaningfulDebt;
    Bool fsMeasuredOverrun;
    Bool fsUnlimited;
    Bool fsDecisionSkipped;
    Bool fsRecoveryPendingAtDecision;

    Bool fsRecoverySeen;
    Uint32 fsRecoveryWork;
    Bool fsRecoveryPendingAtPresent;
    Bool fsRecoveryRebased;

    Uint32 audioConvertInputFrames;
    Uint32 audioFlushFrames;
    Uint32 audioDrainFrames;
    Uint32 spcCalls;
    Uint32 spcRequestedCycles;
    Uint32 spcConsumedCycles;
    Uint32 dspMixCalls;
    Uint32 dspChunks;
    Uint32 dspSamples;

    /* AURORA_SNES_CORE_PROFILER_V3_6_20261005 */
    Uint32 snesCpuBudgetCalls;
    Uint64 snesCpuBudgetCycles;
    Uint32 snesPpuSyncCalls;
    Uint32 snesCostCycles[5];
    Int32 snesCostDepth;
    Int32 snesCostStackBucket[16];
    Uint32 snesCostStackStart[16];
    Uint32 snesPpuCycles[18];
    Int32 snesPpuDepth;
    Int32 snesPpuStackBucket[16];
    Uint32 snesPpuStackStart[16];
    Bool snesProfileInvalid;
    Uint32 snesPpuRenderedLines;
    Uint32 snesPpuModeLines[8];
    Uint32 snesPpuBgMapFetches;
    Uint32 snesPpuBgChrDecodes;
    Uint32 snesPpuMainBgLayers;
    Uint32 snesPpuSubBgLayers;
    Uint32 snesPpuObjOamRefs;
    Uint32 snesPpuObjTiles;
    Uint32 snesPpuObjEnabledLines;
    Uint32 snesPpuObjRangeOverLines;
    Uint32 snesPpuObjTimeOverLines;
    Uint32 snesPpuWindowLines;
    Uint32 snesPpuColorMathLines;
    Uint32 snesPpuSubscreenLines;
    Uint32 snesPpuHiresLines;
    Uint32 snesPpuDirectColorLines;

    /* AURORA_SNES_BGCHR_PROFILER_V3_7_20261005
     * [BG0..3][depth: 2,4,8,other]. */
    Uint32 snesBgChrCalls[4][4];
    Uint32 snesBgChrTiles[4][4];
    Uint32 snesBgChrCycles[4][4];
    Uint32 snesBgChrDecodeCycles[4][4];
    /* flags: offset, mosaic, fine-X != 0, hires subscreen. Overlapping. */
    Uint32 snesBgChrFlagCalls[4];
    Uint32 snesBgChrFlagTiles[4];
    Uint32 snesBgChrFlagCycles[4];
    Uint32 snesBgChrFlagDecodeCycles[4];
    Uint32 snesBgChrMeasuredCalls;
    Uint32 snesBgChrMeasuredTiles;
    Uint32 snesBgChrMeasuredCycles;
    Uint32 snesBgChrMeasuredDecodeCycles;
    Uint32 snesBgChrBadRecords;
    Uint32 snesBgChrTraitsCycles; /* AURORA_V23_SNES_BGCHR_TRAITS_COST_20261009 */

    /* AURORA_SNES_DEEP_OPT_V3_14_20261005 */
    Uint32 snesDeepCalls[69];
    Uint32 snesDeepUnits[69];
    Uint32 snesDeepCycles[69];

    /* AURORA_MULTICORE_DEEP_PROFILER_V3_48_20261008 */
    Uint32 externalCalls[AURORA_EXT_BUCKET_MAX];
    Uint32 externalUnits[AURORA_EXT_BUCKET_MAX];
    Uint32 externalCycles[AURORA_EXT_BUCKET_MAX];

    /* AURORA_SNES_MICROSCOPE_V3_27_20261006 */
    Uint32 snesChr4LocalityCalls;
    Uint32 snesChr4LocalityTiles;
    Uint32 snesChr4OpaqueTiles;
    Uint32 snesChr4PaletteReuses;
    Uint32 snesChr4PaletteRuns;
    Uint32 snesChr4FlipSame;
    Uint32 snesChr4FlipChanges;
    Uint32 snesChr4RowSame;
    Uint32 snesChr4RowPlus16;
    Uint32 snesChr4RowMinus16;
    Uint32 snesChr4RowOther;

    Uint32 snesObjMicroscopeCalls;
    Uint32 snesObjHitTiles;
    Uint32 snesObjHitCycles;
    Uint32 snesObjMissTiles;
    Uint32 snesObjMissCycles;
    Uint32 snesObjHitTransparent;
    Uint32 snesObjHitFullOpaque;
    Uint32 snesObjHitPartial;
    Uint32 snesObjMicMissTransparent;
    Uint32 snesObjMicMissFullOpaque;
    Uint32 snesObjMicMissPartial;
    Uint32 snesObjRowSame;
    Uint32 snesObjRowPlus16;
    Uint32 snesObjRowMinus16;
    Uint32 snesObjRowOther;

    /* AURORA_CHR4_REUSE_MICROSCOPE_V3_28_20261007 */
    Uint32 snesChr4OpaqueTransitions;
    Uint32 snesChr4AdjacentOpaqueTransitions;
    Uint32 snesChr4PaletteIndexReuses;
    Uint32 snesChr4DecodeKeyReuses;
    Uint32 snesChr4RowSameFlipChanged;
    Uint32 snesChr4PixelKeyReuses;
    Uint32 snesChr4AdjacentPaletteIndexReuses;
    Uint32 snesChr4AdjacentDecodeKeyReuses;
    Uint32 snesChr4AdjacentPixelKeyReuses;

    /* AURORA_CHR4_PATH_BG_MICROSCOPE_V3_29_20261007
     * [BG0..3][NOOFFSET,OFFSET,M5_MAIN,M5_SUB,M6_MAIN,M6_SUB]. */
    Uint32 snesChr4CellCalls[4][6];
    Uint32 snesChr4CellTiles[4][6];
    Uint32 snesChr4CellOpaqueTiles[4][6];
    Uint32 snesChr4CellOpaqueTransitions[4][6];
    Uint32 snesChr4CellAdjacentOpaqueTransitions[4][6];
    Uint32 snesChr4CellPaletteIndexReuses[4][6];
    Uint32 snesChr4CellDecodeKeyReuses[4][6];
    Uint32 snesChr4CellRowSameFlipChanged[4][6];
    Uint32 snesChr4CellPixelKeyReuses[4][6];
    Uint32 snesChr4CellAdjacentPaletteIndexReuses[4][6];
    Uint32 snesChr4CellAdjacentDecodeKeyReuses[4][6];
    Uint32 snesChr4CellAdjacentPixelKeyReuses[4][6];

    /* AURORA_SNES_DSP_VOICES_PROFILER_V3_5_1_20261005 */
    Uint32 dspVoiceEnvelopeCalls;
    Uint32 dspVoiceEnvelopeActive;
    Uint32 dspVoiceSampleNormalCalls;
    Uint32 dspVoiceSamplePmonCalls;
    Uint32 dspVoiceBrrDecodes;
    Uint32 dspVoicePitchmodFeeders;
    Uint32 dspVoiceMixCalls;
    Uint32 dspVoiceMixDryCalls;
    Uint32 dspVoiceMixEchoCalls;
    Uint32 dspVoiceNoiseMixes;
    Uint32 dspVoiceSilentFastpathHits;

    Uint64 dspVoiceSampleNormalSamples;
    Uint64 dspVoiceSamplePmonSamples;
    Uint64 dspVoiceMixSamples;
    Uint64 dspVoiceMixDrySamples;
    Uint64 dspVoiceMixEchoSamples;

    Uint32 dspVoiceGainZeroVolume;
    Uint32 dspVoiceGainEnvelopeZero;
    Uint32 dspVoiceGainVeryLow;
    Uint32 dspVoiceGainLow;
    Uint32 dspVoiceGainNormal;
    Uint32 dspVoiceLowGainPmon;
    Uint32 dspVoiceLowGainEcho;
    Uint32 dspVoiceLowGainFeedsPmon;

    Bool open;
};

struct DerivedT
{
    Uint32 frontend;
    Uint32 renderNoVblank;
    Uint32 renderNoVblankAudio;
    Uint32 otherFrontend;
};

struct BucketT
{
    Uint64 frames;
    Uint64 tick;
    Uint64 span[SPAN_COUNT];
    Uint64 frontendExCoreVblank;
    Uint64 renderExVblank;
    Uint64 renderExVblankAudio;
    Uint64 otherFrontend;

    Uint64 audioConvertInputFrames;
    Uint64 audioFlushFrames;
    Uint64 audioDrainFrames;
    Uint64 spcCalls;
    Uint64 spcRequestedCycles;
    Uint64 spcConsumedCycles;
    Uint64 dspMixCalls;
    Uint64 dspChunks;
    Uint64 dspSamples;

    /* AURORA_SNES_CORE_PROFILER_V3_6_20261005 */
    Uint64 snesCpuBudgetCalls;
    Uint64 snesCpuBudgetCycles;
    Uint64 snesPpuSyncCalls;
    Uint64 snesCostCycles[5];
    Uint64 snesPpuCycles[18];
    Uint64 snesProfileInvalidFrames;
    Uint64 snesPpuRenderedLines;
    Uint64 snesPpuModeLines[8];
    Uint64 snesPpuBgMapFetches;
    Uint64 snesPpuBgChrDecodes;
    Uint64 snesPpuMainBgLayers;
    Uint64 snesPpuSubBgLayers;
    Uint64 snesPpuObjOamRefs;
    Uint64 snesPpuObjTiles;
    Uint64 snesPpuObjEnabledLines;
    Uint64 snesPpuObjRangeOverLines;
    Uint64 snesPpuObjTimeOverLines;
    Uint64 snesPpuWindowLines;
    Uint64 snesPpuColorMathLines;
    Uint64 snesPpuSubscreenLines;
    Uint64 snesPpuHiresLines;
    Uint64 snesPpuDirectColorLines;

    /* AURORA_SNES_BGCHR_PROFILER_V3_7_20261005 */
    Uint64 snesBgChrCalls[4][4];
    Uint64 snesBgChrTiles[4][4];
    Uint64 snesBgChrCycles[4][4];
    Uint64 snesBgChrDecodeCycles[4][4];
    Uint64 snesBgChrFlagCalls[4];
    Uint64 snesBgChrFlagTiles[4];
    Uint64 snesBgChrFlagCycles[4];
    Uint64 snesBgChrFlagDecodeCycles[4];
    Uint64 snesBgChrMeasuredCalls;
    Uint64 snesBgChrMeasuredTiles;
    Uint64 snesBgChrMeasuredCycles;
    Uint64 snesBgChrMeasuredDecodeCycles;
    Uint64 snesBgChrBadRecords;
    Uint64 snesBgChrTraitsCycles; /* AURORA_V23_SNES_BGCHR_TRAITS_COST_20261009 */

    /* AURORA_SNES_DEEP_OPT_V3_14_20261005 */
    Uint64 snesDeepCalls[69];
    Uint64 snesDeepUnits[69];
    Uint64 snesDeepCycles[69];

    /* AURORA_MULTICORE_DEEP_PROFILER_V3_48_20261008 */
    Uint64 externalCalls[AURORA_EXT_BUCKET_MAX];
    Uint64 externalUnits[AURORA_EXT_BUCKET_MAX];
    Uint64 externalCycles[AURORA_EXT_BUCKET_MAX];

    /* AURORA_SNES_MICROSCOPE_V3_27_20261006 */
    Uint64 snesChr4LocalityCalls;
    Uint64 snesChr4LocalityTiles;
    Uint64 snesChr4OpaqueTiles;
    Uint64 snesChr4PaletteReuses;
    Uint64 snesChr4PaletteRuns;
    Uint64 snesChr4FlipSame;
    Uint64 snesChr4FlipChanges;
    Uint64 snesChr4RowSame;
    Uint64 snesChr4RowPlus16;
    Uint64 snesChr4RowMinus16;
    Uint64 snesChr4RowOther;

    Uint64 snesObjMicroscopeCalls;
    Uint64 snesObjHitTiles;
    Uint64 snesObjHitCycles;
    Uint64 snesObjMissTiles;
    Uint64 snesObjMissCycles;
    Uint64 snesObjHitTransparent;
    Uint64 snesObjHitFullOpaque;
    Uint64 snesObjHitPartial;
    Uint64 snesObjMicMissTransparent;
    Uint64 snesObjMicMissFullOpaque;
    Uint64 snesObjMicMissPartial;
    Uint64 snesObjRowSame;
    Uint64 snesObjRowPlus16;
    Uint64 snesObjRowMinus16;
    Uint64 snesObjRowOther;

    /* AURORA_CHR4_REUSE_MICROSCOPE_V3_28_20261007 */
    Uint64 snesChr4OpaqueTransitions;
    Uint64 snesChr4AdjacentOpaqueTransitions;
    Uint64 snesChr4PaletteIndexReuses;
    Uint64 snesChr4DecodeKeyReuses;
    Uint64 snesChr4RowSameFlipChanged;
    Uint64 snesChr4PixelKeyReuses;
    Uint64 snesChr4AdjacentPaletteIndexReuses;
    Uint64 snesChr4AdjacentDecodeKeyReuses;
    Uint64 snesChr4AdjacentPixelKeyReuses;

    /* AURORA_CHR4_PATH_BG_MICROSCOPE_V3_29_20261007
     * [BG0..3][NOOFFSET,OFFSET,M5_MAIN,M5_SUB,M6_MAIN,M6_SUB]. */
    Uint64 snesChr4CellCalls[4][6];
    Uint64 snesChr4CellTiles[4][6];
    Uint64 snesChr4CellOpaqueTiles[4][6];
    Uint64 snesChr4CellOpaqueTransitions[4][6];
    Uint64 snesChr4CellAdjacentOpaqueTransitions[4][6];
    Uint64 snesChr4CellPaletteIndexReuses[4][6];
    Uint64 snesChr4CellDecodeKeyReuses[4][6];
    Uint64 snesChr4CellRowSameFlipChanged[4][6];
    Uint64 snesChr4CellPixelKeyReuses[4][6];
    Uint64 snesChr4CellAdjacentPaletteIndexReuses[4][6];
    Uint64 snesChr4CellAdjacentDecodeKeyReuses[4][6];
    Uint64 snesChr4CellAdjacentPixelKeyReuses[4][6];

    Uint64 dspVoiceEnvelopeCalls;
    Uint64 dspVoiceEnvelopeActive;
    Uint64 dspVoiceSampleNormalCalls;
    Uint64 dspVoiceSamplePmonCalls;
    Uint64 dspVoiceBrrDecodes;
    Uint64 dspVoicePitchmodFeeders;
    Uint64 dspVoiceMixCalls;
    Uint64 dspVoiceMixDryCalls;
    Uint64 dspVoiceMixEchoCalls;
    Uint64 dspVoiceNoiseMixes;
    Uint64 dspVoiceSilentFastpathHits;

    Uint64 dspVoiceSampleNormalSamples;
    Uint64 dspVoiceSamplePmonSamples;
    Uint64 dspVoiceMixSamples;
    Uint64 dspVoiceMixDrySamples;
    Uint64 dspVoiceMixEchoSamples;

    Uint64 dspVoiceGainZeroVolume;
    Uint64 dspVoiceGainEnvelopeZero;
    Uint64 dspVoiceGainVeryLow;
    Uint64 dspVoiceGainLow;
    Uint64 dspVoiceGainNormal;
    Uint64 dspVoiceLowGainPmon;
    Uint64 dspVoiceLowGainEcho;
    Uint64 dspVoiceLowGainFeedsPmon;
};

struct WorstT
{
    Uint32 serial;
    Uint32 state;
    Uint32 core;
    Uint32 tick;
    Uint32 frontend;
    Uint32 coreCycles;
    Uint32 vblank;
    Uint32 inputPoll;
    Uint32 inputSnapshot;
    Uint32 inputLogic;
    Uint32 preCore;
    Uint32 postCore;
    Uint32 upload;
    Uint32 renderHost;
    Uint32 audioDrain;
    Uint32 postRender;
    Uint32 other;
    Uint32 fsClass;
    Bool spike;
};

struct FsDiagT
{
    Uint64 decisions;
    Uint64 debtDecisions;
    Uint64 overrunDecisions;
    Uint64 unlimitedDecisions;
    Uint64 skippedDecisions;
    Uint64 recoveryPendingDecisions;
    Uint64 recoveryPresented;
    Uint64 recoveryRebases;
    Uint64 periodSum;
    Uint64 latenessSum;
    Uint64 latenessSamples;
    Uint64 lastPresentedWorkSum;
    Uint64 lastPresentedWorkSamples;
    Uint64 recoveryWorkSum;
};

/* AURORA_V45_VBLANK_CADENCE_PROFILER_ONLY_20261011
 * All counters live in the existing frontend profiler TU.  No GS IRQ,
 * scheduling decisions or presentation changes. */
struct V45CadenceT
{
    Uint64 presented, skipped;
    Uint64 periodSamples, periodSum, preWorkSum, waitSum;
    Uint64 preWorkOverPeriod, preWorkOver2Periods;
    Uint64 intervalPairs, gapSum, expectedGapSum, excessGapSum;
    Uint64 gapExcessHalfPeriod, pairsAfterSkips, skipsBetweenSum;
    Uint64 gapHistogram[5]; /* <1P, [1,2)P, [2,3)P, [3,4)P, >=4P */
    Uint32 lastPresentStamp, lastPresentPeriod, skipsSincePresent;
    Bool haveLastPresent;
};

struct StatsT
{
    V45CadenceT v45[AURORA_FP_CORE_COUNT];
    BucketT stateAll[AURORA_FP_STATE_COUNT];
    BucketT stateNormal[AURORA_FP_STATE_COUNT];
    BucketT stateSpike[AURORA_FP_STATE_COUNT];

    BucketT coreAll[AURORA_FP_CORE_COUNT];
    BucketT coreNormal[AURORA_FP_CORE_COUNT];
    BucketT coreSpike[AURORA_FP_CORE_COUNT];

    BucketT fsAll[AURORA_FP_CORE_COUNT][AURORA_FP_FS_CLASS_COUNT];
    BucketT fsNormal[AURORA_FP_CORE_COUNT][AURORA_FP_FS_CLASS_COUNT];
    BucketT fsSpike[AURORA_FP_CORE_COUNT][AURORA_FP_FS_CLASS_COUNT];
    FsDiagT fsDiag[AURORA_FP_CORE_COUNT];

    WorstT worst[16];

    Uint64 totalFrames;
    Uint64 normalFrames;
    Uint64 spikeFrames;
    Uint64 gameplayFrames;
    Uint64 menuFrames;
    Uint64 idleFrames;
};

struct StampT
{
    Uint32 year, month, day, hour, minute, second;
};

/* AURORA_PROFILER_ROM_IDENTITY_V3_1_20261005
 * Snapshot metadata is captured before live stats are reset, while the
 * current game object is still loaded. It is diagnostic-only. */
struct MetadataT
{
    Char gameTitle[256];
    Char romFilename[256];
    Char region[32];
    Uint32 core;
    Uint32 runtimeCRC32;
    Bool runtimeCRCValid;
};

struct SnapshotT
{
    StatsT stats;
    StampT stamp;
    MetadataT meta;
    Uint32 serial;
};

static FrameT s_frame;
static StatsT s_live;
static SnapshotT s_pending;
static Uint32 s_hostSerial = 0;
static Uint32 s_dumpSerial = 0;
static Bool s_captureArmed = FALSE;
static Uint32 s_menuFramesNeeded = 0;
static Bool s_writePending = FALSE;
static Bool s_initialIdleDumpDone = FALSE;

static Uint32 Bcd(Uint8 v)
{
    return (((v >> 4) & 15u) * 10u) + (v & 15u);
}

static StampT CaptureStamp(void)
{
    StampT s;
    memset(&s, 0, sizeof(s));
    sceCdCLOCK rtc;
    if (!sceCdReadClock(&rtc))
        return s;
    configConvertToLocalTime(&rtc);
    s.year = 2000u + Bcd(rtc.year);
    s.month = Bcd(rtc.month);
    s.day = Bcd(rtc.day);
    s.hour = Bcd(rtc.hour);
    s.minute = Bcd(rtc.minute);
    s.second = Bcd(rtc.second);
    return s;
}

static Uint64 Avg(Uint64 total, Uint64 n)
{
    return n ? ((total + n / 2u) / n) : 0u;
}

/* AURORA_SNES_CORE_PROFILER_V3_6_20261005 */
static Uint64 SnesScaledDiv(Uint64 n, Uint64 d, Uint64 scale)
{
    return d ? ((n * scale + d / 2u) / d) : 0u;
}

static Uint32 SnesBgBits(Uint32 mask)
{
    mask &= 0x0Fu;
    return ((mask & 1u) ? 1u : 0u) +
           ((mask & 2u) ? 1u : 0u) +
           ((mask & 4u) ? 1u : 0u) +
           ((mask & 8u) ? 1u : 0u);
}

static Uint32 SubSat(Uint32 a, Uint32 b)
{
    return a > b ? a - b : 0u;
}

static const char *StateName(Uint32 s)
{
    switch (s)
    {
        case AURORA_FP_GAMEPLAY: return "GAMEPLAY";
        case AURORA_FP_MENU_WITH_CORE: return "MENU_WITH_CORE";
        case AURORA_FP_IDLE_UI: return "IDLE_UI";
        default: return "UNKNOWN";
    }
}

static const char *CoreName(Uint32 c)
{
    switch (c)
    {
        case AURORA_FP_CORE_SNES: return "SNES";
        case AURORA_FP_CORE_NES: return "NES";
        case AURORA_FP_CORE_FDS: return "FDS";
        case AURORA_FP_CORE_SEGA: return "SEGA";
        case AURORA_FP_CORE_PCE: return "PCE";
        case AURORA_FP_CORE_GB: return "GB";
        case AURORA_FP_CORE_GBA: return "GBA";
        default: return "NONE";
    }
}

/* AURORA_PROFILER_ROM_IDENTITY_V3_1_20261005 */
static void MetadataCopy(Char *dst, Uint32 cap, const char *src)
{
    if (!dst || !cap)
        return;

    Uint32 out = 0;
    if (src)
    {
        while (*src && out + 1u < cap)
        {
            unsigned char c = (unsigned char)*src++;
            if (c < 0x20u || c == 0x7Fu)
                c = (unsigned char)'?';
            dst[out++] = (Char)c;
        }
    }
    dst[out] = 0;
}

static Uint32 SnapshotCore(const StatsT &s)
{
    Uint32 best = AURORA_FP_CORE_NONE;
    Uint64 bestFrames = 0;
    for (Uint32 core = 1; core < AURORA_FP_CORE_COUNT; ++core)
    {
        if (s.coreAll[core].frames > bestFrames)
        {
            bestFrames = s.coreAll[core].frames;
            best = core;
        }
    }
    return best;
}

static MetadataT CaptureMetadata(const StatsT &s)
{
    MetadataT m;
    memset(&m, 0, sizeof(m));
    m.core = SnapshotCore(s);
    MetadataCopy(m.gameTitle, sizeof(m.gameTitle), "UNKNOWN");
    MetadataCopy(m.romFilename, sizeof(m.romFilename),
                 _RomName[0] ? _RomName : "NONE");
    MetadataCopy(m.region, sizeof(m.region), "UNKNOWN");

    switch (m.core)
    {
        case AURORA_FP_CORE_SNES:
            if (_pSnesRom && _pSnesRom->IsLoaded())
            {
                const char *title = _pSnesRom->GetRomTitle();
                MetadataCopy(m.gameTitle, sizeof(m.gameTitle),
                             (title && *title) ? title :
                             (_RomName[0] ? _RomName : "UNKNOWN"));
                m.runtimeCRC32 = _pSnesRom->GetRuntimeCRC32();
                m.runtimeCRCValid = TRUE;
                MetadataCopy(m.region, sizeof(m.region),
                    _pSnesRom->m_eVideoType == SNROM_VIDEO_PAL
                        ? "PAL" : "NTSC");
            }
            break;

        case AURORA_FP_CORE_NES:
            if (_pNesRom && _pNesRom->IsLoaded())
            {
                const char *title = _pNesRom->GetRomTitle();
                MetadataCopy(m.gameTitle, sizeof(m.gameTitle),
                             (title && *title) ? title :
                             (_RomName[0] ? _RomName : "UNKNOWN"));
            }
            break;

        case AURORA_FP_CORE_FDS:
            if (_pNesFDSDisk && _pNesFDSDisk->IsLoaded())
                MetadataCopy(m.gameTitle, sizeof(m.gameTitle),
                             _pNesFDSDisk->GetRomTitle());
            break;

        case AURORA_FP_CORE_SEGA:
            if (_pSegaRom && _pSegaRom->IsLoaded())
            {
                const char *title = _pSegaRom->GetRomTitle();
                MetadataCopy(m.gameTitle, sizeof(m.gameTitle),
                             (title && *title) ? title :
                             (_RomName[0] ? _RomName : "UNKNOWN"));
            }
            break;

        case AURORA_FP_CORE_PCE:
            if (_pPceRom && _pPceRom->IsLoaded())
            {
                const char *title = _pPceRom->GetRomTitle();
                MetadataCopy(m.gameTitle, sizeof(m.gameTitle),
                             (title && *title) ? title :
                             (_RomName[0] ? _RomName : "UNKNOWN"));
            }
            break;

        case AURORA_FP_CORE_GB:
            MetadataCopy(m.gameTitle, sizeof(m.gameTitle),
                         _RomName[0] ? _RomName : "UNKNOWN");
            if (_pGb && _pGb->IsGameLoaded())
            {
                m.runtimeCRC32 = _pGb->GetGameCRC();
                m.runtimeCRCValid = TRUE;
            }
            break;

        case AURORA_FP_CORE_GBA:
            MetadataCopy(m.gameTitle, sizeof(m.gameTitle),
                         _RomName[0] ? _RomName : "UNKNOWN");
            if (_pGba && _pGba->IsGameLoaded())
            {
                m.runtimeCRC32 = _pGba->GetGameCRC();
                m.runtimeCRCValid = TRUE;
            }
            break;

        default:
            MetadataCopy(m.gameTitle, sizeof(m.gameTitle), "NONE");
            MetadataCopy(m.romFilename, sizeof(m.romFilename), "NONE");
            break;
    }

    return m;
}


static const char *SafeFrameskipClassName(Uint32 c)
{
    switch (c)
    {
        case AURORA_FP_FS_OFF: return "FS_OFF";
        case AURORA_FP_FS_ON_BLOCKED: return "FS_ON_BLOCKED";
        case AURORA_FP_FS_ON_PRESENTED: return "FS_ON_PRESENTED";
        case AURORA_FP_FS_ON_SKIPPED: return "FS_ON_SKIPPED";
        default: return "FS_UNKNOWN";
    }
}

static Uint32 SafeFrameskipClassForFrame(void)
{
    if (!s_frame.fsSeen)
        return AURORA_FP_FS_UNKNOWN;
    if (!s_frame.fsEnabled)
        return AURORA_FP_FS_OFF;
    if (!s_frame.fsAllowed)
        return AURORA_FP_FS_ON_BLOCKED;
    return s_frame.fsSkipped
        ? AURORA_FP_FS_ON_SKIPPED
        : AURORA_FP_FS_ON_PRESENTED;
}

static void ResetLive(void)
{
    memset(&s_live, 0, sizeof(s_live));
}

static void BeginSpan(Uint32 span)
{
    if (!s_frame.open || span >= SPAN_COUNT || s_frame.spanOpen[span])
        return;
    s_frame.spanStart[span] = ProfCtrGetCycle();
    s_frame.spanOpen[span] = TRUE;
}

static void EndSpan(Uint32 span)
{
    if (!s_frame.open || span >= SPAN_COUNT || !s_frame.spanOpen[span])
        return;
    const Uint32 now = ProfCtrGetCycle();
    s_frame.cycles[span] += (Uint32)(now - s_frame.spanStart[span]);
    s_frame.spanOpen[span] = FALSE;
}

static DerivedT Derive(Uint32 tick)
{
    DerivedT d;
    const Uint32 core = s_frame.cycles[SPAN_CORE];
    const Uint32 vb = s_frame.cycles[SPAN_VBLANK];
    const Uint32 render = s_frame.cycles[SPAN_RENDER];
    const Uint32 audio = s_frame.cycles[SPAN_AUDIO_DRAIN];

    d.frontend = SubSat(SubSat(tick, core), vb);
    d.renderNoVblank = SubSat(render, vb);
    d.renderNoVblankAudio = SubSat(d.renderNoVblank, audio);

    /* Top-level exclusive partition. upload is nested in post_core.
     * render_setup/game/UI/GP/audio are nested in render_total. */
    Uint32 other = d.frontend;
    other = SubSat(other, s_frame.cycles[SPAN_INPUT_POLL]);
    other = SubSat(other, s_frame.cycles[SPAN_INPUT_SNAPSHOT]);
    other = SubSat(other, s_frame.cycles[SPAN_INPUT_LOGIC]);
    other = SubSat(other, s_frame.cycles[SPAN_PROCESS_PRE_CORE]);
    other = SubSat(other, s_frame.cycles[SPAN_PROCESS_POST_CORE]);
    other = SubSat(other, d.renderNoVblank);
    other = SubSat(other, s_frame.cycles[SPAN_POST_RENDER]);
    d.otherFrontend = other;
    return d;
}

static void AccumulateBucket(BucketT &b, Uint32 tick, const DerivedT &d)
{
    ++b.frames;
    b.tick += tick;
    for (Uint32 i = 0; i < SPAN_COUNT; ++i)
        b.span[i] += s_frame.cycles[i];

    b.frontendExCoreVblank += d.frontend;
    b.renderExVblank += d.renderNoVblank;
    b.renderExVblankAudio += d.renderNoVblankAudio;
    b.otherFrontend += d.otherFrontend;

    b.audioConvertInputFrames += s_frame.audioConvertInputFrames;
    b.audioFlushFrames += s_frame.audioFlushFrames;
    b.audioDrainFrames += s_frame.audioDrainFrames;
    b.spcCalls += s_frame.spcCalls;
    b.spcRequestedCycles += s_frame.spcRequestedCycles;
    b.spcConsumedCycles += s_frame.spcConsumedCycles;
    b.dspMixCalls += s_frame.dspMixCalls;
    b.dspChunks += s_frame.dspChunks;
    b.dspSamples += s_frame.dspSamples;

    /* The cost/PPU stacks must close exactly at the host-frame boundary.
     * Fail closed for this diagnostic block only; all existing profiler
     * statistics remain valid even if a future callsite is mismatched. */
    if (s_frame.core == AURORA_FP_CORE_SNES)
    {
        if (s_frame.snesProfileInvalid ||
            s_frame.snesCostDepth != 0 || s_frame.snesPpuDepth != 0)
        {
            ++b.snesProfileInvalidFrames;
        }
        else
        {
            b.snesCpuBudgetCalls += s_frame.snesCpuBudgetCalls;
            b.snesCpuBudgetCycles += s_frame.snesCpuBudgetCycles;
            b.snesPpuSyncCalls += s_frame.snesPpuSyncCalls;
            for (Uint32 i = 0; i < 5u; ++i)
                b.snesCostCycles[i] += s_frame.snesCostCycles[i];
            for (Uint32 i = 0; i < 18u; ++i)
                b.snesPpuCycles[i] += s_frame.snesPpuCycles[i];
            b.snesPpuRenderedLines += s_frame.snesPpuRenderedLines;
            for (Uint32 i = 0; i < 8u; ++i)
                b.snesPpuModeLines[i] += s_frame.snesPpuModeLines[i];
            b.snesPpuBgMapFetches += s_frame.snesPpuBgMapFetches;
            b.snesPpuBgChrDecodes += s_frame.snesPpuBgChrDecodes;
            b.snesPpuMainBgLayers += s_frame.snesPpuMainBgLayers;
            b.snesPpuSubBgLayers += s_frame.snesPpuSubBgLayers;
            b.snesPpuObjOamRefs += s_frame.snesPpuObjOamRefs;
            b.snesPpuObjTiles += s_frame.snesPpuObjTiles;
            b.snesPpuObjEnabledLines += s_frame.snesPpuObjEnabledLines;
            b.snesPpuObjRangeOverLines += s_frame.snesPpuObjRangeOverLines;
            b.snesPpuObjTimeOverLines += s_frame.snesPpuObjTimeOverLines;
            b.snesPpuWindowLines += s_frame.snesPpuWindowLines;
            b.snesPpuColorMathLines += s_frame.snesPpuColorMathLines;
            b.snesPpuSubscreenLines += s_frame.snesPpuSubscreenLines;
            b.snesPpuHiresLines += s_frame.snesPpuHiresLines;
            b.snesPpuDirectColorLines += s_frame.snesPpuDirectColorLines;

            for (Uint32 bg = 0; bg < 4u; ++bg)
            {
                for (Uint32 depth = 0; depth < 4u; ++depth)
                {
                    b.snesBgChrCalls[bg][depth] += s_frame.snesBgChrCalls[bg][depth];
                    b.snesBgChrTiles[bg][depth] += s_frame.snesBgChrTiles[bg][depth];
                    b.snesBgChrCycles[bg][depth] += s_frame.snesBgChrCycles[bg][depth];
                    b.snesBgChrDecodeCycles[bg][depth] +=
                        s_frame.snesBgChrDecodeCycles[bg][depth];
                }
            }
            for (Uint32 flag = 0; flag < 4u; ++flag)
            {
                b.snesBgChrFlagCalls[flag] += s_frame.snesBgChrFlagCalls[flag];
                b.snesBgChrFlagTiles[flag] += s_frame.snesBgChrFlagTiles[flag];
                b.snesBgChrFlagCycles[flag] += s_frame.snesBgChrFlagCycles[flag];
                b.snesBgChrFlagDecodeCycles[flag] +=
                    s_frame.snesBgChrFlagDecodeCycles[flag];
            }
            b.snesBgChrMeasuredCalls += s_frame.snesBgChrMeasuredCalls;
            b.snesBgChrMeasuredTiles += s_frame.snesBgChrMeasuredTiles;
            b.snesBgChrMeasuredCycles += s_frame.snesBgChrMeasuredCycles;
            b.snesBgChrMeasuredDecodeCycles +=
                s_frame.snesBgChrMeasuredDecodeCycles;
            b.snesBgChrBadRecords += s_frame.snesBgChrBadRecords;
            b.snesBgChrTraitsCycles += s_frame.snesBgChrTraitsCycles;
            for (Uint32 i = 0; i < 69u; ++i)
            {
                b.snesDeepCalls[i] += s_frame.snesDeepCalls[i];
                b.snesDeepUnits[i] += s_frame.snesDeepUnits[i];
                b.snesDeepCycles[i] += s_frame.snesDeepCycles[i];
            }

            b.snesChr4LocalityCalls += s_frame.snesChr4LocalityCalls;
            b.snesChr4LocalityTiles += s_frame.snesChr4LocalityTiles;
            b.snesChr4OpaqueTiles += s_frame.snesChr4OpaqueTiles;
            b.snesChr4PaletteReuses += s_frame.snesChr4PaletteReuses;
            b.snesChr4PaletteRuns += s_frame.snesChr4PaletteRuns;
            b.snesChr4FlipSame += s_frame.snesChr4FlipSame;
            b.snesChr4FlipChanges += s_frame.snesChr4FlipChanges;
            b.snesChr4RowSame += s_frame.snesChr4RowSame;
            b.snesChr4RowPlus16 += s_frame.snesChr4RowPlus16;
            b.snesChr4RowMinus16 += s_frame.snesChr4RowMinus16;
            b.snesChr4RowOther += s_frame.snesChr4RowOther;

            b.snesObjMicroscopeCalls += s_frame.snesObjMicroscopeCalls;
            b.snesObjHitTiles += s_frame.snesObjHitTiles;
            b.snesObjHitCycles += s_frame.snesObjHitCycles;
            b.snesObjMissTiles += s_frame.snesObjMissTiles;
            b.snesObjMissCycles += s_frame.snesObjMissCycles;
            b.snesObjHitTransparent += s_frame.snesObjHitTransparent;
            b.snesObjHitFullOpaque += s_frame.snesObjHitFullOpaque;
            b.snesObjHitPartial += s_frame.snesObjHitPartial;
            b.snesObjMicMissTransparent += s_frame.snesObjMicMissTransparent;
            b.snesObjMicMissFullOpaque += s_frame.snesObjMicMissFullOpaque;
            b.snesObjMicMissPartial += s_frame.snesObjMicMissPartial;
            b.snesObjRowSame += s_frame.snesObjRowSame;
            b.snesObjRowPlus16 += s_frame.snesObjRowPlus16;
            b.snesObjRowMinus16 += s_frame.snesObjRowMinus16;
            b.snesObjRowOther += s_frame.snesObjRowOther;

            b.snesChr4OpaqueTransitions += s_frame.snesChr4OpaqueTransitions;
            b.snesChr4AdjacentOpaqueTransitions += s_frame.snesChr4AdjacentOpaqueTransitions;
            b.snesChr4PaletteIndexReuses += s_frame.snesChr4PaletteIndexReuses;
            b.snesChr4DecodeKeyReuses += s_frame.snesChr4DecodeKeyReuses;
            b.snesChr4RowSameFlipChanged += s_frame.snesChr4RowSameFlipChanged;
            b.snesChr4PixelKeyReuses += s_frame.snesChr4PixelKeyReuses;
            b.snesChr4AdjacentPaletteIndexReuses += s_frame.snesChr4AdjacentPaletteIndexReuses;
            b.snesChr4AdjacentDecodeKeyReuses += s_frame.snesChr4AdjacentDecodeKeyReuses;
            b.snesChr4AdjacentPixelKeyReuses += s_frame.snesChr4AdjacentPixelKeyReuses;

            for (Uint32 uBG = 0; uBG < 4u; ++uBG)
            {
                for (Uint32 uPath = 0; uPath < 6u; ++uPath)
                {
                    b.snesChr4CellCalls[uBG][uPath] += s_frame.snesChr4CellCalls[uBG][uPath];
                    b.snesChr4CellTiles[uBG][uPath] += s_frame.snesChr4CellTiles[uBG][uPath];
                    b.snesChr4CellOpaqueTiles[uBG][uPath] += s_frame.snesChr4CellOpaqueTiles[uBG][uPath];
                    b.snesChr4CellOpaqueTransitions[uBG][uPath] += s_frame.snesChr4CellOpaqueTransitions[uBG][uPath];
                    b.snesChr4CellAdjacentOpaqueTransitions[uBG][uPath] += s_frame.snesChr4CellAdjacentOpaqueTransitions[uBG][uPath];
                    b.snesChr4CellPaletteIndexReuses[uBG][uPath] += s_frame.snesChr4CellPaletteIndexReuses[uBG][uPath];
                    b.snesChr4CellDecodeKeyReuses[uBG][uPath] += s_frame.snesChr4CellDecodeKeyReuses[uBG][uPath];
                    b.snesChr4CellRowSameFlipChanged[uBG][uPath] += s_frame.snesChr4CellRowSameFlipChanged[uBG][uPath];
                    b.snesChr4CellPixelKeyReuses[uBG][uPath] += s_frame.snesChr4CellPixelKeyReuses[uBG][uPath];
                    b.snesChr4CellAdjacentPaletteIndexReuses[uBG][uPath] += s_frame.snesChr4CellAdjacentPaletteIndexReuses[uBG][uPath];
                    b.snesChr4CellAdjacentDecodeKeyReuses[uBG][uPath] += s_frame.snesChr4CellAdjacentDecodeKeyReuses[uBG][uPath];
                    b.snesChr4CellAdjacentPixelKeyReuses[uBG][uPath] += s_frame.snesChr4CellAdjacentPixelKeyReuses[uBG][uPath];
                }
            }
        }
    }

    if (s_frame.core == AURORA_FP_CORE_PCE ||
        s_frame.core == AURORA_FP_CORE_GBA ||
        s_frame.core == AURORA_FP_CORE_SEGA)
    {
        for (Uint32 i=0; i<AURORA_EXT_BUCKET_MAX; ++i)
        {
            b.externalCalls[i] += s_frame.externalCalls[i];
            b.externalUnits[i] += s_frame.externalUnits[i];
            b.externalCycles[i] += s_frame.externalCycles[i];
        }
    }

    b.dspVoiceEnvelopeCalls += s_frame.dspVoiceEnvelopeCalls;
    b.dspVoiceEnvelopeActive += s_frame.dspVoiceEnvelopeActive;
    b.dspVoiceSampleNormalCalls += s_frame.dspVoiceSampleNormalCalls;
    b.dspVoiceSamplePmonCalls += s_frame.dspVoiceSamplePmonCalls;
    b.dspVoiceBrrDecodes += s_frame.dspVoiceBrrDecodes;
    b.dspVoicePitchmodFeeders += s_frame.dspVoicePitchmodFeeders;
    b.dspVoiceMixCalls += s_frame.dspVoiceMixCalls;
    b.dspVoiceMixDryCalls += s_frame.dspVoiceMixDryCalls;
    b.dspVoiceMixEchoCalls += s_frame.dspVoiceMixEchoCalls;
    b.dspVoiceNoiseMixes += s_frame.dspVoiceNoiseMixes;
    b.dspVoiceSilentFastpathHits += s_frame.dspVoiceSilentFastpathHits;

    b.dspVoiceSampleNormalSamples += s_frame.dspVoiceSampleNormalSamples;
    b.dspVoiceSamplePmonSamples += s_frame.dspVoiceSamplePmonSamples;
    b.dspVoiceMixSamples += s_frame.dspVoiceMixSamples;
    b.dspVoiceMixDrySamples += s_frame.dspVoiceMixDrySamples;
    b.dspVoiceMixEchoSamples += s_frame.dspVoiceMixEchoSamples;

    b.dspVoiceGainZeroVolume += s_frame.dspVoiceGainZeroVolume;
    b.dspVoiceGainEnvelopeZero += s_frame.dspVoiceGainEnvelopeZero;
    b.dspVoiceGainVeryLow += s_frame.dspVoiceGainVeryLow;
    b.dspVoiceGainLow += s_frame.dspVoiceGainLow;
    b.dspVoiceGainNormal += s_frame.dspVoiceGainNormal;
    b.dspVoiceLowGainPmon += s_frame.dspVoiceLowGainPmon;
    b.dspVoiceLowGainEcho += s_frame.dspVoiceLowGainEcho;
    b.dspVoiceLowGainFeedsPmon += s_frame.dspVoiceLowGainFeedsPmon;
}

static const char *DominantName(const WorstT &w)
{
    const Uint32 values[] =
    {
        w.inputPoll, w.inputSnapshot, w.inputLogic, w.preCore, w.postCore,
        w.renderHost, w.audioDrain, w.postRender, w.other
    };
    static const char *names[] =
    {
        "INPUT_POLL", "INPUT_SNAPSHOT", "INPUT_LOGIC",
        "PRE_CORE_OR_RENDER", "POST_CORE", "RENDER_HOST",
        "AUDIO_DRAIN", "POST_RENDER", "OTHER"
    };

    Uint32 best = 0;
    for (Uint32 i = 1; i < sizeof(values) / sizeof(values[0]); ++i)
        if (values[i] > values[best])
            best = i;
    return names[best];
}

static void InsertWorst(
    Uint32 serial, Uint32 state, Uint32 core,
    Uint32 tick, const DerivedT &d, Bool spike)
{
    WorstT w;
    memset(&w, 0, sizeof(w));
    w.serial = serial;
    w.state = state;
    w.core = core;
    w.tick = tick;
    w.frontend = d.frontend;
    w.coreCycles = s_frame.cycles[SPAN_CORE];
    w.vblank = s_frame.cycles[SPAN_VBLANK];
    w.inputPoll = s_frame.cycles[SPAN_INPUT_POLL];
    w.inputSnapshot = s_frame.cycles[SPAN_INPUT_SNAPSHOT];
    w.inputLogic = s_frame.cycles[SPAN_INPUT_LOGIC];
    w.preCore = s_frame.cycles[SPAN_PROCESS_PRE_CORE];
    w.postCore = s_frame.cycles[SPAN_PROCESS_POST_CORE];
    w.upload = s_frame.cycles[SPAN_UPLOAD];
    w.renderHost = d.renderNoVblankAudio;
    w.audioDrain = s_frame.cycles[SPAN_AUDIO_DRAIN];
    w.postRender = s_frame.cycles[SPAN_POST_RENDER];
    w.other = d.otherFrontend;
    w.fsClass = SafeFrameskipClassForFrame();
    w.spike = spike;

    Int32 pos = -1;
    for (Int32 i = 0; i < 16; ++i)
    {
        if (w.frontend > s_live.worst[i].frontend)
        {
            pos = i;
            break;
        }
    }
    if (pos < 0)
        return;

    for (Int32 i = 15; i > pos; --i)
        s_live.worst[i] = s_live.worst[i - 1];
    s_live.worst[pos] = w;
}

static void SealSnapshot(void)
{
    if (s_writePending || !s_live.totalFrames)
        return;

    memset(&s_pending, 0, sizeof(s_pending));
    s_pending.stats = s_live;
    s_pending.stamp = CaptureStamp();
    s_pending.meta = CaptureMetadata(s_live);
    s_pending.serial = ++s_dumpSerial;

    ResetLive();
    s_captureArmed = FALSE;
    s_menuFramesNeeded = 0;
    s_writePending = TRUE;
}

static Bool WriteBucket(FILE *fp, const char *name, const BucketT &b)
{
    const Uint64 f = b.frames;
    return fprintf(fp,
        "%s frames=%llu tick_avg=%llu "
        "core_avg=%llu frontend_ex_core_vblank_avg=%llu "
        "render_ex_vblank_avg=%llu render_ex_vblank_audio_avg=%llu "
        "input_poll_avg=%llu input_snapshot_prepare_avg=%llu "
        "input_logic_avg=%llu pre_core_or_render_avg=%llu "
        "post_core_avg=%llu upload_avg=%llu "
        "render_total_avg=%llu render_setup_avg=%llu "
        "game_draw_avg=%llu ui_avg=%llu gp_flush_avg=%llu "
        "vblank_avg=%llu audio_drain_avg=%llu post_render_avg=%llu "
        "other_frontend_avg=%llu\n",
        name,
        (unsigned long long)f,
        (unsigned long long)Avg(b.tick, f),
        (unsigned long long)Avg(b.span[SPAN_CORE], f),
        (unsigned long long)Avg(b.frontendExCoreVblank, f),
        (unsigned long long)Avg(b.renderExVblank, f),
        (unsigned long long)Avg(b.renderExVblankAudio, f),
        (unsigned long long)Avg(b.span[SPAN_INPUT_POLL], f),
        (unsigned long long)Avg(b.span[SPAN_INPUT_SNAPSHOT], f),
        (unsigned long long)Avg(b.span[SPAN_INPUT_LOGIC], f),
        (unsigned long long)Avg(b.span[SPAN_PROCESS_PRE_CORE], f),
        (unsigned long long)Avg(b.span[SPAN_PROCESS_POST_CORE], f),
        (unsigned long long)Avg(b.span[SPAN_UPLOAD], f),
        (unsigned long long)Avg(b.span[SPAN_RENDER], f),
        (unsigned long long)Avg(b.span[SPAN_RENDER_SETUP], f),
        (unsigned long long)Avg(b.span[SPAN_GAME_DRAW], f),
        (unsigned long long)Avg(b.span[SPAN_UI], f),
        (unsigned long long)Avg(b.span[SPAN_GP_FLUSH], f),
        (unsigned long long)Avg(b.span[SPAN_VBLANK], f),
        (unsigned long long)Avg(b.span[SPAN_AUDIO_DRAIN], f),
        (unsigned long long)Avg(b.span[SPAN_POST_RENDER], f),
        (unsigned long long)Avg(b.otherFrontend, f)) >= 0
        ? TRUE : FALSE;
}

static Bool WriteStateSet(FILE *fp, const char *section, const BucketT *set)
{
    if (fprintf(fp, "\n[%s]\n", section) < 0)
        return FALSE;
    for (Uint32 i = 0; i < AURORA_FP_STATE_COUNT; ++i)
        if (!WriteBucket(fp, StateName(i), set[i]))
            return FALSE;
    return TRUE;
}

static Bool WriteCoreSet(FILE *fp, const char *section, const BucketT *set)
{
    if (fprintf(fp, "\n[%s]\n", section) < 0)
        return FALSE;
    for (Uint32 i = 0; i < AURORA_FP_CORE_COUNT; ++i)
    {
        if (!set[i].frames)
            continue;
        if (!WriteBucket(fp, CoreName(i), set[i]))
            return FALSE;
    }
    return TRUE;
}

static Bool WriteSafeFrameskipSet(
    FILE *fp,
    const char *section,
    const BucketT set[AURORA_FP_CORE_COUNT][AURORA_FP_FS_CLASS_COUNT])
{
    if (fprintf(fp, "\n[%s]\n", section) < 0)
        return FALSE;

    for (Uint32 core = 0; core < AURORA_FP_CORE_COUNT; ++core)
    {
        for (Uint32 cls = 0; cls < AURORA_FP_FS_CLASS_COUNT; ++cls)
        {
            const BucketT &b = set[core][cls];
            if (!b.frames)
                continue;

            Char label[64];
            snprintf(label, sizeof(label), "%s/%s",
                CoreName(core), SafeFrameskipClassName(cls));
            if (!WriteBucket(fp, label, b))
                return FALSE;
        }
    }
    return TRUE;
}

static Bool WriteSafeFrameskipSummary(FILE *fp, const StatsT &s)
{
    if (fprintf(fp,
        "\n[SAFE_FRAMESKIP_SUMMARY]\n"
        "core,gameplay,fs_off,fs_unknown,fs_on_blocked,"
        "fs_on_presented,fs_on_skipped,allowed_ticks,skip_pct_x100\n") < 0)
        return FALSE;

    for (Uint32 core = 0; core < AURORA_FP_CORE_COUNT; ++core)
    {
        const Uint64 unknown = s.fsAll[core][AURORA_FP_FS_UNKNOWN].frames;
        const Uint64 off = s.fsAll[core][AURORA_FP_FS_OFF].frames;
        const Uint64 blocked = s.fsAll[core][AURORA_FP_FS_ON_BLOCKED].frames;
        const Uint64 presented = s.fsAll[core][AURORA_FP_FS_ON_PRESENTED].frames;
        const Uint64 skipped = s.fsAll[core][AURORA_FP_FS_ON_SKIPPED].frames;
        const Uint64 gameplay = unknown + off + blocked + presented + skipped;
        const Uint64 allowed = presented + skipped;
        const Uint64 skipPctX100 =
            allowed ? ((skipped * 10000u + allowed / 2u) / allowed) : 0u;

        if (!gameplay)
            continue;

        if (fprintf(fp,
            "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            CoreName(core),
            (unsigned long long)gameplay,
            (unsigned long long)off,
            (unsigned long long)unknown,
            (unsigned long long)blocked,
            (unsigned long long)presented,
            (unsigned long long)skipped,
            (unsigned long long)allowed,
            (unsigned long long)skipPctX100) < 0)
            return FALSE;
    }
    return TRUE;
}

static Bool WriteAudioSet(
    FILE *fp, const char *section, const BucketT *set)
{
    if (fprintf(fp,
        "\n[%s]\n"
        "core,frames,convert_cycles_avg,flush_cycles_avg,drain_cycles_avg,"
        "convert_input_frames_avg,flush_frames_avg,drain_sent_frames_avg\n",
        section) < 0)
        return FALSE;

    for (Uint32 core = 0; core < AURORA_FP_CORE_COUNT; ++core)
    {
        const BucketT &b = set[core];
        const Uint64 f = b.frames;
        if (!f)
            continue;
        if (fprintf(fp, "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            CoreName(core),
            (unsigned long long)f,
            (unsigned long long)Avg(b.span[SPAN_AUDIO_CONVERT], f),
            (unsigned long long)Avg(b.span[SPAN_AUDIO_FLUSH], f),
            (unsigned long long)Avg(b.span[SPAN_AUDIO_DRAIN], f),
            (unsigned long long)Avg(b.audioConvertInputFrames, f),
            (unsigned long long)Avg(b.audioFlushFrames, f),
            (unsigned long long)Avg(b.audioDrainFrames, f)) < 0)
            return FALSE;
    }
    return TRUE;
}

static Bool WriteSnesAudioSet(
    FILE *fp, const char *section, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 f = b.frames;

    if (fprintf(fp,
        "\n[%s]\n"
        "frames,spc_exec_avg,dsp_total_avg,dsp_sync_avg,dsp_noise_avg,"
        "dsp_voices_avg,dsp_echo_avg,dsp_final_avg,"
        "spc_calls_avg_x100,spc_requested_cycles_avg,spc_consumed_cycles_avg,"
        "dsp_mix_calls_avg_x100,dsp_chunks_avg_x100,dsp_samples_avg\n",
        section) < 0)
        return FALSE;

    if (!f)
        return TRUE;

    return fprintf(fp,
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)f,
        (unsigned long long)Avg(b.span[SPAN_SNES_SPC], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_TOTAL], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_SYNC], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_NOISE], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_VOICES], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_ECHO], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_FINAL], f),
        (unsigned long long)Avg(b.spcCalls * 100u, f),
        (unsigned long long)Avg(b.spcRequestedCycles, f),
        (unsigned long long)Avg(b.spcConsumedCycles, f),
        (unsigned long long)Avg(b.dspMixCalls * 100u, f),
        (unsigned long long)Avg(b.dspChunks * 100u, f),
        (unsigned long long)Avg(b.dspSamples, f)) >= 0
        ? TRUE : FALSE;
}

/* AURORA_SNES_DSP_VOICES_PROFILER_V3_5_1_20261005
 * BRR decode is nested inside sample_normal/sample_pmon and is therefore a
 * drill-down metric, not an extra term in residual.  Dry/echo mixer spans are
 * mutually exclusive and their sum is the old mix_stereo bucket. */
static Uint64 SnesVoiceMixTotal(const BucketT &b)
{
    return b.span[SPAN_SNES_DSP_MIX_DRY] +
           b.span[SPAN_SNES_DSP_MIX_ECHO];
}

static Uint64 SnesVoiceResidualTotal(const BucketT &b)
{
    const Uint64 attributed =
        b.span[SPAN_SNES_DSP_ENVELOPE] +
        b.span[SPAN_SNES_DSP_SAMPLE_NORMAL] +
        b.span[SPAN_SNES_DSP_SAMPLE_PMON] +
        b.span[SPAN_SNES_DSP_PITCHMOD_FEEDER] +
        SnesVoiceMixTotal(b);
    return b.span[SPAN_SNES_DSP_VOICES] > attributed
        ? b.span[SPAN_SNES_DSP_VOICES] - attributed : 0u;
}

static Uint64 SnesVoiceDiv(Uint64 n, Uint64 d)
{
    return d ? n / d : 0u;
}

static Bool WriteSnesDspVoicesSet(
    FILE *fp, const char *section, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 f = b.frames;
    const Uint64 chunks = b.dspChunks;
    const Uint64 classifiedSlots =
        b.dspVoiceEnvelopeCalls + b.dspVoiceSilentFastpathHits;

    if (fprintf(fp,
        "\n[%s]\n"
        "frames,voices_total_avg,envelope_avg,sample_normal_avg,sample_pmon_avg,"
        "brr_decode_avg,pitchmod_feeder_avg,mix_stereo_avg,mix_dry_avg,"
        "mix_echo_avg,residual_avg,dsp_chunks_avg_x100,"
        "classified_voice_slots_avg_x100,visited_voices_avg_x100,"
        "envelope_active_avg_x100,silent_fastpath_avg_x100,"
        "produced_voices_avg_x100,pmon_attempts_avg_x100,noise_mix_avg_x100,"
        "brr_decodes_avg_x100,pitchmod_feeders_avg_x100,mix_calls_avg_x100,"
        "mix_dry_calls_avg_x100,mix_echo_calls_avg_x100\n",
        section) < 0)
        return FALSE;

    if (!f)
        return TRUE;

    return fprintf(fp,
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)f,
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_VOICES], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_ENVELOPE], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_SAMPLE_NORMAL], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_SAMPLE_PMON], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_BRR_DECODE], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_PITCHMOD_FEEDER], f),
        (unsigned long long)Avg(SnesVoiceMixTotal(b), f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_MIX_DRY], f),
        (unsigned long long)Avg(b.span[SPAN_SNES_DSP_MIX_ECHO], f),
        (unsigned long long)Avg(SnesVoiceResidualTotal(b), f),
        (unsigned long long)Avg(b.dspChunks * 100u, f),
        (unsigned long long)SnesVoiceDiv(classifiedSlots * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceEnvelopeCalls * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceEnvelopeActive * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceSilentFastpathHits * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceMixCalls * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceSamplePmonCalls * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceNoiseMixes * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceBrrDecodes * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoicePitchmodFeeders * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceMixCalls * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceMixDryCalls * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceMixEchoCalls * 100u, chunks)) >= 0
        ? TRUE : FALSE;
}

static Bool WriteSnesDspVoiceGainSet(
    FILE *fp, const char *section, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 f = b.frames;
    const Uint64 chunks = b.dspChunks;
    const Uint64 lowGain =
        b.dspVoiceGainZeroVolume + b.dspVoiceGainEnvelopeZero +
        b.dspVoiceGainVeryLow + b.dspVoiceGainLow;

    if (fprintf(fp,
        "\n[%s]\n"
        "frames,zero_volume_avg_x100,envelope_zero_avg_x100,"
        "gain_1_256_avg_x100,gain_257_1024_avg_x100,gain_gt_1024_avg_x100,"
        "lowgain_le1024_avg_x100,zero_volume_mix_pct_x100,"
        "lowgain_mix_pct_x100,lowgain_pmon_avg_x100,lowgain_echo_avg_x100,"
        "lowgain_feeds_pmon_avg_x100\n",
        section) < 0)
        return FALSE;

    if (!f)
        return TRUE;

    return fprintf(fp,
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)f,
        (unsigned long long)SnesVoiceDiv(b.dspVoiceGainZeroVolume * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceGainEnvelopeZero * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceGainVeryLow * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceGainLow * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceGainNormal * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(lowGain * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceGainZeroVolume * 10000u,
                                         b.dspVoiceMixCalls),
        (unsigned long long)SnesVoiceDiv(lowGain * 10000u,
                                         b.dspVoiceMixCalls),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceLowGainPmon * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceLowGainEcho * 100u, chunks),
        (unsigned long long)SnesVoiceDiv(b.dspVoiceLowGainFeedsPmon * 100u, chunks)) >= 0
        ? TRUE : FALSE;
}

static Bool WriteSnesDspVoiceSampleCostSet(
    FILE *fp, const char *section, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 f = b.frames;
    const Uint64 voiceSamples =
        b.dspVoiceSampleNormalSamples + b.dspVoiceSamplePmonSamples;

    if (fprintf(fp,
        "\n[%s]\n"
        "frames,voice_samples_avg,normal_voice_samples_avg,pmon_voice_samples_avg,"
        "mix_voice_samples_avg,mix_dry_samples_avg,mix_echo_samples_avg,"
        "sample_normal_cycles_per_voice_sample_x1000,"
        "sample_pmon_cycles_per_voice_sample_x1000,"
        "sample_all_cycles_per_voice_sample_x1000,"
        "mix_cycles_per_voice_sample_x1000,"
        "mix_dry_cycles_per_voice_sample_x1000,"
        "mix_echo_cycles_per_voice_sample_x1000\n",
        section) < 0)
        return FALSE;

    if (!f)
        return TRUE;

    return fprintf(fp,
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)f,
        (unsigned long long)Avg(voiceSamples, f),
        (unsigned long long)Avg(b.dspVoiceSampleNormalSamples, f),
        (unsigned long long)Avg(b.dspVoiceSamplePmonSamples, f),
        (unsigned long long)Avg(b.dspVoiceMixSamples, f),
        (unsigned long long)Avg(b.dspVoiceMixDrySamples, f),
        (unsigned long long)Avg(b.dspVoiceMixEchoSamples, f),
        (unsigned long long)SnesVoiceDiv(
            b.span[SPAN_SNES_DSP_SAMPLE_NORMAL] * 1000u,
            b.dspVoiceSampleNormalSamples),
        (unsigned long long)SnesVoiceDiv(
            b.span[SPAN_SNES_DSP_SAMPLE_PMON] * 1000u,
            b.dspVoiceSamplePmonSamples),
        (unsigned long long)SnesVoiceDiv(
            (b.span[SPAN_SNES_DSP_SAMPLE_NORMAL] +
             b.span[SPAN_SNES_DSP_SAMPLE_PMON]) * 1000u,
            voiceSamples),
        (unsigned long long)SnesVoiceDiv(
            SnesVoiceMixTotal(b) * 1000u, b.dspVoiceMixSamples),
        (unsigned long long)SnesVoiceDiv(
            b.span[SPAN_SNES_DSP_MIX_DRY] * 1000u,
            b.dspVoiceMixDrySamples),
        (unsigned long long)SnesVoiceDiv(
            b.span[SPAN_SNES_DSP_MIX_ECHO] * 1000u,
            b.dspVoiceMixEchoSamples)) >= 0
        ? TRUE : FALSE;
}

/* AURORA_SNES_CORE_PROFILER_V3_6_20261005 */
static Bool WriteSnesCoreBreakdownSet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 frames = b.frames;
    Uint64 tracked = 0;
    for (Uint32 i = 0; i < 5u; ++i)
        tracked += b.snesCostCycles[i];
    const Uint64 core = b.span[SPAN_CORE];
    const Uint64 residual = core > tracked ? core - tracked : 0u;

    if (fprintf(fp,
        "\n[%s]\n"
        "frames,core_avg,cpu_avg,ppu_avg,raster_avg,spc_avg,dsp_avg,"
        "tracked_avg,residual_avg,tracked_pct_x100,"
        "cpu_budget_calls_avg_x100,cpu_budget_cycles_avg,"
        "cpu_host_cycles_per_emulated_x1000,ppu_sync_calls_avg_x100,"
        "invalid_frames\n",
        title) < 0) return FALSE;

    return fprintf(fp,
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)frames,
        (unsigned long long)Avg(core, frames),
        (unsigned long long)Avg(b.snesCostCycles[0], frames),
        (unsigned long long)Avg(b.snesCostCycles[1], frames),
        (unsigned long long)Avg(b.snesCostCycles[2], frames),
        (unsigned long long)Avg(b.snesCostCycles[3], frames),
        (unsigned long long)Avg(b.snesCostCycles[4], frames),
        (unsigned long long)Avg(tracked, frames),
        (unsigned long long)Avg(residual, frames),
        (unsigned long long)SnesScaledDiv(tracked, core, 10000u),
        (unsigned long long)SnesScaledDiv(b.snesCpuBudgetCalls, frames, 100u),
        (unsigned long long)Avg(b.snesCpuBudgetCycles, frames),
        (unsigned long long)SnesScaledDiv(
            b.snesCostCycles[0], b.snesCpuBudgetCycles, 1000u),
        (unsigned long long)SnesScaledDiv(b.snesPpuSyncCalls, frames, 100u),
        (unsigned long long)b.snesProfileInvalidFrames) >= 0;
}

static Bool WriteSnesPpuBreakdownSet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 frames = b.frames;
    Uint64 detail = 0;
    for (Uint32 i = 0; i < 18u; ++i)
        detail += b.snesPpuCycles[i];
    const Uint64 ppu = b.snesCostCycles[1];
    const Uint64 residual = ppu > detail ? ppu - detail : 0u;

    if (fprintf(fp,
        "\n[%s]\n"
        "frames,ppu_avg,sync_avg,render_other_avg,prep_avg,raster_other_avg,"
        "bg_map_avg,bg_chr_avg,bg_main_avg,bg_sub_avg,bg_other_avg,"
        "obj_legacy_avg,obj_update_avg,obj_fetch_avg,obj_main_avg,obj_sub_avg,"
        "mode7_avg,color_mask_avg,blend_avg,color_other_avg,"
        "detail_total_avg,ppu_unclassified_avg,invalid_frames\n",
        title) < 0) return FALSE;

    if (fprintf(fp, "%llu,%llu",
        (unsigned long long)frames,
        (unsigned long long)Avg(ppu, frames)) < 0) return FALSE;
    for (Uint32 i = 0; i < 18u; ++i)
        if (fprintf(fp, ",%llu",
            (unsigned long long)Avg(b.snesPpuCycles[i], frames)) < 0)
            return FALSE;
    return fprintf(fp, ",%llu,%llu,%llu\n",
        (unsigned long long)Avg(detail, frames),
        (unsigned long long)Avg(residual, frames),
        (unsigned long long)b.snesProfileInvalidFrames) >= 0;
}

static Bool WriteSnesPpuWorkSet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 frames = b.frames;
    const Uint64 objLines = b.snesPpuObjEnabledLines;

    if (fprintf(fp,
        "\n[%s]\n"
        "frames,rendered_lines_avg_x100,"
        "mode0_lines_avg_x100,mode1_lines_avg_x100,mode2_lines_avg_x100,"
        "mode3_lines_avg_x100,mode4_lines_avg_x100,mode5_lines_avg_x100,"
        "mode6_lines_avg_x100,mode7_lines_avg_x100,"
        "bg_map_fetches_avg_x100,bg_chr_decodes_avg_x100,"
        "main_bg_layers_avg_x100,sub_bg_layers_avg_x100,"
        "obj_oam_refs_avg_x100,obj_tiles_avg_x100,obj_enabled_lines_avg_x100,"
        "obj_range_over_lines_avg_x100,obj_time_over_lines_avg_x100,"
        "window_lines_avg_x100,color_math_lines_avg_x100,subscreen_lines_avg_x100,"
        "hires_lines_avg_x100,direct_color_lines_avg_x100,"
        "obj_refs_per_enabled_line_x100,obj_tiles_per_enabled_line_x100\n",
        title) < 0) return FALSE;

    if (fprintf(fp, "%llu,%llu",
        (unsigned long long)frames,
        (unsigned long long)SnesScaledDiv(b.snesPpuRenderedLines, frames, 100u)) < 0)
        return FALSE;
    for (Uint32 i = 0; i < 8u; ++i)
        if (fprintf(fp, ",%llu",
            (unsigned long long)SnesScaledDiv(b.snesPpuModeLines[i], frames, 100u)) < 0)
            return FALSE;

    return fprintf(fp,
        ",%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)SnesScaledDiv(b.snesPpuBgMapFetches, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuBgChrDecodes, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuMainBgLayers, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuSubBgLayers, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuObjOamRefs, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuObjTiles, frames, 100u),
        (unsigned long long)SnesScaledDiv(objLines, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuObjRangeOverLines, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuObjTimeOverLines, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuWindowLines, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuColorMathLines, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuSubscreenLines, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuHiresLines, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuDirectColorLines, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuObjOamRefs, objLines, 100u),
        (unsigned long long)SnesScaledDiv(b.snesPpuObjTiles, objLines, 100u)) >= 0;
}

static Bool WriteSnesDmaBreakdownSet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 frames = b.frames;
    if (fprintf(fp,
        "\n[%s]\n"
        "frames,raster_bucket_avg,mdma_avg,hdma_setup_avg,hdma_data_avg\n",
        title) < 0) return FALSE;
    return fprintf(fp, "%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)frames,
        (unsigned long long)Avg(b.snesCostCycles[2], frames),
        (unsigned long long)Avg(b.span[SPAN_SNES_MDMA], frames),
        (unsigned long long)Avg(b.span[SPAN_SNES_HDMA_SETUP], frames),
        (unsigned long long)Avg(b.span[SPAN_SNES_HDMA_DATA], frames)) >= 0;
}

/* AURORA_SNES_BGCHR_PROFILER_V3_7_20261005 */
static const char *SnesBgChrDepthName(Uint32 depth)
{
    static const char *const names[4] = { "2", "4", "8", "other" };
    return depth < 4u ? names[depth] : "?";
}

static Bool WriteSnesBgChrSummarySet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 frames = b.frames;
    const Uint64 ppuBgChr = b.snesPpuCycles[5];
    const Uint64 measured = b.snesBgChrMeasuredCycles;
    const Uint64 decode = b.snesBgChrMeasuredDecodeCycles;
    const Uint64 post = measured >= decode ? measured - decode : 0u;
    const Uint64 outer = ppuBgChr >= measured ? ppuBgChr - measured : 0u;

    if (fprintf(fp,
        "\n[%s]\n"
        "frames,ppu_bg_chr_avg,measured_total_avg,decode_avg,post_avg,"
        "outer_residual_avg,calls_avg_x100,tiles_avg_x100,"
        "cycles_per_call_x1000,cycles_per_tile_x1000,"
        "decode_cycles_per_tile_x1000,post_cycles_per_tile_x1000,bad_records\n",
        title) < 0) return FALSE;

    return fprintf(fp,
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)frames,
        (unsigned long long)Avg(ppuBgChr, frames),
        (unsigned long long)Avg(measured, frames),
        (unsigned long long)Avg(decode, frames),
        (unsigned long long)Avg(post, frames),
        (unsigned long long)Avg(outer, frames),
        (unsigned long long)SnesScaledDiv(b.snesBgChrMeasuredCalls, frames, 100u),
        (unsigned long long)SnesScaledDiv(b.snesBgChrMeasuredTiles, frames, 100u),
        (unsigned long long)SnesScaledDiv(measured, b.snesBgChrMeasuredCalls, 1000u),
        (unsigned long long)SnesScaledDiv(measured, b.snesBgChrMeasuredTiles, 1000u),
        (unsigned long long)SnesScaledDiv(decode, b.snesBgChrMeasuredTiles, 1000u),
        (unsigned long long)SnesScaledDiv(post, b.snesBgChrMeasuredTiles, 1000u),
        (unsigned long long)b.snesBgChrBadRecords) >= 0;
}

/* AURORA_V23_SNES_BGCHR_TRAITS_COST_20261009
 * Estimates observer/collector cost inside parent BGCHR; not a core speedup. */
static Bool WriteSnesBgChrTraitsCostSet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 frames = b.frames;
    const Uint64 bgchr = b.snesPpuCycles[5];
    const Uint64 measured = b.snesBgChrMeasuredCycles;
    const Uint64 outer = bgchr >= measured ? bgchr - measured : 0u;
    const Uint64 traits = b.snesBgChrTraitsCycles;
    const Uint64 leftover = outer >= traits ? outer - traits : 0u;
    return fprintf(fp,
        "\n[%s]\n"
        "frames,traits_scan_avg,outer_ex_traits_avg,outer_raw_avg\n"
        "%llu,%llu,%llu,%llu\n",
        title,
        (unsigned long long)frames,
        (unsigned long long)Avg(traits, frames),
        (unsigned long long)Avg(leftover, frames),
        (unsigned long long)Avg(outer, frames)) >= 0;
}

static Bool WriteSnesBgChrLayerDepthSet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 frames = b.frames;
    if (fprintf(fp,
        "\n[%s]\n"
        "bg,bitdepth,calls_avg_x100,tiles_avg_x100,total_cycles_avg,"
        "decode_cycles_avg,post_cycles_avg,cycles_per_call_x1000,"
        "cycles_per_tile_x1000,decode_cycles_per_tile_x1000,"
        "post_cycles_per_tile_x1000\n", title) < 0) return FALSE;

    for (Uint32 bg = 0; bg < 4u; ++bg)
    {
        for (Uint32 depth = 0; depth < 4u; ++depth)
        {
            const Uint64 calls = b.snesBgChrCalls[bg][depth];
            const Uint64 tiles = b.snesBgChrTiles[bg][depth];
            const Uint64 total = b.snesBgChrCycles[bg][depth];
            const Uint64 decode = b.snesBgChrDecodeCycles[bg][depth];
            const Uint64 post = total >= decode ? total - decode : 0u;
            if (fprintf(fp,
                "BG%u,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                (unsigned)(bg + 1u), SnesBgChrDepthName(depth),
                (unsigned long long)SnesScaledDiv(calls, frames, 100u),
                (unsigned long long)SnesScaledDiv(tiles, frames, 100u),
                (unsigned long long)Avg(total, frames),
                (unsigned long long)Avg(decode, frames),
                (unsigned long long)Avg(post, frames),
                (unsigned long long)SnesScaledDiv(total, calls, 1000u),
                (unsigned long long)SnesScaledDiv(total, tiles, 1000u),
                (unsigned long long)SnesScaledDiv(decode, tiles, 1000u),
                (unsigned long long)SnesScaledDiv(post, tiles, 1000u)) < 0)
                return FALSE;
        }
    }
    return TRUE;
}

static Bool WriteSnesBgChrFlagsSet(
    FILE *fp, const char *title, const BucketT *set)
{
    static const char *const names[4] = {
        "OFFSET", "MOSAIC", "FINE_X_NONZERO", "HIRES_SUBSCREEN"
    };
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 frames = b.frames;
    if (fprintf(fp,
        "\n[%s]\n"
        "flag,calls_avg_x100,tiles_avg_x100,total_cycles_avg,decode_cycles_avg,"
        "post_cycles_avg,cycles_per_tile_x1000,decode_cycles_per_tile_x1000,"
        "post_cycles_per_tile_x1000\n", title) < 0) return FALSE;

    for (Uint32 flag = 0; flag < 4u; ++flag)
    {
        const Uint64 calls = b.snesBgChrFlagCalls[flag];
        const Uint64 tiles = b.snesBgChrFlagTiles[flag];
        const Uint64 total = b.snesBgChrFlagCycles[flag];
        const Uint64 decode = b.snesBgChrFlagDecodeCycles[flag];
        const Uint64 post = total >= decode ? total - decode : 0u;
        if (fprintf(fp, "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            names[flag],
            (unsigned long long)SnesScaledDiv(calls, frames, 100u),
            (unsigned long long)SnesScaledDiv(tiles, frames, 100u),
            (unsigned long long)Avg(total, frames),
            (unsigned long long)Avg(decode, frames),
            (unsigned long long)Avg(post, frames),
            (unsigned long long)SnesScaledDiv(total, tiles, 1000u),
            (unsigned long long)SnesScaledDiv(decode, tiles, 1000u),
            (unsigned long long)SnesScaledDiv(post, tiles, 1000u)) < 0)
            return FALSE;
    }
    return TRUE;
}

/* AURORA_SNES_MICROSCOPE_REPORT_V3_26_20261006 */
static Bool WriteSnesChr4LocalitySet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 f = b.frames;
    const Uint64 flipTransitions = b.snesChr4FlipSame + b.snesChr4FlipChanges;
    const Uint64 rowTransitions = b.snesChr4RowSame + b.snesChr4RowPlus16 +
        b.snesChr4RowMinus16 + b.snesChr4RowOther;
    const Uint64 rowPm16 = b.snesChr4RowPlus16 + b.snesChr4RowMinus16;
    const Uint64 rowPredictable = b.snesChr4RowSame + rowPm16;

    if (fprintf(fp,
        "\n[%s]\n"
        "kind,calls_avg_x100,tiles_avg_x100,opaque_tiles_avg_x100,"
        "palette_reusable_avg_x100,palette_runs_avg_x100,"
        "palette_reuse_pct_x100,palette_avg_run_x1000,"
        "flip_same_avg_x100,flip_change_avg_x100,flip_same_pct_x100,"
        "row_same_avg_x100,row_plus16_avg_x100,row_minus16_avg_x100,"
        "row_other_avg_x100,row_pm16_pct_x100,row_predictable_pct_x100\n",
        title) < 0) return FALSE;

    return fprintf(fp,
        "CHR4,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)SnesScaledDiv(b.snesChr4LocalityCalls, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4LocalityTiles, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4OpaqueTiles, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4PaletteReuses, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4PaletteRuns, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4PaletteReuses,
            b.snesChr4OpaqueTiles, 10000u),
        (unsigned long long)SnesScaledDiv(b.snesChr4OpaqueTiles,
            b.snesChr4PaletteRuns, 1000u),
        (unsigned long long)SnesScaledDiv(b.snesChr4FlipSame, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4FlipChanges, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4FlipSame,
            flipTransitions, 10000u),
        (unsigned long long)SnesScaledDiv(b.snesChr4RowSame, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4RowPlus16, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4RowMinus16, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4RowOther, f, 100u),
        (unsigned long long)SnesScaledDiv(rowPm16, rowTransitions, 10000u),
        (unsigned long long)SnesScaledDiv(rowPredictable, rowTransitions, 10000u)) >= 0;
}

static Bool WriteSnesObjMicroscopeSet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 f = b.frames;
    const Uint64 totalTiles = b.snesObjHitTiles + b.snesObjMissTiles;
    const Uint64 hitPathX1000 = SnesScaledDiv(
        b.snesObjHitCycles, b.snesObjHitTiles, 1000u);
    const Uint64 missPathX1000 = SnesScaledDiv(
        b.snesObjMissCycles, b.snesObjMissTiles, 1000u);
    const Uint64 missExtraX1000 = missPathX1000 >= hitPathX1000
        ? missPathX1000 - hitPathX1000 : 0u;
    const Uint64 rowTransitions = b.snesObjRowSame + b.snesObjRowPlus16 +
        b.snesObjRowMinus16 + b.snesObjRowOther;
    const Uint64 rowPm16 = b.snesObjRowPlus16 + b.snesObjRowMinus16;
    const Uint64 rowPredictable = b.snesObjRowSame + rowPm16;

    if (fprintf(fp,
        "\n[%s]\n"
        "kind,calls_avg_x100,hit_tiles_avg_x100,miss_tiles_avg_x100,hit_pct_x100,"
        "hit_path_cycles_avg,miss_path_cycles_avg,hit_path_cycles_per_tile_x1000,"
        "miss_path_cycles_per_tile_x1000,miss_extra_cycles_per_tile_x1000,"
        "hit_transparent_avg_x100,hit_full_opaque_avg_x100,hit_partial_avg_x100,"
        "hit_transparent_pct_x100,hit_full_opaque_pct_x100,hit_partial_pct_x100,"
        "miss_transparent_avg_x100,miss_full_opaque_avg_x100,miss_partial_avg_x100,"
        "row_same_avg_x100,row_plus16_avg_x100,row_minus16_avg_x100,row_other_avg_x100,"
        "row_pm16_pct_x100,row_predictable_pct_x100\n",
        title) < 0) return FALSE;

    return fprintf(fp,
        "OBJ,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)SnesScaledDiv(b.snesObjMicroscopeCalls, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjHitTiles, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjMissTiles, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjHitTiles, totalTiles, 10000u),
        (unsigned long long)Avg(b.snesObjHitCycles, f),
        (unsigned long long)Avg(b.snesObjMissCycles, f),
        (unsigned long long)hitPathX1000,
        (unsigned long long)missPathX1000,
        (unsigned long long)missExtraX1000,
        (unsigned long long)SnesScaledDiv(b.snesObjHitTransparent, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjHitFullOpaque, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjHitPartial, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjHitTransparent,
            b.snesObjHitTiles, 10000u),
        (unsigned long long)SnesScaledDiv(b.snesObjHitFullOpaque,
            b.snesObjHitTiles, 10000u),
        (unsigned long long)SnesScaledDiv(b.snesObjHitPartial,
            b.snesObjHitTiles, 10000u),
        (unsigned long long)SnesScaledDiv(b.snesObjMicMissTransparent, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjMicMissFullOpaque, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjMicMissPartial, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjRowSame, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjRowPlus16, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjRowMinus16, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesObjRowOther, f, 100u),
        (unsigned long long)SnesScaledDiv(rowPm16, rowTransitions, 10000u),
        (unsigned long long)SnesScaledDiv(rowPredictable, rowTransitions, 10000u)) >= 0;
}

/* AURORA_CHR4_REUSE_REPORT_V3_28_20261007 */
static Bool WriteSnesChr4ReuseSet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 f = b.frames;
    const Uint64 opaque = b.snesChr4OpaqueTiles;
    const Uint64 palRuns = opaque >= b.snesChr4PaletteIndexReuses
        ? opaque - b.snesChr4PaletteIndexReuses : 0u;
    const Uint64 decodeRuns = opaque >= b.snesChr4DecodeKeyReuses
        ? opaque - b.snesChr4DecodeKeyReuses : 0u;
    const Uint64 pixelRuns = opaque >= b.snesChr4PixelKeyReuses
        ? opaque - b.snesChr4PixelKeyReuses : 0u;
    const Uint64 sameRowAny =
        b.snesChr4DecodeKeyReuses + b.snesChr4RowSameFlipChanged;

    if (fprintf(fp,
        "\n[%s]\n"
        "kind,opaque_tiles_avg_x100,opaque_transitions_avg_x100,"
        "adjacent_opaque_transitions_avg_x100,"
        "palette_index_reuse_avg_x100,palette_index_reuse_pct_x100,"
        "palette_index_avg_run_x1000,"
        "decode_key_reuse_avg_x100,decode_key_reuse_pct_x100,"
        "decode_key_avg_run_x1000,"
        "row_same_flip_changed_avg_x100,row_same_any_pct_x100,"
        "pixel_key_reuse_avg_x100,pixel_key_reuse_pct_x100,"
        "pixel_key_avg_run_x1000,"
        "adjacent_palette_index_reuse_pct_x100,"
        "adjacent_decode_key_reuse_pct_x100,"
        "adjacent_pixel_key_reuse_pct_x100\n",
        title) < 0) return FALSE;

    return fprintf(fp,
        "CHR4,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)SnesScaledDiv(opaque, f, 100u),
        (unsigned long long)SnesScaledDiv(b.snesChr4OpaqueTransitions, f, 100u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4AdjacentOpaqueTransitions, f, 100u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4PaletteIndexReuses, f, 100u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4PaletteIndexReuses, opaque, 10000u),
        (unsigned long long)SnesScaledDiv(opaque, palRuns, 1000u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4DecodeKeyReuses, f, 100u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4DecodeKeyReuses, opaque, 10000u),
        (unsigned long long)SnesScaledDiv(opaque, decodeRuns, 1000u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4RowSameFlipChanged, f, 100u),
        (unsigned long long)SnesScaledDiv(
            sameRowAny, b.snesChr4OpaqueTransitions, 10000u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4PixelKeyReuses, f, 100u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4PixelKeyReuses, opaque, 10000u),
        (unsigned long long)SnesScaledDiv(opaque, pixelRuns, 1000u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4AdjacentPaletteIndexReuses,
            b.snesChr4AdjacentOpaqueTransitions, 10000u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4AdjacentDecodeKeyReuses,
            b.snesChr4AdjacentOpaqueTransitions, 10000u),
        (unsigned long long)SnesScaledDiv(
            b.snesChr4AdjacentPixelKeyReuses,
            b.snesChr4AdjacentOpaqueTransitions, 10000u)) >= 0;
}

/* AURORA_CHR4_PATH_BG_REPORT_V3_29_20261007 */
static const char *SnesChr4PathName(Uint32 path)
{
    static const char *const names[6] = {
        "NOOFFSET", "OFFSET", "MODE5_MAIN", "MODE5_SUB",
        "MODE6_MAIN", "MODE6_SUB"
    };
    return path < 6u ? names[path] : "BAD";
}

static Bool WriteSnesChr4ReuseMatrixSet(
    FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 f = b.frames;

    if (fprintf(fp,
        "\n[%s]\n"
        "bg,path,calls_avg_x100,tiles_avg_x100,opaque_avg_x100,opaque_pct_x100,"
        "opaque_transitions_avg_x100,palette_reuse_of_transitions_pct_x100,"
        "palette_avg_run_x1000,decode_reuse_of_transitions_pct_x100,"
        "decode_avg_run_x1000,row_same_any_of_transitions_pct_x100,"
        "pixel_reuse_of_transitions_pct_x100,pixel_avg_run_x1000,"
        "adjacent_transitions_avg_x100,adjacent_palette_reuse_pct_x100,"
        "adjacent_decode_reuse_pct_x100,adjacent_pixel_reuse_pct_x100\n",
        title) < 0) return FALSE;

    for (Uint32 bg = 0; bg < 4u; ++bg)
    {
        for (Uint32 path = 0; path < 6u; ++path)
        {
            const Uint64 calls = b.snesChr4CellCalls[bg][path];
            if (!calls)
                continue;
            const Uint64 tiles = b.snesChr4CellTiles[bg][path];
            const Uint64 opaque = b.snesChr4CellOpaqueTiles[bg][path];
            const Uint64 transitions = b.snesChr4CellOpaqueTransitions[bg][path];
            const Uint64 adjacent = b.snesChr4CellAdjacentOpaqueTransitions[bg][path];
            const Uint64 palReuse = b.snesChr4CellPaletteIndexReuses[bg][path];
            const Uint64 decodeReuse = b.snesChr4CellDecodeKeyReuses[bg][path];
            const Uint64 rowFlipChange = b.snesChr4CellRowSameFlipChanged[bg][path];
            const Uint64 pixelReuse = b.snesChr4CellPixelKeyReuses[bg][path];
            const Uint64 palRuns = opaque >= palReuse ? opaque - palReuse : 0u;
            const Uint64 decodeRuns = opaque >= decodeReuse ? opaque - decodeReuse : 0u;
            const Uint64 pixelRuns = opaque >= pixelReuse ? opaque - pixelReuse : 0u;

            if (fprintf(fp,
                "BG%u,%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
                "%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
                (unsigned)bg, SnesChr4PathName(path),
                (unsigned long long)SnesScaledDiv(calls, f, 100u),
                (unsigned long long)SnesScaledDiv(tiles, f, 100u),
                (unsigned long long)SnesScaledDiv(opaque, f, 100u),
                (unsigned long long)SnesScaledDiv(opaque, tiles, 10000u),
                (unsigned long long)SnesScaledDiv(transitions, f, 100u),
                (unsigned long long)SnesScaledDiv(palReuse, transitions, 10000u),
                (unsigned long long)SnesScaledDiv(opaque, palRuns, 1000u),
                (unsigned long long)SnesScaledDiv(decodeReuse, transitions, 10000u),
                (unsigned long long)SnesScaledDiv(opaque, decodeRuns, 1000u),
                (unsigned long long)SnesScaledDiv(
                    decodeReuse + rowFlipChange, transitions, 10000u),
                (unsigned long long)SnesScaledDiv(pixelReuse, transitions, 10000u),
                (unsigned long long)SnesScaledDiv(opaque, pixelRuns, 1000u),
                (unsigned long long)SnesScaledDiv(adjacent, f, 100u),
                (unsigned long long)SnesScaledDiv(
                    b.snesChr4CellAdjacentPaletteIndexReuses[bg][path],
                    adjacent, 10000u),
                (unsigned long long)SnesScaledDiv(
                    b.snesChr4CellAdjacentDecodeKeyReuses[bg][path],
                    adjacent, 10000u),
                (unsigned long long)SnesScaledDiv(
                    b.snesChr4CellAdjacentPixelKeyReuses[bg][path],
                    adjacent, 10000u)) < 0)
                return FALSE;
        }
    }
    return TRUE;
}

/* AURORA_SNES_DEEP_OPT_V3_14_20261005 */
static const char *SnesDeepName(Uint32 i)
{
    static const char *const names[69] = {
        "M7_SETUP",
        "M7_FETCH_REPEAT_DY0",
        "M7_FETCH_REPEAT_UNITX",
        "M7_FETCH_REPEAT_GENERIC",
        "M7_FETCH_CLAMP",
        "M7_FETCH_BLACK",
        "M7_HALF_EXPAND",
        "M7_OPAQUE",
        "M7_EXTBG_COPY",
        "M7_EXTBG_PRIORITY",
        "M7_EXTBG_OPAQUE",
        "M7_MOSAIC_BG1",
        "M7_MOSAIC_BG2",
        "PPU_QUEUE_APPLY",
        "PPU_VBLANK_DRAIN",
        "BLEND_PLANAR",
        "BLEND_GIF_WAIT",
        "BLEND_LIST_REBUILD",
        "BLEND_STAGE_COPY",
        "BLEND_SET_PARAMS",
        "BLEND_KICK",
        "BG_MAIN_BG1",
        "BG_MAIN_BG2",
        "BG_MAIN_BG3",
        "BG_MAIN_BG4",
        "BG_SUB_BG1",
        "BG_SUB_BG2",
        "BG_SUB_BG3",
        "BG_SUB_BG4",
        "OBJ_ROTATE",
        "OBJ_FETCH_DECODE",
        "HDMA_DATA_PHASE",
        "HDMA_TABLE_PHASE",
        "MDMA_FAST",
        "MDMA_ACCURATE",
        "MDMA_READ",
        "MDMA_SDD1",
        "CPU_EXEC",
        "CPU_SA1",
        "CPU_WAI",
        "BG_MAIN_DIRECT",
        "BG_SUB_DIRECT",
        "CHR2_OPAQUE_ROWS",
        "CHR2_TRANSPARENT_ROWS",
        "CHR4_OPAQUE_ROWS",
        "CHR4_TRANSPARENT_ROWS",
        "CHR2_OPAQUE_FLIP0",
        "CHR2_OPAQUE_FLIPPED",
        "CHR4_OPAQUE_FLIP0",
        "CHR4_OPAQUE_FLIPPED",
        "M7_CLAMP_GENERIC_INSIDE_PIXELS",
        "M7_CLAMP_GENERIC_OUTSIDE_PIXELS",
        "M7_CLAMP_GENERIC_TILE_REUSE",
        "M7_CLAMP_GENERIC_TILE_LOADS",
        "M7_CLAMP_GENERIC_BOUNDARY_TRANSITIONS",
        "M7_CLAMP_GENERIC_ALL_INSIDE_CALLS",
        "M7_CLAMP_GENERIC_MIXED_CALLS",
        "M7_CLAMP_GENERIC_ALL_OUTSIDE_CALLS",
        "M7_CLAMP_DX0_CALLS",
        "M7_CLAMP_DY0_CALLS",
        "M7_CLAMP_UNITX_CALLS",
        "M7_CLAMP_UNITY_CALLS",
        "OBJ_CACHE_HITS",
        "OBJ_CACHE_MISSES",
        "OBJ_MISS_TRANSPARENT",
        "OBJ_MISS_OPAQUE",
        "OBJ_HFLIP_TILES",
        "OBJ_SECOND_TABLE_TILES",
        "OBJ_COMPACT"
    };
    return i < 69u ? names[i] : "?";
}

static Bool WriteSnesDeepSet(FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 frames = b.frames;
    if (fprintf(fp,
        "\n[%s]\n"
        "bucket,calls_avg_x100,units_avg_x100,cycles_avg,cycles_per_call_x1000,cycles_per_unit_x1000\n",
        title) < 0) return FALSE;
    for (Uint32 i = 0; i < 69u; ++i)
    {
        if (fprintf(fp, "%s,%llu,%llu,%llu,%llu,%llu\n",
            SnesDeepName(i),
            (unsigned long long)SnesScaledDiv(b.snesDeepCalls[i], frames, 100u),
            (unsigned long long)SnesScaledDiv(b.snesDeepUnits[i], frames, 100u),
            (unsigned long long)Avg(b.snesDeepCycles[i], frames),
            (unsigned long long)SnesScaledDiv(b.snesDeepCycles[i], b.snesDeepCalls[i], 1000u),
            (unsigned long long)SnesScaledDiv(b.snesDeepCycles[i], b.snesDeepUnits[i], 1000u)) < 0)
            return FALSE;
    }
    return TRUE;
}

static Bool WriteSnesDeepReconcileRow(FILE *fp, const char *name,
    Uint64 frames, Uint64 parent, Uint64 child)
{
    const Uint64 residual = parent >= child ? parent - child : 0u;
    return fprintf(fp, "%s,%llu,%llu,%llu,%llu\n", name,
        (unsigned long long)Avg(parent, frames),
        (unsigned long long)Avg(child, frames),
        (unsigned long long)Avg(residual, frames),
        (unsigned long long)SnesScaledDiv(child, parent, 10000u)) >= 0;
}

static Bool WriteSnesDeepReconcileSet(FILE *fp, const char *title, const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_SNES];
    const Uint64 f = b.frames;
    Uint64 mode7=0, blend=0, bgmain=0, bgsub=0, obj=0, hdma=0, mdma=0, cpu=0;
    for (Uint32 i=0;i<=12u;++i) mode7 += b.snesDeepCycles[i];
    for (Uint32 i=15u;i<=20u;++i) blend += b.snesDeepCycles[i];
    for (Uint32 i=21u;i<=24u;++i) bgmain += b.snesDeepCycles[i];
    bgmain += b.snesDeepCycles[40];
    for (Uint32 i=25u;i<=28u;++i) bgsub += b.snesDeepCycles[i];
    bgsub += b.snesDeepCycles[41];
    obj = b.snesDeepCycles[29] + b.snesDeepCycles[30] +
        b.snesDeepCycles[AURORA_SNES_DEEP_OBJ_COMPACT];
    hdma = b.snesDeepCycles[31] + b.snesDeepCycles[32];
    for (Uint32 i=33u;i<=36u;++i) mdma += b.snesDeepCycles[i];
    cpu = b.snesDeepCycles[37] + b.snesDeepCycles[38];
    const Uint64 sync = b.snesDeepCycles[13] + b.snesDeepCycles[14];

    if (fprintf(fp, "\n[%s]\nfamily,parent_avg,children_avg,residual_avg,children_pct_x100\n", title) < 0)
        return FALSE;
    if (!WriteSnesDeepReconcileRow(fp,"MODE7",f,b.snesPpuCycles[14],mode7)) return FALSE;
    if (!WriteSnesDeepReconcileRow(fp,"PPU_SYNC",f,b.snesPpuCycles[0],sync)) return FALSE;
    if (!WriteSnesDeepReconcileRow(fp,"BG_MAIN",f,b.snesPpuCycles[6],bgmain)) return FALSE;
    if (!WriteSnesDeepReconcileRow(fp,"BG_SUB",f,b.snesPpuCycles[7],bgsub)) return FALSE;
    if (!WriteSnesDeepReconcileRow(fp,"OBJ_FETCH",f,b.snesPpuCycles[11],obj)) return FALSE;
    if (!WriteSnesDeepReconcileRow(fp,"BLEND",f,b.snesPpuCycles[16],blend)) return FALSE;
    if (!WriteSnesDeepReconcileRow(fp,"MDMA",f,b.span[SPAN_SNES_MDMA],mdma)) return FALSE;
    if (!WriteSnesDeepReconcileRow(fp,"HDMA",f,b.span[SPAN_SNES_HDMA_DATA],hdma)) return FALSE;
    if (!WriteSnesDeepReconcileRow(fp,"CPU",f,b.snesCostCycles[0],cpu)) return FALSE;
    return TRUE;
}

/* AURORA_MULTICORE_DEEP_PROFILER_V3_48_20261008 */
enum AuroraExternalRelationE
{
    AURORA_EXT_PARTITION = 0,
    AURORA_EXT_NESTED = 1
};
struct AuroraExternalMetaT
{
    const char *name;
    Int32 parent; /* -1 = SPAN_CORE */
    Uint8 relation;
};

static const AuroraExternalMetaT s_pceDeepMeta[AURORA_PCE_BUCKET_COUNT] =
{
    {"BRIDGE_RETRO_RUN", -1, AURORA_EXT_PARTITION},
    {"BRIDGE_RENDER", -1, AURORA_EXT_PARTITION},
    {"BRIDGE_MIX_FLUSH", -1, AURORA_EXT_PARTITION},
    {"RETRO_INPUT", AURORA_PCE_BRIDGE_RETRO_RUN, AURORA_EXT_PARTITION},
    {"EMULATE_TOTAL", AURORA_PCE_BRIDGE_RETRO_RUN, AURORA_EXT_PARTITION},
    {"RETRO_VIDEO_CB", AURORA_PCE_BRIDGE_RETRO_RUN, AURORA_EXT_PARTITION},
    {"RETRO_AUDIO_CB", AURORA_PCE_BRIDGE_RETRO_RUN, AURORA_EXT_PARTITION},
    {"EMU_INPUT_FRAME", AURORA_PCE_EMULATE_TOTAL, AURORA_EXT_PARTITION},
    {"VDC_FRAME", AURORA_PCE_EMULATE_TOTAL, AURORA_EXT_PARTITION},
    {"EMU_CD_FINAL", AURORA_PCE_EMULATE_TOTAL, AURORA_EXT_PARTITION},
    {"EMU_PSG_END", AURORA_PCE_EMULATE_TOTAL, AURORA_EXT_PARTITION},
    {"EMU_BLIP_READ", AURORA_PCE_EMULATE_TOTAL, AURORA_EXT_PARTITION},
    {"VDC_CPU", AURORA_PCE_VDC_FRAME, AURORA_EXT_PARTITION},
    {"VDC_BG", AURORA_PCE_VDC_FRAME, AURORA_EXT_PARTITION},
    {"VDC_SPR", AURORA_PCE_VDC_FRAME, AURORA_EXT_PARTITION},
    {"VDC_MIX", AURORA_PCE_VDC_FRAME, AURORA_EXT_PARTITION},
    {"VDC_OVERSCAN", AURORA_PCE_VDC_FRAME, AURORA_EXT_PARTITION},
    {"VDC_DMA", AURORA_PCE_VDC_FRAME, AURORA_EXT_PARTITION},
    {"VDC_SATDMA", AURORA_PCE_VDC_FRAME, AURORA_EXT_PARTITION},
    {"VDC_CD_SYNC", AURORA_PCE_VDC_FRAME, AURORA_EXT_PARTITION},
    {"PSG_UPDATE", -1, AURORA_EXT_NESTED},
    {"CD_RUN", -1, AURORA_EXT_NESTED},
    {"CD_DRIVE", AURORA_PCE_CD_RUN, AURORA_EXT_NESTED},
    {"CD_ADPCM", AURORA_PCE_CD_RUN, AURORA_EXT_NESTED},
    {"CD_FADER", AURORA_PCE_CD_RUN, AURORA_EXT_NESTED},

    /* AURORA_PCE_SPRITE_DEEP_PROFILER_V3_49_20261008 */
    {"SPR_PREP", AURORA_PCE_VDC_SPR, AURORA_EXT_PARTITION},
    {"SPR_CLEAR", AURORA_PCE_VDC_SPR, AURORA_EXT_PARTITION},
    {"SPR_DRAW", AURORA_PCE_VDC_SPR, AURORA_EXT_PARTITION},

    {"SPR_WORK_LINES", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_NONEMPTY_LINES", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_LINE_UNITS", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_NATIVE_SELECTED", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_POST_LIMIT", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_SOURCE_SPRITES", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_OVERFLOW_LINES", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_GE8_LINES", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_GE12_LINES", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_GE16_LINES", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_MAX_UNITS", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_MAX_SOURCES", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_OFFSCREEN", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_PARTIAL_CLIP", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_DRAWN_UNITS", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_ZERO_CHUNKS", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_OPAQUE_CHUNKS", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_MIXED_CHUNKS", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_SPRITE0_UNITS", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED},
    {"SPR_WORK_WRITTEN_PIXELS", AURORA_PCE_VDC_SPR, AURORA_EXT_NESTED}
};

static const AuroraExternalMetaT s_gbaDeepMeta[AURORA_GBA_BUCKET_COUNT] =
{
    {"FRAME_INPUT", -1, AURORA_EXT_PARTITION},
    {"CPU_EXEC", -1, AURORA_EXT_PARTITION},
    {"AUDIO_RUN", -1, AURORA_EXT_PARTITION},
    {"VIDEO_RUN", -1, AURORA_EXT_PARTITION},
    {"SERIAL_FRAME", -1, AURORA_EXT_PARTITION},
    {"UPDATE_GBA_TOTAL", AURORA_GBA_CPU_EXEC, AURORA_EXT_PARTITION},
    {"UPDATE_TIMERS", AURORA_GBA_UPDATE_GBA_TOTAL, AURORA_EXT_PARTITION},
    {"SCANLINE_TOTAL", AURORA_GBA_UPDATE_GBA_TOTAL, AURORA_EXT_PARTITION},
    {"SOUND_FRAME_FLUSH", AURORA_GBA_UPDATE_GBA_TOTAL, AURORA_EXT_PARTITION},
    {"DMA_TOTAL", AURORA_GBA_UPDATE_GBA_TOTAL, AURORA_EXT_NESTED},
    {"RASTER_WINDOW", AURORA_GBA_SCANLINE_TOTAL, AURORA_EXT_PARTITION},
    {"RASTER_TEXT_BG", AURORA_GBA_RASTER_WINDOW, AURORA_EXT_NESTED},
    {"RASTER_AFFINE_BG", AURORA_GBA_RASTER_WINDOW, AURORA_EXT_NESTED},
    {"RASTER_BITMAP_BG", AURORA_GBA_RASTER_WINDOW, AURORA_EXT_NESTED},
    {"RASTER_OBJ", AURORA_GBA_RASTER_WINDOW, AURORA_EXT_NESTED},
    {"RASTER_EFFECTS", AURORA_GBA_RASTER_WINDOW, AURORA_EXT_NESTED},
    {"DMA_CH0", AURORA_GBA_DMA_TOTAL, AURORA_EXT_PARTITION},
    {"DMA_CH1", AURORA_GBA_DMA_TOTAL, AURORA_EXT_PARTITION},
    {"DMA_CH2", AURORA_GBA_DMA_TOTAL, AURORA_EXT_PARTITION},
    {"DMA_CH3", AURORA_GBA_DMA_TOTAL, AURORA_EXT_PARTITION},
    {"AUDIO_READ", AURORA_GBA_AUDIO_RUN, AURORA_EXT_PARTITION},
    {"VIDEO_POSTPROCESS", AURORA_GBA_VIDEO_RUN, AURORA_EXT_PARTITION}
};

static const AuroraExternalMetaT s_segaDeepMeta[AURORA_SEGA_BUCKET_COUNT] =
{
    {"BRIDGE_CORE_RUN", -1, AURORA_EXT_PARTITION},
    {"BRIDGE_RENDER", -1, AURORA_EXT_PARTITION},
    {"BRIDGE_MIX_FLUSH", -1, AURORA_EXT_PARTITION},
    {"PICO_FRAME", AURORA_SEGA_BRIDGE_CORE_RUN, AURORA_EXT_PARTITION},
    {"FRAME_HINTS", AURORA_SEGA_PICO_FRAME, AURORA_EXT_PARTITION},
    {"M68K", AURORA_SEGA_FRAME_HINTS, AURORA_EXT_NESTED},
    {"Z80", AURORA_SEGA_FRAME_HINTS, AURORA_EXT_NESTED},
    {"VDP_SYNC", AURORA_SEGA_FRAME_HINTS, AURORA_EXT_NESTED},
    {"VDP_FULL", AURORA_SEGA_FRAME_HINTS, AURORA_EXT_NESTED},
    {"SOUND_TOTAL", AURORA_SEGA_FRAME_HINTS, AURORA_EXT_PARTITION},
    {"SOUND_PSG", AURORA_SEGA_FRAME_HINTS, AURORA_EXT_NESTED},
    {"SOUND_DAC", AURORA_SEGA_SOUND_TOTAL, AURORA_EXT_PARTITION},
    {"SOUND_FM", AURORA_SEGA_FRAME_HINTS, AURORA_EXT_NESTED},
    {"SOUND_PCM", AURORA_SEGA_SOUND_TOTAL, AURORA_EXT_PARTITION},
    {"SOUND_CDDA", AURORA_SEGA_SOUND_TOTAL, AURORA_EXT_PARTITION},
    {"SOUND_MIX", AURORA_SEGA_SOUND_TOTAL, AURORA_EXT_PARTITION},
    {"TIMERS", AURORA_SEGA_FRAME_HINTS, AURORA_EXT_PARTITION},
    {"MCD_S68K", AURORA_SEGA_BRIDGE_CORE_RUN, AURORA_EXT_NESTED}
};

static Bool WriteExternalDeepSet(FILE *fp, const char *title,
    const BucketT *set, Uint32 core,
    const AuroraExternalMetaT *meta, Uint32 count)
{
    const BucketT &b = set[core];
    const Uint64 frames = b.frames;
    if (!frames) return TRUE;
    if (fprintf(fp,
        "\n[%s]\n"
        "bucket,parent,relation,calls_avg_x100,units_avg_x100,cycles_avg,cycles_per_call_x1000,cycles_per_unit_x1000,pct_parent_x100\n",
        title) < 0) return FALSE;
    for (Uint32 i=0; i<count; ++i)
    {
        const Int32 parent = meta[i].parent;
        const Uint64 parentCycles = parent < 0
            ? b.span[SPAN_CORE] : b.externalCycles[(Uint32)parent];
        const char *parentName = parent < 0 ? "CORE" : meta[parent].name;
        if (fprintf(fp, "%s,%s,%s,%llu,%llu,%llu,%llu,%llu,%llu\n",
            meta[i].name, parentName,
            meta[i].relation == AURORA_EXT_PARTITION ? "partition" : "nested",
            (unsigned long long)SnesScaledDiv(b.externalCalls[i], frames, 100u),
            (unsigned long long)SnesScaledDiv(b.externalUnits[i], frames, 100u),
            (unsigned long long)Avg(b.externalCycles[i], frames),
            (unsigned long long)SnesScaledDiv(b.externalCycles[i], b.externalCalls[i], 1000u),
            (unsigned long long)SnesScaledDiv(b.externalCycles[i], b.externalUnits[i], 1000u),
            (unsigned long long)SnesScaledDiv(b.externalCycles[i], parentCycles, 10000u)) < 0)
            return FALSE;
    }
    return TRUE;
}

static Bool WriteExternalReconcileSet(FILE *fp, const char *title,
    const BucketT *set, Uint32 core,
    const AuroraExternalMetaT *meta, Uint32 count)
{
    const BucketT &b = set[core];
    const Uint64 frames = b.frames;
    if (!frames) return TRUE;
    if (fprintf(fp,
        "\n[%s]\nparent,parent_avg,partition_children_avg,residual_avg,coverage_pct_x100\n",
        title) < 0) return FALSE;

    for (Int32 parent=-1; parent<(Int32)count; ++parent)
    {
        Uint64 child = 0;
        Uint32 n = 0;
        for (Uint32 i=0; i<count; ++i)
        {
            if (meta[i].relation == AURORA_EXT_PARTITION && meta[i].parent == parent)
            {
                child += b.externalCycles[i];
                ++n;
            }
        }
        if (!n) continue;
        const Uint64 parentCycles = parent < 0
            ? b.span[SPAN_CORE] : b.externalCycles[(Uint32)parent];
        const Uint64 residual = parentCycles >= child ? parentCycles - child : 0u;
        const char *name = parent < 0 ? "CORE" : meta[parent].name;
        if (fprintf(fp, "%s,%llu,%llu,%llu,%llu\n",
            name,
            (unsigned long long)Avg(parentCycles, frames),
            (unsigned long long)Avg(child, frames),
            (unsigned long long)Avg(residual, frames),
            (unsigned long long)SnesScaledDiv(child, parentCycles, 10000u)) < 0)
            return FALSE;
    }
    return TRUE;
}

static Bool WriteExternalCoreProfiler(FILE *fp, const char *suffix,
    const BucketT *set, Uint32 core)
{
    char deepTitle[48];
    char reconTitle[48];
    const char *prefix = NULL;
    const AuroraExternalMetaT *meta = NULL;
    Uint32 count = 0;
    switch(core)
    {
        case AURORA_FP_CORE_PCE:
            prefix="PCE"; meta=s_pceDeepMeta; count=AURORA_PCE_BUCKET_COUNT; break;
        case AURORA_FP_CORE_GBA:
            prefix="GBA"; meta=s_gbaDeepMeta; count=AURORA_GBA_BUCKET_COUNT; break;
        case AURORA_FP_CORE_SEGA:
            prefix="SEGA"; meta=s_segaDeepMeta; count=AURORA_SEGA_BUCKET_COUNT; break;
        default:
            return TRUE;
    }
    snprintf(deepTitle, sizeof(deepTitle), "%s_DEEP_%s", prefix, suffix);
    snprintf(reconTitle, sizeof(reconTitle), "%s_RECONCILE_%s", prefix, suffix);
    return WriteExternalDeepSet(fp, deepTitle, set, core, meta, count) &&
           WriteExternalReconcileSet(fp, reconTitle, set, core, meta, count);
}

/* AURORA_PCE_SPRITE_DEEP_PROFILER_V3_49_20261008 */
static Bool WritePceSpriteWorkSet(FILE *fp, const char *title,
    const BucketT *set)
{
    const BucketT &b = set[AURORA_FP_CORE_PCE];
    const Uint64 frames = b.frames;
    if (!frames) return TRUE;

    const Uint64 lines = b.externalUnits[AURORA_PCE_SPR_WORK_LINES];
    const Uint64 nonempty = b.externalUnits[AURORA_PCE_SPR_WORK_NONEMPTY_LINES];
    const Uint64 units = b.externalUnits[AURORA_PCE_SPR_WORK_LINE_UNITS];
    const Uint64 nativeSelected = b.externalUnits[AURORA_PCE_SPR_WORK_NATIVE_SELECTED];
    const Uint64 postLimit = b.externalUnits[AURORA_PCE_SPR_WORK_POST_LIMIT];
    const Uint64 sources = b.externalUnits[AURORA_PCE_SPR_WORK_SOURCE_SPRITES];
    const Uint64 overflow = b.externalUnits[AURORA_PCE_SPR_WORK_OVERFLOW_LINES];
    const Uint64 ge8 = b.externalUnits[AURORA_PCE_SPR_WORK_GE8_LINES];
    const Uint64 ge12 = b.externalUnits[AURORA_PCE_SPR_WORK_GE12_LINES];
    const Uint64 ge16 = b.externalUnits[AURORA_PCE_SPR_WORK_GE16_LINES];
    const Uint64 maxUnits = b.externalUnits[AURORA_PCE_SPR_WORK_MAX_UNITS];
    const Uint64 maxSources = b.externalUnits[AURORA_PCE_SPR_WORK_MAX_SOURCES];
    const Uint64 offscreen = b.externalUnits[AURORA_PCE_SPR_WORK_OFFSCREEN];
    const Uint64 partial = b.externalUnits[AURORA_PCE_SPR_WORK_PARTIAL_CLIP];
    const Uint64 drawn = b.externalUnits[AURORA_PCE_SPR_WORK_DRAWN_UNITS];
    const Uint64 zeroChunks = b.externalUnits[AURORA_PCE_SPR_WORK_ZERO_CHUNKS];
    const Uint64 opaqueChunks = b.externalUnits[AURORA_PCE_SPR_WORK_OPAQUE_CHUNKS];
    const Uint64 mixedChunks = b.externalUnits[AURORA_PCE_SPR_WORK_MIXED_CHUNKS];
    const Uint64 sprite0 = b.externalUnits[AURORA_PCE_SPR_WORK_SPRITE0_UNITS];
    const Uint64 pixels = b.externalUnits[AURORA_PCE_SPR_WORK_WRITTEN_PIXELS];
    const Uint64 chunks = zeroChunks + opaqueChunks + mixedChunks;

    if (fprintf(fp,
        "\n[%s]\n"
        "frames,spr_lines_avg_x100,nonempty_line_pct_x100,line_units_avg_x100,units_per_line_x1000,native_selected_avg_x100,post_limit_avg_x100,source_sprites_avg_x100,sources_per_nonempty_line_x1000,overflow_lines_avg_x100,overflow_line_pct_x100,ge8_line_pct_x100,ge12_line_pct_x100,ge16_line_pct_x100,max_units_per_frame_avg_x100,max_sources_per_frame_avg_x100,offscreen_avg_x100,partial_clip_avg_x100,drawn_units_avg_x100,zero_chunks_avg_x100,opaque_chunks_avg_x100,mixed_chunks_avg_x100,zero_chunk_pct_x100,opaque_chunk_pct_x100,mixed_chunk_pct_x100,sprite0_units_avg_x100,written_pixels_avg_x100,written_pixels_per_drawn_unit_x1000\n"
        "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
        title,
        (unsigned long long)frames,
        (unsigned long long)SnesScaledDiv(lines, frames, 100u),
        (unsigned long long)SnesScaledDiv(nonempty, lines, 10000u),
        (unsigned long long)SnesScaledDiv(units, frames, 100u),
        (unsigned long long)SnesScaledDiv(units, lines, 1000u),
        (unsigned long long)SnesScaledDiv(nativeSelected, frames, 100u),
        (unsigned long long)SnesScaledDiv(postLimit, frames, 100u),
        (unsigned long long)SnesScaledDiv(sources, frames, 100u),
        (unsigned long long)SnesScaledDiv(sources, nonempty, 1000u),
        (unsigned long long)SnesScaledDiv(overflow, frames, 100u),
        (unsigned long long)SnesScaledDiv(overflow, lines, 10000u),
        (unsigned long long)SnesScaledDiv(ge8, lines, 10000u),
        (unsigned long long)SnesScaledDiv(ge12, lines, 10000u),
        (unsigned long long)SnesScaledDiv(ge16, lines, 10000u),
        (unsigned long long)SnesScaledDiv(maxUnits, frames, 100u),
        (unsigned long long)SnesScaledDiv(maxSources, frames, 100u),
        (unsigned long long)SnesScaledDiv(offscreen, frames, 100u),
        (unsigned long long)SnesScaledDiv(partial, frames, 100u),
        (unsigned long long)SnesScaledDiv(drawn, frames, 100u),
        (unsigned long long)SnesScaledDiv(zeroChunks, frames, 100u),
        (unsigned long long)SnesScaledDiv(opaqueChunks, frames, 100u),
        (unsigned long long)SnesScaledDiv(mixedChunks, frames, 100u),
        (unsigned long long)SnesScaledDiv(zeroChunks, chunks, 10000u),
        (unsigned long long)SnesScaledDiv(opaqueChunks, chunks, 10000u),
        (unsigned long long)SnesScaledDiv(mixedChunks, chunks, 10000u),
        (unsigned long long)SnesScaledDiv(sprite0, frames, 100u),
        (unsigned long long)SnesScaledDiv(pixels, frames, 100u),
        (unsigned long long)SnesScaledDiv(pixels, drawn, 1000u)) < 0)
        return FALSE;
    return TRUE;
}

static Bool WriteV45Cadence(FILE *fp, const StatsT &s)
{
    if (fprintf(fp,
        "\n[V45_VBLANK_CADENCE]\n"
        "core,presented,skipped,period_samples,period_avg,preflip_work_avg,"
        "preflip_over_1p,preflip_over_2p,syncflip_wait_avg,"
        "interval_pairs,interval_gap_avg,nominal_gap_avg,"
        "excess_gap_avg,excess_gap_gt_half_p,pairs_after_skips,"
        "skips_between,pairs_lt_1p,pairs_1_2p,pairs_2_3p,"
        "pairs_3_4p,pairs_ge_4p\n") < 0)
        return FALSE;
    for (Uint32 core = 0; core < AURORA_FP_CORE_COUNT; ++core)
    {
        const V45CadenceT &c = s.v45[core];
        if (!c.presented && !c.skipped) continue;
        if (fprintf(fp,
            "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%llu,%llu,%llu,%llu,%llu\n",
            CoreName(core),
            (unsigned long long)c.presented,
            (unsigned long long)c.skipped,
            (unsigned long long)c.periodSamples,
            (unsigned long long)Avg(c.periodSum,c.periodSamples),
            (unsigned long long)Avg(c.preWorkSum,c.periodSamples),
            (unsigned long long)c.preWorkOverPeriod,
            (unsigned long long)c.preWorkOver2Periods,
            (unsigned long long)Avg(c.waitSum,c.periodSamples),
            (unsigned long long)c.intervalPairs,
            (unsigned long long)Avg(c.gapSum,c.intervalPairs),
            (unsigned long long)Avg(c.expectedGapSum,c.intervalPairs),
            (unsigned long long)Avg(c.excessGapSum,c.intervalPairs),
            (unsigned long long)c.gapExcessHalfPeriod,
            (unsigned long long)c.pairsAfterSkips,
            (unsigned long long)c.skipsBetweenSum,
            (unsigned long long)c.gapHistogram[0],
            (unsigned long long)c.gapHistogram[1],
            (unsigned long long)c.gapHistogram[2],
            (unsigned long long)c.gapHistogram[3],
            (unsigned long long)c.gapHistogram[4]) < 0)
            return FALSE;
    }
    return TRUE;
}

static Bool WriteSafeFrameskipDiagnostic(FILE *fp, const StatsT &s)
{
    if (fprintf(fp,
        "\n[SAFE_FRAMESKIP_DIAGNOSTIC]\n"
        "core,decisions,debt,overrun_trigger,unlimited,skips,"
        "recovery_pending_decisions,recovery_presented,recovery_rebases,"
        "period_avg,lateness_avg,last_presented_work_avg,recovery_work_avg\n") < 0)
        return FALSE;

    for (Uint32 core = 0; core < AURORA_FP_CORE_COUNT; ++core)
    {
        const FsDiagT &d = s.fsDiag[core];
        if (!d.decisions && !d.recoveryPresented)
            continue;

        if (fprintf(fp,
            "%s,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu,"
            "%llu,%llu,%llu,%llu\n",
            CoreName(core),
            (unsigned long long)d.decisions,
            (unsigned long long)d.debtDecisions,
            (unsigned long long)d.overrunDecisions,
            (unsigned long long)d.unlimitedDecisions,
            (unsigned long long)d.skippedDecisions,
            (unsigned long long)d.recoveryPendingDecisions,
            (unsigned long long)d.recoveryPresented,
            (unsigned long long)d.recoveryRebases,
            (unsigned long long)Avg(d.periodSum, d.decisions),
            (unsigned long long)Avg(d.latenessSum, d.latenessSamples),
            (unsigned long long)Avg(
                d.lastPresentedWorkSum, d.lastPresentedWorkSamples),
            (unsigned long long)Avg(
                d.recoveryWorkSum, d.recoveryPresented)) < 0)
            return FALSE;
    }
    return TRUE;
}

/* AURORA_V45_VBLANK_CADENCE_PROFILER_ONLY_20261011
 * Called once at the end of each GAMEPLAY host tick.  Timebase is COP0 COUNT;
 * all recorded timestamps come from reads the existing profiler already took.
 * 'expected gap' is ONLY a comparison heuristic based on hidden host ticks,
 * not a counter of missed hardware vertical retraces. */
static void V45RecordCadence(Uint32 core)
{
    if (core >= AURORA_FP_CORE_COUNT) return;
    V45CadenceT &c = s_live.v45[core];
    if (s_frame.fsSkipped)
    {
        ++c.skipped;
        if (c.haveLastPresent && c.skipsSincePresent < 1000000u)
            ++c.skipsSincePresent;
        return;
    }
    if (!s_frame.v45Presented)
    {
        /* Interrupted presentation/UI transition: do not bridge time gaps. */
        c.haveLastPresent = FALSE;
        c.skipsSincePresent = 0u;
        return;
    }
    ++c.presented;
    const Uint32 period = s_frame.fsDetailSeen ? s_frame.fsPeriod : 0u;
    if (period != 0u)
    {
        ++c.periodSamples;
        c.periodSum += period;
        c.preWorkSum += s_frame.v45PreFlipWork;
        c.waitSum += s_frame.cycles[SPAN_VBLANK];
        if (s_frame.v45PreFlipWork > period) ++c.preWorkOverPeriod;
        if ((Uint64)s_frame.v45PreFlipWork > (Uint64)period * 2u)
            ++c.preWorkOver2Periods;
    }
    if (c.haveLastPresent && c.lastPresentPeriod != 0u && period != 0u)
    {
        /* Modular subtraction supports one wrap of COP0 COUNT. */
        const Uint32 gap = (Uint32)(s_frame.v45FlipStamp - c.lastPresentStamp);
        const Uint32 refPeriod = (period / 2u) + (c.lastPresentPeriod / 2u)
            + ((period & 1u) && (c.lastPresentPeriod & 1u) ? 1u : 0u);
        const Uint64 expected = (Uint64)refPeriod * (1u + c.skipsSincePresent);
        ++c.intervalPairs;
        c.gapSum += gap;
        c.expectedGapSum += expected;
        if (gap > expected)
            c.excessGapSum += (Uint64)gap - expected;
        if ((Uint64)gap > expected + (Uint64)refPeriod / 2u)
            ++c.gapExcessHalfPeriod;
        if (c.skipsSincePresent)
        {
            ++c.pairsAfterSkips;
            c.skipsBetweenSum += c.skipsSincePresent;
        }
        const Uint64 g = gap, p = refPeriod;
        const Uint32 bin = g < p ? 0u :
                           g < 2u*p ? 1u :
                           g < 3u*p ? 2u :
                           g < 4u*p ? 3u : 4u;
        ++c.gapHistogram[bin];
    }
    c.lastPresentStamp = s_frame.v45FlipStamp;
    c.lastPresentPeriod = period;
    c.skipsSincePresent = 0u;
    c.haveLastPresent = TRUE;
}

static void V45BreakCadenceChain(void)
{
    /* Not clearing aggregate counters: just isolate menu/unload transitions. */
    for (Uint32 core = 0; core < AURORA_FP_CORE_COUNT; ++core)
    {
        s_live.v45[core].haveLastPresent = FALSE;
        s_live.v45[core].skipsSincePresent = 0u;
    }
}

static void AccumulateSafeFrameskipDiagnostic(Uint32 core)
{
    if (core >= AURORA_FP_CORE_COUNT)
        return;

    FsDiagT &d = s_live.fsDiag[core];

    if (s_frame.fsDetailSeen)
    {
        ++d.decisions;
        d.periodSum += s_frame.fsPeriod;
        if (s_frame.fsMeaningfulDebt) ++d.debtDecisions;
        if (s_frame.fsMeasuredOverrun) ++d.overrunDecisions;
        if (s_frame.fsUnlimited) ++d.unlimitedDecisions;
        if (s_frame.fsDecisionSkipped) ++d.skippedDecisions;
        if (s_frame.fsRecoveryPendingAtDecision)
            ++d.recoveryPendingDecisions;
        if (s_frame.fsDiff < 0)
        {
            d.latenessSum += (Uint32)(0u - (Uint32)s_frame.fsDiff);
            ++d.latenessSamples;
        }
        if (s_frame.fsLastPresentedWork)
        {
            d.lastPresentedWorkSum += s_frame.fsLastPresentedWork;
            ++d.lastPresentedWorkSamples;
        }
    }

    if (s_frame.fsRecoverySeen && s_frame.fsRecoveryPendingAtPresent)
    {
        ++d.recoveryPresented;
        d.recoveryWorkSum += s_frame.fsRecoveryWork;
        if (s_frame.fsRecoveryRebased)
            ++d.recoveryRebases;
    }
}


/* AURORA_PROFILER_ROM_IDENTITY_V3_1_20261005 */
static Bool WriteIdentityHeader(FILE *fp)
{
    const MetadataT &m = s_pending.meta;

    if (fprintf(fp,
        "game_title=%s\n"
        "rom_filename=%s\n",
        m.gameTitle[0] ? m.gameTitle : "UNKNOWN",
        m.romFilename[0] ? m.romFilename : "UNKNOWN") < 0)
        return FALSE;

    if (m.runtimeCRCValid)
    {
        if (fprintf(fp, "runtime_crc32=%08X\n",
                    (unsigned)m.runtimeCRC32) < 0)
            return FALSE;
    }
    else if (fprintf(fp, "runtime_crc32=UNKNOWN\n") < 0)
    {
        return FALSE;
    }

    return fprintf(fp,
        "core=%s\n"
        "region=%s\n",
        CoreName(m.core),
        m.region[0] ? m.region : "UNKNOWN") >= 0
        ? TRUE : FALSE;
}

/* AURORA_PCE_V3_DIAGNOSTIC_20261008: PCE-only dump, profiling TU only. */
extern "C" void PCE_AuroraPceV3WriteDiag(FILE *fp);

/* AURORA_V25_PROFILE_GUIDED_20261010: report-only metrics, no hotpath hooks.
 * Counts and costs are observational and do not prove counterfactual savings.
 * Bucket dimensions: depth 0=2bpp, 1=4bpp; deep opaque/transparent units.
 */
extern "C" void PCE_AuroraV25PceBmtSnapshot(Uint32 *out);
static Bool WriteV25CandidateReport(FILE *fp, const StatsT &s)
{
    if (fprintf(fp,
        "\n[V25_BUILD_FEATURES]\n"
        "feature,status\n"
        "build,v25.4-bmt-isolated-20261010\n"
        "snes_chr2_raw_zero,v24-enabled\n"
        "snes_chr4_branchless,v24-experimental\n"
        "pce_bmt_delay_slot,v24-enabled\n"
        "pce_bmt_counter,isolated-noinline-v25.4\n") < 0) return FALSE;
    const BucketT &b = s.coreNormal[AURORA_FP_CORE_SNES];
    const Uint64 f = b.frames;
    if (fprintf(fp,
        "\n[V25_SNES_KERNEL_NORMAL]\n"
        "depth,calls_avg_x100,tiles_avg_x100,decode_avg,post_avg,"
        "decode_per_tile_x1000,transparent_rows_avg_x100,"
        "opaque_rows_avg_x100,flip0_opaque_avg_x100\n") < 0) return FALSE;
    for (Uint32 depth = 0; depth < 2u; ++depth)
    {
        Uint64 calls=0,tiles=0,cycles=0,decode=0;
        for(Uint32 layer=0;layer<4u;++layer)
        {
            calls += b.snesBgChrCalls[layer][depth];
            tiles += b.snesBgChrTiles[layer][depth];
            cycles += b.snesBgChrCycles[layer][depth];
            decode += b.snesBgChrDecodeCycles[layer][depth];
        }
        const Uint32 base = depth ? 44u : 42u;
        const Uint32 flip = depth ? 48u : 46u;
        if (fprintf(fp,"%u,%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu\n",
            (unsigned)(depth ? 4u : 2u),
            (unsigned long long)SnesScaledDiv(calls,f,100u),
            (unsigned long long)SnesScaledDiv(tiles,f,100u),
            (unsigned long long)Avg(decode,f),
            (unsigned long long)Avg(cycles>=decode ? cycles-decode:0u,f),
            (unsigned long long)SnesScaledDiv(decode,tiles,1000u),
            (unsigned long long)SnesScaledDiv(b.snesDeepUnits[base+1u],f,100u),
            (unsigned long long)SnesScaledDiv(b.snesDeepUnits[base],f,100u),
            (unsigned long long)SnesScaledDiv(b.snesDeepUnits[flip],f,100u)) < 0)
            return FALSE;
    }
    const Uint64 parent = b.snesPpuCycles[5];
    const Uint64 measured = b.snesBgChrMeasuredCycles;
    const Uint64 raw = parent >= measured ? parent - measured : 0u;
    const Uint64 traits = b.snesBgChrTraitsCycles;
    const Uint64 rest = raw >= traits ? raw - traits : 0u;
    if (fprintf(fp,
        "\n[V25_SNES_BGCHR_RESIDUAL_NORMAL]\n"
        "frames,bgchr_avg,measured_avg,traits_avg,outer_avg,outer_ex_traits_avg\n"
        "%llu,%llu,%llu,%llu,%llu,%llu\n",
        (unsigned long long)f,
        (unsigned long long)Avg(parent,f),
        (unsigned long long)Avg(measured,f),
        (unsigned long long)Avg(traits,f),
        (unsigned long long)Avg(raw,f),
        (unsigned long long)Avg(rest,f)) < 0) return FALSE;
    if (s_pending.meta.core == AURORA_FP_CORE_PCE)
    {
        Uint32 v[8] = {0};
        PCE_AuroraV25PceBmtSnapshot(v);
        return fprintf(fp,
            "\n[V25_PCE_BMT_USAGE]\n"
            "reset_domain,calls_tii,bytes_tii,calls_tdd,bytes_tdd,"
            "sampled_tii,ticks_tii,sampled_tdd,ticks_tdd\n"
            "since_last_HuC6280_Reset,%u,%u,%u,%u,%u,%u,%u,%u\n",
            (unsigned)v[0], (unsigned)v[1], (unsigned)v[2], (unsigned)v[3],
            (unsigned)v[4], (unsigned)v[5], (unsigned)v[6], (unsigned)v[7]) >= 0;
    }
    return TRUE;
}

static Bool WriteDump(FILE *fp)
{
    const StatsT &s = s_pending.stats;

    if (fprintf(fp,
        "SNESticleAurora frontend profiler v4.11\n"
        "schema=aurora-frontend-v4.11\n"
        "timestamp=%04u-%02u-%02u %02u:%02u:%02u\n"
        "sample_frames=%llu gameplay=%llu menu_with_core=%llu idle_ui=%llu\n"
        "normal_frames=%llu spike_frames=%llu "
        "frontend_spike_budget_cycles=%u\n",
        (unsigned)s_pending.stamp.year,
        (unsigned)s_pending.stamp.month,
        (unsigned)s_pending.stamp.day,
        (unsigned)s_pending.stamp.hour,
        (unsigned)s_pending.stamp.minute,
        (unsigned)s_pending.stamp.second,
        (unsigned long long)s.totalFrames,
        (unsigned long long)s.gameplayFrames,
        (unsigned long long)s.menuFrames,
        (unsigned long long)s.idleFrames,
        (unsigned long long)s.normalFrames,
        (unsigned long long)s.spikeFrames,
        (unsigned)AURORA_FP_FRONTEND_SPIKE_BUDGET) < 0)
        return FALSE;

    if (!WriteIdentityHeader(fp))
        return FALSE;
    if (!WriteV25CandidateReport(fp, s)) return FALSE;

    if (s_pending.meta.core == AURORA_FP_CORE_PCE)
        PCE_AuroraPceV3WriteDiag(fp);

    if (fprintf(fp,
        "\n[METRIC_NOTES]\n"
        "frontend_ex_core_vblank = whole host tick - measured core - blocking GSK_SyncFlip.\n"
        "render_ex_vblank = MainLoopRender total - blocking GSK_SyncFlip.\n"
        "render_ex_vblank_audio = render_ex_vblank - measured async audio drain.\n"
        "NORMAL = frontend_ex_core_vblank <= frontend_spike_budget_cycles.\n"
        "SPIKE = frontend_ex_core_vblank > frontend_spike_budget_cycles.\n"
        "pre_core_or_render = after frontend input until core begin; in UI/no-core, until render begin.\n"
        "post_core = exact core end until render begin; upload is a nested diagnostic subset.\n"
        "other_frontend = residual after the top-level exclusive partition.\n"
        "snes_core buckets are exclusive host-cost scopes; residual is core minus their sum.\n"
        "snes_ppu detail scopes are exclusive; OBJ fetch/main/sub are phase-labeled.\n"
        "SNES DMA spans are drill-downs and are not added to core tracked totals.\n"
        "PPU work counters are counts only; they do not alter renderer policy.\n") < 0)
        return FALSE;

    if (!WriteStateSet(fp, "STATE_ALL", s.stateAll)) return FALSE;
    if (!WriteStateSet(fp, "STATE_NORMAL", s.stateNormal)) return FALSE;
    if (!WriteStateSet(fp, "STATE_SPIKE", s.stateSpike)) return FALSE;

    if (!WriteCoreSet(fp, "CORE_GAMEPLAY_ALL", s.coreAll)) return FALSE;
    if (!WriteCoreSet(fp, "CORE_GAMEPLAY_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteCoreSet(fp, "CORE_GAMEPLAY_SPIKE", s.coreSpike)) return FALSE;

    if (!WriteSafeFrameskipSummary(fp, s)) return FALSE;
    if (!WriteSafeFrameskipSet(fp, "SAFE_FRAMESKIP_ALL", s.fsAll)) return FALSE;
    if (!WriteSafeFrameskipSet(fp, "SAFE_FRAMESKIP_NORMAL", s.fsNormal)) return FALSE;
    if (!WriteSafeFrameskipSet(fp, "SAFE_FRAMESKIP_SPIKE", s.fsSpike)) return FALSE;
    if (!WriteSafeFrameskipDiagnostic(fp, s)) return FALSE;
    if (!WriteV45Cadence(fp, s)) return FALSE; /* AURORA_V45_VBLANK_CADENCE_PROFILER_ONLY_20261011 */

    if (!WriteAudioSet(fp, "AURORA_AUDIO_ALL", s.coreAll)) return FALSE;
    if (!WriteAudioSet(fp, "AURORA_AUDIO_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesAudioSet(fp, "SNES_AUDIO_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesAudioSet(fp, "SNES_AUDIO_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesDspVoicesSet(fp, "SNES_DSP_VOICES_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesDspVoicesSet(fp, "SNES_DSP_VOICES_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesDspVoiceGainSet(fp, "SNES_DSP_VOICE_GAIN_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesDspVoiceGainSet(fp, "SNES_DSP_VOICE_GAIN_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesDspVoiceSampleCostSet(fp, "SNES_DSP_VOICE_SAMPLE_COST_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesDspVoiceSampleCostSet(fp, "SNES_DSP_VOICE_SAMPLE_COST_NORMAL", s.coreNormal)) return FALSE;

    if (!WriteSnesCoreBreakdownSet(fp, "SNES_CORE_BREAKDOWN_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesCoreBreakdownSet(fp, "SNES_CORE_BREAKDOWN_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesPpuBreakdownSet(fp, "SNES_PPU_BREAKDOWN_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesPpuBreakdownSet(fp, "SNES_PPU_BREAKDOWN_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesPpuWorkSet(fp, "SNES_PPU_WORK_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesPpuWorkSet(fp, "SNES_PPU_WORK_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesDmaBreakdownSet(fp, "SNES_DMA_BREAKDOWN_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesDmaBreakdownSet(fp, "SNES_DMA_BREAKDOWN_NORMAL", s.coreNormal)) return FALSE;

    if (!WriteSnesBgChrSummarySet(fp, "SNES_BG_CHR_SUMMARY_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesBgChrSummarySet(fp, "SNES_BG_CHR_SUMMARY_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesBgChrTraitsCostSet(fp, "SNES_BG_CHR_TRAITS_COST_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesBgChrTraitsCostSet(fp, "SNES_BG_CHR_TRAITS_COST_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesBgChrLayerDepthSet(fp, "SNES_BG_CHR_LAYER_DEPTH_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesBgChrLayerDepthSet(fp, "SNES_BG_CHR_LAYER_DEPTH_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesBgChrFlagsSet(fp, "SNES_BG_CHR_FLAGS_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesBgChrFlagsSet(fp, "SNES_BG_CHR_FLAGS_NORMAL", s.coreNormal)) return FALSE;

    if (!WriteSnesChr4LocalitySet(fp, "SNES_CHR4_LOCALITY_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesChr4LocalitySet(fp, "SNES_CHR4_LOCALITY_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesObjMicroscopeSet(fp, "SNES_OBJ_MICROSCOPE_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesObjMicroscopeSet(fp, "SNES_OBJ_MICROSCOPE_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesChr4ReuseSet(fp, "SNES_CHR4_REUSE_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesChr4ReuseSet(fp, "SNES_CHR4_REUSE_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesChr4ReuseMatrixSet(fp, "SNES_CHR4_REUSE_MATRIX_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesChr4ReuseMatrixSet(fp, "SNES_CHR4_REUSE_MATRIX_NORMAL", s.coreNormal)) return FALSE;

    if (!WriteSnesDeepSet(fp, "SNES_DEEP_BREAKDOWN_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesDeepSet(fp, "SNES_DEEP_BREAKDOWN_NORMAL", s.coreNormal)) return FALSE;
    if (!WriteSnesDeepReconcileSet(fp, "SNES_DEEP_RECONCILE_ALL", s.coreAll)) return FALSE;
    if (!WriteSnesDeepReconcileSet(fp, "SNES_DEEP_RECONCILE_NORMAL", s.coreNormal)) return FALSE;

    if (!WriteExternalCoreProfiler(fp, "ALL", s.coreAll, s_pending.meta.core)) return FALSE;
    if (!WriteExternalCoreProfiler(fp, "NORMAL", s.coreNormal, s_pending.meta.core)) return FALSE;
    if (!WritePceSpriteWorkSet(fp, "PCE_SPRITE_WORK_ALL", s.coreAll)) return FALSE;
    if (!WritePceSpriteWorkSet(fp, "PCE_SPRITE_WORK_NORMAL", s.coreNormal)) return FALSE;

    if (fprintf(fp,
        "\n[WORST_FRONTEND]\n"
        "rank,host_tick,class,state,core,fs_class,dominant,tick,frontend,"
        "core,vblank,input_poll,input_snapshot_prepare,input_logic,"
        "pre_core_or_render,post_core,upload,render_host,audio_drain,"
        "post_render,other\n") < 0)
        return FALSE;

    for (Int32 i = 0; i < 16; ++i)
    {
        const WorstT &w = s.worst[i];
        if (!w.frontend)
            continue;

        if (fprintf(fp,
            "%d,%u,%s,%s,%s,%s,%s,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u\n",
            i + 1,
            (unsigned)w.serial,
            w.spike ? "SPIKE" : "NORMAL",
            StateName(w.state),
            CoreName(w.core),
            SafeFrameskipClassName(w.fsClass),
            DominantName(w),
            (unsigned)w.tick,
            (unsigned)w.frontend,
            (unsigned)w.coreCycles,
            (unsigned)w.vblank,
            (unsigned)w.inputPoll,
            (unsigned)w.inputSnapshot,
            (unsigned)w.inputLogic,
            (unsigned)w.preCore,
            (unsigned)w.postCore,
            (unsigned)w.upload,
            (unsigned)w.renderHost,
            (unsigned)w.audioDrain,
            (unsigned)w.postRender,
            (unsigned)w.other) < 0)
            return FALSE;
    }

    if (fprintf(fp,
        "\n[NOTES]\n"
        "- Diagnostic-only: AURORA_FRONTEND_PROFILER=0 still removes the implementation TU and callsites.\n"
        "- Supports SNES, NES/QuickNES, FDS/FCEUmm, Sega/PicoDrive, PCE, GB/Gambatte and GBA/gpSP.\n"
        "- SPIKE split is report-only; it does not suppress, delay, skip, or reschedule emulator work.\n"
        "- input_snapshot_prepare includes pad snapshots plus tiny button-OR preparation before _MainLoopInputProcess.\n"
        "- upload is nested in post_core and must not be added to post_core again.\n"
        "- render_setup/game_draw/ui/gp_flush/audio_drain are nested in render_total.\n"
        "- audio_drain measures existing Aud_BufferedAsyncStart call(s); audio behavior is unchanged.\n"
        "- FS telemetry records debt/period/recovery in the same host tick as frontend/audio data.\n"
        "- v4.6 host scheduler may rebase stale NORMAL-policy debt only after a real post-skip presentation proves host work fits one learned period.\n"
        "- CRC unlimited policy is excluded from that recovery rebase.\n"
        "- FS_OFF / FS_ON_PRESENTED / FS_ON_SKIPPED / FS_ON_BLOCKED are separated per core; mixed sessions remain analyzable.\n"
        "- SNES SPC requested/consumed cycles and DSP samples/chunks are read-only diagnostics; emulated audio timing is not changed.\n"
        "- skip_pct_x100 is percentage times 100 among allowed FS_ON ticks (e.g. 1250 = 12.50%%).\n"
        "- Standalone auroraprofile is the clean frontend baseline; do not combine with c4profile for final numbers.\n"
        "- v4.7 CHR4 locality is collected after BGCHR measured end; use SNES_BG_CHR measured fields for cross-build decode timing.\n"
        "- v4.7 OBJ per-tile path timers include diagnostic counter-read overhead; compare HIT vs MISS inside the same v4.7 build.\n"
        "- CHR4 palette_runs is the predicted minimum LUT-load count for a one-register cache across opaque rows; transparent rows preserve the candidate palette.\n"
        "- CHR4/OBJ row +/-16 classifications use wrapped 0x8000-word VRAM deltas.\n"
        "- v4.8 CHR4_REUSE compares opaque rows to the previous opaque row; transparent rows do not evict the hypothetical one-entry register cache.\n"
        "- decode_key means resolved source-row + H-flip; pixel_key additionally includes palette index (uPal & 7). Priority bit remains independent.\n"
        "- adjacent_* ratios require the immediately previous physical tile to be opaque and show the simpler no-gap reuse ceiling.\n"
        "- v4.9 CHR4_REUSE_MATRIX splits 4bpp reuse by BG0..3 and actual path: NOOFFSET/OFFSET/MODE5_MAIN/MODE5_SUB/MODE6_MAIN/MODE6_SUB.\n"
        "- Mode 5/6 decode_key uses both N/N+1 source-row addresses plus H-flip; MAIN/SUB are separate because their hires packing differs.\n"
        "- General BG-by-BG timing is not duplicated here: SNES_BG_CHR_LAYER_DEPTH_* already reports cycles/tiles per BG and bit depth.\n"
        "- v4.10 OBJ_COMPACT cycles are only the post-fetch R5900 compactor; units are fully-transparent rows removed. OBJ_FETCH_DECODE units remain physical fetched rows.\n"
        "- v4.11 PCE/GBA/SEGA deep sections are generic (no CRC whitelist); relation=partition is additive, relation=nested is drill-down and may overlap another child.\n"
        "- *_RECONCILE_* sums only direct partition children; residual exposes still-unattributed parent cost.\n"
        "- v3.49 PCE SPR_PREP/CLEAR/DRAW partition VDC_SPR; PCE_SPRITE_WORK_* units are semantic counts, not emulated cycles.\n"
        "- PCE sprite work counts 16-pixel SAT-cache units; SOURCE_SPRITES collapses adjacent halves back to original SAT entries after Aurora limiting.\n"
        "- v3.39 Direct Color scratch-LUT impact remains attributable to existing BG_MAIN_DIRECT/BG_SUB_DIRECT deep buckets.\n"
        "- No filesystem write occurs during gameplay. Menu entry waits 60 visible menu frames before snapshot.\n"
        "- Initial no-core browser auto-snapshots once after 120 IDLE_UI frames.\n") < 0)
        return FALSE;

    return ferror(fp) ? FALSE : TRUE;
}

static Bool SavePending(void)
{
    Char path[192];
    const StampT &t = s_pending.stamp;
    snprintf(path, sizeof(path),
        "mass0:/SNESticle/auroraprof_%04u%02u%02u_%02u%02u%02u_%03u.txt",
        (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
        (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second,
        (unsigned)s_pending.serial);

    BgmIOBegin();
    FILE *fp = fopen(path, "wb");
    if (!fp)
    {
        snprintf(path, sizeof(path),
            "mass0:/AURORAPROF_%04u%02u%02u_%02u%02u%02u_%03u.txt",
            (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
            (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second,
            (unsigned)s_pending.serial);
        fp = fopen(path, "wb");
    }

    Bool ok = fp ? WriteDump(fp) : FALSE;
    if (fp)
    {
        if (fflush(fp) != 0) ok = FALSE;
        if (fclose(fp) != 0) ok = FALSE;
    }
    BgmIOEnd();

    if (ok)
        MainLoopStatusPrintf(180, "Aurora profiler v4.11 saved: %s", path);
    else
        MainLoopStatusPrintf(180, "Aurora profiler v4.11: dump write failed.");

    return ok;
}

} /* namespace */

extern "C" {

void AuroraFrontendProfilerTickBegin(void)
{
    if (s_writePending)
    {
        s_frame.open = FALSE;
        return;
    }

    memset(&s_frame, 0, sizeof(s_frame));
    s_frame.state = AURORA_FP_IDLE_UI;
    s_frame.core = AURORA_FP_CORE_NONE;
    s_frame.start = ProfCtrGetCycle();
    s_frame.open = TRUE;
}

void AuroraFrontendProfilerSetContext(Uint32 state, Uint32 core)
{
    if (!s_frame.open)
        return;

    s_frame.state =
        state < AURORA_FP_STATE_COUNT ? state : AURORA_FP_IDLE_UI;

    if (s_frame.state == AURORA_FP_IDLE_UI)
        s_frame.core = AURORA_FP_CORE_NONE;
    else
        s_frame.core =
            core < AURORA_FP_CORE_COUNT ? core : AURORA_FP_CORE_NONE;

    BeginSpan(SPAN_PROCESS_PRE_CORE);
}

void AuroraFrontendProfilerSafeFrameskip(
    Bool enabled, Bool allowed, Bool skipped)
{
    if (!s_frame.open)
        return;

    s_frame.fsSeen = TRUE;
    s_frame.fsEnabled = enabled ? TRUE : FALSE;
    s_frame.fsAllowed = allowed ? TRUE : FALSE;
    s_frame.fsSkipped = skipped ? TRUE : FALSE;
}

void AuroraFrontendProfilerSafeFrameskipDecision(
    Uint32 period, Int32 diff, Uint32 lastPresentedWork,
    Bool meaningfulDebt, Bool measuredOverrun,
    Bool unlimited, Bool skipped, Bool recoveryPending)
{
    if (!s_frame.open)
        return;

    s_frame.fsDetailSeen = TRUE;
    s_frame.fsPeriod = period;
    s_frame.fsDiff = diff;
    s_frame.fsLastPresentedWork = lastPresentedWork;
    s_frame.fsMeaningfulDebt = meaningfulDebt ? TRUE : FALSE;
    s_frame.fsMeasuredOverrun = measuredOverrun ? TRUE : FALSE;
    s_frame.fsUnlimited = unlimited ? TRUE : FALSE;
    s_frame.fsDecisionSkipped = skipped ? TRUE : FALSE;
    s_frame.fsRecoveryPendingAtDecision =
        recoveryPending ? TRUE : FALSE;
}

void AuroraFrontendProfilerSafeFrameskipRecovery(
    Uint32 presentedWork, Bool recoveryPending, Bool rebased)
{
    if (!s_frame.open)
        return;

    s_frame.fsRecoverySeen = TRUE;
    s_frame.fsRecoveryWork = presentedWork;
    s_frame.fsRecoveryPendingAtPresent =
        recoveryPending ? TRUE : FALSE;
    s_frame.fsRecoveryRebased = rebased ? TRUE : FALSE;
}

void AuroraFrontendProfilerTickEnd(void)
{
    if (!s_frame.open)
        return;

    EndSpan(SPAN_POST_RENDER);

    const Uint32 now = ProfCtrGetCycle();
    const Uint32 tick = (Uint32)(now - s_frame.start);

    for (Uint32 i = 0; i < SPAN_COUNT; ++i)
        s_frame.spanOpen[i] = FALSE;

    const Uint32 state =
        s_frame.state < AURORA_FP_STATE_COUNT
            ? s_frame.state : AURORA_FP_IDLE_UI;
    const Uint32 core =
        s_frame.core < AURORA_FP_CORE_COUNT
            ? s_frame.core : AURORA_FP_CORE_NONE;

    const DerivedT d = Derive(tick);
    const Bool spike =
        d.frontend > AURORA_FP_FRONTEND_SPIKE_BUDGET ? TRUE : FALSE;

    AccumulateBucket(s_live.stateAll[state], tick, d);
    if (spike) AccumulateBucket(s_live.stateSpike[state], tick, d);
    else       AccumulateBucket(s_live.stateNormal[state], tick, d);

    ++s_live.totalFrames;
    if (spike) ++s_live.spikeFrames;
    else       ++s_live.normalFrames;

    if (state == AURORA_FP_GAMEPLAY)
    {
        ++s_live.gameplayFrames;
        AccumulateBucket(s_live.coreAll[core], tick, d);
        if (spike) AccumulateBucket(s_live.coreSpike[core], tick, d);
        else       AccumulateBucket(s_live.coreNormal[core], tick, d);

        const Uint32 fsClass = SafeFrameskipClassForFrame();
        V45RecordCadence(core); /* AURORA_V45_VBLANK_CADENCE_PROFILER_ONLY_20261011 */
        AccumulateSafeFrameskipDiagnostic(core);
        AccumulateBucket(s_live.fsAll[core][fsClass], tick, d);
        if (spike)
            AccumulateBucket(s_live.fsSpike[core][fsClass], tick, d);
        else
            AccumulateBucket(s_live.fsNormal[core][fsClass], tick, d);
    }
    else if (state == AURORA_FP_MENU_WITH_CORE)
    {
        V45BreakCadenceChain(); /* AURORA_V45_VBLANK_CADENCE_PROFILER_ONLY_20261011 */
        ++s_live.menuFrames;
    }
    else
    {
        V45BreakCadenceChain();
        ++s_live.idleFrames;
    }

    InsertWorst(++s_hostSerial, state, core, tick, d, spike);
    s_frame.open = FALSE;

    if (s_captureArmed && state != AURORA_FP_GAMEPLAY)
    {
        if (s_menuFramesNeeded > 0)
            --s_menuFramesNeeded;
        if (s_menuFramesNeeded == 0)
            SealSnapshot();
    }
    else if (!s_initialIdleDumpDone &&
             !s_captureArmed &&
             !s_writePending &&
             s_live.gameplayFrames == 0 &&
             s_live.idleFrames >= 120)
    {
        s_initialIdleDumpDone = TRUE;
        SealSnapshot();
    }
}

void AuroraFrontendProfilerInputPollBegin(void) { BeginSpan(SPAN_INPUT_POLL); }
void AuroraFrontendProfilerInputPollEnd(void) { EndSpan(SPAN_INPUT_POLL); }
void AuroraFrontendProfilerInputSnapshotBegin(void) { BeginSpan(SPAN_INPUT_SNAPSHOT); }

void AuroraFrontendProfilerFrontendInputBegin(void)
{
    EndSpan(SPAN_INPUT_SNAPSHOT);
    BeginSpan(SPAN_INPUT_LOGIC);
}

void AuroraFrontendProfilerFrontendInputEnd(void)
{
    EndSpan(SPAN_INPUT_LOGIC);
}

void AuroraFrontendProfilerCoreBegin(void)
{
    EndSpan(SPAN_PROCESS_PRE_CORE);
    BeginSpan(SPAN_CORE);
}

void AuroraFrontendProfilerCoreEnd(void)
{
    EndSpan(SPAN_CORE);
    BeginSpan(SPAN_PROCESS_POST_CORE);
}

void AuroraFrontendProfilerUploadBegin(void) { BeginSpan(SPAN_UPLOAD); }
void AuroraFrontendProfilerUploadEnd(void) { EndSpan(SPAN_UPLOAD); }

void AuroraFrontendProfilerRenderBegin(void)
{
    EndSpan(SPAN_PROCESS_POST_CORE);
    EndSpan(SPAN_PROCESS_PRE_CORE);
    BeginSpan(SPAN_RENDER);
}

void AuroraFrontendProfilerRenderEnd(void)
{
    EndSpan(SPAN_RENDER);
    BeginSpan(SPAN_POST_RENDER);
}

void AuroraFrontendProfilerRenderSetupBegin(void) { BeginSpan(SPAN_RENDER_SETUP); }
void AuroraFrontendProfilerRenderSetupEnd(void) { EndSpan(SPAN_RENDER_SETUP); }
void AuroraFrontendProfilerGameDrawBegin(void) { BeginSpan(SPAN_GAME_DRAW); }
void AuroraFrontendProfilerGameDrawEnd(void) { EndSpan(SPAN_GAME_DRAW); }
void AuroraFrontendProfilerUiBegin(void) { BeginSpan(SPAN_UI); }
void AuroraFrontendProfilerUiEnd(void) { EndSpan(SPAN_UI); }
void AuroraFrontendProfilerGpFlushBegin(void) { BeginSpan(SPAN_GP_FLUSH); }
void AuroraFrontendProfilerGpFlushEnd(void) { EndSpan(SPAN_GP_FLUSH); }
/* AURORA_V45_VBLANK_CADENCE_PROFILER_ONLY_20261011
 * Reuse BeginSpan/EndSpan timestamps; NO additional COP0 read in hot path. */
void AuroraFrontendProfilerVBlankBegin(void)
{
    BeginSpan(SPAN_VBLANK);
    if (s_frame.open && s_frame.spanOpen[SPAN_VBLANK])
        s_frame.v45PreFlipWork =
            (Uint32)(s_frame.spanStart[SPAN_VBLANK] - s_frame.start);
}
void AuroraFrontendProfilerVBlankEnd(void)
{
    const Bool hadOpenSpan = s_frame.open && s_frame.spanOpen[SPAN_VBLANK];
    EndSpan(SPAN_VBLANK);
    if (hadOpenSpan)
    {
        /* EndSpan() has already stored this precise end timestamp. */
        s_frame.v45FlipStamp = s_frame.spanStart[SPAN_VBLANK]
                               + s_frame.cycles[SPAN_VBLANK];
        s_frame.v45Presented = TRUE;
    }
}
void AuroraFrontendProfilerAudioDrainBegin(void) { BeginSpan(SPAN_AUDIO_DRAIN); }
void AuroraFrontendProfilerAudioDrainEnd(void) { EndSpan(SPAN_AUDIO_DRAIN); }

void AuroraFrontendProfilerAudioDrainFrames(Uint32 frames)
{
    if (s_frame.open)
        s_frame.audioDrainFrames += frames;
}

void AuroraFrontendProfilerAudioConvertBegin(Uint32 inputFrames)
{
    if (s_frame.open)
        s_frame.audioConvertInputFrames += inputFrames;
    BeginSpan(SPAN_AUDIO_CONVERT);
}
void AuroraFrontendProfilerAudioConvertEnd(void)
{
    EndSpan(SPAN_AUDIO_CONVERT);
}

void AuroraFrontendProfilerAudioFlushBegin(Uint32 outputFrames)
{
    if (s_frame.open)
        s_frame.audioFlushFrames += outputFrames;
    BeginSpan(SPAN_AUDIO_FLUSH);
}
void AuroraFrontendProfilerAudioFlushEnd(void)
{
    EndSpan(SPAN_AUDIO_FLUSH);
}

/* AURORA_SNES_CORE_PROFILER_V3_6_20261005 */
/* AURORA_SNES_BGCHR_PROFILER_V3_7_20261005 */
void AuroraFrontendProfilerSnesBgChrRecord(
    Uint32 bgIndex, Uint32 bitDepth, Uint32 tiles,
    Bool offset, Bool mosaic, Bool fineXNonZero, Bool hiresSubscreen,
    Uint32 startCycles, Uint32 decodeEndCycles, Uint32 endCycles)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES || bgIndex >= 4u)
        return;

    Uint32 depth;
    if (bitDepth == 2u) depth = 0u;
    else if (bitDepth == 4u) depth = 1u;
    else if (bitDepth == 8u) depth = 2u;
    else depth = 3u;

    const Uint32 decode = (Uint32)(decodeEndCycles - startCycles);
    const Uint32 total = (Uint32)(endCycles - startCycles);
    if (decode > total)
    {
        ++s_frame.snesBgChrBadRecords;
        return;
    }

    ++s_frame.snesBgChrCalls[bgIndex][depth];
    s_frame.snesBgChrTiles[bgIndex][depth] += tiles;
    s_frame.snesBgChrCycles[bgIndex][depth] += total;
    s_frame.snesBgChrDecodeCycles[bgIndex][depth] += decode;
    ++s_frame.snesBgChrMeasuredCalls;
    s_frame.snesBgChrMeasuredTiles += tiles;
    s_frame.snesBgChrMeasuredCycles += total;
    s_frame.snesBgChrMeasuredDecodeCycles += decode;

    const Bool flags[4] = { offset, mosaic, fineXNonZero, hiresSubscreen };
    for (Uint32 i = 0; i < 4u; ++i)
    {
        if (!flags[i]) continue;
        ++s_frame.snesBgChrFlagCalls[i];
        s_frame.snesBgChrFlagTiles[i] += tiles;
        s_frame.snesBgChrFlagCycles[i] += total;
        s_frame.snesBgChrFlagDecodeCycles[i] += decode;
    }
}

/* AURORA_MULTICORE_DEEP_PROFILER_V3_48_20261008 */
void AuroraFrontendProfilerExternalRecord(
    uint32_t core, uint32_t bucket, uint32_t cycles, uint32_t units)
{
    if (!s_frame.open || s_frame.core != core ||
        bucket >= AURORA_EXT_BUCKET_MAX ||
        (core != AURORA_EXT_CORE_PCE &&
         core != AURORA_EXT_CORE_GBA &&
         core != AURORA_EXT_CORE_SEGA))
        return;
    ++s_frame.externalCalls[bucket];
    s_frame.externalUnits[bucket] += units;
    s_frame.externalCycles[bucket] += cycles;
}

/* AURORA_SNES_DEEP_OPT_V3_14_20261005 */
void AuroraFrontendProfilerSnesDeepRecord(
    Uint32 bucket, Uint32 cycles, Uint32 units)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid || bucket >= 69u)
        return;
    ++s_frame.snesDeepCalls[bucket];
    s_frame.snesDeepUnits[bucket] += units;
    s_frame.snesDeepCycles[bucket] += cycles;
}

/* AURORA_V23_SNES_BGCHR_TRAITS_COST_20261009
 * Approximate cost of the existing 33-tile traits collector (auroraprofile only).
 * A pair of clock reads itself introduces small measurement overhead. */
void AuroraFrontendProfilerSnesChrTraitsCost(Uint32 ticks)
{
    if (s_frame.open && s_frame.core == AURORA_FP_CORE_SNES &&
        !s_frame.snesProfileInvalid)
        s_frame.snesBgChrTraitsCycles += ticks;
}

void AuroraFrontendProfilerSnesChrTraitsRecord(
    Uint32 bitDepth, Uint32 opaqueRows, Uint32 transparentRows,
    Uint32 flip0OpaqueRows, Uint32 flippedOpaqueRows)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid || (bitDepth != 2u && bitDepth != 4u))
        return;
    const Uint32 base = bitDepth == 4u
        ? AURORA_SNES_DEEP_CHR4_OPAQUE_ROWS : AURORA_SNES_DEEP_CHR2_OPAQUE_ROWS;
    const Uint32 flipBase = bitDepth == 4u
        ? AURORA_SNES_DEEP_CHR4_OPAQUE_FLIP0 : AURORA_SNES_DEEP_CHR2_OPAQUE_FLIP0;
    ++s_frame.snesDeepCalls[base];
    s_frame.snesDeepUnits[base] += opaqueRows;
    ++s_frame.snesDeepCalls[base + 1u];
    s_frame.snesDeepUnits[base + 1u] += transparentRows;
    ++s_frame.snesDeepCalls[flipBase];
    s_frame.snesDeepUnits[flipBase] += flip0OpaqueRows;
    ++s_frame.snesDeepCalls[flipBase + 1u];
    s_frame.snesDeepUnits[flipBase + 1u] += flippedOpaqueRows;
}

/* AURORA_SNES_MICROSCOPE_V3_27_20261006 */
void AuroraFrontendProfilerSnesChr4LocalityRecord(
    Uint32 tiles, Uint32 opaqueTiles,
    Uint32 paletteReuses, Uint32 paletteRuns,
    Uint32 flipSame, Uint32 flipChanges,
    Uint32 rowSame, Uint32 rowPlus16, Uint32 rowMinus16, Uint32 rowOther)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid)
        return;

    ++s_frame.snesChr4LocalityCalls;
    s_frame.snesChr4LocalityTiles += tiles;
    s_frame.snesChr4OpaqueTiles += opaqueTiles;
    s_frame.snesChr4PaletteReuses += paletteReuses;
    s_frame.snesChr4PaletteRuns += paletteRuns;
    s_frame.snesChr4FlipSame += flipSame;
    s_frame.snesChr4FlipChanges += flipChanges;
    s_frame.snesChr4RowSame += rowSame;
    s_frame.snesChr4RowPlus16 += rowPlus16;
    s_frame.snesChr4RowMinus16 += rowMinus16;
    s_frame.snesChr4RowOther += rowOther;
}

void AuroraFrontendProfilerSnesObjMicroscopeRecord(
    Uint32 hitTiles, Uint32 hitCycles,
    Uint32 missTiles, Uint32 missCycles,
    Uint32 hitTransparent, Uint32 hitFullOpaque, Uint32 hitPartial,
    Uint32 missTransparent, Uint32 missFullOpaque, Uint32 missPartial,
    Uint32 rowSame, Uint32 rowPlus16, Uint32 rowMinus16, Uint32 rowOther)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid)
        return;

    ++s_frame.snesObjMicroscopeCalls;
    s_frame.snesObjHitTiles += hitTiles;
    s_frame.snesObjHitCycles += hitCycles;
    s_frame.snesObjMissTiles += missTiles;
    s_frame.snesObjMissCycles += missCycles;
    s_frame.snesObjHitTransparent += hitTransparent;
    s_frame.snesObjHitFullOpaque += hitFullOpaque;
    s_frame.snesObjHitPartial += hitPartial;
    s_frame.snesObjMicMissTransparent += missTransparent;
    s_frame.snesObjMicMissFullOpaque += missFullOpaque;
    s_frame.snesObjMicMissPartial += missPartial;
    s_frame.snesObjRowSame += rowSame;
    s_frame.snesObjRowPlus16 += rowPlus16;
    s_frame.snesObjRowMinus16 += rowMinus16;
    s_frame.snesObjRowOther += rowOther;
}

/* AURORA_CHR4_REUSE_MICROSCOPE_V3_28_20261007 */
/* AURORA_CHR4_PATH_BG_MICROSCOPE_V3_29_20261007 */
void AuroraFrontendProfilerSnesChr4ReuseRecord(
    Uint32 bgIndex, Uint32 pathIndex, Uint32 tiles, Uint32 opaqueTiles,
    Uint32 opaqueTransitions, Uint32 adjacentOpaqueTransitions,
    Uint32 paletteIndexReuses, Uint32 decodeKeyReuses,
    Uint32 rowSameFlipChanged, Uint32 pixelKeyReuses,
    Uint32 adjacentPaletteIndexReuses, Uint32 adjacentDecodeKeyReuses,
    Uint32 adjacentPixelKeyReuses)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid || bgIndex >= 4u || pathIndex >= 6u)
        return;

    s_frame.snesChr4OpaqueTransitions += opaqueTransitions;
    s_frame.snesChr4AdjacentOpaqueTransitions += adjacentOpaqueTransitions;
    s_frame.snesChr4PaletteIndexReuses += paletteIndexReuses;
    s_frame.snesChr4DecodeKeyReuses += decodeKeyReuses;
    s_frame.snesChr4RowSameFlipChanged += rowSameFlipChanged;
    s_frame.snesChr4PixelKeyReuses += pixelKeyReuses;
    s_frame.snesChr4AdjacentPaletteIndexReuses += adjacentPaletteIndexReuses;
    s_frame.snesChr4AdjacentDecodeKeyReuses += adjacentDecodeKeyReuses;
    s_frame.snesChr4AdjacentPixelKeyReuses += adjacentPixelKeyReuses;

    ++s_frame.snesChr4CellCalls[bgIndex][pathIndex];
    s_frame.snesChr4CellTiles[bgIndex][pathIndex] += tiles;
    s_frame.snesChr4CellOpaqueTiles[bgIndex][pathIndex] += opaqueTiles;
    s_frame.snesChr4CellOpaqueTransitions[bgIndex][pathIndex] += opaqueTransitions;
    s_frame.snesChr4CellAdjacentOpaqueTransitions[bgIndex][pathIndex] += adjacentOpaqueTransitions;
    s_frame.snesChr4CellPaletteIndexReuses[bgIndex][pathIndex] += paletteIndexReuses;
    s_frame.snesChr4CellDecodeKeyReuses[bgIndex][pathIndex] += decodeKeyReuses;
    s_frame.snesChr4CellRowSameFlipChanged[bgIndex][pathIndex] += rowSameFlipChanged;
    s_frame.snesChr4CellPixelKeyReuses[bgIndex][pathIndex] += pixelKeyReuses;
    s_frame.snesChr4CellAdjacentPaletteIndexReuses[bgIndex][pathIndex] += adjacentPaletteIndexReuses;
    s_frame.snesChr4CellAdjacentDecodeKeyReuses[bgIndex][pathIndex] += adjacentDecodeKeyReuses;
    s_frame.snesChr4CellAdjacentPixelKeyReuses[bgIndex][pathIndex] += adjacentPixelKeyReuses;
}

void AuroraFrontendProfilerSnesCostScopeEnter(Int32 bucket)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid)
        return;
    if (bucket < 0 || bucket >= 5)
    {
        s_frame.snesProfileInvalid = TRUE;
        return;
    }

    const Uint32 now = ProfCtrGetCycle();
    if (s_frame.snesCostDepth > 0)
    {
        const Int32 parent =
            s_frame.snesCostStackBucket[s_frame.snesCostDepth - 1];
        s_frame.snesCostCycles[parent] +=
            (Uint32)(now - s_frame.snesCostStackStart[s_frame.snesCostDepth - 1]);
    }
    if (s_frame.snesCostDepth >= 16)
    {
        s_frame.snesProfileInvalid = TRUE;
        return;
    }
    s_frame.snesCostStackBucket[s_frame.snesCostDepth] = bucket;
    s_frame.snesCostStackStart[s_frame.snesCostDepth] = now;
    ++s_frame.snesCostDepth;
}

void AuroraFrontendProfilerSnesCostScopeLeave(Int32 bucket)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid)
        return;
    if (s_frame.snesCostDepth <= 0 ||
        s_frame.snesCostStackBucket[s_frame.snesCostDepth - 1] != bucket)
    {
        s_frame.snesProfileInvalid = TRUE;
        return;
    }

    const Uint32 now = ProfCtrGetCycle();
    s_frame.snesCostCycles[bucket] +=
        (Uint32)(now - s_frame.snesCostStackStart[s_frame.snesCostDepth - 1]);
    --s_frame.snesCostDepth;
    if (s_frame.snesCostDepth > 0)
        s_frame.snesCostStackStart[s_frame.snesCostDepth - 1] = now;
}

void AuroraFrontendProfilerSnesPpuDetailEnter(Int32 bucket)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid)
        return;
    if (bucket < 0 || bucket >= 18)
    {
        s_frame.snesProfileInvalid = TRUE;
        return;
    }

    const Uint32 now = ProfCtrGetCycle();
    if (s_frame.snesPpuDepth > 0)
    {
        const Int32 parent =
            s_frame.snesPpuStackBucket[s_frame.snesPpuDepth - 1];
        s_frame.snesPpuCycles[parent] +=
            (Uint32)(now - s_frame.snesPpuStackStart[s_frame.snesPpuDepth - 1]);
    }
    if (s_frame.snesPpuDepth >= 16)
    {
        s_frame.snesProfileInvalid = TRUE;
        return;
    }
    s_frame.snesPpuStackBucket[s_frame.snesPpuDepth] = bucket;
    s_frame.snesPpuStackStart[s_frame.snesPpuDepth] = now;
    ++s_frame.snesPpuDepth;
}

void AuroraFrontendProfilerSnesPpuDetailLeave(Int32 bucket)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid)
        return;
    if (s_frame.snesPpuDepth <= 0 ||
        s_frame.snesPpuStackBucket[s_frame.snesPpuDepth - 1] != bucket)
    {
        s_frame.snesProfileInvalid = TRUE;
        return;
    }

    const Uint32 now = ProfCtrGetCycle();
    s_frame.snesPpuCycles[bucket] +=
        (Uint32)(now - s_frame.snesPpuStackStart[s_frame.snesPpuDepth - 1]);
    --s_frame.snesPpuDepth;
    if (s_frame.snesPpuDepth > 0)
        s_frame.snesPpuStackStart[s_frame.snesPpuDepth - 1] = now;
}

void AuroraFrontendProfilerSnesPpuWork(
    Uint32 mainMask, Uint32 subMask, Uint32 mapFetches, Uint32 chrDecodes)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid)
        return;
    s_frame.snesPpuBgMapFetches += mapFetches;
    s_frame.snesPpuBgChrDecodes += chrDecodes;
    s_frame.snesPpuMainBgLayers += SnesBgBits(mainMask);
    s_frame.snesPpuSubBgLayers += SnesBgBits(subMask);
}

void AuroraFrontendProfilerSnesPpuLine(
    Uint32 bgMode, Uint32 objOamRefs, Uint32 objTiles,
    Bool objEnabled, Bool rangeOver, Bool timeOver,
    Bool windowed, Bool colorMath, Bool subscreen,
    Bool hires, Bool directColor)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES ||
        s_frame.snesProfileInvalid)
        return;
    ++s_frame.snesPpuRenderedLines;
    if (bgMode < 8u) ++s_frame.snesPpuModeLines[bgMode];
    s_frame.snesPpuObjOamRefs += objOamRefs;
    s_frame.snesPpuObjTiles += objTiles;
    if (objEnabled) ++s_frame.snesPpuObjEnabledLines;
    if (rangeOver) ++s_frame.snesPpuObjRangeOverLines;
    if (timeOver) ++s_frame.snesPpuObjTimeOverLines;
    if (windowed) ++s_frame.snesPpuWindowLines;
    if (colorMath) ++s_frame.snesPpuColorMathLines;
    if (subscreen) ++s_frame.snesPpuSubscreenLines;
    if (hires) ++s_frame.snesPpuHiresLines;
    if (directColor) ++s_frame.snesPpuDirectColorLines;
}

void AuroraFrontendProfilerSnesCpuBudget(Uint32 cycles)
{
    if (!s_frame.open || s_frame.core != AURORA_FP_CORE_SNES) return;
    ++s_frame.snesCpuBudgetCalls;
    s_frame.snesCpuBudgetCycles += cycles;
}

void AuroraFrontendProfilerSnesPpuSyncCall(void)
{
    if (s_frame.open && s_frame.core == AURORA_FP_CORE_SNES)
        ++s_frame.snesPpuSyncCalls;
}

void AuroraFrontendProfilerSnesMdmaBegin(void) { BeginSpan(SPAN_SNES_MDMA); }
void AuroraFrontendProfilerSnesMdmaEnd(void) { EndSpan(SPAN_SNES_MDMA); }
void AuroraFrontendProfilerSnesHdmaSetupBegin(void) { BeginSpan(SPAN_SNES_HDMA_SETUP); }
void AuroraFrontendProfilerSnesHdmaSetupEnd(void) { EndSpan(SPAN_SNES_HDMA_SETUP); }
void AuroraFrontendProfilerSnesHdmaDataBegin(void) { BeginSpan(SPAN_SNES_HDMA_DATA); }
void AuroraFrontendProfilerSnesHdmaDataEnd(void) { EndSpan(SPAN_SNES_HDMA_DATA); }

void AuroraFrontendProfilerSnesSpcBegin(Uint32 requestedCycles)
{
    if (s_frame.open)
    {
        ++s_frame.spcCalls;
        s_frame.spcRequestedCycles += requestedCycles;
    }
    BeginSpan(SPAN_SNES_SPC);
}
void AuroraFrontendProfilerSnesSpcEnd(Uint32 consumedCycles)
{
    if (s_frame.open)
        s_frame.spcConsumedCycles += consumedCycles;
    EndSpan(SPAN_SNES_SPC);
}

void AuroraFrontendProfilerSnesDspBegin(void)
{
    if (s_frame.open)
        ++s_frame.dspMixCalls;
    BeginSpan(SPAN_SNES_DSP_TOTAL);
}
void AuroraFrontendProfilerSnesDspEnd(void)
{
    EndSpan(SPAN_SNES_DSP_TOTAL);
}
void AuroraFrontendProfilerSnesDspChunk(Uint32 samples)
{
    if (s_frame.open)
    {
        ++s_frame.dspChunks;
        s_frame.dspSamples += samples;
    }
}
void AuroraFrontendProfilerSnesDspSyncBegin(void)
    { BeginSpan(SPAN_SNES_DSP_SYNC); }
void AuroraFrontendProfilerSnesDspSyncEnd(void)
    { EndSpan(SPAN_SNES_DSP_SYNC); }
void AuroraFrontendProfilerSnesDspNoiseBegin(void)
    { BeginSpan(SPAN_SNES_DSP_NOISE); }
void AuroraFrontendProfilerSnesDspNoiseEnd(void)
    { EndSpan(SPAN_SNES_DSP_NOISE); }
void AuroraFrontendProfilerSnesDspVoicesBegin(void)
    { BeginSpan(SPAN_SNES_DSP_VOICES); }
void AuroraFrontendProfilerSnesDspVoicesEnd(void)
    { EndSpan(SPAN_SNES_DSP_VOICES); }

/* AURORA_SNES_DSP_VOICES_PROFILER_V3_5_1_20261005 */
void AuroraFrontendProfilerSnesDspEnvelopeBegin(void)
{
    if (s_frame.open) ++s_frame.dspVoiceEnvelopeCalls;
    BeginSpan(SPAN_SNES_DSP_ENVELOPE);
}
void AuroraFrontendProfilerSnesDspEnvelopeEnd(Bool active)
{
    if (s_frame.open && active) ++s_frame.dspVoiceEnvelopeActive;
    EndSpan(SPAN_SNES_DSP_ENVELOPE);
}

void AuroraFrontendProfilerSnesDspSampleNormalBegin(void)
{
    if (s_frame.open) ++s_frame.dspVoiceSampleNormalCalls;
    BeginSpan(SPAN_SNES_DSP_SAMPLE_NORMAL);
}
void AuroraFrontendProfilerSnesDspSampleNormalEnd(Bool produced, Uint32 samples)
{
    if (s_frame.open && produced)
        s_frame.dspVoiceSampleNormalSamples += samples;
    EndSpan(SPAN_SNES_DSP_SAMPLE_NORMAL);
}

void AuroraFrontendProfilerSnesDspSamplePmonBegin(void)
{
    if (s_frame.open) ++s_frame.dspVoiceSamplePmonCalls;
    BeginSpan(SPAN_SNES_DSP_SAMPLE_PMON);
}
void AuroraFrontendProfilerSnesDspSamplePmonEnd(Bool produced, Uint32 samples)
{
    if (s_frame.open && produced)
        s_frame.dspVoiceSamplePmonSamples += samples;
    EndSpan(SPAN_SNES_DSP_SAMPLE_PMON);
}

void AuroraFrontendProfilerSnesDspBrrDecodeBegin(void)
{
    if (s_frame.open) ++s_frame.dspVoiceBrrDecodes;
    BeginSpan(SPAN_SNES_DSP_BRR_DECODE);
}
void AuroraFrontendProfilerSnesDspBrrDecodeEnd(void)
    { EndSpan(SPAN_SNES_DSP_BRR_DECODE); }

void AuroraFrontendProfilerSnesDspPitchmodFeederBegin(void)
{
    if (s_frame.open) ++s_frame.dspVoicePitchmodFeeders;
    BeginSpan(SPAN_SNES_DSP_PITCHMOD_FEEDER);
}
void AuroraFrontendProfilerSnesDspPitchmodFeederEnd(void)
    { EndSpan(SPAN_SNES_DSP_PITCHMOD_FEEDER); }

void AuroraFrontendProfilerSnesDspMixStereoBegin(
    Bool noise, Bool echo, Uint32 samples)
{
    if (s_frame.open)
    {
        ++s_frame.dspVoiceMixCalls;
        s_frame.dspVoiceMixSamples += samples;
        if (noise) ++s_frame.dspVoiceNoiseMixes;
        if (echo)
        {
            ++s_frame.dspVoiceMixEchoCalls;
            s_frame.dspVoiceMixEchoSamples += samples;
        }
        else
        {
            ++s_frame.dspVoiceMixDryCalls;
            s_frame.dspVoiceMixDrySamples += samples;
        }
    }
    BeginSpan(echo ? SPAN_SNES_DSP_MIX_ECHO : SPAN_SNES_DSP_MIX_DRY);
}
void AuroraFrontendProfilerSnesDspMixStereoEnd(Bool echo)
    { EndSpan(echo ? SPAN_SNES_DSP_MIX_ECHO : SPAN_SNES_DSP_MIX_DRY); }

void AuroraFrontendProfilerSnesDspSilentFastpath(void)
{
    if (s_frame.open) ++s_frame.dspVoiceSilentFastpathHits;
}

void AuroraFrontendProfilerSnesDspVoiceGain(
    Uint32 envProxy, Int32 volL, Int32 volR,
    Bool pmon, Bool echo, Bool feedsPitchMod)
{
    if (!s_frame.open) return;

    const Uint32 absL = (Uint32)(volL < 0 ? -volL : volL);
    const Uint32 absR = (Uint32)(volR < 0 ? -volR : volR);
    const Uint32 maxVol = absL > absR ? absL : absR;
    const Uint32 gain = envProxy * maxVol;

    if (!maxVol)
        ++s_frame.dspVoiceGainZeroVolume;
    else if (!envProxy)
        ++s_frame.dspVoiceGainEnvelopeZero;
    else if (gain <= 256u)
        ++s_frame.dspVoiceGainVeryLow;
    else if (gain <= 1024u)
        ++s_frame.dspVoiceGainLow;
    else
        ++s_frame.dspVoiceGainNormal;

    if (gain <= 1024u)
    {
        if (pmon) ++s_frame.dspVoiceLowGainPmon;
        if (echo) ++s_frame.dspVoiceLowGainEcho;
        if (feedsPitchMod) ++s_frame.dspVoiceLowGainFeedsPmon;
    }
}

void AuroraFrontendProfilerSnesDspEchoBegin(void)
    { BeginSpan(SPAN_SNES_DSP_ECHO); }
void AuroraFrontendProfilerSnesDspEchoEnd(void)
    { EndSpan(SPAN_SNES_DSP_ECHO); }
void AuroraFrontendProfilerSnesDspFinalBegin(void)
    { BeginSpan(SPAN_SNES_DSP_FINAL); }
void AuroraFrontendProfilerSnesDspFinalEnd(void)
    { EndSpan(SPAN_SNES_DSP_FINAL); }

/* AURORA_PROFILER_AUTO20_STATELOAD_V21_20261009
 * A savestate load defines the benchmark's start; discard frontend startup
 * history without discarding the restored emulation state. Separate core
 * workprof/v3 counters retain their native lifecycle. */
Bool AuroraFrontendProfilerAutoCloseBeginWindow(void)
{
    /* Resolve any prior idle/menu dump instead of accidentally writing it
     * as the benchmark's result 20s later. All I/O is during state loading,
     * never during a measured gameplay frame. */
    if (s_writePending)
    {
        if (!SavePending())
            return FALSE;
        s_writePending = FALSE;
    }
    ResetLive();
    s_captureArmed = FALSE;
    s_menuFramesNeeded = 0;
    s_frame.open = FALSE; /* Discard this tick's expensive load-state I/O. */
    return TRUE;
}

/* Only called at the end of a completed host tick, after TickEnd().
 * Snapshot while the ROM is STILL live: PCE v3 and WorkProf dump hooks
 * need the core/context, and normal menu's 60-frame delay is undesirable. */
Bool AuroraFrontendProfilerAutoCloseSave(void)
{
    if (!s_writePending)
    {
        if (!s_live.gameplayFrames)
            return FALSE;
        SealSnapshot();
    }
    if (!s_writePending || !SavePending())
        return FALSE;
    s_writePending = FALSE;
    return TRUE;
}

/* AURORA_BENCHMARK_CHAINED_V3_20261010_PROF_API_BEGIN */
Bool AuroraFrontendProfilerBatchBegin(void)
{
    if (s_frame.open || s_writePending || s_captureArmed) return FALSE;
    ResetLive();
    s_menuFramesNeeded=0;
    s_initialIdleDumpDone=TRUE;
    return TRUE;
}

/* AURORA_BENCHMARK_CHAINED_V3_20261010_PROF_API_END */
void AuroraFrontendProfilerMenuEnter(void)
{
    if (s_writePending || s_captureArmed || !s_live.gameplayFrames)
        return;
    s_captureArmed = TRUE;
    s_menuFramesNeeded = 60u;
}

void AuroraFrontendProfilerMenuUpdate(Bool canWrite)
{
    if (!canWrite || !s_writePending)
        return;
    (void)SavePending();
    s_writePending = FALSE;
}

} /* extern "C" */

#endif /* AURORA_FRONTEND_PROFILER */
