#ifndef AURORA_FRONTEND_PROFILER_H
#define AURORA_FRONTEND_PROFILER_H

/* AURORA_FRONTEND_PROFILER_V4_6_FS_RECOVERY_AUDIO_20261004
 *
 * Diagnostic-only all-core Aurora/frontend profiler.
 * =0: implementation TU is not linked and every callsite preprocesses away.
 */
#include "types.h"

#ifndef AURORA_FRONTEND_PROFILER
#define AURORA_FRONTEND_PROFILER 0
#endif

#if AURORA_FRONTEND_PROFILER
#ifdef __cplusplus
extern "C" {
#endif

enum AuroraFrontendStateE
{
    AURORA_FP_GAMEPLAY = 0,
    AURORA_FP_MENU_WITH_CORE,
    AURORA_FP_IDLE_UI,
    AURORA_FP_STATE_COUNT
};

enum AuroraFrontendCoreE
{
    AURORA_FP_CORE_NONE = 0,
    AURORA_FP_CORE_SNES,
    AURORA_FP_CORE_NES,
    AURORA_FP_CORE_FDS,
    AURORA_FP_CORE_SEGA,
    AURORA_FP_CORE_PCE,
    AURORA_FP_CORE_GB,
    AURORA_FP_CORE_GBA,
    AURORA_FP_CORE_COUNT
};

void AuroraFrontendProfilerTickBegin(void);
void AuroraFrontendProfilerSetContext(Uint32 state, Uint32 core);
/* AURORA_BENCHMARK_CHAINED_V3_20261010_PROF_HEADER_BEGIN */
Bool AuroraFrontendProfilerBatchBegin(void);
/* AURORA_BENCHMARK_CHAINED_V3_20261010_PROF_HEADER_END */
void AuroraFrontendProfilerTickEnd(void);
void AuroraFrontendProfilerSafeFrameskip(
    Bool enabled, Bool allowed, Bool skipped);
void AuroraFrontendProfilerSafeFrameskipDecision(
    Uint32 period, Int32 diff, Uint32 lastPresentedWork,
    Bool meaningfulDebt, Bool measuredOverrun,
    Bool unlimited, Bool skipped, Bool recoveryPending);
void AuroraFrontendProfilerSafeFrameskipRecovery(
    Uint32 presentedWork, Bool recoveryPending, Bool rebased);

void AuroraFrontendProfilerInputPollBegin(void);
void AuroraFrontendProfilerInputPollEnd(void);
void AuroraFrontendProfilerInputSnapshotBegin(void);
void AuroraFrontendProfilerFrontendInputBegin(void);
void AuroraFrontendProfilerFrontendInputEnd(void);

void AuroraFrontendProfilerCoreBegin(void);
void AuroraFrontendProfilerCoreEnd(void);
void AuroraFrontendProfilerUploadBegin(void);
void AuroraFrontendProfilerUploadEnd(void);

void AuroraFrontendProfilerRenderBegin(void);
void AuroraFrontendProfilerRenderEnd(void);
void AuroraFrontendProfilerRenderSetupBegin(void);
void AuroraFrontendProfilerRenderSetupEnd(void);
void AuroraFrontendProfilerGameDrawBegin(void);
void AuroraFrontendProfilerGameDrawEnd(void);
void AuroraFrontendProfilerUiBegin(void);
void AuroraFrontendProfilerUiEnd(void);
void AuroraFrontendProfilerGpFlushBegin(void);
void AuroraFrontendProfilerGpFlushEnd(void);
void AuroraFrontendProfilerVBlankBegin(void);
void AuroraFrontendProfilerVBlankEnd(void);
void AuroraFrontendProfilerAudioDrainBegin(void);
void AuroraFrontendProfilerAudioDrainEnd(void);
void AuroraFrontendProfilerAudioDrainFrames(Uint32 frames);

void AuroraFrontendProfilerAudioConvertBegin(Uint32 inputFrames);
void AuroraFrontendProfilerAudioConvertEnd(void);
void AuroraFrontendProfilerAudioFlushBegin(Uint32 outputFrames);
void AuroraFrontendProfilerAudioFlushEnd(void);

/* AURORA_SNES_CORE_PROFILER_V3_6_20261005
 * Broad SNES core/PPU diagnostic feed. Bucket ids come from
 * aurora_snes_cost_profiler.h; these functions do not affect emulation. */
void AuroraFrontendProfilerSnesCostScopeEnter(Int32 bucket);
void AuroraFrontendProfilerSnesCostScopeLeave(Int32 bucket);
void AuroraFrontendProfilerSnesPpuDetailEnter(Int32 bucket);
void AuroraFrontendProfilerSnesPpuDetailLeave(Int32 bucket);
void AuroraFrontendProfilerSnesPpuWork(
    Uint32 mainMask, Uint32 subMask, Uint32 mapFetches, Uint32 chrDecodes);
void AuroraFrontendProfilerSnesPpuLine(
    Uint32 bgMode, Uint32 objOamRefs, Uint32 objTiles,
    Bool objEnabled, Bool rangeOver, Bool timeOver,
    Bool windowed, Bool colorMath, Bool subscreen,
    Bool hires, Bool directColor);
void AuroraFrontendProfilerSnesCpuBudget(Uint32 cycles);
void AuroraFrontendProfilerSnesPpuSyncCall(void);
void AuroraFrontendProfilerSnesMdmaBegin(void);
void AuroraFrontendProfilerSnesMdmaEnd(void);
void AuroraFrontendProfilerSnesHdmaSetupBegin(void);
void AuroraFrontendProfilerSnesHdmaSetupEnd(void);
void AuroraFrontendProfilerSnesHdmaDataBegin(void);
void AuroraFrontendProfilerSnesHdmaDataEnd(void);

/* AURORA_SNES_BGCHR_PROFILER_V3_7_20261005
 * One aggregate record per BG CHR invocation. Timestamps are captured at the
 * RenderLine8 callsite so the hot path pays no begin/end function-call pair. */
void AuroraFrontendProfilerSnesBgChrRecord(
    Uint32 bgIndex, Uint32 bitDepth, Uint32 tiles,
    Bool offset, Bool mosaic, Bool fineXNonZero, Bool hiresSubscreen,
    Uint32 startCycles, Uint32 decodeEndCycles, Uint32 endCycles);

/* AURORA_SNES_DEEP_OPT_V3_14_20261005 */
void AuroraFrontendProfilerSnesDeepRecord(
    Uint32 bucket, Uint32 cycles, Uint32 units);
/* AURORA_V23_SNES_BGCHR_TRAITS_COST_20261009: profiler-only collector cost; no emulation changes. */
void AuroraFrontendProfilerSnesChrTraitsCost(Uint32 ticks);
void AuroraFrontendProfilerSnesChrTraitsRecord(
    Uint32 bitDepth, Uint32 opaqueRows, Uint32 transparentRows,
    Uint32 flip0OpaqueRows, Uint32 flippedOpaqueRows);

void AuroraFrontendProfilerSnesSpcBegin(Uint32 requestedCycles);
void AuroraFrontendProfilerSnesSpcEnd(Uint32 consumedCycles);
void AuroraFrontendProfilerSnesDspBegin(void);
void AuroraFrontendProfilerSnesDspEnd(void);
void AuroraFrontendProfilerSnesDspChunk(Uint32 samples);
void AuroraFrontendProfilerSnesDspSyncBegin(void);
void AuroraFrontendProfilerSnesDspSyncEnd(void);
void AuroraFrontendProfilerSnesDspNoiseBegin(void);
void AuroraFrontendProfilerSnesDspNoiseEnd(void);
void AuroraFrontendProfilerSnesDspVoicesBegin(void);
void AuroraFrontendProfilerSnesDspVoicesEnd(void);

/* AURORA_SNES_DSP_VOICES_PROFILER_V3_5_1_20261005
 * Diagnostic-only subdivision of the existing voices bucket. */
void AuroraFrontendProfilerSnesDspEnvelopeBegin(void);
void AuroraFrontendProfilerSnesDspEnvelopeEnd(Bool active);
void AuroraFrontendProfilerSnesDspSampleNormalBegin(void);
void AuroraFrontendProfilerSnesDspSampleNormalEnd(Bool produced, Uint32 samples);
void AuroraFrontendProfilerSnesDspSamplePmonBegin(void);
void AuroraFrontendProfilerSnesDspSamplePmonEnd(Bool produced, Uint32 samples);
void AuroraFrontendProfilerSnesDspBrrDecodeBegin(void);
void AuroraFrontendProfilerSnesDspBrrDecodeEnd(void);
void AuroraFrontendProfilerSnesDspPitchmodFeederBegin(void);
void AuroraFrontendProfilerSnesDspPitchmodFeederEnd(void);
void AuroraFrontendProfilerSnesDspMixStereoBegin(Bool noise, Bool echo, Uint32 samples);
void AuroraFrontendProfilerSnesDspMixStereoEnd(Bool echo);
void AuroraFrontendProfilerSnesDspSilentFastpath(void);
void AuroraFrontendProfilerSnesDspVoiceGain(
    Uint32 envProxy, Int32 volL, Int32 volR,
    Bool pmon, Bool echo, Bool feedsPitchMod);

void AuroraFrontendProfilerSnesDspEchoBegin(void);
void AuroraFrontendProfilerSnesDspEchoEnd(void);
void AuroraFrontendProfilerSnesDspFinalBegin(void);
void AuroraFrontendProfilerSnesDspFinalEnd(void);

/* AURORA_PROFILER_AUTO20_STATELOAD_V21_20261009
 * Start a new host/frontend benchmark and synchronously save at end-of-ROM.
 * Do not touch emulated CPU clocks, memory, frame policies or audio. */
Bool AuroraFrontendProfilerAutoCloseBeginWindow(void);
Bool AuroraFrontendProfilerAutoCloseSave(void);
void AuroraFrontendProfilerMenuEnter(void);
void AuroraFrontendProfilerMenuUpdate(Bool canWrite);

#ifdef __cplusplus
}
#endif
#endif
#endif
