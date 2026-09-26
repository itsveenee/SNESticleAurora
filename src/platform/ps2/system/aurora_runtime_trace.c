/* AURORA_SNES_BINARY_TRACE_V6_20260918 */
/* AURORA_EE_CRASH_DIAG_DKC_V1_20260918 */
/* AURORA_RUNTIME_TRACE_INFRA_V1_20260918
 * Aurora-owned trace engine. SNES payloads are the first client;
 * investigation policy is intentionally kept out of this file. */
#include "platform/ps2/system/aurora_runtime_trace.h"
#include "platform/ps2/system/aurora_runtime_trace_profile.h"
#include "platform/ps2/system/aurora_ee_crash_diag.h"

#if AURORA_RUNTIME_TRACE

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

#include "snes/cpu/sncpu.h"
#include "snes/apu/snspc.h"

#define ATR_RING_CAP       2048u
#define ATR_RING_MASK      (ATR_RING_CAP - 1u)
#define ATR_CONTEXT_TAIL   128u
#define ATR_CHECKPOINT_TAIL 16u
#define ATR_CTX_FRAME_CHECKPOINT 0xE1u
#define ATR_CTX_HIGHSIGNAL_CHECKPOINT 0xE2u
#define ATR_CTX_AUTO_CHECKPOINT 0xE3u
#define ATR_CTX_PHASE_CHECKPOINT 0xE4u
#define ATR_AUTO_COMMIT    512u

typedef struct __attribute__((packed))
{
    Uint8 magic[8];
    Uint16 version;
    Uint16 rec_size;
    Uint16 scpu_size;
    Uint16 spc_size;
    Uint32 baseline_prefix;
    char game[40];
    Uint32 reserved;
} AuroraTraceHeaderT;

typedef struct __attribute__((packed))
{
    Uint8 type;
    Uint8 flags;
    Uint16 x;
    Uint32 seq;
    Uint32 a;
    Uint32 b;
} AuroraTraceRecT;

typedef struct __attribute__((packed))
{
    Uint32 seq;
    Uint32 pc;
    Int32 frame;
    Int32 line;
    Int32 cycles;
    Uint16 a, x, y, s;
    Uint8 opcode, p_raw, signal, e;
} AuroraSCPUTraceEntryT;

typedef struct __attribute__((packed))
{
    Uint32 seq;
    Uint16 pc;
    Uint8 opcode, halt;
    Int32 total, frame, cycles;
    Uint8 a, x, y, sp, psw, f2, f3, rom;
    Uint8 port0, port1, port2, port3;
} AuroraSPCTraceEntryT;

typedef char _atr_hdr_64[(sizeof(AuroraTraceHeaderT) == 64) ? 1 : -1];
typedef char _atr_rec_16[(sizeof(AuroraTraceRecT) == 16) ? 1 : -1];
typedef char _atr_scpu_32[(sizeof(AuroraSCPUTraceEntryT) == 32) ? 1 : -1];
typedef char _atr_spc_32[(sizeof(AuroraSPCTraceEntryT) == 32) ? 1 : -1];

typedef struct __attribute__((packed))
{
    AuroraTraceRecT rec;
    AuroraSCPUTraceEntryT scpu[2];
    AuroraSPCTraceEntryT spc[2];
} AuroraTraceSnapshotT;

typedef char _atr_snapshot_144[
    (sizeof(AuroraTraceSnapshotT) == 144) ? 1 : -1];

volatile Uint32 g_AuroraTraceEnabled = 0;

/* Written directly by sn65816.S before opcode dispatch. */
volatile AuroraSCPUTraceEntryT
    g_AuroraTraceSCPU[ATR_RING_CAP] __attribute__((aligned(64)));
volatile Uint32 g_AuroraTraceSCPUWrite = 0;
volatile Uint32 g_AuroraTraceSeq = 0;

static volatile AuroraSPCTraceEntryT
    s_spc[ATR_RING_CAP] __attribute__((aligned(64)));
static volatile Uint32 s_spc_write = 0;

static FILE *s_fp = NULL;
static char s_path[384] = {0};
static char s_game[128] = {0};
static char s_core[32] = {0};
static Uint32 s_records_since_commit = 0;
static Uint8 s_trace_io_failed = 0;
static Uint8 s_brr_armed = 0;
static Uint8 s_brr_inflight = 0;

