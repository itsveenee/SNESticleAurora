/* AURORA_V13_UNIFIED_GBC_AUDIO_32X_FRAMESKIP_20260910 */
/* mainloop_render.cpp
 *
 * Hosts MainLoopRender() and the file-static state it needs
 * (_uVblankCycle and the screen-size #defines used to clear / blit the
 * SNES output texture).
 *
 * Extracted from mainloop.cpp during the Batch 3 split. No logic,
 * literal, or attribute change.
 */

#include <stdio.h>
#include <string.h> /* AURORA_TSUKURU_8M_GBA_LOAD_AUDIO_REDERR_V1_20260913_RED_ERRORS */

#include "mainloop_debug.h"
#include "mainloop_shared.h"
#if AURORA_C4_PROFILER
#include "platform/ps2/system/aurora_c4_profiler.h"
#endif
#include "mainloop_ui.h"
#include "mainloop_menu.h" /* AURORA_FINAL_V1_5_RENDER_MENU_API_INCLUDE_20260901 */
#include "mainloop_bgm.h"
#include "mainloop_safe_frameskip.h" /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */
#include "platform/ps2/system/aurora_snes_cost_profiler.h" /* AURORA_SNES_COST_PROFILER_V1_20260920 */
#if AURORA_FRONTEND_PROFILER
#include "platform/ps2/system/aurora_frontend_profiler.h"
#endif
#include "sega/picodrive/picodrive_bridge.h"
/* AURORA_ALLCORES_PERF_V5_20260824 */
#include "nes/quicknes/quicknes_bridge.h"
/* AURORA_FCEUMM_FDS_PERF_DIRECT_T8_V3_20260827 */
#include "nes/fceumm/fceumm_fds_bridge.h"
#include "pce/beetle/pce_bridge.h"
#include "gb/system/gambattesystem.h" /* AURORA_GAMBATTE_SQUARE_ASPECT_V13_20260911 */
#include "gba/system/gpspsystem.h" /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912 */

/* AURORA_PS2_PERF_V4_20260824 */
#include "types.h"
#include "console.h"
#include "snes.h"
#include "rendersurface.h"
#include "texture.h"
#include "font.h"
#include "poly.h"
#include "prof.h"
#include "snstate.h"
#include "snppublend_gs.h"
#include "common/debug/dbgterm.h"

/* AURORA_PCE_RELEASE_CLEANUP_FINAL_V15_20260830
 * Hidden release diagnostics. Keep the probes available for future
 * PCE/QuickNES investigations without drawing anything by default. */
#ifndef AURORA_RUNTIME_DIAG_OVERLAY
#define AURORA_RUNTIME_DIAG_OVERLAY 0
#endif

#include "mainloop_iop.h"

extern "C" {
#include "hw.h"
#include "gs.h"
#include "gpfifo.h"
#include "gpprim.h"
#include "gskit_backend.h"
#include "audio.h"
};

extern "C" {
#include "mcsave_ee.h"
};


/* Same MAINLOOP_SCREENWIDTH / HEIGHT pair as mainloop_init.cpp. The
   render path uses these to size the output blit; the init path uses
   them to size the GS framebuffer. Three other historical layouts are
   kept commented out in mainloop_init.cpp for reference. */
#define MAINLOOP_SCREENWIDTH 256
#define MAINLOOP_SCREENHEIGHT 240


static Uint32 _uVblankCycle;

/* AURORA_SAFE_FRAMESKIP_30FPS_FLOOR_V21_20261001
 *
 * Host-only catch-up scheduler with a hard presentation floor.
 *
 * The old scheduler accumulated multiple burst/flicker/recovery policies.
 * This version keeps only the measurements needed to answer one question:
 * would presenting this host tick knowingly miss the next VBlank budget?
 *
 * Skip budget: one token costs 5 units, each eligible host tick earns 2.
 * The bucket holds at most one token and two skips are never consecutive.
 * Under sustained debt this limits hidden presentations to <= 2/5:
 *   50 Hz host -> >= 30 visible fps
 *   ~60 Hz host -> >= 36 visible fps
 * The 5-tick ratio is odd, so presented-frame parity naturally walks instead
 * of phase-locking to every-other-frame sprite flicker; no separate flicker
 * protection state or forced parity swap is needed.
 *
 * Emulated CPU/audio time is never skipped. Only host video presentation and
 * its VBlank wait are omitted, exactly like the previous Safe Frameskip path.
 */
/* AURORA_V48_3_BOUND_NORMAL_HOST_DEBT_20261012
 * Compile-time A/B for the NORMAL Safe Frameskip host scheduler only.
 * All video buffers, audio timing and emulated clocks are unchanged. */
#ifndef AURORA_V48_3_BOUND_NORMAL_HOST_DEBT
#define AURORA_V48_3_BOUND_NORMAL_HOST_DEBT 1
#endif

static Int32  s_SafeFrameskipLevel = 1;
static Bool   s_SafeFrameskipSkipPresentation = FALSE;
static Bool   s_SafeFrameskipGameplayActive = FALSE;

static Uint32 s_SafeFrameskipAim = 0;
static Uint32 s_SafeFrameskipPeriod = 0;
static Uint32 s_SafeFrameskipSamples[3] = { 0, 0, 0 };
static Uint32 s_SafeFrameskipSampleCount = 0;
static Uint32 s_SafeFrameskipSamplePos = 0;
static Uint32 s_SafeFrameskipLastFlip = 0;
static Uint32 s_SafeFrameskipFrontendLastFlip = 0;

/* Take()->pre-VSync host work from the last visible gameplay tick. */
static Uint32 s_SafeFrameskipTickStart = 0;
static Uint32 s_SafeFrameskipPreFlip = 0;
static Uint32 s_SafeFrameskipLastPresentedWork = 0;
static Bool   s_SafeFrameskipLastPresentedWorkValid = FALSE;
static Bool   s_SafeFrameskipOverrunPending = FALSE;

/* AURORA_SAFE_FRAMESKIP_HEALTHY_REBASE_V22_20261004
 * A normal-policy skip arms one recovery proof. The next real
 * presented gameplay tick may clear stale Aim debt only if its
 * measured Take()->pre-VBlank work fits the learned period.
 * CRC-unlimited policy never arms this state. */
static Bool   s_SafeFrameskipRecoveryPending = FALSE;

/* Exact 2/5 token bucket. Capacity 6 preserves the one-unit remainder
 * when +2 credit crosses the 5-unit skip cost; <10 still banks at most one
 * complete skip. The no-consecutive guard remains the hard burst bound. */
static Uint32 s_SafeFrameskipCredit = 5;
static Bool   s_SafeFrameskipPreviousSkipped = FALSE;

/* AURORA_SAFE_FRAMESKIP_FLICKER_PHASE_SWAP_V21_1_20261001
 * Lightweight phase rotation only. Count ordinary debt-driven skip candidates.
 * Every 16th candidate is shown without spending its token. If debt persists,
 * the next tick naturally spends that still-full token, turning S/P into P/S
 * with no compensation flag and no sustained-throughput loss. */
static Uint32 s_SafeFrameskipPhaseSwapCount = 0;


/* AURORA_SAFE_FRAMESKIP_CRC_UNLIMITED_V21_2_20261001
 * Unlimited policy is selected once at ROM attach time. The allow-list switch
 * is therefore completely outside the host-tick hot path. During gameplay the
 * only policy test is one owner-pointer comparison. */
static const void *s_SafeFrameskipUnlimitedOwner = NULL;

