#ifndef AURORA_SNES_COST_PROFILER_H
#define AURORA_SNES_COST_PROFILER_H

/* AURORA_SNES_COST_PROFILER_V1_20260920
 * AURORA_SNES_PPU_BREAKDOWN_V2_20260920
 * AURORA_SNES_PPU_FOCUS_V3_20260920
 * Diagnostic-only SNES host-cycle profiler.
 *
 * Top-level buckets and PPU-detail buckets are independently exclusive.
 * Entering a child scope pauses its parent. Thus BG OTHER and COLOR OTHER
 * contain only work not charged to their named child scopes.
 *
 * Build capability is controlled by AURORA_SNES_COST_PROFILER. Runtime state
 * starts OFF and is intentionally not serialized in video.cfg.
 */

#include "types.h"

#ifndef AURORA_SNES_COST_PROFILER
#define AURORA_SNES_COST_PROFILER 0
#endif

#ifndef AURORA_FRONTEND_PROFILER
#define AURORA_FRONTEND_PROFILER 0
#endif

/* AURORA_SNES_CORE_PROFILER_V3_6_20261005
 * Forward declarations only: avoid coupling this generic SNES profiler header
 * to the PS2 frontend header while allowing the existing scope callsites to
 * feed auroraprofile in diagnostic builds. */