static Uint32 s_trace_frame_serial = 0;
static Uint32 s_trace_emu_frame = 0;
static Uint8 s_trace_frame_crumb_mask = 0;
static Uint32 s_trace_last_durable_serial = 0;
static Uint8 s_trace_probe_active = 0;

static Uint32 nextseq(void)
{
    Uint32 v = g_AuroraTraceSeq + 1u;
    g_AuroraTraceSeq = v;
    return v;
}

static void copystr(char *d, Uint32 n, const char *s)
{
    Uint32 i = 0;
    if (!n) return;
    if (!s) s = "";
    while (i + 1u < n && s[i]) { d[i] = s[i]; ++i; }
    d[i] = 0;
}

static void makestamp(char *o, Uint32 n)
{
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t && t->tm_year + 1900 >= 2000)
        snprintf(o, n, "%04d%02d%02d-%02d%02d%02d",
                 t->tm_year + 1900, t->tm_mon + 1, t->tm_mday,
                 t->tm_hour, t->tm_min, t->tm_sec);
    else
        snprintf(o, n, "ps2-%lu", (unsigned long)now);
}

static FILE *tryopen(const char *root, const char *ts)
{
    char p[384];
    FILE *f;
    Uint32 n;
    long endPos;

    (void)mkdir(root, 0777);

    /* Do not let two toggles/loads within the same RTC second destroy the
       earlier crash capture.  "wb" is used only after a non-existent name
       has been selected. */
    for (n = 0; n < 100u; ++n)
    {
        if (n == 0u)
            snprintf(p, sizeof(p), "%s/a6.%s.bin", root, ts);
        else
            snprintf(p, sizeof(p), "%s/a6.%s.%02lu.bin", root, ts,
                     (unsigned long)n);

        /* Stay entirely on newlib/POSIX stdio.  Never use destructive "wb"
           here: direct fio/fileXio calls are forbidden by PS2SDK's newlib
           port, while libc stat() cannot distinguish ENOENT from every raw
           fioGetstat failure.  Append-open is non-destructive; after opening,
           accept only a truly empty candidate.  Existing non-empty captures
           are closed untouched and the next suffix is tried. */
        f = fopen(p, "ab");
        if (!f)
            return NULL;
        if (fseek(f, 0, SEEK_END) != 0)
        {
            fclose(f);
            return NULL;
        }
        endPos = ftell(f);
        if (endPos < 0)
        {
            fclose(f);
            return NULL;
        }
        if (endPos != 0)
        {
            fclose(f);
            continue;
        }
        if (f)
        {
            copystr(s_path, sizeof(s_path), p);
            return f;
        }
    }
    return NULL;
}

/* AURORA_SNES_BINARY_TRACE_V6D_SPARSE_HIGHSIGNAL_20260918 */
static void flush_only(void)
{
    if (!s_fp || s_trace_io_failed) return;
    /* A flush is not a durable FAT metadata commit.  Keep
       s_records_since_commit accumulating until a real fclose boundary. */
    if (fflush(s_fp) != 0)
    {
        s_trace_io_failed = 1;
        (void)fclose(s_fp);
        s_fp = NULL;
    }
}

/* AURORA_SNES_BINARY_TRACE_V7_1_KEEP_OPEN_20260918 */
/* AURORA_SNES_BINARY_TRACE_V7_2_MASS_CRASHSAFE_20260918 */
/* AURORA_SNES_BINARY_TRACE_V7_3_REOPEN_GUARD_20260918 */
/* AURORA_SNES_BINARY_TRACE_V7_R4_FRAMEBEGIN_CONTEXT_IOGUARD_20260918 */
/* AURORA_SNES_BINARY_TRACE_V7_R5_CLOSE_FENCE_HIGHSIGNAL_20260918 */
/* AURORA_SNES_BINARY_TRACE_V7_R6_ACK_SAFE_CREATE_20260918 */
/* AURORA_SNES_BINARY_TRACE_V7_R7_NEWGAME_PATH_GUARD_20260918 */
/* AURORA_SNES_BINARY_TRACE_V7_R8_NEWLIB_SAFE_CREATE_20260918 */
/* AURORA_SNES_BINARY_TRACE_V7_R9_DKC_PHASEPROBE_20260918 */
/* AURORA_SNES_BINARY_TRACE_V7_R10_RETURN_BOUNDARY_20260918 */
static int reopen_append(void)
{
    struct stat st;

    if (!s_path[0] || s_trace_io_failed) return -1;

    /* Recovery must never create a new file: without the 64-byte AUR6BIN
       header it would only produce an undecodable tail.  stat() also makes a
       transient/missing-path failure harmless until a later retry. */
    if (stat(s_path, &st) != 0)
        return -1;
    if ((Uint32)st.st_size < (Uint32)(sizeof(AuroraTraceHeaderT) + sizeof(AuroraTraceRecT)))
        return -1;

    /* "ab" is the original V6 append path and is the least surprising mode
       for PS2 stdio.  The existence check above prevents its create semantic
       from silently manufacturing a headerless replacement. */
    s_fp = fopen(s_path, "ab");
    if (!s_fp) return -1;
    setvbuf(s_fp, NULL, _IONBF, 0);
    return 0;
}