static Bool _MainLoopSafeFrameskipCRCUsesUnlimited(Uint32 crc32)
{
    switch (crc32)
    {
        /* Generic CRC allow-list. Add future ROM identities here. */
        case 0xD34C49B7u:
        case 0xB0150052u:
        case 0xE5A57B12u:
        case 0x2B88BEE8u:
        case 0x531463E1u:
        case 0x31DAEBB2u:
        case 0xA20BE998u:
        case 0x493FDB13u:
        case 0xB9B9DF06u:
        /* AURORA_V24_AUDIO_FRAME_BUDGET_SONICMAX_AUDIT_20261001
         * Sonic 3D family intentionally NOT in unlimited/max Safe Frameskip.
         * Easy opt-in if testing changes the decision:
         *   case 0x44A2CA44u:  // Sonic 3D Blast / Flickies' Island
         *   case 0x3029C4F2u:  // Sonic 3D Director's Cut (Demo)
         *   case 0x9767E840u:  // Sonic 3D Director's Cut (v1.0 Final)
         */
            return TRUE;
        default:
            return FALSE;
    }
}

Uint32 MainLoopSafeFrameskipRomCRC32(const void *pDataVoid, Uint32 nBytes)
{
    static Uint32 table[256];
    static Bool tableReady = FALSE;
    const Uint8 *pData = (const Uint8 *)pDataVoid;
    Uint32 crc = 0xFFFFFFFFu;

    if (!pData || nBytes == 0u)
        return 0u;

    if (!tableReady)
    {
        for (Uint32 i = 0; i < 256u; ++i)
        {
            Uint32 c = i;
            for (Uint32 bit = 0; bit < 8u; ++bit)
                c = (c >> 1) ^ ((c & 1u) ? 0xEDB88320u : 0u);
            table[i] = c;
        }
        tableReady = TRUE;
    }

    while (nBytes--)
        crc = table[(crc ^ *pData++) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

void MainLoopSafeFrameskipSetRomIdentityCRC32(
    const void *pSystemOwner, Uint32 crc32)
{
    s_SafeFrameskipUnlimitedOwner =
        (pSystemOwner && _MainLoopSafeFrameskipCRCUsesUnlimited(crc32))
            ? pSystemOwner : NULL;

    /* Media attach is a clean policy boundary. Do not inherit bucket or
     * anti-flicker phase from the previous cartridge in the same core. */
    s_SafeFrameskipCredit = 5u;
    s_SafeFrameskipPreviousSkipped = FALSE;
    s_SafeFrameskipPhaseSwapCount = 0u;
    s_SafeFrameskipRecoveryPending = FALSE;
}

/* UI pause preserves scheduler phase without sampling time spent in menus. */
static Bool        s_SafeFrameskipCommittedValid = FALSE;
static Int32       s_SafeFrameskipCommittedAimPhase = 0;
static const void *s_SafeFrameskipCommittedSystem = NULL;
static Uint32      s_SafeFrameskipCommittedFrame = 0;
static Bool        s_SafeFrameskipUiFreezeValid = FALSE;
static Bool        s_SafeFrameskipUiFreezeHadAim = FALSE;
static Int32       s_SafeFrameskipUiFreezeAimPhase = 0;
static const void *s_SafeFrameskipUiFreezeSystem = NULL;
static Uint32      s_SafeFrameskipUiFreezeFrame = 0;
static Bool        s_SafeFrameskipUiResumePending = FALSE;
static Int32       s_SafeFrameskipUiResumeAimPhase = 0;

/* Legacy CD window ABI; modern streaming-CD paths normally disallow FS. */
static Bool s_SafeFrameskipCdAudioWindowRequested = FALSE;

static Uint32 _MainLoopSafeFrameskipMedian3(Uint32 a, Uint32 b, Uint32 c)
{
    if (a > b) { const Uint32 x = a; a = b; b = x; }
    if (b > c) { const Uint32 x = b; b = c; c = x; }
    if (a > b) { const Uint32 x = a; a = b; b = x; }
    return b;
}

static void _MainLoopSafeFrameskipInvalidateCommitted(void)
{
    s_SafeFrameskipCommittedValid = FALSE;
    s_SafeFrameskipCommittedAimPhase = 0;
    s_SafeFrameskipCommittedSystem = NULL;
    s_SafeFrameskipCommittedFrame = 0;
}

static void _MainLoopSafeFrameskipResetRuntime(void)
{
    s_SafeFrameskipAim = 0;
    s_SafeFrameskipSkipPresentation = FALSE;
    s_SafeFrameskipTickStart = 0;
    s_SafeFrameskipPreFlip = 0;
    s_SafeFrameskipLastPresentedWork = 0;
    s_SafeFrameskipLastPresentedWorkValid = FALSE;
    s_SafeFrameskipOverrunPending = FALSE;
    s_SafeFrameskipRecoveryPending = FALSE;
    s_SafeFrameskipCredit = 5;
    s_SafeFrameskipPreviousSkipped = FALSE;
    s_SafeFrameskipPhaseSwapCount = 0;
    s_SafeFrameskipUiResumePending = FALSE;
    s_SafeFrameskipUiResumeAimPhase = 0;
    _MainLoopSafeFrameskipInvalidateCommitted();
}

static void _MainLoopSafeFrameskipCommitGameplayBoundary(Uint32 now)
{
    const void *system = (const void *)_pSystem;

    if (system != NULL && s_SafeFrameskipAim != 0u)
    {
        s_SafeFrameskipCommittedValid = TRUE;
        s_SafeFrameskipCommittedAimPhase =
            (Int32)(s_SafeFrameskipAim - now);
        s_SafeFrameskipCommittedSystem = system;
        s_SafeFrameskipCommittedFrame = (Uint32)_pSystem->GetFrame();
    }
    else
    {
        _MainLoopSafeFrameskipInvalidateCommitted();
    }
}

static void _MainLoopSafeFrameskipLearn(Uint32 delta, Bool gameplay)
{
    if (delta == 0u)
        return;

    if (s_SafeFrameskipPeriod == 0u)
    {
        if (s_SafeFrameskipSampleCount != 0u)
        {
            const Uint32 lastPos = (s_SafeFrameskipSamplePos + 2u) % 3u;
            const Uint32 ref = s_SafeFrameskipSamples[lastPos];
            if ((Uint64)delta * 4u < (Uint64)ref * 3u ||
                (Uint64)delta * 4u > (Uint64)ref * 5u)
            {
                s_SafeFrameskipSamples[0] = delta;
                s_SafeFrameskipSamples[1] = 0;
                s_SafeFrameskipSamples[2] = 0;
                s_SafeFrameskipSamplePos = 1u;
                s_SafeFrameskipSampleCount = 1u;
                return;
            }
        }

        s_SafeFrameskipSamples[s_SafeFrameskipSamplePos] = delta;
        s_SafeFrameskipSamplePos = (s_SafeFrameskipSamplePos + 1u) % 3u;
        if (s_SafeFrameskipSampleCount < 3u)
            ++s_SafeFrameskipSampleCount;
        if (s_SafeFrameskipSampleCount < 3u)
            return;

        s_SafeFrameskipPeriod = _MainLoopSafeFrameskipMedian3(
            s_SafeFrameskipSamples[0],
            s_SafeFrameskipSamples[1],
            s_SafeFrameskipSamples[2]);
        _MainLoopSafeFrameskipResetRuntime();
        return;
    }

    if (gameplay)
    {
        if ((Uint64)delta * 4u < (Uint64)s_SafeFrameskipPeriod * 3u ||
            (Uint64)delta * 4u > (Uint64)s_SafeFrameskipPeriod * 5u)
            return;
    }
    else
    {
        if ((Uint64)delta * 3u < (Uint64)s_SafeFrameskipPeriod * 2u ||
            (Uint64)delta * 2u > (Uint64)s_SafeFrameskipPeriod * 3u)
            return;
    }

    s_SafeFrameskipSamples[s_SafeFrameskipSamplePos] = delta;
    s_SafeFrameskipSamplePos = (s_SafeFrameskipSamplePos + 1u) % 3u;
    s_SafeFrameskipPeriod = _MainLoopSafeFrameskipMedian3(
        s_SafeFrameskipSamples[0],
        s_SafeFrameskipSamples[1],
        s_SafeFrameskipSamples[2]);
}

Int32 MainLoopSafeFrameskipGetLevel(void)
{
    return s_SafeFrameskipLevel > 0 ? 1 : 0;
}

void MainLoopSafeFrameskipSetLevel(Int32 level)
{
#if AURORA_FRONTEND_PROFILER
    s_SafeFrameskipLevel = level > 0 ? 1 : 0;
#else /* AURORA_V44_CLEAN_SAFE_FS_ON_20261011: clean always ON, including old video.cfg */
    (void)level;
    s_SafeFrameskipLevel = 1;
#endif
    s_SafeFrameskipCdAudioWindowRequested = FALSE;
    s_SafeFrameskipUiFreezeValid = FALSE;
    s_SafeFrameskipUiFreezeHadAim = FALSE;
    s_SafeFrameskipUiFreezeAimPhase = 0;
    s_SafeFrameskipUiFreezeSystem = NULL;
    s_SafeFrameskipUiFreezeFrame = 0;
    s_SafeFrameskipLastFlip = 0;
    s_SafeFrameskipFrontendLastFlip = 0;
    _MainLoopSafeFrameskipResetRuntime();
}

Bool MainLoopSafeFrameskipGetEnabled(void)
{
    return s_SafeFrameskipLevel > 0 ? TRUE : FALSE;
}

void MainLoopSafeFrameskipSetEnabled(Bool enabled)
{
    MainLoopSafeFrameskipSetLevel(enabled ? 1 : 0);
}

void MainLoopSafeFrameskipSetGameplayActive(Bool active)
{
    const void *system = (const void *)_pSystem;
    const Uint32 frame = _pSystem ? (Uint32)_pSystem->GetFrame() : 0u;

    if (active && s_SafeFrameskipGameplayActive)
        return;

    if (!active)
    {
        if (s_SafeFrameskipGameplayActive)
        {
            const Bool sameCommitted =
                (s_SafeFrameskipCommittedValid && system != NULL &&
                 s_SafeFrameskipCommittedSystem == system &&
                 s_SafeFrameskipCommittedFrame == frame) ? TRUE : FALSE;

            s_SafeFrameskipGameplayActive = FALSE;
            s_SafeFrameskipUiFreezeValid = (system != NULL) ? TRUE : FALSE;
            s_SafeFrameskipUiFreezeHadAim = sameCommitted;
            s_SafeFrameskipUiFreezeAimPhase =
                sameCommitted ? s_SafeFrameskipCommittedAimPhase : 0;
            s_SafeFrameskipUiFreezeSystem = system;
            s_SafeFrameskipUiFreezeFrame = frame;

            s_SafeFrameskipSkipPresentation = FALSE;
            s_SafeFrameskipPreviousSkipped = FALSE;
            s_SafeFrameskipLastFlip = 0;
            s_SafeFrameskipFrontendLastFlip = 0;
            s_SafeFrameskipTickStart = 0;
            s_SafeFrameskipPreFlip = 0;
            return;
        }

        if (s_SafeFrameskipUiFreezeValid &&
            (system == NULL || system != s_SafeFrameskipUiFreezeSystem ||
             frame != s_SafeFrameskipUiFreezeFrame))
        {
            s_SafeFrameskipUiFreezeValid = FALSE;
            s_SafeFrameskipUiFreezeHadAim = FALSE;
            s_SafeFrameskipUiFreezeAimPhase = 0;
            s_SafeFrameskipUiFreezeSystem = system;
            s_SafeFrameskipUiFreezeFrame = frame;
            s_SafeFrameskipCdAudioWindowRequested = FALSE;
            _MainLoopSafeFrameskipResetRuntime();
        }
        return;
    }

    if (!s_SafeFrameskipGameplayActive)
    {
        const Bool samePausedState =
            (s_SafeFrameskipUiFreezeValid && system != NULL &&
             system == s_SafeFrameskipUiFreezeSystem &&
             frame == s_SafeFrameskipUiFreezeFrame) ? TRUE : FALSE;

        if (samePausedState && s_SafeFrameskipUiFreezeHadAim)
        {
            s_SafeFrameskipUiResumePending = TRUE;
            s_SafeFrameskipUiResumeAimPhase =
                s_SafeFrameskipUiFreezeAimPhase;
        }
        else if (!samePausedState)
        {
            s_SafeFrameskipCdAudioWindowRequested = FALSE;
            _MainLoopSafeFrameskipResetRuntime();
        }
        else
        {
            s_SafeFrameskipAim = 0u;
        }

        s_SafeFrameskipUiFreezeValid = FALSE;
        s_SafeFrameskipUiFreezeHadAim = FALSE;
        s_SafeFrameskipUiFreezeAimPhase = 0;
        s_SafeFrameskipUiFreezeSystem = NULL;
        s_SafeFrameskipUiFreezeFrame = 0;
        s_SafeFrameskipSkipPresentation = FALSE;
        s_SafeFrameskipPreviousSkipped = FALSE;
        s_SafeFrameskipLastFlip = 0;
        s_SafeFrameskipFrontendLastFlip = 0;
        s_SafeFrameskipTickStart = 0;
        s_SafeFrameskipPreFlip = 0;
        s_SafeFrameskipGameplayActive = TRUE;
    }
}

void MainLoopSafeFrameskipRequestCdAudioWindow(void)
{
    if (s_SafeFrameskipLevel > 0)
        s_SafeFrameskipCdAudioWindowRequested = TRUE;
}

void MainLoopSafeFrameskipCancelCdAudioWindow(void)
{
    s_SafeFrameskipCdAudioWindowRequested = FALSE;
}

Bool MainLoopSafeFrameskipTake(Bool allowed)
{
    Bool skip = FALSE;
    Bool meaningfulDebt = FALSE;
    Bool measuredOverrunForDiag = FALSE;
    Int32 diffForDiag = 0;
    Uint32 now;

    s_SafeFrameskipSkipPresentation = FALSE;

    if (!allowed || s_SafeFrameskipLevel <= 0)
    {
        s_SafeFrameskipCdAudioWindowRequested = FALSE;
        _MainLoopSafeFrameskipResetRuntime();
        return FALSE;
    }

    if (s_SafeFrameskipPeriod == 0u || s_SafeFrameskipSampleCount < 3u)
    {
        s_SafeFrameskipPreviousSkipped = FALSE;
        return FALSE;
    }

    now = ProfCtrGetCycle();
    s_SafeFrameskipTickStart = now;

    if (s_SafeFrameskipUiResumePending)
    {
        s_SafeFrameskipAim =
            now + (Uint32)s_SafeFrameskipUiResumeAimPhase;
        s_SafeFrameskipUiResumePending = FALSE;
        s_SafeFrameskipUiResumeAimPhase = 0;
    }

    if (s_SafeFrameskipAim == 0u)
        s_SafeFrameskipAim = now;

    /* AURORA_V48_3_BOUND_NORMAL_HOST_DEBT_20261012
     * The 32-bit COP0 host counter and the 32-bit Aim are modular.
     * The signed (Aim - now) check below only identifies elapsed debt
     * unambiguously while it stays below 2^31 counts.  When the normal
     * 2/5 presentation floor is saturated, work can exceed a period
     * indefinitely and Aim falls farther behind with every host tick.
     * Limit stale Aim to eight measured periods while it is still
     * unambiguously negative.  This keeps ordinary debt-driven skip
     * decisions TRUE, including after the clamp: eight periods is
     * far above any normal scheduler catch-up threshold.
     * Do not modify CRC-unlimited policy, throttle, audio or core time.
     * No unbounded loop, no 64-bit math on the normal fast path. */
#if AURORA_V48_3_BOUND_NORMAL_HOST_DEBT
    if (s_SafeFrameskipPeriod != 0u &&
        s_SafeFrameskipPeriod <= 0x0FFFFFFFu &&
        (s_SafeFrameskipUnlimitedOwner == NULL ||
         s_SafeFrameskipUnlimitedOwner != (const void *)_pSystem))
    {
        const Uint32 maxDebt = s_SafeFrameskipPeriod * 8u;
        const Int32 phase = (Int32)(s_SafeFrameskipAim - now);
        if (phase < 0 && (Uint32)(now - s_SafeFrameskipAim) > maxDebt)
            s_SafeFrameskipAim = now - maxDebt;
    }
#endif

    {
        const Int32 diff = (Int32)(s_SafeFrameskipAim - now);
        const Bool measuredOverrun = s_SafeFrameskipOverrunPending;
        diffForDiag = diff;
        measuredOverrunForDiag = measuredOverrun;
        s_SafeFrameskipOverrunPending = FALSE;

        if (measuredOverrun)
            meaningfulDebt = TRUE;

        if (diff < 0)
        {
            const Uint32 lateness = 0u - (Uint32)diff;
            if (s_SafeFrameskipLastPresentedWorkValid)
            {
                if ((Uint64)lateness +
                    (Uint64)s_SafeFrameskipLastPresentedWork >=
                    (Uint64)s_SafeFrameskipPeriod)
                    meaningfulDebt = TRUE;
            }
            else if (lateness >= s_SafeFrameskipPeriod)
            {
                meaningfulDebt = TRUE;
            }
        }
    }

    /* AURORA_SAFE_FRAMESKIP_BALANCED_RECOVERY_V23_20261005
     *
     * v22 already gave us the important safety proof: a REAL presented frame
     * is measured after a normal-policy skip. Keep that proof, but do not
     * throw away residual Aim debt too early.
     *
     * During an armed recovery only, more than half a learned host period of
     * residual lateness is still actionable debt. Below that threshold normal
     * VBlank cadence can absorb the phase error, so disarm recovery without
     * manufacturing another skip. The public 2/5 token bucket and the
     * no-consecutive-skip rule below remain untouched. */
    if (s_SafeFrameskipRecoveryPending &&
        s_SafeFrameskipPeriod > 0u)
    {
        const Int32 recoveryThreshold =
            (s_SafeFrameskipPeriod > 1u)
                ? (Int32)(s_SafeFrameskipPeriod >> 1)
                : 1;

        if (diffForDiag < -recoveryThreshold)
            meaningfulDebt = TRUE;
        else if (!measuredOverrunForDiag)
            s_SafeFrameskipRecoveryPending = FALSE;
    }

    /* AURORA_SAFE_FRAMESKIP_CRC_UNLIMITED_V21_2_20261001
     * The CRC allow-list was resolved at media attach time. No CRC, table or
     * switch work occurs here: only this owner-pointer comparison. */
    const Bool unlimitedSkip =
        (s_SafeFrameskipUnlimitedOwner != NULL &&
         s_SafeFrameskipUnlimitedOwner == (const void *)_pSystem)
            ? TRUE : FALSE;

    /* Normal policy remains the exact v21.1 2/5 bucket. Unlimited entries do
     * not spend or maintain bucket credit because their only bound is real
     * scheduler debt plus the periodic anti-flicker visible phase break. */
    if (!unlimitedSkip && s_SafeFrameskipCredit < 6u)
    {
        s_SafeFrameskipCredit += 2u;
        if (s_SafeFrameskipCredit > 6u)
            s_SafeFrameskipCredit = 6u;
    }

    const Bool skipCandidate = unlimitedSkip
        ? ((meaningfulDebt || s_SafeFrameskipCdAudioWindowRequested) ? TRUE : FALSE)
        : (((meaningfulDebt || s_SafeFrameskipCdAudioWindowRequested) &&
            !s_SafeFrameskipPreviousSkipped &&
            s_SafeFrameskipCredit >= 5u) ? TRUE : FALSE);

    if (skipCandidate)
    {
        Bool phaseSwapPresent = FALSE;

        /* Retain v21.1's sprite/flicker phase protection even in unlimited
         * mode. CD-audio refill windows are never delayed by this guard. */
        if (meaningfulDebt && !s_SafeFrameskipCdAudioWindowRequested)
        {
            ++s_SafeFrameskipPhaseSwapCount;
            if (s_SafeFrameskipPhaseSwapCount >= 16u)
            {
                s_SafeFrameskipPhaseSwapCount = 0u;
                phaseSwapPresent = TRUE;
            }
        }

        if (!phaseSwapPresent)
        {
            if (!unlimitedSkip)
            {
                s_SafeFrameskipCredit -= 5u;
                s_SafeFrameskipRecoveryPending = TRUE;
            }
            s_SafeFrameskipPreviousSkipped = TRUE;
            s_SafeFrameskipCdAudioWindowRequested = FALSE;
            skip = TRUE;
        }
        else
        {
            /* Keep the token untouched in normal mode; under continuing debt
             * the next eligible tick naturally performs the phase swap. */
            s_SafeFrameskipPreviousSkipped = FALSE;
        }
    }
    else
    {
        s_SafeFrameskipPreviousSkipped = FALSE;
    }

#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerSafeFrameskipDecision(
        s_SafeFrameskipPeriod, diffForDiag,
        s_SafeFrameskipLastPresentedWork, meaningfulDebt,
        measuredOverrunForDiag, unlimitedSkip, skip,
        s_SafeFrameskipRecoveryPending);
#endif

    s_SafeFrameskipAim += s_SafeFrameskipPeriod;
    s_SafeFrameskipSkipPresentation = skip;
    return skip;
}

Bool MainLoopSafeFrameskipConsumePresentationSkip(void)
{
    const Bool skip = s_SafeFrameskipSkipPresentation;
    s_SafeFrameskipSkipPresentation = FALSE;
    return skip;
}

static void _MainLoopSafeFrameskipAfterFlip(void)
{
    const Uint32 now = ProfCtrGetCycle();
    const Bool gameplay =
        (!_bMenu && _pSystem && !_MainLoop_BlackScreen) ? TRUE : FALSE;
    const Uint32 preFlip = s_SafeFrameskipPreFlip;

    s_SafeFrameskipPreFlip = 0;

    if (gameplay)
    {
        const Bool hostWorkValid =
            (s_SafeFrameskipTickStart != 0u && preFlip != 0u) ? TRUE : FALSE;
        const Uint32 hostWork =
            hostWorkValid ? (preFlip - s_SafeFrameskipTickStart) : 0u;
        const Bool recoveryWasPending =
            s_SafeFrameskipRecoveryPending;
        Bool recoveryRebased = FALSE;

        s_SafeFrameskipFrontendLastFlip = 0;

        if (hostWorkValid)
        {
            s_SafeFrameskipLastPresentedWork = hostWork;
            s_SafeFrameskipLastPresentedWorkValid = TRUE;
            if (s_SafeFrameskipPeriod > 0u &&
                (Uint64)hostWork > (Uint64)s_SafeFrameskipPeriod)
                s_SafeFrameskipOverrunPending = TRUE;
        }

        if (s_SafeFrameskipLastFlip != 0u)
        {
            const Uint32 delta = now - s_SafeFrameskipLastFlip;
            if (s_SafeFrameskipPeriod == 0u ||
                (hostWorkValid && hostWork <= s_SafeFrameskipPeriod))
                _MainLoopSafeFrameskipLearn(delta, TRUE);
        }

        /* AURORA_SAFE_FRAMESKIP_HEALTHY_REBASE_V22_20261004
         * A skipped NORMAL-policy tick is followed by a real presentation
         * because the 2/5 policy forbids consecutive skips. v3.3 deliberately
         * asks for >=25% host-work headroom before declaring the catch-up
         * complete immediately. A merely just-under-budget frame is stable,
         * but not strong proof that residual Aim debt has disappeared.
         *
         * Sustained/near-sustained load keeps recovery armed; Take() then uses
         * the v3.3 half-period residual threshold. CRC unlimited mode never
         * arms recovery, so Top Gear/Top Racer max policy is unchanged. */
        if (recoveryWasPending &&
            hostWorkValid &&
            s_SafeFrameskipPeriod > 0u &&
            ((Uint64)hostWork * 4u) <=
                ((Uint64)s_SafeFrameskipPeriod * 3u))
        {
            s_SafeFrameskipAim = now + s_SafeFrameskipPeriod;
            s_SafeFrameskipOverrunPending = FALSE;
            s_SafeFrameskipRecoveryPending = FALSE;
            s_SafeFrameskipPreviousSkipped = FALSE;
            recoveryRebased = TRUE;
        }

#if AURORA_FRONTEND_PROFILER
        AuroraFrontendProfilerSafeFrameskipRecovery(
            hostWork, recoveryWasPending, recoveryRebased);
#endif

        s_SafeFrameskipLastFlip = now;
        s_SafeFrameskipTickStart = 0;
    }
    else if (!_pSystem)
    {
        s_SafeFrameskipLastFlip = 0;
        if (s_SafeFrameskipFrontendLastFlip != 0u)
            _MainLoopSafeFrameskipLearn(
                now - s_SafeFrameskipFrontendLastFlip, FALSE);
        s_SafeFrameskipFrontendLastFlip = now;
        s_SafeFrameskipTickStart = 0;
    }
    else
    {
        s_SafeFrameskipLastFlip = 0;
        s_SafeFrameskipFrontendLastFlip = 0;
        s_SafeFrameskipTickStart = 0;
        s_SafeFrameskipPreFlip = 0;
    }
}

void MainLoopRender()
{
	static Uint32 _iFrame=0;
#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerRenderBegin();
#endif

        /* AURORA_SAFE_FRAMESKIP_UI_EDGE_RENDER_FALLBACK_V9_20260926
         *
         * Some modal/status/frontend paths render directly and therefore do
         * not pass through MainLoopProcess(). Observe the gameplay/UI state
         * again here so such a path cannot bypass the V8.1 timing barrier.
         *
         * Normal gameplay already observed the same state before Take(); the
         * observer is edge-triggered/idempotent, so this is a zero-mutation
         * no-op on the ordinary hot path.
         *
         * IMPORTANT: this runs BEFORE consuming a queued presentation skip.
         * If UI became active after the scheduler decision, the V8.1 edge
         * reset cancels that stale one-shot instead of hiding the first UI
         * presentation or carrying its phase through the menu.
         */
        MainLoopSafeFrameskipSetGameplayActive(
            (!_bMenu && _pSystem && !_MainLoop_BlackScreen)
                ? TRUE : FALSE);

        /* AURORA_SAFE_FRAMESKIP_PICODRIVE_AUTO_V1: standalone PicoDrive skip path has no
         * finalize/present/flip/VBlank wait. Core/audio already ran. */
        if (MainLoopSafeFrameskipConsumePresentationSkip())
        {
            if (!_bMenu && _pSystem && !_MainLoop_BlackScreen)
            {
                /* AURORA_CD_AUDIO_STREAM_V2_SKIP_AUDIO_CATCHUP_20260829
                 * The presentation/VBlank wait is already being skipped, so
                 * spend that recovered host time on at most one EXTRA
                 * nonblocking async-audio drain. No wait_audio(), no PCM drop.
                 * With no backlog the second call returns immediately. */
#if AURORA_C4_PROFILER
                const Uint32 c4SkipAud=g_AuroraC4ProfilerActive?ProfCtrGetCycle():0u;
#endif
#if AURORA_FRONTEND_PROFILER
                AuroraFrontendProfilerAudioDrainBegin();
#endif
                Aud_BufferedAsyncStart();
                Aud_BufferedAsyncStart();
#if AURORA_FRONTEND_PROFILER
                AuroraFrontendProfilerAudioDrainEnd();
#endif
#if AURORA_C4_PROFILER
                if(g_AuroraC4ProfilerActive)AuroraC4ProfilerPostAudio((Uint32)(ProfCtrGetCycle()-c4SkipAud),TRUE);
#endif

                /* V15 completed hidden-host-tick boundary. Core/audio have
                 * already advanced; no VBlank/presentation wait follows. */
                _MainLoopSafeFrameskipCommitGameplayBoundary(
                    ProfCtrGetCycle());
            }
#if AURORA_FRONTEND_PROFILER
            AuroraFrontendProfilerRenderEnd();
#endif
            ++_iFrame;
            return;
        }

        /* AURORA_SEGA_UI_PRESENTATION_V3
         *
         * Keep two different host presentations:
         *
         *   UI / browser / prompts -> Aurora's normal 256-wide raster
         *   plain MD gameplay      -> native 320-wide raster in 240p
         *   SMS and other systems  -> normal 256-wide raster
         *
         * The important distinction from the old late-init path is that
         * the FIRST MD 320 raster is still selected in mainloop_load.cpp
         * before PicoDrive is initialised. These later switches happen
         * only when entering/leaving Aurora's internal UI.
         *
         * In 480i/1080i MainLoopEnsureGameplayRasterWidth() does not
         * rebuild the GS; those modes keep their normal 640 framebuffer.
         */
        const Bool bMdVideo =
            (_pSystem == _pSega &&
             PicoDriveBridge_IsMegaDriveVideo()) ? TRUE : FALSE;

        {
            Int32 wantedRaster = 256;

            /* AURORA_SEGA_CD_32X_MD_SCALING_V2R1_20260828
             * Cartridge MD, Sega CD and 32X share H32/H40 presentation. */
            if (bMdVideo)
                wantedRaster = 320;


            /* AURORA_PCE_SSF2_FINAL_R2_20260913_PCE_FIXED512_MENU256
             * Real PS2: rebuilding 512->256 while Beetle/PCE-CD is alive can
             * destroy the live GS/VRAM epoch and hang the console.  Keep the
             * physical PCE raster at 512 for the entire core lifetime.  The
             * menu is a 256-sample presentation inside that backing instead. */
            if (_pSystem == _pPce)
                wantedRaster = 512;
            /* Menu/prompts use exact 256-source integer presentation on the
             * still-alive 320 framebuffer, not 256->320 resampling. */
            /* AURORA_PCE_SSF2_FINAL_R2_20260913_PCE_FIXED512_MENU256 */
            GSK_SetUi256OnWideFramebuffer(
                (_bMenu && (bMdVideo || _pSystem == _pPce)) ? 1 : 0);

            if (_bMenu)
                GSK_SetNative240pPar(0);

            if (!MainLoopEnsureGameplayRasterWidth(wantedRaster))
            {
                printf("[video] warning: could not switch to %d-wide presentation\n",
                       (int)wantedRaster);
            }

            /* AURORA_PCE_SSF2_FINAL_20260913_PCE_MENU_CLEAR_GAME_WINDOW
             * A gameplay crop/window never belongs to Aurora UI. */
            if (_pSystem != _pPce || _bMenu || _MainLoop_BlackScreen)
                GSK_Clear240pVisibleWindow();


        }



        /* AURORA_GLOBAL_PERF_V2_20260922
         * Read-only presentation facts for this host render. Query each bridge
         * at most once; no persistent capability cache is introduced. */
        const Bool bSegaDirectGs =
            (!_MainLoop_BlackScreen && _pSystem == _pSega &&
             PicoDriveBridge_CanDirectGsVideo()) ? TRUE : FALSE;
        const Bool bPceDirectGs =
            (!_MainLoop_BlackScreen && _pSystem == _pPce &&
             PceBridge_CanDirectGsVideo()) ? TRUE : FALSE;
        const Bool bGbaDirectGs =
            (!_MainLoop_BlackScreen && _pSystem == _pGba && _pGba &&
             _pGba->CanDirectGsVideo()) ? TRUE : FALSE;

        /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911
         * Both standalone GB/GBC and GBA LCDs use square source pixels.
         * Reuse v13's presentation-only policy: no core framebuffer
         * resampling, no effect on dynamic SGB, menus or other systems. */
        const Bool bSquareHandheldGameplay =
            (!_bMenu && !_MainLoop_BlackScreen &&
             ((_pSystem == _pGb && _pGb &&
               _pGb->UsesSquarePixelPresentation()) ||
              (_pSystem == _pGba))) ? TRUE : FALSE;
        GSK_SetGbSquarePixelPresentation(bSquareHandheldGameplay ? 1 : 0);

        /*
         * NES/SNES/8-bit-console 240p: keep the framebuffer strictly 256x240 (1 source
         * pixel = 1 framebuffer pixel) and correct horizontal size at
         * the PCRTC level instead. This avoids uneven pixel widths
         * caused by scaling 256 pixels into a smaller PolyRect.
         */
        static int s_native240pPar = -1;
        int native240pPar =
            (g_GskVideoMode == GSK_VIDMODE_240P &&
             (_pSystem == _pNes ||
              _pSystem == _pFds || /* AURORA_FCEUMM_FDS_V0_5_RENDER */
              _pSystem == _pSnes ||
              (_pSystem == _pSega &&
               (bMdVideo ||
                PicoDriveBridge_IsMasterSystem()))) &&
             !_bMenu) ? 1 : 0;

        if (native240pPar != s_native240pPar)
        {
            GSK_SetNative240pPar(native240pPar);
            s_native240pPar = native240pPar;
        }

        /* AURORA_MD_YOFFSET_MINUS9_V1
         * Plain MD only: effective Y = configured menu Y - 9. */
        {
            static int s_lastMdYBias = 9999;
            int mdYBias = 0;

            if (mdYBias != s_lastMdYBias)
            {
                GSK_SetGameplayYOffsetBias(mdYBias);
                s_lastMdYBias = mdYBias;
            }
        }



    /* Re-anchor FRAME_1 to gsKit's current draw buffer before any
       primitive runs this frame. The legacy GS_SetDrawFB used to do
       this implicitly per frame; gsKit_sync_flip only swaps the
       display buffer, not the draw buffer. Without this, prims drew
       to a stale (or, after the SNES blender ran, completely wrong)
       buffer and the visible framebuffer flickered black on every
       other frame. See gskit_backend.h for the longer rationale. */
    /* AURORA_GS_PARTIAL_GAMEPLAY_CLEAR_V1
     * Only enable the reduced clear when this frame is guaranteed to draw the
     * normal game output texture. Menu, boot and black-screen frames retain
     * the complete physical clear. */
    /* AURORA_PD_DIRECT_MD_SKIP_CLEAR_V3_RENDER_20260821
     * DrawDirectGs() for plain MD always emits an opaque backdrop over
     * the entire transformed 256x240 canvas, which is the entire active
     * framebuffer in 240p/480i/1080i. Avoid clearing pixels that are
     * guaranteed to be replaced later in this same frame. */
    /* AURORA_PD_DIRECT_8BIT_SKIP_CLEAR_V4_20260821 */
    /* AURORA_PCE_EXPERIMENTAL_V13_NATIVE_PAR_REBUILD
     *
     * PCE base geometry is 256x240 with a narrower-than-4:3 presentation.
     * Use the same native-240p PCRTC technique already used by Aurora's
     * NES/SNES path: change scanout magnification, NOT framebuffer sampling.
     * Every one of the 256 source columns is still scanned exactly once.
     *
     * Keep menu/black-screen presentation untouched. The loaded system remains
     * _pPce while Aurora's menu is open, so explicitly turn the PCE-only PAR
     * correction back off there. GSK_SetNative240pPar internally affects only
     * the 240p display mode.
     */
    if (_pSystem == _pPce)
    {
        /* AURORA_PCE_CRT_OVERSCAN_352_V9_20260830
         * PCE has explicit 256/352/512 PCRTC profiles in gskit_backend.
         * Do not feed the fixed 512 framebuffer through NES/SNES PAR math. */
        GSK_SetNative240pPar(0);
    }

#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerRenderSetupBegin();
#endif
    GSK_SetGameplaySkipClear(
        (!_bMenu && !_MainLoop_BlackScreen &&
         ((bSegaDirectGs &&
           (bMdVideo ||
            g_GskVideoMode == GSK_VIDMODE_240P)) ||
          bPceDirectGs ||
          (bGbaDirectGs &&
           GSK_GetActiveVideoMode() == GSK_VIDMODE_240P))) ? TRUE : FALSE); /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912 */
    /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911
     * Interlaced 2x2 handheld presentation leaves physical side bars, so use
     * the existing complete framebuffer clear instead of full-width fast-clear. */
    /* AURORA_GAMBATTE_SQUARE_CLEAR_V13_20260911: retained and extended by GBA V14. */
    GSK_SetGameplayFastClear(
        (!_bMenu && _pSystem && !_MainLoop_BlackScreen &&
         !(GSK_GetActiveVideoMode() != GSK_VIDMODE_240P &&
           bSquareHandheldGameplay)) ? TRUE : FALSE);
    GSK_ResetFrame();

    // render frame
    GPPrimDisableZBuf();

    /* Per-frame full-screen clear to black.
     *
     * MainLoopRender historically NEVER cleared the framebuffer: it
     * relied on the full-screen _OutTex blit below to repaint every
     * pixel.  But that blit is (a) skipped entirely when
     * _MainLoop_BlackScreen is set (boot log + menus) and (b) even when
     * drawn it starts at dy=8, so the top rows are never touched.  With
     * DoubleBuffering=ON each draw goes to the alternate buffer, so any
     * row we don't repaint shows stale content from two frames ago --
     * which appears as a fixed-position horizontal "faixa"/stripe
     * through the text (worst in the log and menus, where nothing
     * covers the background).  Clearing to black first costs a single
     * sprite and removes the band entirely.  GSK_ResetFrame now clears
     * the complete PHYSICAL framebuffer, including any overscan borders.
     * Keep the Poly state reset here, but do not queue a duplicate
     * logical-canvas clear. */
    PolyTexture(NULL);
    PolyBlend(FALSE);
    PolyColor4f(0.0f, 0.0f, 0.0f, 1.0f);
#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerRenderSetupEnd();
    AuroraFrontendProfilerGameDrawBegin();
#endif

	if (!_MainLoop_BlackScreen)
	{
//		Float32 fDestColor = (_bMenu || _MainLoop_ModalCount) ? 0.10f : 0.80f;
		Float32 fDestColor = 0.10f;
		
		if  (!_bMenu && !_MainLoop_ModalCount)
		{
			fDestColor = _MainLoop_fOutputIntensity;
		}

		static Float32 fColor=0.0f;
		Float32 dx = 0.0f;
		Float32 dy = 8.0f;

		if (fColor < fDestColor) 
		{
			fColor+=0.06f;
			if (fColor > fDestColor) 
			{
				fColor = fDestColor;
			}
		} 

		if (fColor > fDestColor) 
		{
			fColor-=0.06f;
			if (fColor < fDestColor) 
			{
				fColor = fDestColor;
			}
		}


        /* AURORA_PD_DIRECT_8BIT_GS_V1_20260821 */
        if (_pSystem == _pNes &&
            QuicknesBridge_CanDirectGsVideo())
        {
            QuicknesBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP,
                g_GskVideoMode == GSK_VIDMODE_240P ? 2 : 4,
                fColor);
        }
        else if (_pSystem == _pFds &&
                 FceummFdsBridge_CanDirectGsVideo())
        {
            /* AURORA_FCEUMM_FDS_PERF_DIRECT_T8_V3_20260827: native FCEUmm XBuf -> GS T8 + CLUT. */
            FceummFdsBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP,
                g_GskVideoMode == GSK_VIDMODE_240P ? 2 : 4,
                fColor);
        }
        else if (!_bMenu && bPceDirectGs)
        {
            /* AURORA_PCE_EXPERIMENTAL_V10_DIRECT_GS */
            PceBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP, fColor);
        }
        else if (bGbaDirectGs)
        {
            /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912
             * GPPrimTexRect uses the same transform as PolyRect. Scope the
             * interlaced exact-2x handheld transform around the direct draw,
             * then restore normal UI/status geometry immediately. */
            const Bool bGbaSquareDraw =
                (GSK_GetActiveVideoMode() != GSK_VIDMODE_240P &&
                 bSquareHandheldGameplay) ? TRUE : FALSE;

            if (bGbaSquareDraw)
                GSK_SetGba240pMatchedDraw(1); /* AURORA_GBA_480I_MATCH_240P_ASPECT_V3_2_20261005 */

            _pGba->DrawDirectGs(_MainLoop_uOutTexTBP, fColor);

            if (bGbaSquareDraw)
                GSK_SetGba240pMatchedDraw(0);
        }
        else if (bSegaDirectGs)
        {
            /* AURORA_PD_DIRECT_T8_RENDER */
            PicoDriveBridge_DrawDirectGs(
                _MainLoop_uOutTexTBP, fColor);
        }
        else
        {
            /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911
             * Scope the 480i/1080i 2x2 transform to the handheld game image;
             * overlays/status/modal geometry immediately returns to normal. */
            /* AURORA_GAMBATTE_DRAW_SCOPE_V13_20260911: retained and extended by GBA V14. */
            const Bool bHandheldSquareDraw =
                (GSK_GetActiveVideoMode() != GSK_VIDMODE_240P &&
                 bSquareHandheldGameplay) ? TRUE : FALSE;
            if (bHandheldSquareDraw)
            {
                if (_pSystem == _pGba)
                    GSK_SetGba240pMatchedDraw(1); /* AURORA_GBA_480I_MATCH_240P_ASPECT_V3_2_20261005 */
                else
                    GSK_SetGbSquarePixelDraw(1);
            }

            PolyBlend(FALSE);
            PolyTexture(&_OutTex);
            PolyUV(0,0,256,240);
    		PolyColor4f(fColor, fColor, fColor, 1.0f);
    
    
                    if (g_GskVideoMode == GSK_VIDMODE_240P &&
                        (_pSystem == _pNes || _pSystem == _pFds)) /* AURORA_FCEUMM_FDS_V0_5_RENDER */
            {
    /*
     * InfoNES 240p overscan compensation.
     *
     * Keep the NES framebuffer at its native 256x240 size and
     * preserve a 1:1 pixel mapping. Only reposition the image
     * to compensate for CRT overscan.
     */
    PolyRect(0.0f, 2.0f, 256.0f, 240.0f);
            }
            else if (g_GskVideoMode == GSK_VIDMODE_240P && _pSystem == _pSega)
            {
    /* AURORA_SEGA_NATIVE_240P_V1
     * PicoDriveBridge already composes MD/SMS/GG into an exact 256x240 logical
     * raster. Present that raster 1:1; PCRTC handles the CRT pixel aspect. */
    PolyRect(0.0f, 0.0f, 256.0f, 240.0f);
            }
            else if (_pSystem == _pSnes)
            {
                /* AURORA_GB_HOTFIX_R13F_STANDALONE_BOOT_AUDIO_Y_20260909_Y_SCOPE:
                 * GB standalone positioning is done in GambatteSystem.
                 * Native SNES/SGB outer presentation remains at Y=8. */
                PolyRect(0.0f, 8.0f, 256.0f, 240.0f);
            }
            else if (_pSystem == _pGba)
            {
                /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911 */
                PolyRect(0.0f, 0.0f, 256.0f, 240.0f);
            }
            else
            {
    PolyRect(0.0f, 4.0f, 256.0f, 240.0f);
            }
    
            PolyBlend(TRUE);

            if (bHandheldSquareDraw)
            {
                if (_pSystem == _pGba)
                    GSK_SetGba240pMatchedDraw(0);
                else
                    GSK_SetGbSquarePixelDraw(0);
            }
        }

        /* AURORA_QN_LIGHTGUN_OVERLAY_V7_20260829
         * Post-frame overlay: never enters the QuickNES sensor framebuffer. */
        if (_pSystem == _pNes && QuicknesBridge_LightGunActive())
            QuicknesBridge_DrawLightGunCursor(
                g_GskVideoMode == GSK_VIDMODE_240P ? 2 : 4);

        //PolyTexture(NULL);
        //PolyRect(dx,dy,128,120);
    }