#if AURORA_FRONTEND_PROFILER
#ifdef __cplusplus
extern "C" {
#endif
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
void AuroraFrontendProfilerSnesBgChrRecord(
    Uint32 bgIndex, Uint32 bitDepth, Uint32 tiles,
    Bool offset, Bool mosaic, Bool fineXNonZero, Bool hiresSubscreen,
    Uint32 startCycles, Uint32 decodeEndCycles, Uint32 endCycles);

/* AURORA_SNES_DEEP_OPT_V3_14_20261005 */
enum AuroraSnesDeepBucketE
{
    AURORA_SNES_DEEP_M7_SETUP = 0,
    AURORA_SNES_DEEP_M7_FETCH_REPEAT_DY0 = 1,
    AURORA_SNES_DEEP_M7_FETCH_REPEAT_UNITX = 2,
    AURORA_SNES_DEEP_M7_FETCH_REPEAT_GENERIC = 3,
    AURORA_SNES_DEEP_M7_FETCH_CLAMP = 4,
    AURORA_SNES_DEEP_M7_FETCH_BLACK = 5,
    AURORA_SNES_DEEP_M7_HALF_EXPAND = 6,
    AURORA_SNES_DEEP_M7_OPAQUE = 7,
    AURORA_SNES_DEEP_M7_EXTBG_COPY = 8,
    AURORA_SNES_DEEP_M7_EXTBG_PRIORITY = 9,
    AURORA_SNES_DEEP_M7_EXTBG_OPAQUE = 10,
    AURORA_SNES_DEEP_M7_MOSAIC_BG1 = 11,
    AURORA_SNES_DEEP_M7_MOSAIC_BG2 = 12,
    AURORA_SNES_DEEP_PPU_QUEUE_APPLY = 13,
    AURORA_SNES_DEEP_PPU_VBLANK_DRAIN = 14,
    AURORA_SNES_DEEP_BLEND_PLANAR = 15,
    AURORA_SNES_DEEP_BLEND_GIF_WAIT = 16,
    AURORA_SNES_DEEP_BLEND_LIST_REBUILD = 17,
    AURORA_SNES_DEEP_BLEND_STAGE_COPY = 18,
    AURORA_SNES_DEEP_BLEND_SET_PARAMS = 19,
    AURORA_SNES_DEEP_BLEND_KICK = 20,
    AURORA_SNES_DEEP_BG_MAIN_BG1 = 21,
    AURORA_SNES_DEEP_BG_MAIN_BG2 = 22,
    AURORA_SNES_DEEP_BG_MAIN_BG3 = 23,
    AURORA_SNES_DEEP_BG_MAIN_BG4 = 24,
    AURORA_SNES_DEEP_BG_SUB_BG1 = 25,
    AURORA_SNES_DEEP_BG_SUB_BG2 = 26,
    AURORA_SNES_DEEP_BG_SUB_BG3 = 27,
    AURORA_SNES_DEEP_BG_SUB_BG4 = 28,
    AURORA_SNES_DEEP_OBJ_ROTATE = 29,
    AURORA_SNES_DEEP_OBJ_FETCH_DECODE = 30,
    AURORA_SNES_DEEP_HDMA_DATA_PHASE = 31,
    AURORA_SNES_DEEP_HDMA_TABLE_PHASE = 32,
    AURORA_SNES_DEEP_MDMA_FAST = 33,
    AURORA_SNES_DEEP_MDMA_ACCURATE = 34,
    AURORA_SNES_DEEP_MDMA_READ = 35,
    AURORA_SNES_DEEP_MDMA_SDD1 = 36,
    AURORA_SNES_DEEP_CPU_EXEC = 37,
    AURORA_SNES_DEEP_CPU_SA1 = 38,
    AURORA_SNES_DEEP_CPU_WAI = 39,
    AURORA_SNES_DEEP_BG_MAIN_DIRECT = 40,
    AURORA_SNES_DEEP_BG_SUB_DIRECT = 41,
    AURORA_SNES_DEEP_CHR2_OPAQUE_ROWS = 42,
    AURORA_SNES_DEEP_CHR2_TRANSPARENT_ROWS = 43,
    AURORA_SNES_DEEP_CHR4_OPAQUE_ROWS = 44,
    AURORA_SNES_DEEP_CHR4_TRANSPARENT_ROWS = 45,
    AURORA_SNES_DEEP_CHR2_OPAQUE_FLIP0 = 46,
    AURORA_SNES_DEEP_CHR2_OPAQUE_FLIPPED = 47,
    AURORA_SNES_DEEP_CHR4_OPAQUE_FLIP0 = 48,
    AURORA_SNES_DEEP_CHR4_OPAQUE_FLIPPED = 49,

    /* AURORA_SNES_TARGET_TRAITS_V3_21_1_20261006 */
    AURORA_SNES_DEEP_M7_CLAMP_INSIDE_PIXELS = 50,
    AURORA_SNES_DEEP_M7_CLAMP_OUTSIDE_PIXELS = 51,
    AURORA_SNES_DEEP_M7_CLAMP_TILE_REUSE = 52,
    AURORA_SNES_DEEP_M7_CLAMP_TILE_LOADS = 53,
    AURORA_SNES_DEEP_M7_CLAMP_BOUNDARY_TRANSITIONS = 54,
    AURORA_SNES_DEEP_M7_CLAMP_ALL_INSIDE_CALLS = 55,
    AURORA_SNES_DEEP_M7_CLAMP_MIXED_CALLS = 56,
    AURORA_SNES_DEEP_M7_CLAMP_ALL_OUTSIDE_CALLS = 57,
    AURORA_SNES_DEEP_M7_CLAMP_DX0_CALLS = 58,
    AURORA_SNES_DEEP_M7_CLAMP_DY0_CALLS = 59,
    AURORA_SNES_DEEP_M7_CLAMP_UNITX_CALLS = 60,
    AURORA_SNES_DEEP_M7_CLAMP_UNITY_CALLS = 61,
    AURORA_SNES_DEEP_OBJ_CACHE_HITS = 62,
    AURORA_SNES_DEEP_OBJ_CACHE_MISSES = 63,
    AURORA_SNES_DEEP_OBJ_MISS_TRANSPARENT = 64,
    AURORA_SNES_DEEP_OBJ_MISS_OPAQUE = 65,
    AURORA_SNES_DEEP_OBJ_HFLIP_TILES = 66,
    AURORA_SNES_DEEP_OBJ_SECOND_TABLE_TILES = 67,
    /* AURORA_LUT_OBJ2_FINAL_V3_41_20261008
     * cycles = post-fetch compactor only; units = transparent rows removed. */
    AURORA_SNES_DEEP_OBJ_COMPACT = 68,
    AURORA_SNES_DEEP_COUNT = 69
};
void AuroraFrontendProfilerSnesDeepRecord(
    Uint32 bucket, Uint32 cycles, Uint32 units);
void AuroraFrontendProfilerSnesChrTraitsRecord(
    Uint32 bitDepth, Uint32 opaqueRows, Uint32 transparentRows,
    Uint32 flip0OpaqueRows, Uint32 flippedOpaqueRows);
/* AURORA_V23_3_TRAITS_COST_DECL_20261010: declaration used by snppurender8.cpp. */
void AuroraFrontendProfilerSnesChrTraitsCost(Uint32 ticks);

/* AURORA_SNES_MICROSCOPE_V3_27_20261006
 * Diagnostic-only aggregate recorders.  Callers are compile-gated by
 * AURORA_FRONTEND_PROFILER, so normal builds gain no hot-path work. */
void AuroraFrontendProfilerSnesChr4LocalityRecord(
    Uint32 tiles, Uint32 opaqueTiles,
    Uint32 paletteReuses, Uint32 paletteRuns,
    Uint32 flipSame, Uint32 flipChanges,
    Uint32 rowSame, Uint32 rowPlus16, Uint32 rowMinus16, Uint32 rowOther);
void AuroraFrontendProfilerSnesObjMicroscopeRecord(
    Uint32 hitTiles, Uint32 hitCycles,
    Uint32 missTiles, Uint32 missCycles,
    Uint32 hitTransparent, Uint32 hitFullOpaque, Uint32 hitPartial,
    Uint32 missTransparent, Uint32 missFullOpaque, Uint32 missPartial,
    Uint32 rowSame, Uint32 rowPlus16, Uint32 rowMinus16, Uint32 rowOther);
/* AURORA_CHR4_REUSE_MICROSCOPE_V3_28_20261007
 * Final CHR4 correlation lens; diagnostic-only. */
/* AURORA_CHR4_PATH_BG_MICROSCOPE_V3_29_20261007 */
void AuroraFrontendProfilerSnesChr4ReuseRecord(
    Uint32 bgIndex, Uint32 pathIndex, Uint32 tiles, Uint32 opaqueTiles,
    Uint32 opaqueTransitions, Uint32 adjacentOpaqueTransitions,
    Uint32 paletteIndexReuses, Uint32 decodeKeyReuses,
    Uint32 rowSameFlipChanged, Uint32 pixelKeyReuses,
    Uint32 adjacentPaletteIndexReuses, Uint32 adjacentDecodeKeyReuses,
    Uint32 adjacentPixelKeyReuses);
#ifdef __cplusplus
}
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum AuroraSnesCostBucketE
{
    AURORA_SNES_COST_CPU = 0,
    AURORA_SNES_COST_PPU,
    AURORA_SNES_COST_RASTER,
    AURORA_SNES_COST_SPC,
    AURORA_SNES_COST_DSP,
    AURORA_SNES_COST_BUCKET_COUNT
};