static Bool ensure_trace_open(void)
{
    if (s_trace_io_failed) return FALSE;
    if (s_fp) return TRUE;
    if (!g_AuroraTraceEnabled || !s_path[0]) return FALSE;
    return (reopen_append() == 0) ? TRUE : FALSE;
}

static Bool write_one(const void *p, Uint32 size);
static void write_rec_raw(
    Uint8 type, Uint8 flags, Uint16 x, Uint32 seq, Uint32 a, Uint32 b);

static void durable_commit(void)
{
    Bool bDurable;
    Uint16 pending;
    Uint32 fenceSeq = 0;

    if (!s_fp) return;
    bDurable = s_trace_io_failed ? FALSE : TRUE;

    /* The fence is PRE-close evidence, not a self-authenticating success
       marker.  Keep its sequence so a later post-close ACK can name exactly
       which fence was followed by a successful fflush/fclose return. */
    if (bDurable)
    {
        pending = (s_records_since_commit > 0xFFFFu)
            ? 0xFFFFu : (Uint16)s_records_since_commit;
        fenceSeq = nextseq();
        write_rec_raw(
            ATR_CLOSE_FENCE, ATR_F_PRE, pending, fenceSeq,
            s_trace_frame_serial, s_trace_emu_frame);
        if (s_trace_io_failed)
            bDurable = FALSE;
    }

    /* A short fwrite or failed fflush/fclose means this epoch cannot be
       advertised as durable.  Do not append after a structural write failure:
       the existing prefix remains decodable up to its last complete record. */
    if (fflush(s_fp) != 0)
    {
        bDurable = FALSE;
        s_trace_io_failed = 1;
    }
    if (fclose(s_fp) != 0)
    {
        bDurable = FALSE;
        s_trace_io_failed = 1;
    }
    s_fp = NULL;
    s_records_since_commit = 0;

    if (!bDurable)
        return;

    s_trace_last_durable_serial = s_trace_frame_serial;

    /* No extra close is introduced.  The ACK is appended after the successful
       close and will itself become visible only if a later close boundary
       persists it.  That gives the offline decoder stronger evidence about
       the previous fence without perturbing DKC with twice as many closes. */
    if (reopen_append() == 0)
        write_rec_raw(
            ATR_CLOSE_ACK, ATR_F_POST, 0, nextseq(),
            fenceSeq, s_trace_frame_serial);
}

static Bool write_one(const void *p, Uint32 size)
{
    if (!s_fp || s_trace_io_failed || !p || !size)
        return FALSE;
    if (fwrite(p, (size_t)size, 1, s_fp) != 1)
    {
        s_trace_io_failed = 1;
        return FALSE;
    }
    return TRUE;
}

static void write_rec_raw(
    Uint8 type, Uint8 flags, Uint16 x, Uint32 seq, Uint32 a, Uint32 b)
{
    AuroraTraceRecT r;
    if (!ensure_trace_open()) return;
    r.type = type;
    r.flags = flags;
    r.x = x;
    r.seq = seq;
    r.a = a;
    r.b = b;
    if (write_one(&r, sizeof(r)))
        ++s_records_since_commit;
}