#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerGameDrawEnd();
#endif

    #if AURORA_RUNTIME_DIAG_OVERLAY
    /* AURORA_PCE_KRAZY_RUNTIME_DIAG_V11R3_20260830
     * Hidden diagnostics retained for future PCE/QuickNES investigations. */
    if (!_bMenu && _pSystem == _pPce)
    {
        unsigned vw=0, vh=0, pp=0;
        int pfb=0, nativeClass=0;
        int gfb=0, win=0, dw=0, mh=0, sx=0, ovs=0, ws=0;

        PceBridge_GetVideoDebug(&vw, &vh, &pp, &pfb, &nativeClass);
        GSK_GetPceDebugState(&gfb, &win, &dw, &mh, &sx, &ovs, &ws);

        FontSelect(2);
        FontColor4f(1.0f, 1.0f, 0.0f, 1.0f);
        FontPrintf(24, 20, "PCE W%u H%u P%u FB%d N%d", vw, vh, pp, pfb, nativeClass);
        FontPrintf(24, 32, "GFB%d WIN%d DW%d M%d O%d W%d", gfb, win, dw, mh+1, ovs, ws);
    }
    /* AURORA_V6_1G_QN_KRAZY_DEBUG_CONSUMER_CURE_20260831
     * Retired obsolete QuickNES mapper-79 debug overlay; PCE diagnostics above remain. */
    #endif /* AURORA_RUNTIME_DIAG_OVERLAY */

    if (!_bMenu)
    {	
	
#if AURORA_SNES_COST_PROFILER
		if (_pSystem == _pSnes)
			AuroraSnesCostProfilerDrawOverlay();
#endif

		if (s_pMovieClip->IsPlaying())
		{
	        FontSelect(2);
	        FontColor4f(0.5, 0.5f, 0.5f, 1.0f);
	        FontPrintf(240,220, ">");
		}

		if (s_pMovieClip->IsRecording())
		{
	        FontSelect(2);
	        FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
	        FontPrintf(240,220, "O");
		}


		switch (_MainLoop_uDebugDisplay)
        {
		case 0:
/*	        FontSelect(2);
	        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
	        FontPrintf(40,170, "%08X", InputGetPadData(0));
  */

//		        FontSelect(2);
//		        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
//		        FontPrintf(40,190, "%3d", xpadGetFrameCount(0,0));
			break;
		case 1:
		/*
	        FontSelect(2);
	        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
	        FontPrintf(40,190, "%3d %3d", NetInput.InputSize[0], NetInput.OutputSize[0]);
	        FontPrintf(40,200, "%3d %3d", NetInput.InputSize[1], NetInput.OutputSize[1]);
	        FontPrintf(40,210, "%3d %3d", NetInput.InputSize[2], NetInput.OutputSize[2]);
	        FontPrintf(40,220, "%3d %3d", NetInput.InputSize[3], NetInput.OutputSize[3]);
			*/
			break;
		case 2:
	        FontSelect(2);
	        FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
	        FontPrintf(40,170, "%08X", _uInputFrame);
	        FontPrintf(40,180, "%08X", _uInputChecksum[0]);
	        FontPrintf(40,190, "%08X", _uInputChecksum[1]);
	        FontPrintf(40,200, "%08X", _uInputChecksum[2]);
	        FontPrintf(40,210, "%08X", _uInputChecksum[3]);
	        FontPrintf(40,220, "%08X", _uInputChecksum[4]);
			break;
		case 3:
			FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
			FontPrintf(195, 210, "%8d", _uVblankCycle / 1024);
			break;
        }

        FontSelect(2);
		FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
		{

/*
		FontPrintf(15, 180, "%08X %08X Y", (Int32)(_ColorCalib.y_mul * 0x10000), (Int32)(_ColorCalib.y_add * 0x10000));
		FontPrintf(15, 190, "%08X %08X I", (Int32)(_ColorCalib.i_mul * 0x10000), (Int32)(_ColorCalib.i_add * 0x10000));
		FontPrintf(15, 200, "%08X %08X Q", (Int32)(_ColorCalib.q_mul * 0x10000), (Int32)(_ColorCalib.q_add * 0x10000));
  */

		/*
		FontPrintf(195, 180, "%6.3f %6.3f", _ColorCalib.y_mul, _ColorCalib.y_add);
		FontPrintf(195, 190, "%6.3f %6.3f", _ColorCalib.i_mul, _ColorCalib.i_add);
		FontPrintf(195, 200, "%6.3f %6.3f", _ColorCalib.q_mul, _ColorCalib.q_add);
		*/
		}

//			FontPrintf(195, 210, "%8d", _AudMix.GetLastOutput());
    }