enum AuroraSnesPpuDetailBucketE
{
    AURORA_SNES_PPU_DETAIL_SYNC = 0,
    AURORA_SNES_PPU_DETAIL_RENDER,
    AURORA_SNES_PPU_DETAIL_PREP,
    AURORA_SNES_PPU_DETAIL_RASTER,

    AURORA_SNES_PPU_DETAIL_BG_MAP,
    AURORA_SNES_PPU_DETAIL_BG_CHR,
    AURORA_SNES_PPU_DETAIL_BG_MAIN,
    AURORA_SNES_PPU_DETAIL_BG_SUB,
    AURORA_SNES_PPU_DETAIL_BG_OTHER,

    AURORA_SNES_PPU_DETAIL_OBJ, /* V4 legacy aggregate slot; V5 leaves it unused. */
    /* AURORA_SNES_PROFILER_DEEP_V5_20260920: existing OBJ scopes, relabeled by phase; no new timing scopes. */
    AURORA_SNES_PPU_DETAIL_OBJ_UPDATE,
    AURORA_SNES_PPU_DETAIL_OBJ_FETCH,
    AURORA_SNES_PPU_DETAIL_OBJ_MAIN,
    AURORA_SNES_PPU_DETAIL_OBJ_SUB,
    AURORA_SNES_PPU_DETAIL_MODE7,

