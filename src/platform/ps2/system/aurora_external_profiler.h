#ifndef AURORA_EXTERNAL_PROFILER_H
#define AURORA_EXTERNAL_PROFILER_H

#include <stdint.h>

#ifndef AURORA_FRONTEND_PROFILER
#define AURORA_FRONTEND_PROFILER 0
#endif

/* AURORA_MULTICORE_DEEP_PROFILER_V3_48_20261008
 * Numeric values deliberately mirror AuroraFrontendCoreE. */
enum AuroraExternalCoreE
{
    AURORA_EXT_CORE_SEGA = 4,
    AURORA_EXT_CORE_PCE  = 5,
    AURORA_EXT_CORE_GBA  = 7
};

enum AuroraPceDeepBucketE
{
    AURORA_PCE_BRIDGE_RETRO_RUN = 0,
    AURORA_PCE_BRIDGE_RENDER,
    AURORA_PCE_BRIDGE_MIX_FLUSH,
    AURORA_PCE_RETRO_INPUT,
    AURORA_PCE_EMULATE_TOTAL,
    AURORA_PCE_RETRO_VIDEO_CB,
    AURORA_PCE_RETRO_AUDIO_CB,
    AURORA_PCE_EMU_INPUT_FRAME,
    AURORA_PCE_VDC_FRAME,
    AURORA_PCE_EMU_CD_FINAL,
    AURORA_PCE_EMU_PSG_END,
    AURORA_PCE_EMU_BLIP_READ,
    AURORA_PCE_VDC_CPU,
    AURORA_PCE_VDC_BG,
    AURORA_PCE_VDC_SPR,
    AURORA_PCE_VDC_MIX,
    AURORA_PCE_VDC_OVERSCAN,
    AURORA_PCE_VDC_DMA,
    AURORA_PCE_VDC_SATDMA,
    AURORA_PCE_VDC_CD_SYNC,
    AURORA_PCE_PSG_UPDATE,
    AURORA_PCE_CD_RUN,
    AURORA_PCE_CD_DRIVE,
    AURORA_PCE_CD_ADPCM,
    AURORA_PCE_CD_FADER,

    /* AURORA_PCE_SPRITE_DEEP_PROFILER_V3_49_20261008: cycle partitions inside VDC_SPR. */
    AURORA_PCE_VDC_SPR_PREP,
    AURORA_PCE_VDC_SPR_CLEAR,
    AURORA_PCE_VDC_SPR_DRAW,

    /* Work-only buckets: cycles=0, units carry the semantic counter. */
    AURORA_PCE_SPR_WORK_LINES,
    AURORA_PCE_SPR_WORK_NONEMPTY_LINES,
    AURORA_PCE_SPR_WORK_LINE_UNITS,
    AURORA_PCE_SPR_WORK_NATIVE_SELECTED,
    AURORA_PCE_SPR_WORK_POST_LIMIT,
    AURORA_PCE_SPR_WORK_SOURCE_SPRITES,
    AURORA_PCE_SPR_WORK_OVERFLOW_LINES,
    AURORA_PCE_SPR_WORK_GE8_LINES,
    AURORA_PCE_SPR_WORK_GE12_LINES,
    AURORA_PCE_SPR_WORK_GE16_LINES,
    AURORA_PCE_SPR_WORK_MAX_UNITS,
    AURORA_PCE_SPR_WORK_MAX_SOURCES,
    AURORA_PCE_SPR_WORK_OFFSCREEN,
    AURORA_PCE_SPR_WORK_PARTIAL_CLIP,
    AURORA_PCE_SPR_WORK_DRAWN_UNITS,
    AURORA_PCE_SPR_WORK_ZERO_CHUNKS,
    AURORA_PCE_SPR_WORK_OPAQUE_CHUNKS,
    AURORA_PCE_SPR_WORK_MIXED_CHUNKS,
    AURORA_PCE_SPR_WORK_SPRITE0_UNITS,
    AURORA_PCE_SPR_WORK_WRITTEN_PIXELS,
    AURORA_PCE_BUCKET_COUNT
};