#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerUiBegin();
#endif
	/* Keep menu audio alive even while a modal overlays the UI. Previously
	   BgmUpdate lived only in the non-modal branch below, so every fixed-time
	   message starved audsrv regardless of whether any I/O was happening. */
	if (_bMenu)
	{
		/* AURORA_FINAL_V1_3_NORMAL_MENU_BGM_SESSION_20260901
		 * _bMenu alone is insufficient: isolated quick-state/device/format
		 * prompts also pause the core without acquiring the CD transport.
		 * BGM may scan/open storage, so it is legal only for a session that
		 * successfully entered through _MenuEnable(TRUE). */
		if (MainLoopNormalMenuBgmSessionActive() &&
		    MainLoopCdUiReady() && /* AURORA_SSF2_PCE_MENU_FIX_V2_20260913_PCE_MENU_ASYNC */
		    _MainLoop_pScreen != (CScreen *)_MainLoop_pStateConfirmScreen)
			BgmUpdate();
		/* Draw the live menu first, then place modal/status text on top. The
		   previous order painted _MenuDraw after the status and hid it. */
		_MenuDraw();
	}

	if (_MainLoop_ModalCount > 0)
	{
		FontSelect(0);
		{
			const Int32 textW = FontGetStrWidth(_MainLoop_ModalStr);
			const Int32 textX = 128 - textW / 2;
			const Bool bErrorModal =
				!strncmp(_MainLoop_ModalStr, "ERROR:", 6) ? TRUE : FALSE;

			/* AURORA_TSUKURU_8M_GBA_LOAD_AUDIO_REDERR_V1_20260913_RED_ERRORS
			 * Loader failures can leave the previous framebuffer white, black or
			 * partially rendered. ERROR: is already Aurora's modal error contract;
			 * erase that accidental background and give every error the same
			 * opaque red screen. Non-error informational modals retain the old
			 * compact black backing.
			 */
			PolyTexture(NULL);
			PolyBlend(FALSE);
			if (bErrorModal)
			{
				PolyColor4f(0.55f, 0.0f, 0.0f, 1.0f);
				PolyRect(0.0f, 0.0f,
				         (Float32)MAINLOOP_SCREENWIDTH,
				         (Float32)MAINLOOP_SCREENHEIGHT);
				/* Darker red text plate: readable but still visibly an error. */
				PolyColor4f(0.30f, 0.0f, 0.0f, 1.0f);
			}
			else
			{
				PolyColor4f(0.0f, 0.0f, 0.0f, 1.0f);
			}
			PolyRect((Float32)(textX - 4), 96.0f,
			         (Float32)(textW + 8), 16.0f);
			PolyBlend(TRUE);

			FontColor4f(1.0, 1.0f, 1.0f, 1.0f);
			FontPrintf(textX, 100, _MainLoop_ModalStr);
		}

		_MainLoop_ModalCount--;
	}
	else
	{
		if (_MainLoop_StatusCount > 0)
		{
			FontSelect(0);
			/* AURORA_V15_STATUS_SHADOW_REVERT_20260824
			 * Revert V15 black double-draw: transient status text is plain cyan again. */
			FontColor4f(0.0, 0.8f, 0.8f, 1.0f);
			FontPrintf(20, 200, _MainLoop_StatusStr);
			_MainLoop_StatusCount--;
		}
	}

