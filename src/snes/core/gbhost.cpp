#include <string.h>
#include <new>

#include "gbhost.h"
#include "snsgb_icd2.h"

/* AURORA_SGB_GAMBATTE_BACKEND_V1_1_20260907
 * Match the staged Gambatte PS2 archive's public-header ABI locally. */
#ifndef HAVE_STDINT_H
#define HAVE_STDINT_H 1
#define AURORA_UNDEF_HAVE_STDINT_H 1
#endif
#ifndef VIDEO_SGB_SHADE8
#define VIDEO_SGB_SHADE8 1
#define AURORA_UNDEF_VIDEO_SGB_SHADE8 1
#endif
#include "gambatte.h"
#ifdef AURORA_UNDEF_VIDEO_SGB_SHADE8
#undef VIDEO_SGB_SHADE8
#undef AURORA_UNDEF_VIDEO_SGB_SHADE8
#endif
#ifdef AURORA_UNDEF_HAVE_STDINT_H
#undef HAVE_STDINT_H
#undef AURORA_UNDEF_HAVE_STDINT_H
#endif

extern "C" void AuroraSgbBootTrace(const char *pText);

static const Uint32 GBHOST_STATE_MAGIC = 0x424D4147U; /* "GAMB" LE */
static const Uint32 GBHOST_STATE_VERSION = 5U; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */
static const Uint32 AUDIO_FRAMES = 4096U;
/* AURORA_SGB_GAMBATTE_SHADE8_JOYP_SYNC_PERF_V3_20260908
 * 24576 GB clocks normally produce 12288 raw stereo frames. The 16K scratch
 * leaves generous instruction/event headroom while cutting runForClocks()
 * call frequency by ~6x versus V2's 4096-clock batch. */
static const Uint32 AUDIO_SCRATCH_FRAMES = 16384U;
static const Uint32 AUDIO_DECIMATE = 64U; /* raw 1/2 clocks -> FIFO 1/128 */
static const Uint32 RUN_BATCH_CLOCKS = 24576U;
static const Uint32 SGB1_CLOCK_HZ = 4295454U;
static const Uint32 SGB2_CLOCK_HZ = 4194304U;


