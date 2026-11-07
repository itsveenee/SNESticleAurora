/* AURORA_SMK_PROFILER_V2_20261003
 * Super Mario Kart DSP-1 + Mode 7 diagnostic profiler.
 * V2 adds Clamp path-local timing and exact O(1) scanline geometry counters.
 *
 * This TU is added to SRCS only with AURORA_SMK_PROFILER=1.  Runtime
 * collection is additionally exact-CRC gated.  Gameplay hot paths only update
 * fixed-size counters; all RTC and filesystem work happens after menu entry.
 */
#include "aurora_smk_profiler.h"
#include "mainloop_safe_frameskip.h"

#if AURORA_SMK_PROFILER

#include <stdio.h>
#include <string.h>
#include <libcdvd.h>
#include <osd_config.h>

#include "prof.h"
#include "mainloop_bgm.h"
#include "mainloop_ui.h"

namespace
{

static const Uint32 SMK_CRC_USA = 0xCD80DB86u;
static const Uint32 SMK_CRC_JAPAN = 0xC8002453u;
static const Uint32 SMK_CRC_EUROPE = 0x56410E5Eu;

enum
{
    SMK_DSP_OPS = 64,
    SMK_MENU_WRITE_DELAY = 1
};

struct SmkStatsT
{
    Uint64 frameCycles;
    Uint32 frames;
    Uint32 mode7Frames;

    Uint64 dspCycles[SMK_DSP_OPS];
    Uint32 dspCalls[SMK_DSP_OPS];

    Uint64 mode7TotalCycles;
    Uint64 mode7SetupCycles;
    Uint64 mode7FetchCycles;
    Uint64 mode7PostCycles;
    Uint32 mode7Lines;
    Uint64 mode7Pixels;

    Uint64 mode7MainComposeCycles;
    Uint64 mode7SubComposeCycles;
    Uint32 mode7MainComposeCalls;
    Uint32 mode7SubComposeCalls;

    Uint32 repeatMode[4];
    Uint32 repeatDy0;
    Uint32 repeatUnitX;
    Uint32 repeatGeneric;
    Uint32 dxZero;
    Uint32 dxUnit;
    Uint32 dxOther;
    Uint32 dyZero;
    Uint32 m7bZero;
    Uint32 m7cZero;
    Uint32 flipX;
    Uint32 flipY;
    Uint32 mosaicLines;
    Uint32 halfLines;

    Uint64 clampDy0FetchCycles;
    Uint32 clampDy0Lines;
    Uint64 clampGenericFetchCycles;
    Uint32 clampGenericLines;