static void write_context_tail(Uint8 reason, Uint32 tail)
{
    Uint32 cw, sw, c0, s0, cn, sn, i;

    if (!ensure_trace_open() || s_trace_io_failed) return;

    if (tail > ATR_RING_CAP)
        tail = ATR_RING_CAP;

    cw = g_AuroraTraceSCPUWrite;
    sw = s_spc_write;
    c0 = (cw > tail) ? (cw - tail) : 0u;
    s0 = (sw > tail) ? (sw - tail) : 0u;
    cn = cw - c0;
    sn = sw - s0;

    /* Context header: x=SCPU count, a=SPC count, b=reason tag. */
    write_rec_raw(ATR_CONTEXT, 0, (Uint16)cn, nextseq(), sn, reason);
    if (s_trace_io_failed) return;

    for (i = c0; i < cw; ++i)
    {
        AuroraSCPUTraceEntryT e = g_AuroraTraceSCPU[i & ATR_RING_MASK];
        if (!write_one(&e, sizeof(e))) return;
    }
    for (i = s0; i < sw; ++i)
    {
        AuroraSPCTraceEntryT e = s_spc[i & ATR_RING_MASK];
        if (!write_one(&e, sizeof(e))) return;
    }
}

static void write_context(Uint8 reason)
{
    write_context_tail(reason, ATR_CONTEXT_TAIL);
}

/* AURORA_SNES_BINARY_TRACE_V7_VISUAL_BREADCRUMBS_20260918 */
static void write_snapshot(
    Uint8 kind, Uint16 payload, Uint32 emuFrame, Bool flush)
{
    AuroraTraceSnapshotT q;
    Uint32 cw, sw, i, n;

    if (!g_AuroraTraceEnabled || !ensure_trace_open()) return;

    memset(&q, 0, sizeof(q));
    q.rec.type = ATR_SNAPSHOT;
    q.rec.flags = kind;
    q.rec.x = payload;
    q.rec.seq = nextseq();
    q.rec.a = s_trace_frame_serial;
    q.rec.b = emuFrame;

    cw = g_AuroraTraceSCPUWrite;
    sw = s_spc_write;

    n = (cw >= 2u) ? 2u : cw;
    for (i = 0; i < n; ++i)
        q.scpu[2u - n + i] =
            g_AuroraTraceSCPU[(cw - n + i) & ATR_RING_MASK];

    n = (sw >= 2u) ? 2u : sw;
    for (i = 0; i < n; ++i)
        q.spc[2u - n + i] =
            s_spc[(sw - n + i) & ATR_RING_MASK];

    if (write_one(&q, sizeof(q)))
        ++s_records_since_commit;
    if (flush)
        flush_only();
}

void AuroraTraceFrameBegin(Uint32 emuFrame)
{
    Bool bCheckpoint;

    if (!g_AuroraTraceEnabled || !ensure_trace_open()) return;
    ++s_trace_frame_serial;
    s_trace_emu_frame = emuFrame;
    s_trace_frame_crumb_mask = 0;

    /* The engine only asks the active profile whether this client/frame
       should receive expensive durable phase checkpoints. */
    s_trace_probe_active =
        AuroraTraceProfileFrameActive(s_core, emuFrame) ? 1u : 0u;

    /* Keep normal cadence; a profile may additionally make its target frame
       or frame window durable at FrameBegin. */
    bCheckpoint =
        (s_trace_frame_serial == 1u ||
         (s_trace_frame_serial & 3u) == 0u ||
         (s_trace_probe_active &&
          AuroraTraceProfileForceFrameCommit())) ? TRUE : FALSE;

    if (bCheckpoint)
        write_context_tail(ATR_CTX_FRAME_CHECKPOINT, ATR_CHECKPOINT_TAIL);

    write_snapshot(ATR_SNAP_FRAME_BEGIN, 0, emuFrame, FALSE);

    if (bCheckpoint)
        durable_commit();
    else
        flush_only();
}

void AuroraTraceFrameEnd(Uint32 emuFrame)
{
    if (!g_AuroraTraceEnabled || !ensure_trace_open()) return;
    write_snapshot(ATR_SNAP_FRAME_END, 0, emuFrame, FALSE);
    /* The periodic durable boundary now lives at FrameBegin so an incomplete
       ExecuteFrame can be identified from a committed frame> marker. */
    flush_only();
    s_trace_probe_active = 0;
}