static Uint32 AuroraGambatteCRC32(const Uint8 *pData, Uint32 nBytes)
{
    Uint32 crc = 0xffffffffU;
    Uint32 i, b;
    for (i = 0; i < nBytes; ++i)
    {
        crc ^= pData[i];
        for (b = 0; b < 8; ++b)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

struct GBHost::Impl
{
    gambatte::GB gb;
    Bool initialized;
    Bool loaded;
    ModelE model;

    Uint32 romBytes;
    Uint32 romCRC;
    Int64 clockCredit;
    Uint32 pendingClocks; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */

    JoypHookT joypHook;
    PixelHookT pixelHook;
    ResetHookT hresetHook;
    ResetHookT vresetHook;
    void *hookContext;
    SNSGBICD2 *icd2FastPath;

    gambatte::video_pixel_t screen[SNSGBICD2::LCD_WIDTH *
                                   SNSGBICD2::LCD_VISIBLE_LINES];

    Uint32 lcdClock;
    Int32 lcdLine;

    gambatte::uint_least32_t audioScratch[AUDIO_SCRATCH_FRAMES];
    Int16 audio[AUDIO_FRAMES * 2U];
    Uint32 audioRead;
    Uint32 audioWrite;
    Uint32 audioCount;
    Uint32 audioDecimCount;
    Int32 audioDecimLeft;
    Int32 audioDecimRight;

    Impl()
        : initialized(TRUE), loaded(FALSE), model(MODEL_SGB1),
          romBytes(0), romCRC(0), clockCredit(0), pendingClocks(0),
          joypHook(NULL), pixelHook(NULL),
          hresetHook(NULL), vresetHook(NULL), hookContext(NULL),
          icd2FastPath(NULL),
          lcdClock(0), lcdLine(0),
          audioRead(0), audioWrite(0), audioCount(0),
          audioDecimCount(0), audioDecimLeft(0), audioDecimRight(0)
    {
        Uint32 i;
        for (i = 0;
             i < (Uint32)(SNSGBICD2::LCD_WIDTH * SNSGBICD2::LCD_VISIBLE_LINES);
             ++i)
            screen[i] = (gambatte::video_pixel_t)0U;
        memset(audio, 0, sizeof(audio));
    }
};

/* AURORA_SGB_GAMBATTE_XPOS168_RASTER_V1_2_1_20260907
 * Aurora has one active SGB GBHost. Keep the staged callback ABI tiny and
 * avoid reintroducing a generic libretro frontend. */
static GBHost::Impl *g_AuroraGambatteRasterHost = NULL;

extern "C" void AuroraGambatteSgbNewLy(unsigned line)
{
    GBHost::Impl *p = g_AuroraGambatteRasterHost;

    if (!p || !p->loaded || line >= SNSGBICD2::LCD_TOTAL_LINES)
        return;

    /* AURORA_SGB_GAMBATTE_SHADE8_JOYP_SYNC_PERF_V3_20260908
     * video_pixel_t is one literal final DMG shade byte in this PS2-only
     * Gambatte build. No RGB16 reinterpretation/conversion is involved. */
    if (p->icd2FastPath)
        p->icd2FastPath->GambatteNewLy(
            line, (const Uint8 *)p->screen);
}

GBHost::GBHost() : m_p(NULL) {}
GBHost::~GBHost() { Shutdown(); }

Bool GBHost::Init()
{
    if (m_p && m_p->initialized)
        return TRUE;

    if (m_p)
        Shutdown();

    m_p = new (std::nothrow) Impl;
    if (!m_p)
        return FALSE;

    AuroraSgbBootTrace("SGB GB1: Gambatte host ready");
    return TRUE;
}

void GBHost::Shutdown()
{
    if (!m_p)
        return;

    if (g_AuroraGambatteRasterHost == m_p)
        g_AuroraGambatteRasterHost = NULL;

    delete m_p;
    m_p = NULL;
}

Bool GBHost::IsInitialized() const
{
    return m_p && m_p->initialized ? TRUE : FALSE;
}

Bool GBHost::IsLoaded() const
{
    return m_p && m_p->loaded ? TRUE : FALSE;
}

unsigned char GBHost::GambatteJoypCallback(
    void *pOpaque, unsigned char p14p15, bool bWrite)
{
    Impl *p = (Impl *)pOpaque;
    Bool p14, p15;

    if (!p)
        return 0x0fU;

    p14 = (p14p15 & 0x10U) ? TRUE : FALSE;
    p15 = (p14p15 & 0x20U) ? TRUE : FALSE;

    if (p->joypHook)
        return (unsigned char)(p->joypHook(
            p->hookContext, p14, p15, bWrite ? TRUE : FALSE) & 0x0fU);

    return 0x0fU;
}

static void AuroraGambattePushAudio(GBHost::Impl *p, Int16 left, Int16 right)
{
    Uint32 w;

    if (p->audioCount >= AUDIO_FRAMES)
    {
        p->audioRead = (p->audioRead + 1U) & (AUDIO_FRAMES - 1U);
        --p->audioCount;
    }

    w = p->audioWrite;
    p->audio[w * 2U + 0U] = left;
    p->audio[w * 2U + 1U] = right;
    p->audioWrite = (w + 1U) & (AUDIO_FRAMES - 1U);
    ++p->audioCount;
}

static void AuroraGambatteConsumeAudio(GBHost::Impl *p, Uint32 nRawFrames)
{
    /* AURORA_SGB_GAMBATTE_SHADE8_JOYP_SYNC_PERF_V3_20260908
     * Exact same 64-sample box filter as before, but operate on complete
     * groups. V2 updated member accumulators and tested the threshold for
     * every ~2.1 MHz raw sample. */
    const gambatte::uint_least32_t *src = p->audioScratch;
    Uint32 leftFrames = nRawFrames;
    Uint32 count = p->audioDecimCount;
    Int32 sumLeft = p->audioDecimLeft;
    Int32 sumRight = p->audioDecimRight;
    Uint32 i;

    if (count && leftFrames)
    {
        Uint32 take = AUDIO_DECIMATE - count;
        if (take > leftFrames) take = leftFrames;

        for (i = 0; i < take; ++i)
        {
            Uint32 packed = (Uint32)*src++;
            sumLeft += (Int32)(Int16)(packed & 0xffffU);
            sumRight += (Int32)(Int16)((packed >> 16) & 0xffffU);
        }

        count += take;
        leftFrames -= take;
        if (count == AUDIO_DECIMATE)
        {
            AuroraGambattePushAudio(
                p,
                (Int16)(sumLeft / (Int32)AUDIO_DECIMATE),
                (Int16)(sumRight / (Int32)AUDIO_DECIMATE));
            count = 0;
            sumLeft = 0;
            sumRight = 0;
        }
    }

    while (leftFrames >= AUDIO_DECIMATE)
    {
        Int32 blockLeft = 0;
        Int32 blockRight = 0;

        for (i = 0; i < AUDIO_DECIMATE; ++i)
        {
            Uint32 packed = (Uint32)src[i];
            blockLeft += (Int32)(Int16)(packed & 0xffffU);
            blockRight += (Int32)(Int16)((packed >> 16) & 0xffffU);
        }

        AuroraGambattePushAudio(
            p,
            (Int16)(blockLeft / (Int32)AUDIO_DECIMATE),
            (Int16)(blockRight / (Int32)AUDIO_DECIMATE));
        src += AUDIO_DECIMATE;
        leftFrames -= AUDIO_DECIMATE;
    }

    while (leftFrames--)
    {
        Uint32 packed = (Uint32)*src++;
        sumLeft += (Int32)(Int16)(packed & 0xffffU);
        sumRight += (Int32)(Int16)((packed >> 16) & 0xffffU);
        ++count;
    }

    p->audioDecimCount = count;
    p->audioDecimLeft = sumLeft;
    p->audioDecimRight = sumRight;
}


Bool GBHost::LoadROM(const Uint8 *pData, Uint32 nBytes, ModelE eModel)
{
    Int32 pal, shade;
    /* AURORA_SGB_GAMBATTE_DIRECT_SHADE_V1_2_2_20260907
     * These are intentionally not display colors. Gambatte applies BGP/OBP
     * to select among them, yielding the exact final SGB shade 0..3. */
    static const Uint32 s_DmgShade[4] = { 0U, 1U, 2U, 3U };

    if (!pData || nBytes < 0x150U)
        return FALSE;

    if (!Init())
        return FALSE;

    m_p->model = (eModel == MODEL_SGB2) ? MODEL_SGB2 : MODEL_SGB1;

    /* Aurora HLE supplies the SGB header handshake; Gambatte starts post-boot. */
    m_p->gb.setBootloaderGetter(NULL);
    m_p->gb.setSgbJoypCallback(&GBHost::GambatteJoypCallback, m_p);
    m_p->gb.setScanlineCallback(&AuroraGambatteSgbNewLy); /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */

    if (m_p->gb.load(
            pData, (unsigned)nBytes, gambatte::GB::FORCE_DMG) != 0)
    {
        m_p->loaded = FALSE;
        return FALSE;
    }

    /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907
     * HLE of the SGB bootstrap must leave the same accumulator value as the
     * real SGB1/SGB2 bootstrap before cartridge execution begins. */
    m_p->gb.setSgbPostBootState(m_p->model == MODEL_SGB2);

    /* AURORA_SGB_GAMBATTE_VIDEO_PERF_FIX_V1_1_3_20260907
     * Attach once after load/full_init. Never reset the active fbline_ on every
     * small RunClocks() grant. */
    m_p->gb.setSgbVideoBuffer(m_p->screen, SNSGBICD2::LCD_WIDTH);

    /* Preserve DMG shade identity. SGB color attributes live on SNES side. */
    for (pal = 0; pal < 3; ++pal)
        for (shade = 0; shade < 4; ++shade)
            m_p->gb.setDmgPaletteColor(
                (unsigned)pal, (unsigned)shade, s_DmgShade[shade]);

    m_p->romBytes = nBytes;
    m_p->romCRC = AuroraGambatteCRC32(pData, nBytes);
    m_p->clockCredit = 0;
    m_p->pendingClocks = 0; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */
    m_p->lcdClock = 0;
    m_p->lcdLine = 0;
    m_p->audioRead = m_p->audioWrite = m_p->audioCount = 0;
    m_p->audioDecimCount = 0;
    m_p->audioDecimLeft = 0;
    m_p->audioDecimRight = 0;
    m_p->loaded = TRUE;
    g_AuroraGambatteRasterHost = m_p;
    m_p->gb.clearSavedataDirty();

    AuroraSgbBootTrace(
        m_p->model == MODEL_SGB2
            ? "SGB GB2: Gambatte SGB2 boot"
            : "SGB GB2: Gambatte SGB1 boot");
    return TRUE;
}

void GBHost::UnloadROM()
{
    if (!m_p)
        return;

    if (g_AuroraGambatteRasterHost == m_p)
        g_AuroraGambatteRasterHost = NULL;

    m_p->loaded = FALSE;
    m_p->romBytes = 0;
    m_p->romCRC = 0;
    m_p->clockCredit = 0;
    m_p->pendingClocks = 0; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */
    m_p->lcdClock = 0;
    m_p->lcdLine = 0;
    ClearAudio();
}

void GBHost::Reset(ModelE eModel)
{
    if (!m_p || !m_p->loaded)
        return;

    m_p->model = (eModel == MODEL_SGB2) ? MODEL_SGB2 : MODEL_SGB1;
    m_p->gb.reset();
    m_p->gb.setSgbPostBootState(m_p->model == MODEL_SGB2);
    m_p->gb.setScanlineCallback(&AuroraGambatteSgbNewLy);
    m_p->gb.setSgbVideoBuffer(m_p->screen, SNSGBICD2::LCD_WIDTH);

    m_p->clockCredit = 0;
    m_p->pendingClocks = 0; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */
    m_p->lcdClock = 0;
    m_p->lcdLine = 0;
    ClearAudio();
}

Bool GBHost::AttachSavedata(const Uint8 *pData, Uint32 nBytes)
{
    Uint8 *dst;
    Uint32 sramBytes, rtcBytes, pos = 0, n;

    if (!m_p || !m_p->loaded || (nBytes && !pData))
        return FALSE;

    sramBytes = (Uint32)m_p->gb.savedata_size();
    rtcBytes = (Uint32)m_p->gb.rtcdata_size();

    dst = (Uint8 *)m_p->gb.savedata_ptr();
    if (dst && sramBytes && pos < nBytes)
    {
        n = nBytes - pos;
        if (n > sramBytes) n = sramBytes;
        memcpy(dst, pData + pos, n);
        pos += n;
    }

    dst = (Uint8 *)m_p->gb.rtcdata_ptr();
    if (dst && rtcBytes && pos < nBytes)
    {
        n = nBytes - pos;
        if (n > rtcBytes) n = rtcBytes;
        memcpy(dst, pData + pos, n);
        pos += n;
    }

    m_p->gb.clearSavedataDirty();
    return TRUE;
}

Uint32 GBHost::GetSavedataBytes()
{
    if (!m_p || !m_p->loaded)
        return 0;
    return (Uint32)m_p->gb.savedata_size() +
           (Uint32)m_p->gb.rtcdata_size();
}

Bool GBHost::ExportSavedata(
    Uint8 *pData, Uint32 nCapacity, Uint32 *pActualBytes)
{
    const Uint8 *src;
    Uint32 sramBytes, rtcBytes, total, pos = 0;

    if (pActualBytes)
        *pActualBytes = 0;

    if (!m_p || !m_p->loaded)
        return FALSE;

    sramBytes = (Uint32)m_p->gb.savedata_size();
    rtcBytes = (Uint32)m_p->gb.rtcdata_size();
    total = sramBytes + rtcBytes;

    if (pActualBytes)
        *pActualBytes = total;

    if (!total)
        return TRUE;
    if (!pData || nCapacity < total)
        return FALSE;

    src = (const Uint8 *)m_p->gb.savedata_ptr();
    if (src && sramBytes)
    {
        memcpy(pData + pos, src, sramBytes);
        pos += sramBytes;
    }

    src = (const Uint8 *)m_p->gb.rtcdata_ptr();
    if (src && rtcBytes)
    {
        memcpy(pData + pos, src, rtcBytes);
        pos += rtcBytes;
    }

    return pos == total ? TRUE : FALSE;
}

Bool GBHost::SavedataDirty() const
{
    return m_p && m_p->loaded && m_p->gb.savedataDirty()
        ? TRUE : FALSE;
}

void GBHost::ClearSavedataDirty()
{
    if (m_p && m_p->loaded)
        m_p->gb.clearSavedataDirty();
}

/* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907
 * Execute only clocks that the SFC has already granted.
 *
 * V1.1.3 forced every tiny grant up to 456 clocks and carried the overshoot as
 * credit. That reduced call overhead, but LY callbacks and ICD2 ring writes
 * became externally visible up to one GB scanline BEFORE SFC time reached
 * them. The new scheduler batches in the opposite direction: time may remain
 * pending internally, but it never runs ahead. Any SGB MMIO/audio observation
 * calls FlushClocks() first.
 */
static Uint32 AuroraGambatteDrainPending(GBHost::Impl *p)
{
    Uint64 target, credit, need, advanced = 0;
    Uint32 guard = 0;

    if (!p || !p->loaded || !p->pendingClocks)
        return 0;

    target = (Uint64)p->pendingClocks;
    p->pendingClocks = 0;

    credit = p->clockCredit > 0 ? (Uint64)p->clockCredit : 0;
    if (credit >= target)
    {
        p->clockCredit = (Int64)(credit - target);
        return (Uint32)target;
    }

    need = target - credit;
    p->clockCredit = 0;

    while (advanced < need && guard++ < 0x10000U)
    {
        Uint64 left = need - advanced;
        unsigned long request =
            left > (Uint64)RUN_BATCH_CLOCKS
                ? (unsigned long)RUN_BATCH_CLOCKS
                : (unsigned long)left;
        unsigned samples = 0;
        unsigned long step;

        step = p->gb.runForClocks(
            p->screen, SNSGBICD2::LCD_WIDTH,
            p->audioScratch, AUDIO_SCRATCH_FRAMES,
            request, samples);

        if (!step)
            break;

        if (samples > AUDIO_SCRATCH_FRAMES)
            samples = AUDIO_SCRATCH_FRAMES;
        AuroraGambatteConsumeAudio(p, (Uint32)samples);
        advanced += (Uint64)step;
    }

    if (credit + advanced >= target)
    {
        p->clockCredit = (Int64)(credit + advanced - target);
        return (Uint32)target;
    }

    p->pendingClocks = (Uint32)(target - (credit + advanced));
    return (Uint32)(credit + advanced);
}

Uint32 GBHost::RunClocks(Uint32 nTargetClocks)
{
    if (!m_p || !m_p->loaded || !nTargetClocks)
        return 0;

    if (m_p->pendingClocks > 0xffffffffU - nTargetClocks)
        (void)AuroraGambatteDrainPending(m_p);

    m_p->pendingClocks += nTargetClocks;

    if (m_p->pendingClocks >= RUN_BATCH_CLOCKS)
        (void)AuroraGambatteDrainPending(m_p);

    return nTargetClocks;
}

void GBHost::FlushClocks()
{
    if (m_p && m_p->loaded)
        (void)AuroraGambatteDrainPending(m_p);
}

Uint32 GBHost::DebugPreTickState() const { return 0; }
const char *GBHost::DebugPreEventName() const { return "Gambatte runForClocks"; }
Int32 GBHost::DebugPreEventDelta() const { return 0; }
Bool GBHost::DebugSkipDueAudioSample() { return FALSE; }

Uint32 GBHost::GetClockHz() const
{
    if (!m_p || !m_p->loaded)
        return 0;
    return m_p->model == MODEL_SGB2 ? SGB2_CLOCK_HZ : SGB1_CLOCK_HZ;
}

Int64 GBHost::GetClockCredit() const
{
    return m_p ? m_p->clockCredit : 0;
}

Uint32 GBHost::GetROMBytes() const
{
    return m_p ? m_p->romBytes : 0;
}

Uint32 GBHost::GetROMCRC() const
{
    return m_p ? m_p->romCRC : 0;
}

Uint32 GBHost::ReadAudioFrames(
    Int16 *pStereoInterleaved, Uint32 nFrames)
{
    Uint32 out = 0;

    if (!m_p || !pStereoInterleaved)
        return 0;

    while (out < nFrames && m_p->audioCount)
    {
        Uint32 r = m_p->audioRead;
        pStereoInterleaved[out * 2U + 0U] = m_p->audio[r * 2U + 0U];
        pStereoInterleaved[out * 2U + 1U] = m_p->audio[r * 2U + 1U];
        m_p->audioRead = (r + 1U) & (AUDIO_FRAMES - 1U);
        --m_p->audioCount;
        ++out;
    }

    return out;
}

void GBHost::ClearAudio()
{
    if (!m_p)
        return;

    m_p->audioRead = 0;
    m_p->audioWrite = 0;
    m_p->audioCount = 0;
    m_p->audioDecimCount = 0;
    m_p->audioDecimLeft = 0;
    m_p->audioDecimRight = 0;
}

void GBHost::SetHooks(
    JoypHookT pJoyp, PixelHookT pPixel,
    ResetHookT pHReset, ResetHookT pVReset,
    void *pContext)
{
    if (!m_p)
        return;

    m_p->joypHook = pJoyp;
    m_p->pixelHook = pPixel;
    m_p->hresetHook = pHReset;
    m_p->vresetHook = pVReset;
    m_p->hookContext = pContext;
}

void GBHost::SetICD2FastPath(SNSGBICD2 *pICD2)
{
    if (m_p)
        m_p->icd2FastPath = pICD2;
}

Bool GBHost::SaveState(StateT *pState) const
{
    size_t n;

    if (!m_p || !m_p->loaded || !pState)
        return FALSE;

    n = m_p->gb.stateSize();
    if (!n || n > SERIALIZED_BYTES)
        return FALSE;

    memset(pState, 0, sizeof(*pState));
    pState->Magic = GBHOST_STATE_MAGIC;
    pState->Version = GBHOST_STATE_VERSION;
    pState->Model = (Uint32)m_p->model;
    pState->Reserved = (Uint32)n;
    pState->ClockCredit = m_p->clockCredit;
    pState->PendingClocks = m_p->pendingClocks;
    pState->Reserved2 = 0; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */
    m_p->gb.saveState(pState->Serialized);
    return TRUE;
}

Bool GBHost::RestoreState(const StateT *pState)
{
    if (!m_p || !m_p->loaded || !pState)
        return FALSE;

    if (pState->Magic != GBHOST_STATE_MAGIC ||
        pState->Version != GBHOST_STATE_VERSION ||
        pState->Reserved == 0 ||
        pState->Reserved > SERIALIZED_BYTES)
        return FALSE;

    if (!m_p->gb.loadState(
            pState->Serialized, (size_t)pState->Reserved))
        return FALSE;

    m_p->model =
        pState->Model == MODEL_SGB2 ? MODEL_SGB2 : MODEL_SGB1;
    m_p->gb.setScanlineCallback(&AuroraGambatteSgbNewLy);
    m_p->gb.setSgbVideoBuffer(m_p->screen, SNSGBICD2::LCD_WIDTH);
    m_p->clockCredit = pState->ClockCredit;
    m_p->pendingClocks = pState->PendingClocks;
    m_p->lcdClock = 0;
    m_p->lcdLine = 0;
    g_AuroraGambatteRasterHost = m_p;
    ClearAudio();
    m_p->gb.clearSavedataDirty();
    return TRUE;
}