    Uint64 clampGenericInPixels;
    Uint64 clampGenericOutPixels;
    Uint32 clampGenericFullInLines;
    Uint32 clampGenericMixedLines;
    Uint32 clampGenericFullOutLines;
    Uint64 clampGenericSourceYChanges;
    Uint64 clampGenericTileXChanges;
    Uint64 clampGenericTileYChanges;
};

struct SmkStampT
{
    Uint32 year, month, day, hour, minute, second;
    Bool valid;
};

struct SmkSnapshotT
{
    SmkStatsT stats;
    SmkStampT stamp;
    Uint32 crc32;
    Uint32 clockPairMin;
    Uint32 serial;
    Char title[64];
};

static SmkStatsT s_live;
static SmkSnapshotT s_pending;
static Uint32 s_crc32 = 0;
static Uint32 s_clockPairMin = 0;
static Uint32 s_frameStart = 0;
static Uint32 s_serial = 0;
static Bool s_frameOpen = FALSE;
static Bool s_frameSawMode7 = FALSE;
static Bool s_writePending = FALSE;
static Int32 s_writeDelay = 0;
static Char s_title[64] = { 0 };

static void ResetLive(void)
{
    memset(&s_live, 0, sizeof(s_live));
    s_frameOpen = FALSE;
    s_frameSawMode7 = FALSE;
}

static Bool IsSmkCRC(Uint32 crc)
{
    crc = AuroraRomIdentityCRCForDetection(crc);
    return (crc == SMK_CRC_USA || crc == SMK_CRC_JAPAN ||
            crc == SMK_CRC_EUROPE) ? TRUE : FALSE;
}

static const char *RegionName(Uint32 crc)
{
    crc = AuroraRomIdentityCRCForDetection(crc);
    switch (crc)
    {
        case SMK_CRC_USA: return "USA";
        case SMK_CRC_JAPAN: return "Japan";
        case SMK_CRC_EUROPE: return "Europe";
        default: return "Unknown";
    }
}

static Uint32 Bcd(Uint8 value)
{
    return (Uint32)(((value >> 4) & 0x0Fu) * 10u + (value & 0x0Fu));
}

static SmkStampT CaptureStamp(void)
{
    SmkStampT s;
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
    s.valid = TRUE;
    return s;
}

static Uint32 CalibrateClockPair(void)
{
    Uint32 best = 0xFFFFFFFFu;
    for (Int32 i = 0; i < 32; ++i)
    {
        const Uint32 a = ProfCtrGetCycle();
        const Uint32 b = ProfCtrGetCycle();
        const Uint32 d = (Uint32)(b - a);
        if (d < best) best = d;
    }
    return (best == 0xFFFFFFFFu) ? 0u : best;
}

static Uint32 Percent10(Uint64 part, Uint64 total)
{
    if (!total) return 0;
    Uint64 v = (part * 1000u + total / 2u) / total;
    if (v > 99999u) v = 99999u;
    return (Uint32)v;
}

static Uint64 Avg(Uint64 total, Uint32 count)
{
    return count ? ((total + count / 2u) / count) : 0u;
}

/* Geometry diagnostics are deliberately computed after the measured Mode 7
 * fetch.  They use fixed O(1) arithmetic per scanline: no shadow 256-pixel
 * loop is added to the timed renderer hot path. */
static long long FloorDivLL(long long a, long long b)
{
    long long q = a / b;
    const long long r = a % b;
    if (r < 0) --q;
    return q;
}

static long long CeilDivLL(long long a, long long b)
{
    return -FloorDivLL(-a, b);
}

static long long FloorShiftLL(long long value, Uint32 shift)
{
    return FloorDivLL(value, (long long)1 << shift);
}

static Uint32 ShiftTransitions(
    long long value, long long delta, Int32 count, Uint32 shift)
{
    if (count <= 1 || delta == 0)
        return 0u;

    const long long scale = (long long)1 << shift;
    if (delta >= scale || delta <= -scale)
        return (Uint32)(count - 1);

    const long long first = FloorShiftLL(value, shift);
    const long long last =
        FloorShiftLL(value + (long long)(count - 1) * delta, shift);
    const long long diff = last - first;
    return (Uint32)(diff < 0 ? -diff : diff);
}

static Bool AxisInRangeInterval(
    long long value, long long delta, Int32 count, Int32 *outLo, Int32 *outHi)
{
    const long long kMax = 1024LL * 256LL - 1LL;
    if (count <= 0)
        return FALSE;

    long long lo = 0;
    long long hi = (long long)count - 1LL;

    if (delta == 0)
    {
        if (value < 0 || value > kMax)
            return FALSE;
    }
    else if (delta > 0)
    {
        const long long a = CeilDivLL(-value, delta);
        const long long b = FloorDivLL(kMax - value, delta);
        if (a > lo) lo = a;
        if (b < hi) hi = b;
    }
    else
    {
        const long long step = -delta;
        const long long a = CeilDivLL(value - kMax, step);
        const long long b = FloorDivLL(value, step);
        if (a > lo) lo = a;
        if (b < hi) hi = b;
    }

    if (lo < 0) lo = 0;
    if (hi >= count) hi = (long long)count - 1LL;
    if (lo > hi)
        return FALSE;

    *outLo = (Int32)lo;
    *outHi = (Int32)hi;
    return TRUE;
}

static void AccumulateClampGenericGeometry(
    Int32 x, Int32 y, Int32 dx, Int32 dy, Int32 nPixels)
{
    Int32 xLo = 0, xHi = -1;
    Int32 yLo = 0, yHi = -1;
    const Bool xOk = AxisInRangeInterval(x, dx, nPixels, &xLo, &xHi);
    const Bool yOk = AxisInRangeInterval(y, dy, nPixels, &yLo, &yHi);

    Int32 lo = 0;
    Int32 hi = -1;
    if (xOk && yOk)
    {
        lo = xLo > yLo ? xLo : yLo;
        hi = xHi < yHi ? xHi : yHi;
    }

    const Uint32 inPixels =
        (hi >= lo) ? (Uint32)(hi - lo + 1) : 0u;
    const Uint32 totalPixels = nPixels > 0 ? (Uint32)nPixels : 0u;
    const Uint32 outPixels =
        totalPixels > inPixels ? totalPixels - inPixels : 0u;

    s_live.clampGenericInPixels += inPixels;
    s_live.clampGenericOutPixels += outPixels;
    s_live.clampGenericSourceYChanges +=
        ShiftTransitions(y, dy, nPixels, 8u);

    if (inPixels == totalPixels && totalPixels)
        ++s_live.clampGenericFullInLines;
    else if (!inPixels)
        ++s_live.clampGenericFullOutLines;
    else
        ++s_live.clampGenericMixedLines;

    if (inPixels)
    {
        const long long x0 = (long long)x + (long long)lo * dx;
        const long long y0 = (long long)y + (long long)lo * dy;
        s_live.clampGenericTileXChanges +=
            ShiftTransitions(x0, dx, (Int32)inPixels, 11u);
        s_live.clampGenericTileYChanges +=
            ShiftTransitions(y0, dy, (Int32)inPixels, 11u);
    }
}

static const char *DspName(Uint8 op)
{
    switch (op & 0x3Fu)
    {
        case 0x00: return "Multiply";
        case 0x20: return "Multiply2";
        case 0x04: case 0x24: return "Triangle";
        case 0x08: return "Radius";
        case 0x18: return "Range";
        case 0x38: return "Range2";
        case 0x28: return "Distance";
        case 0x0C: case 0x2C: return "Rotate";
        case 0x1C: case 0x3C: return "Polar";
        case 0x10: case 0x30: return "Inverse";
        case 0x07: case 0x0F: return "MemoryTest";
        case 0x27: case 0x2F: return "MemorySize";
        case 0x17: case 0x1F: case 0x37: case 0x3F: return "MemoryDump";
        case 0x01: case 0x05: case 0x31: case 0x35: return "AttitudeA";
        case 0x11: case 0x15: return "AttitudeB";
        case 0x21: case 0x25: return "AttitudeC";
        case 0x03: case 0x33: return "SubjectiveA";
        case 0x13: return "SubjectiveB";
        case 0x23: return "SubjectiveC";
        case 0x09: case 0x0D: case 0x39: case 0x3D: return "ObjectiveA";
        case 0x19: case 0x1D: return "ObjectiveB";
        case 0x29: case 0x2D: return "ObjectiveC";
        case 0x0B: case 0x3B: return "ScalarA";
        case 0x1B: return "ScalarB";
        case 0x2B: return "ScalarC";
        case 0x14: case 0x34: return "Gyrate";
        case 0x02: case 0x12: case 0x22: case 0x32: return "Parameter";
        case 0x06: case 0x16: case 0x26: case 0x36: return "Project";
        case 0x0A: case 0x1A: case 0x2A: case 0x3A: return "Raster";
        case 0x0E: case 0x1E: case 0x2E: case 0x3E: return "Target";
        default: return "Other";
    }
}

static Bool WriteDump(FILE *f, const SmkSnapshotT &snap)
{
    const SmkStatsT &s = snap.stats;
    Uint64 dspRaw = 0;
    Uint64 dspAdjusted = 0;
    Uint32 dspCalls = 0;
    for (Uint32 i = 0; i < SMK_DSP_OPS; ++i)
    {
        dspRaw += s.dspCycles[i];
        dspCalls += s.dspCalls[i];
        const Uint64 ov = (Uint64)s.dspCalls[i] * snap.clockPairMin;
        dspAdjusted += s.dspCycles[i] > ov ? (s.dspCycles[i] - ov) : 0u;
    }

    const Uint64 mode7Compose = s.mode7MainComposeCycles + s.mode7SubComposeCycles;
    const Uint64 tracked = dspRaw + s.mode7TotalCycles + mode7Compose;
    const Uint64 remaining = s.frameCycles > tracked ? s.frameCycles - tracked : 0u;

    if (fprintf(f,
        "SNESticleAurora Super Mario Kart profiler v2\n"
        "timestamp=%04u-%02u-%02u %02u:%02u:%02u\n"
        "crc32=%08X\nregion=%s\nrom_title=%s\n"
        "sample_frames=%u\nmode7_frames=%u\n"
        "frame_cycles=%llu\navg_frame_cycles=%llu\n"
        "clock_pair_min_cycles=%u\n\n",
        (unsigned)snap.stamp.year, (unsigned)snap.stamp.month,
        (unsigned)snap.stamp.day, (unsigned)snap.stamp.hour,
        (unsigned)snap.stamp.minute, (unsigned)snap.stamp.second,
        (unsigned)snap.crc32, RegionName(snap.crc32), snap.title,
        (unsigned)s.frames, (unsigned)s.mode7Frames,
        (unsigned long long)s.frameCycles,
        (unsigned long long)Avg(s.frameCycles, s.frames),
        (unsigned)snap.clockPairMin) < 0) return FALSE;

    Uint32 pct = Percent10(dspRaw, s.frameCycles);
    Uint32 pctAdj = Percent10(dspAdjusted, s.frameCycles);
    Uint32 pctM7 = Percent10(s.mode7TotalCycles, s.frameCycles);
    Uint32 pctComp = Percent10(mode7Compose, s.frameCycles);
    Uint32 pctRemain = Percent10(remaining, s.frameCycles);

    if (fprintf(f,
        "[SUMMARY]\n"
        "DSP1 raw:      %llu cycles  %u.%u%% frame  calls=%u\n"
        "DSP1 adjusted: %llu cycles  %u.%u%% frame  (subtracts clock-pair minimum only)\n"
        "Mode7 fetch:   %llu cycles  %u.%u%% frame  lines=%u\n"
        "Mode7 compose: %llu cycles  %u.%u%% frame\n"
        "Untracked:     %llu cycles  %u.%u%% frame\n\n",
        (unsigned long long)dspRaw, pct / 10u, pct % 10u, (unsigned)dspCalls,
        (unsigned long long)dspAdjusted, pctAdj / 10u, pctAdj % 10u,
        (unsigned long long)s.mode7TotalCycles, pctM7 / 10u, pctM7 % 10u,
        (unsigned)s.mode7Lines,
        (unsigned long long)mode7Compose, pctComp / 10u, pctComp % 10u,
        (unsigned long long)remaining, pctRemain / 10u, pctRemain % 10u) < 0)
        return FALSE;

    if (fprintf(f,
        "[MODE7]\n"
        "lines=%u pixels=%llu lines_per_frame_x100=%u\n"
        "total_cycles=%llu avg_line=%llu\n"
        "setup_cycles=%llu avg_line=%llu\n"
        "fetch_cycles=%llu avg_line=%llu\n"
        "post_cycles=%llu avg_line=%llu\n"
        "main_compose_cycles=%llu calls=%u avg=%llu\n"
        "sub_compose_cycles=%llu calls=%u avg=%llu\n"
        "repeat_wrap0=%u repeat_wrap1=%u repeat_black2=%u repeat_clamp3=%u\n"
        "repeat_dy0=%u repeat_unitx=%u repeat_generic=%u\n"
        "dx_zero=%u dx_unit=%u dx_other=%u dy_zero=%u\n"
        "m7b_zero=%u m7c_zero=%u flip_x=%u flip_y=%u mosaic_lines=%u half_lines=%u\n\n",
        (unsigned)s.mode7Lines, (unsigned long long)s.mode7Pixels,
        s.frames ? (unsigned)((s.mode7Lines * 100u + s.frames / 2u) / s.frames) : 0u,
        (unsigned long long)s.mode7TotalCycles,
        (unsigned long long)Avg(s.mode7TotalCycles, s.mode7Lines),
        (unsigned long long)s.mode7SetupCycles,
        (unsigned long long)Avg(s.mode7SetupCycles, s.mode7Lines),
        (unsigned long long)s.mode7FetchCycles,
        (unsigned long long)Avg(s.mode7FetchCycles, s.mode7Lines),
        (unsigned long long)s.mode7PostCycles,
        (unsigned long long)Avg(s.mode7PostCycles, s.mode7Lines),
        (unsigned long long)s.mode7MainComposeCycles,
        (unsigned)s.mode7MainComposeCalls,
        (unsigned long long)Avg(s.mode7MainComposeCycles, s.mode7MainComposeCalls),
        (unsigned long long)s.mode7SubComposeCycles,
        (unsigned)s.mode7SubComposeCalls,
        (unsigned long long)Avg(s.mode7SubComposeCycles, s.mode7SubComposeCalls),
        (unsigned)s.repeatMode[0], (unsigned)s.repeatMode[1],
        (unsigned)s.repeatMode[2], (unsigned)s.repeatMode[3],
        (unsigned)s.repeatDy0, (unsigned)s.repeatUnitX, (unsigned)s.repeatGeneric,
        (unsigned)s.dxZero, (unsigned)s.dxUnit, (unsigned)s.dxOther,
        (unsigned)s.dyZero, (unsigned)s.m7bZero, (unsigned)s.m7cZero,
        (unsigned)s.flipX, (unsigned)s.flipY, (unsigned)s.mosaicLines,
        (unsigned)s.halfLines) < 0) return FALSE;

    const Uint32 pctClampDy0 =
        Percent10(s.clampDy0FetchCycles, s.mode7FetchCycles);
    const Uint32 pctClampGeneric =
        Percent10(s.clampGenericFetchCycles, s.mode7FetchCycles);
    if (fprintf(f,
        "[MODE7_CLAMP]\n"
        "dy0_lines=%u fetch_cycles=%llu avg_line=%llu fetch_pct=%u.%u%%\n"
        "generic_lines=%u fetch_cycles=%llu avg_line=%llu fetch_pct=%u.%u%%\n"
        "generic_in_pixels=%llu generic_out_pixels=%llu\n"
        "generic_full_in_lines=%u generic_mixed_lines=%u generic_full_out_lines=%u\n"
        "generic_source_y_changes=%llu avg_per_line_x100=%llu\n"
        "generic_tile_x_changes=%llu avg_per_line_x100=%llu\n"
        "generic_tile_y_changes=%llu avg_per_line_x100=%llu\n\n",
        (unsigned)s.clampDy0Lines,
        (unsigned long long)s.clampDy0FetchCycles,
        (unsigned long long)Avg(s.clampDy0FetchCycles, s.clampDy0Lines),
        pctClampDy0 / 10u, pctClampDy0 % 10u,
        (unsigned)s.clampGenericLines,
        (unsigned long long)s.clampGenericFetchCycles,
        (unsigned long long)Avg(s.clampGenericFetchCycles, s.clampGenericLines),
        pctClampGeneric / 10u, pctClampGeneric % 10u,
        (unsigned long long)s.clampGenericInPixels,
        (unsigned long long)s.clampGenericOutPixels,
        (unsigned)s.clampGenericFullInLines,
        (unsigned)s.clampGenericMixedLines,
        (unsigned)s.clampGenericFullOutLines,
        (unsigned long long)s.clampGenericSourceYChanges,
        (unsigned long long)Avg(
            s.clampGenericSourceYChanges * 100u, s.clampGenericLines),
        (unsigned long long)s.clampGenericTileXChanges,
        (unsigned long long)Avg(
            s.clampGenericTileXChanges * 100u, s.clampGenericLines),
        (unsigned long long)s.clampGenericTileYChanges,
        (unsigned long long)Avg(
            s.clampGenericTileYChanges * 100u, s.clampGenericLines)) < 0)
        return FALSE;

    struct RankT { Uint8 op; Uint64 cycles; } rank[SMK_DSP_OPS];
    for (Uint32 i = 0; i < SMK_DSP_OPS; ++i)
    {
        rank[i].op = (Uint8)i;
        rank[i].cycles = s.dspCycles[i];
    }
    for (Uint32 i = 0; i < SMK_DSP_OPS; ++i)
    {
        Uint32 best = i;
        for (Uint32 j = i + 1; j < SMK_DSP_OPS; ++j)
            if (rank[j].cycles > rank[best].cycles) best = j;
        if (best != i)
        {
            const RankT t = rank[i]; rank[i] = rank[best]; rank[best] = t;
        }
    }

    if (fprintf(f, "[DSP1_TOP]\nrank opcode name count total_cycles avg_cycles frame_pct\n") < 0)
        return FALSE;
    Uint32 shown = 0;
    for (Uint32 i = 0; i < SMK_DSP_OPS && shown < 12u; ++i)
    {
        const Uint8 op = rank[i].op;
        if (!s.dspCalls[op]) continue;
        const Uint32 p = Percent10(s.dspCycles[op], s.frameCycles);
        if (fprintf(f, "%u %02X %s %u %llu %llu %u.%u%%\n",
            (unsigned)(shown + 1u), (unsigned)op, DspName(op),
            (unsigned)s.dspCalls[op],
            (unsigned long long)s.dspCycles[op],
            (unsigned long long)Avg(s.dspCycles[op], s.dspCalls[op]),
            p / 10u, p % 10u) < 0) return FALSE;
        ++shown;
    }

    if (fprintf(f, "\n[DSP1_ALL_NONZERO]\nopcode,name,count,total_cycles,avg_cycles,frame_pct\n") < 0)
        return FALSE;
    for (Uint32 i = 0; i < SMK_DSP_OPS; ++i)
    {
        if (!s.dspCalls[i]) continue;
        const Uint32 p = Percent10(s.dspCycles[i], s.frameCycles);
        if (fprintf(f, "%02X,%s,%u,%llu,%llu,%u.%u%%\n",
            (unsigned)i, DspName((Uint8)i), (unsigned)s.dspCalls[i],
            (unsigned long long)s.dspCycles[i],
            (unsigned long long)Avg(s.dspCycles[i], s.dspCalls[i]),
            p / 10u, p % 10u) < 0) return FALSE;
    }

    if (fprintf(f,
        "\n[NOTES]\n"
        "- Times are host EE cycle-counter measurements from an instrumented build.\n"
        "- DSP1 raw command timing includes one ProfCtrGetCycle pair per command.\n"
        "- adjusted DSP1 subtracts only the minimum measured empty clock-pair cost.\n"
        "- Clamp DY0/generic buckets reuse the existing per-line fetch timer; no extra timer call is placed inside either pixel loop.\n"
        "- V2 geometry counters run after fetch timing, so they do not inflate path-local fetch_cycles.\n"
        "- V2 geometry bookkeeping does increase instrumented whole-frame time; use path-local fetch cycles for Clamp comparisons.\n"
        "- No RTC/filesystem work occurs during gameplay; this file is written after menu entry.\n") < 0)
        return FALSE;

    return TRUE;
}

static Bool SavePending(void)
{
    Char path[192];
    const SmkStampT &t = s_pending.stamp;
    snprintf(path, sizeof(path),
        "mass0:/SNESticle/smkprof_%04u%02u%02u_%02u%02u%02u_%08X_%03u.txt",
        (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
        (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second,
        (unsigned)s_pending.crc32, (unsigned)s_pending.serial);

    BgmIOBegin();
    FILE *f = fopen(path, "wb");
    if (!f)
    {
        snprintf(path, sizeof(path),
            "mass0:/SMKPROF_%08X_%04u%02u%02u_%02u%02u%02u_%03u.txt",
            (unsigned)s_pending.crc32,
            (unsigned)t.year, (unsigned)t.month, (unsigned)t.day,
            (unsigned)t.hour, (unsigned)t.minute, (unsigned)t.second,
            (unsigned)s_pending.serial);
        f = fopen(path, "wb");
    }

    Bool ok = f ? WriteDump(f, s_pending) : FALSE;
    if (f)
    {
        if (fflush(f) != 0) ok = FALSE;
        if (fclose(f) != 0) ok = FALSE;
    }
    BgmIOEnd();

    if (!ok)
    {
        if (f) remove(path);
        MainLoopStatusPrintf(180, "SMK profiler: dump write failed.");
        return FALSE;
    }

    MainLoopStatusPrintf(180, "SMK profiler saved: %s", path);
    return TRUE;
}

} /* namespace */

extern "C" {

Bool g_AuroraSmkProfilerActive = FALSE;

void AuroraSmkProfilerConfigureGame(Uint32 crc32, const char *title)
{
    s_crc32 = crc32;
    g_AuroraSmkProfilerActive = IsSmkCRC(crc32);
    s_clockPairMin = g_AuroraSmkProfilerActive ? CalibrateClockPair() : 0u;
    s_writePending = FALSE;
    s_writeDelay = 0;
    ResetLive();
    memset(s_title, 0, sizeof(s_title));
    if (g_AuroraSmkProfilerActive && title)
        strncpy(s_title, title, sizeof(s_title) - 1u);
}

void AuroraSmkProfilerFrameBegin(void)
{
    if (!g_AuroraSmkProfilerActive) return;
    s_frameStart = ProfCtrGetCycle();
    s_frameOpen = TRUE;
    s_frameSawMode7 = FALSE;
}

void AuroraSmkProfilerFrameEnd(void)
{
    if (!g_AuroraSmkProfilerActive || !s_frameOpen) return;
    const Uint32 now = ProfCtrGetCycle();
    s_live.frameCycles += (Uint32)(now - s_frameStart);
    ++s_live.frames;
    if (s_frameSawMode7) ++s_live.mode7Frames;
    s_frameOpen = FALSE;
}

void AuroraSmkProfilerDsp1Command(Uint8 opcode, Uint32 cycles)
{
    if (!g_AuroraSmkProfilerActive || !s_frameOpen) return;
    const Uint32 op = opcode & 0x3Fu;
    ++s_live.dspCalls[op];
    s_live.dspCycles[op] += cycles;
}

void AuroraSmkProfilerMode7Line(
    Uint8 m7sel, Uint8 mosaic, Uint8 softwareHacks,
    Int32 x, Int32 y, Int32 dx, Int32 dy,
    Int32 m7b, Int32 m7c, Int32 nPixels,
    Uint32 setupCycles, Uint32 fetchCycles, Uint32 postCycles,
    Uint32 totalCycles)
{
    if (!g_AuroraSmkProfilerActive || !s_frameOpen) return;

    s_frameSawMode7 = TRUE;
    ++s_live.mode7Lines;
    s_live.mode7Pixels += (Uint32)nPixels;
    s_live.mode7SetupCycles += setupCycles;
    s_live.mode7FetchCycles += fetchCycles;
    s_live.mode7PostCycles += postCycles;
    s_live.mode7TotalCycles += totalCycles;

    const Uint32 repeat = (m7sel >> 6) & 3u;
    ++s_live.repeatMode[repeat];
    if (repeat <= 1u)
    {
        if (dy == 0) ++s_live.repeatDy0;
        else if (dx == 0 || dx == 256 || dx == -256) ++s_live.repeatUnitX;
        else ++s_live.repeatGeneric;
    }
    else if (repeat == 3u)
    {
        if (dy == 0)
        {
            ++s_live.clampDy0Lines;
            s_live.clampDy0FetchCycles += fetchCycles;
        }
        else
        {
            ++s_live.clampGenericLines;
            s_live.clampGenericFetchCycles += fetchCycles;
            AccumulateClampGenericGeometry(x, y, dx, dy, nPixels);
        }
    }

    if (dx == 0) ++s_live.dxZero;
    else if (dx == 256 || dx == -256) ++s_live.dxUnit;
    else ++s_live.dxOther;
    if (dy == 0) ++s_live.dyZero;
    if (m7b == 0) ++s_live.m7bZero;
    if (m7c == 0) ++s_live.m7cZero;
    if (m7sel & 0x01u) ++s_live.flipX;
    if (m7sel & 0x02u) ++s_live.flipY;
    if ((mosaic & 0x01u) && (((mosaic >> 4) & 0x0Fu) != 0u))
        ++s_live.mosaicLines;
    if ((softwareHacks & 0x04u) || nPixels == 128)
        ++s_live.halfLines;
}

void AuroraSmkProfilerMode7Compose(Int32 subScreen, Uint32 cycles)
{
    if (!g_AuroraSmkProfilerActive || !s_frameOpen) return;
    if (subScreen)
    {
        s_live.mode7SubComposeCycles += cycles;
        ++s_live.mode7SubComposeCalls;
    }
    else
    {
        s_live.mode7MainComposeCycles += cycles;
        ++s_live.mode7MainComposeCalls;
    }
}

void AuroraSmkProfilerMenuOpen(void)
{
    if (!g_AuroraSmkProfilerActive || s_writePending || !s_live.frames)
        return;

    memset(&s_pending, 0, sizeof(s_pending));
    s_pending.stats = s_live;
    s_pending.stamp = CaptureStamp();
    s_pending.crc32 = s_crc32;
    s_pending.clockPairMin = s_clockPairMin;
    s_pending.serial = ++s_serial;
    strncpy(s_pending.title, s_title, sizeof(s_pending.title) - 1u);

    ResetLive();
    s_writePending = TRUE;
    s_writeDelay = SMK_MENU_WRITE_DELAY;
    MainLoopStatusPrintf(90, "SMK profiler: snapshot captured.");
}

void AuroraSmkProfilerMenuUpdate(void)
{
    if (!s_writePending) return;
    if (s_writeDelay > 0)
    {
        --s_writeDelay;
        return;
    }

    (void)SavePending();
    s_writePending = FALSE;
}

void AuroraSmkProfilerCancelPending(void)
{
    s_writePending = FALSE;
    s_writeDelay = 0;
}

} /* extern \"C\" */

#endif /* AURORA_SMK_PROFILER */