/* AURORA_SNES_BINARY_TRACE_V7_R9_DKC_PHASEPROBE_20260918_API */
/* AURORA_TRACE_PROFILE_DKC_HUNT_V1_20260918: profile owns expensive phase selection. */
/* AURORA_EE_HANG_WATCHDOG_DKC_V2_20260918: RAM-first phase breadcrumb before any file-backed work. */
void AuroraTracePhase(Uint16 phase, Uint32 a, Uint32 b)
{
    if (!g_AuroraTraceEnabled || !s_trace_probe_active)
        return;

    AuroraEECrashDiagBreadcrumb(
        AED_TRACE_PHASE,
        ((Uint32)phase << 16) | (a & 0xFFFFu),
        b);

    if (!AuroraTraceProfilePhaseSelected(phase) ||
        !ensure_trace_open())
        return;

    /* Four instructions from each CPU are enough to identify the boundary
       while keeping each phase checkpoint small.  The close, not the byte
       count, dominates PS2 mass: overhead. */
    write_context_tail(
        ATR_CTX_PHASE_CHECKPOINT,
        AURORA_TRACE_PROFILE_PHASE_CONTEXT_TAIL);
    write_rec_raw(ATR_PHASE, ATR_F_PRE, phase, nextseq(), a, b);
    durable_commit();
}

void AuroraTraceLinePhase(Uint32 line, Uint32 cpuFrame)
{
    /* SNES supplies the hook; the active profile decides which lines matter. */
    if (!s_trace_probe_active ||
        !AuroraTraceProfileSnesLineSelected(line))
        return;
    AuroraTracePhase(ATR_PHASE_LINE_ENTER, line, cpuFrame);
}

void AuroraTraceBreadcrumb(Uint8 kind, Uint16 payload)
{
    Uint8 bit = 0;

    if (!g_AuroraTraceEnabled || !ensure_trace_open()) return;

    if (kind == ATR_SNAP_MDMA) bit = 1u;
    else if (kind == ATR_SNAP_HDMA) bit = 2u;

    if (bit)
    {
        if (s_trace_frame_crumb_mask & bit) return;
        s_trace_frame_crumb_mask |= bit;
    }

    write_snapshot(kind, payload, s_trace_emu_frame, FALSE);

    if (kind == ATR_SNAP_APUIO || kind == ATR_SNAP_BRR)
    {
        /* High-signal write-ahead breadcrumb. Rate-limit real USB metadata
           commits to at most one per two emulated frames.  When a close is
           already going to happen, spend only ~1 KiB more to preserve the
           16 most recent instructions from each CPU at that exact moment. */
        if (s_trace_frame_serial == 0u ||
            (s_trace_frame_serial - s_trace_last_durable_serial) >= 2u)
        {
            write_context_tail(
                ATR_CTX_HIGHSIGNAL_CHECKPOINT, ATR_CHECKPOINT_TAIL);
            durable_commit();
        }
        else
            flush_only();
    }
}

static void reset_rings(void)
{
    g_AuroraTraceSCPUWrite = 0;
    s_spc_write = 0;
    g_AuroraTraceSeq = 0;
    s_records_since_commit = 0;
    s_trace_io_failed = 0;
    s_brr_armed = 0;
    s_brr_inflight = 0;
    s_trace_frame_serial = 0;
    s_trace_emu_frame = 0;
    s_trace_frame_crumb_mask = 0;
    s_trace_last_durable_serial = 0;
    s_trace_probe_active = 0;
    memset((void *)g_AuroraTraceSCPU, 0, sizeof(g_AuroraTraceSCPU));
    memset((void *)s_spc, 0, sizeof(s_spc));
}

Bool AuroraTraceIsEnabled(void)
{
    return g_AuroraTraceEnabled ? TRUE : FALSE;
}

const char *AuroraTraceProfileName(void)
{
    return AURORA_TRACE_PROFILE_NAME;
}

Bool AuroraTraceToggleRuntime(void)
{
    if (g_AuroraTraceEnabled)
    {
        if (s_fp)
        {
            write_rec_raw(ATR_TOGGLE, ATR_F_POST, 0, nextseq(), 0, 0);
            durable_commit();
            if (s_fp) { fclose(s_fp); s_fp = NULL; }
        }
        AuroraEECrashDiagRemove();
        g_AuroraTraceEnabled = 0;
        return FALSE;
    }

    reset_rings();
    s_path[0] = 0;
    s_game[0] = 0;
    s_core[0] = 0;
    g_AuroraTraceEnabled = 1;
    AuroraEECrashDiagReset();
    (void)AuroraEECrashDiagInstall();
    return TRUE;
}

/* AURORA_RUNTIME_DEBUGGER_MENU_V5_20260919
 * Idempotent runtime setter for menu/UI callers. Preserve the existing
 * toggle's durable close/open ownership and do nothing when already in the
 * requested state. */