#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerUiEnd();
#endif
	#if CODE_DEBUG
	if (_MainLoop_bMCSaveReady && MCSave_WriteSync(FALSE, NULL))
	{
		FontSelect(1);
		FontColor4f(1.0, 0.0f, 0.0f, 1.0f);
		if (_iFrame & 4)
			FontPrintf(235,216, "#");
	}
	#endif



#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerGpFlushBegin();
#endif
    PROF_ENTER("GPFlush");
#if AURORA_C4_PROFILER
    const Uint32 c4Flush=g_AuroraC4ProfilerActive?ProfCtrGetCycle():0u;
#endif
    GPFifoFlush();
#if AURORA_C4_PROFILER
    if(g_AuroraC4ProfilerActive)AuroraC4ProfilerGpFlush((Uint32)(ProfCtrGetCycle()-c4Flush));
#endif
    PROF_LEAVE("GPFlush");
#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerGpFlushEnd();
#endif

    /* gsKit_sync_flip waits for vsync, swaps the display buffer
       and resets gsKit's draw queue for the next frame. The
       legacy WaitForNextVRstart / GS_SetCrtFB / GS_SetDrawFB
       block is now subsumed by this single call. */
    /* V4: end recovery work timing before the blocking VSync wait. */
    s_SafeFrameskipPreFlip =
        (s_SafeFrameskipTickStart != 0u) ? ProfCtrGetCycle() : 0u;

    PROF_ENTER("WaitVBlank");