    AURORA_SNES_PPU_DETAIL_COLOR_MASK,
    AURORA_SNES_PPU_DETAIL_BLEND,
    AURORA_SNES_PPU_DETAIL_COLOR_OTHER,

    AURORA_SNES_PPU_DETAIL_COUNT
};

typedef struct AuroraSnesCostFrameT
{
    Uint32 total;
    Uint32 bucket[AURORA_SNES_COST_BUCKET_COUNT];
} AuroraSnesCostFrameT;

#if AURORA_SNES_COST_PROFILER

Bool AuroraSnesCostProfilerSetEnabled(Bool enabled);
Bool AuroraSnesCostProfilerIsEnabled(void);
Bool AuroraSnesCostProfilerGetLastFrame(AuroraSnesCostFrameT *out);
void AuroraSnesCostProfilerFrameBegin(void);
void AuroraSnesCostProfilerFrameEnd(void);
void AuroraSnesCostProfilerScopeEnter(Int32 bucket);
void AuroraSnesCostProfilerScopeLeave(Int32 bucket);
void AuroraSnesCostProfilerPpuEnter(Int32 bucket);
void AuroraSnesCostProfilerPpuLeave(Int32 bucket);
void AuroraSnesCostProfilerPpuWork(Uint32 mainMask, Uint32 subMask,
                                   Uint32 mapFetches, Uint32 chrDecodes);
void AuroraSnesCostProfilerDrawOverlay(void);

#else

#define AuroraSnesCostProfilerSetEnabled(_enabled) FALSE
#define AuroraSnesCostProfilerIsEnabled() FALSE
#define AuroraSnesCostProfilerGetLastFrame(_out) FALSE
#define AuroraSnesCostProfilerFrameBegin() ((void)0)
#define AuroraSnesCostProfilerFrameEnd() ((void)0)
#define AuroraSnesCostProfilerScopeEnter(_bucket) ((void)0)
#define AuroraSnesCostProfilerScopeLeave(_bucket) ((void)0)
#define AuroraSnesCostProfilerPpuEnter(_bucket) ((void)0)
#define AuroraSnesCostProfilerPpuLeave(_bucket) ((void)0)
#define AuroraSnesCostProfilerPpuWork(_main,_sub,_map,_chr) ((void)0)
#define AuroraSnesCostProfilerDrawOverlay() ((void)0)

#endif

#ifdef __cplusplus
}
#endif

/* AURORA_SNES_CORE_PROFILER_V3_6_20261005 */
#if AURORA_FRONTEND_PROFILER
#define AURORA_SNES_FP_COST_BEGIN(_bucket) \
    AuroraFrontendProfilerSnesCostScopeEnter((Int32)(_bucket))
#define AURORA_SNES_FP_COST_END(_bucket) \
    AuroraFrontendProfilerSnesCostScopeLeave((Int32)(_bucket))
#define AURORA_SNES_FP_PPU_BEGIN(_bucket) \
    AuroraFrontendProfilerSnesPpuDetailEnter((Int32)(_bucket))
#define AURORA_SNES_FP_PPU_END(_bucket) \
    AuroraFrontendProfilerSnesPpuDetailLeave((Int32)(_bucket))
#define AURORA_SNES_FP_PPU_WORK(_main,_sub,_map,_chr) \
    AuroraFrontendProfilerSnesPpuWork((_main),(_sub),(_map),(_chr))
#define AURORA_SNES_PPU_LINE(_mode,_refs,_tiles,_enabled,_range,_time,_win,_cm,_sub,_hires,_dc) \
    AuroraFrontendProfilerSnesPpuLine( \
        (_mode),(_refs),(_tiles),(_enabled),(_range),(_time), \
        (_win),(_cm),(_sub),(_hires),(_dc))
#define AURORA_SNES_BGCHR_RECORD(_bg,_depth,_tiles,_offset,_mosaic,_finex,_hires,_start,_decode_end,_end) \
    AuroraFrontendProfilerSnesBgChrRecord( \
        (_bg),(_depth),(_tiles),(_offset),(_mosaic),(_finex),(_hires), \
        (_start),(_decode_end),(_end))