Bool AuroraTraceSetEnabled(Bool enabled)
{
    const Bool want = enabled ? TRUE : FALSE;
    const Bool have = g_AuroraTraceEnabled ? TRUE : FALSE;

    if (have == want)
        return have;

    return AuroraTraceToggleRuntime();
}

void AuroraTraceBeginGame(const char *game, const char *core)
{
    static const char *roots[] = {
        "mass:/SNESticle", "mass0:/SNESticle", "mass1:/SNESticle"
    };
    AuroraTraceHeaderT h;
    char ts[64];
    Uint32 i;

    if (!g_AuroraTraceEnabled) return;

    /* BeginGame is called from the frame wrapper.  If a previous durable
       close succeeded but its immediate reopen failed, s_fp is NULL while
       s_path/s_game still identify a perfectly valid existing capture.
       Retry that capture and return either way; never reach tryopen("wb") for
       the same game, because that could truncate the only crash evidence. */
    if (game && s_path[0] && !strcmp(s_game, game) &&
        ((!core && !s_core[0]) ||
         (core && !strcmp(s_core, core))))
    {
        (void)ensure_trace_open();
        return;
    }

    if (s_fp)
    {
        durable_commit();
        if (s_fp) { fclose(s_fp); s_fp = NULL; }
    }

    /* We are switching to a different game.  Never leave the previous
       capture path paired with the new game name: if creation of the new
       BIN fails, the next BeginGame() must retry creation, not reopen and
       append the new game's records to the old game's capture. */
    s_path[0] = 0;

    reset_rings();
    copystr(s_game, sizeof(s_game), game ? game : "");
    copystr(s_core, sizeof(s_core), core ? core : "");
    AuroraEECrashDiagReset();
    makestamp(ts, sizeof(ts));

    for (i = 0; i < (Uint32)(sizeof(roots) / sizeof(roots[0])); ++i)
    {
        s_fp = tryopen(roots[i], ts);
        if (s_fp) break;
    }
    if (!s_fp) return;

    setvbuf(s_fp, NULL, _IONBF, 0);
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, "AUR6BIN", 7);
    h.version = 6;
    h.rec_size = sizeof(AuroraTraceRecT);
    h.scpu_size = sizeof(AuroraSCPUTraceEntryT);
    h.spc_size = sizeof(AuroraSPCTraceEntryT);
    h.baseline_prefix = 0xE0CFC2B2u;
    copystr(h.game, sizeof(h.game), s_game);
    h.reserved = 0x30315256u; /* bytes: "VR10" */
    (void)write_one(&h, sizeof(h));
    write_rec_raw(ATR_TOGGLE, ATR_F_PRE, 0, nextseq(), 1, 0);
    durable_commit();
}

void AuroraTraceClose(void)
{
    if (!s_fp) return;
    durable_commit();
    if (s_fp) { fclose(s_fp); s_fp = NULL; }
}

void AuroraTraceRecord(
    Uint8 type, Uint8 flags, Uint16 x, Uint32 a, Uint32 b, Uint8 persist)
{
    if (!g_AuroraTraceEnabled || !ensure_trace_open()) return;

    /* Context comes first, so the PRE record is the final semantic record
       persisted before the risky operation executes. */
    if (persist == ATR_P_CONTEXT)
        write_context(type);

    write_rec_raw(type, flags, x, nextseq(), a, b);

    if (persist == ATR_P_CONTEXT)
        durable_commit();
    else if (persist == ATR_P_COMMIT)
    {
        write_context_tail(
            ATR_CTX_HIGHSIGNAL_CHECKPOINT, ATR_CHECKPOINT_TAIL);
        durable_commit();
    }
    else if (persist == ATR_P_FLUSH)
    {
        /* V6D/V7 high-signal events remain cheap most of the time, but a
           not-recently-durable event gets a real mass: checkpoint. */
        if (s_trace_frame_serial == 0u ||
            (s_trace_frame_serial - s_trace_last_durable_serial) >= 2u)
        {
            write_context_tail(
                ATR_CTX_HIGHSIGNAL_CHECKPOINT, ATR_CHECKPOINT_TAIL);
            durable_commit();
        }
        else
            flush_only();
    }
    else if (s_records_since_commit >= ATR_AUTO_COMMIT)
    {
        write_context_tail(ATR_CTX_AUTO_CHECKPOINT, ATR_CHECKPOINT_TAIL);

        /* AURORA_TRACE_SAMEFRAME_AUTOCOMMIT_GUARD_V1_1_20260926
         * A polling-heavy CPU loop can generate several 512-record automatic
         * checkpoints without advancing a single emulated frame.  Once this
         * frame has already crossed a successful durable close boundary,
         * closing/reopening the mass: stream again buys no newer frame-level
         * crash boundary and needlessly exercises PS2 newlib + USB/FAT I/O.
         *
         * Preserve the checkpoint payload and flush its bytes, but reserve the
         * expensive durable close/reopen for the first auto checkpoint after
         * frame progress. Explicit/high-signal commits are unchanged. */
        if (s_trace_frame_serial == s_trace_last_durable_serial)
        {
            flush_only();
            if (!s_trace_io_failed)
                s_records_since_commit = 0;
        }
        else
        {
            durable_commit();
        }
    }
}