#if AURORA_C4_PROFILER
    const Uint32 c4Vb=g_AuroraC4ProfilerActive?ProfCtrGetCycle():0u;
#endif
    if ( (_iFrame&15)==0)   _uVblankCycle = ProfCtrGetCycle();
#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerVBlankBegin();
#endif
    GSK_SyncFlip();
#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerVBlankEnd();
#endif
    if ( (_iFrame&15)==0)   _uVblankCycle = ProfCtrGetCycle() - _uVblankCycle;
#if AURORA_C4_PROFILER
    if(g_AuroraC4ProfilerActive)AuroraC4ProfilerVBlank((Uint32)(ProfCtrGetCycle()-c4Vb));
#endif
    _MainLoopSafeFrameskipAfterFlip();
    PROF_LEAVE("WaitVBlank");

    /* AURORA_AUDIO_FAILSOFT_POSTVBLANK_V1
     * The frame is already presented. Keep the normal gameplay audio drain
     * here so synchronous audsrv RPC latency is moved out of the pre-render
     * deadline. Aud_BufferedAsyncStart uses only wait=0 drains. */
    if (!_bMenu && _pSystem && !_MainLoop_BlackScreen)
    {
#if AURORA_C4_PROFILER
        const Uint32 c4PostAud=g_AuroraC4ProfilerActive?ProfCtrGetCycle():0u;
#endif
#if AURORA_FRONTEND_PROFILER
        AuroraFrontendProfilerAudioDrainBegin();
#endif
        Aud_BufferedAsyncStart();
#if AURORA_FRONTEND_PROFILER
        AuroraFrontendProfilerAudioDrainEnd();
#endif
#if AURORA_C4_PROFILER
        if(g_AuroraC4ProfilerActive)AuroraC4ProfilerPostAudio((Uint32)(ProfCtrGetCycle()-c4PostAud),FALSE);
#endif

        /* V15 completed presented-host-tick boundary. Commit AFTER the
         * post-VBlank drain because that work occurs before the next Take()
         * during uninterrupted gameplay and therefore belongs to phase. */
        _MainLoopSafeFrameskipCommitGameplayBoundary(
            ProfCtrGetCycle());
    }

#if AURORA_FRONTEND_PROFILER
    AuroraFrontendProfilerRenderEnd();
#endif
    _iFrame++;
}


/* AURORA_PCE_KRAZY_RUNTIME_DIAG_V11R3_20260830 */

/* AURORA_PCE_ROOT512_KRAZY_LATCH_V12_20260830 */

/* AURORA_PCE_RELEASE_CLEANUP_FINAL_V15_20260830 */