#define AURORA_SNES_DEEP_RECORD(_bucket,_cycles,_units) \
    AuroraFrontendProfilerSnesDeepRecord((Uint32)(_bucket),(Uint32)(_cycles),(Uint32)(_units))
#define AURORA_SNES_CHR_TRAITS_RECORD(_depth,_opaque,_transparent,_flip0,_flipped) \
    AuroraFrontendProfilerSnesChrTraitsRecord((Uint32)(_depth),(Uint32)(_opaque),(Uint32)(_transparent),(Uint32)(_flip0),(Uint32)(_flipped))
#else
#define AURORA_SNES_FP_COST_BEGIN(_bucket) ((void)0)
#define AURORA_SNES_FP_COST_END(_bucket) ((void)0)
#define AURORA_SNES_FP_PPU_BEGIN(_bucket) ((void)0)
#define AURORA_SNES_FP_PPU_END(_bucket) ((void)0)
#define AURORA_SNES_FP_PPU_WORK(_main,_sub,_map,_chr) ((void)0)
#define AURORA_SNES_PPU_LINE(_mode,_refs,_tiles,_enabled,_range,_time,_win,_cm,_sub,_hires,_dc) ((void)0)
#define AURORA_SNES_BGCHR_RECORD(_bg,_depth,_tiles,_offset,_mosaic,_finex,_hires,_start,_decode_end,_end) ((void)0)
#define AURORA_SNES_DEEP_RECORD(_bucket,_cycles,_units) ((void)0)
#define AURORA_SNES_CHR_TRAITS_RECORD(_depth,_opaque,_transparent,_flip0,_flipped) ((void)0)
#endif

#define AURORA_SNES_COST_SCOPE_BEGIN(_bucket) do { \
    AuroraSnesCostProfilerScopeEnter((Int32)(_bucket)); \
    AURORA_SNES_FP_COST_BEGIN((_bucket)); \
} while (0)
#define AURORA_SNES_COST_SCOPE_END(_bucket) do { \
    AuroraSnesCostProfilerScopeLeave((Int32)(_bucket)); \
    AURORA_SNES_FP_COST_END((_bucket)); \
} while (0)

#define AURORA_SNES_PPU_DETAIL_BEGIN(_bucket) do { \
    AuroraSnesCostProfilerPpuEnter((Int32)(_bucket)); \
    AURORA_SNES_FP_PPU_BEGIN((_bucket)); \
} while (0)
#define AURORA_SNES_PPU_DETAIL_END(_bucket) do { \
    AuroraSnesCostProfilerPpuLeave((Int32)(_bucket)); \
    AURORA_SNES_FP_PPU_END((_bucket)); \
} while (0)
#define AURORA_SNES_PPU_WORK(_main,_sub,_map,_chr) do { \
    AuroraSnesCostProfilerPpuWork((_main),(_sub),(_map),(_chr)); \
    AURORA_SNES_FP_PPU_WORK((_main),(_sub),(_map),(_chr)); \
} while (0)

/* RAII is used only for scopes with early returns (Sync/RenderLine). */
#if (AURORA_SNES_COST_PROFILER || AURORA_FRONTEND_PROFILER) && defined(__cplusplus)
class AuroraSnesPpuDetailAutoScope
{
public:
    explicit AuroraSnesPpuDetailAutoScope(Int32 bucket)
        : m_bucket(bucket)
    {
        AURORA_SNES_PPU_DETAIL_BEGIN(m_bucket);
    }
    ~AuroraSnesPpuDetailAutoScope()
    {
        AURORA_SNES_PPU_DETAIL_END(m_bucket);
    }
private:
    Int32 m_bucket;
};
#define AURORA_SNES_JOIN2(_a,_b) _a##_b
#define AURORA_SNES_JOIN(_a,_b) AURORA_SNES_JOIN2(_a,_b)
#define AURORA_SNES_PPU_DETAIL_AUTO(_bucket) \
    AuroraSnesPpuDetailAutoScope \
        AURORA_SNES_JOIN(_auroraSnesPpuDetail_, __LINE__)((Int32)(_bucket))
#else
#define AURORA_SNES_PPU_DETAIL_AUTO(_bucket) ((void)0)
#endif

#endif