void AuroraTracePeriodic(Uint32 frame)
{
    /* V7: frame snapshots supersede the old 60-frame full-context heartbeat. */
    (void)frame;
}

void AuroraTraceArmVoice(Int32 channel)
{
    if (!g_AuroraTraceEnabled) return;
    if (channel >= 0 && channel < 8)
        s_brr_armed |= (Uint8)(1u << (Uint32)channel);
}

void AuroraTraceBrrPre(Int32 channel, Uint16 addr, Int32 prev14, Int32 prev15)
{
    Uint8 bit;
    Uint32 packed;

    if (!g_AuroraTraceEnabled || channel < 0 || channel >= 8) return;
    bit = (Uint8)(1u << (Uint32)channel);
    if (!(s_brr_armed & bit)) return;

    s_brr_inflight |= bit;
    packed = ((Uint32)(Uint16)prev14 << 16) | (Uint16)prev15;
    AuroraTraceBreadcrumb(
        ATR_SNAP_BRR, (Uint16)(((channel & 7) << 12) | (addr & 0x0FFFu)));
    AuroraTraceRecord(
        ATR_BRR, (Uint8)(ATR_F_PRE | ((channel & 7) << 2)), addr,
        packed, 0, ATR_P_FLUSH);
}

void AuroraTraceBrrPost(Int32 channel, Uint16 addr, Uint8 flags,
                        Int32 s0, Int32 s1, Int32 s14, Int32 s15)
{
    Uint8 bit;
    Uint32 p0, p1;

    if (!g_AuroraTraceEnabled || channel < 0 || channel >= 8) return;
    bit = (Uint8)(1u << (Uint32)channel);
    if (!(s_brr_inflight & bit)) return;

    p0 = ((Uint32)(Uint16)s0 << 16) | (Uint16)s1;
    p1 = ((Uint32)(Uint16)s14 << 16) | (Uint16)s15;
    AuroraTraceRecord(
        ATR_BRR,
        (Uint8)(ATR_F_POST |
                ((channel & 7) << 2) |
                ((flags & 3) << 5)),
        addr, p0, p1, ATR_P_FLUSH);

    s_brr_inflight &= (Uint8)~bit;
    s_brr_armed &= (Uint8)~bit;
}

void AuroraRuntimeTraceSPC(struct SNSpc_t *c, Uint16 pc, Uint8 op)
{
    Uint32 w;
    volatile AuroraSPCTraceEntryT *e;

    if (!g_AuroraTraceEnabled || !c) return;

    w = s_spc_write++;
    e = &s_spc[w & ATR_RING_MASK];
    e->seq = nextseq();
    e->pc = pc;
    e->opcode = op;
    e->halt = c->Regs.uPad;
    e->total = SNSPCGetCounter(c, SNSPC_COUNTER_TOTAL);
    e->frame = SNSPCGetCounter(c, SNSPC_COUNTER_FRAME);
    e->cycles = c->Cycles;
    e->a = c->Regs.rA;
    e->x = c->Regs.rX;
    e->y = c->Regs.rY;
    e->sp = c->Regs.rSP;
    e->psw = c->Regs.rPSW;
    e->f2 = c->Mem[0xF2];
    e->f3 = c->Mem[0xF3];
    e->rom = c->bRomEnable ? 1u : 0u;
    e->port0 = c->Mem[0xF4];
    e->port1 = c->Mem[0xF5];
    e->port2 = c->Mem[0xF6];
    e->port3 = c->Mem[0xF7];
}

#else

int g_AuroraRuntimeTraceDisabled = 0;

#endif