enum AuroraGbaDeepBucketE
{
    AURORA_GBA_FRAME_INPUT = 0,
    AURORA_GBA_CPU_EXEC,
    AURORA_GBA_AUDIO_RUN,
    AURORA_GBA_VIDEO_RUN,
    AURORA_GBA_SERIAL_FRAME,
    AURORA_GBA_UPDATE_GBA_TOTAL,
    AURORA_GBA_UPDATE_TIMERS,
    AURORA_GBA_SCANLINE_TOTAL,
    AURORA_GBA_SOUND_FRAME_FLUSH,
    AURORA_GBA_DMA_TOTAL,
    AURORA_GBA_RASTER_WINDOW,
    AURORA_GBA_RASTER_TEXT_BG,
    AURORA_GBA_RASTER_AFFINE_BG,
    AURORA_GBA_RASTER_BITMAP_BG,
    AURORA_GBA_RASTER_OBJ,
    AURORA_GBA_RASTER_EFFECTS,
    AURORA_GBA_DMA_CH0,
    AURORA_GBA_DMA_CH1,
    AURORA_GBA_DMA_CH2,
    AURORA_GBA_DMA_CH3,
    AURORA_GBA_AUDIO_READ,
    AURORA_GBA_VIDEO_POSTPROCESS,
    AURORA_GBA_BUCKET_COUNT
};

enum AuroraSegaDeepBucketE
{
    AURORA_SEGA_BRIDGE_CORE_RUN = 0,
    AURORA_SEGA_BRIDGE_RENDER,
    AURORA_SEGA_BRIDGE_MIX_FLUSH,
    AURORA_SEGA_PICO_FRAME,
    AURORA_SEGA_FRAME_HINTS,
    AURORA_SEGA_M68K,
    AURORA_SEGA_Z80,
    AURORA_SEGA_VDP_SYNC,
    AURORA_SEGA_VDP_FULL,
    AURORA_SEGA_SOUND_TOTAL,
    AURORA_SEGA_SOUND_PSG,
    AURORA_SEGA_SOUND_DAC,
    AURORA_SEGA_SOUND_FM,
    AURORA_SEGA_SOUND_PCM,
    AURORA_SEGA_SOUND_CDDA,
    AURORA_SEGA_SOUND_MIX,
    AURORA_SEGA_TIMERS,
    AURORA_SEGA_MCD_S68K,
    AURORA_SEGA_BUCKET_COUNT
};

#define AURORA_EXT_BUCKET_MAX 48u

#if AURORA_FRONTEND_PROFILER
#ifdef __cplusplus
extern "C" {
#endif
void AuroraFrontendProfilerExternalRecord(
    uint32_t core, uint32_t bucket, uint32_t cycles, uint32_t units);
#ifdef __cplusplus
}
#endif

static inline uint32_t AuroraExternalProfilerCycle(void)
{
#if defined(__mips__) || defined(_EE) || defined(PS2)
    uint32_t v;
    __asm__ volatile("mfc0 %0,$9" : "=r"(v));
    return v;
#else
    return 0;
#endif
}

#define AURORA_EXT_BEGIN(name_) \
    uint32_t name_ = AuroraExternalProfilerCycle()
#define AURORA_EXT_RECORD(core_, bucket_, cycles_, units_) \
    AuroraFrontendProfilerExternalRecord((uint32_t)(core_), (uint32_t)(bucket_), \
        (uint32_t)(cycles_), (uint32_t)(units_))
#define AURORA_EXT_END(core_, bucket_, name_, units_) \
    AURORA_EXT_RECORD((core_), (bucket_), \
        (uint32_t)(AuroraExternalProfilerCycle() - (name_)), (units_))
#define AURORA_EXT_ACCUM_DECL(cycles_, calls_) \
    uint32_t cycles_ = 0, calls_ = 0
#define AURORA_EXT_ACCUM(cycles_, calls_, ...) \
    do { \
        uint32_t _aur_ext_t = AuroraExternalProfilerCycle(); \
        __VA_ARGS__; \
        cycles_ += (uint32_t)(AuroraExternalProfilerCycle() - _aur_ext_t); \
        ++calls_; \
    } while (0)
#else
#define AURORA_EXT_BEGIN(name_) do { } while (0)
#define AURORA_EXT_RECORD(core_, bucket_, cycles_, units_) do { } while (0)
#define AURORA_EXT_END(core_, bucket_, name_, units_) do { } while (0)
#define AURORA_EXT_ACCUM_DECL(cycles_, calls_)
#define AURORA_EXT_ACCUM(cycles_, calls_, ...) do { __VA_ARGS__; } while (0)
#endif

#endif
