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
#ifndef VIDEO_ABGR1555
#define VIDEO_ABGR1555 1
#define AURORA_UNDEF_VIDEO_ABGR1555 1
#endif
#include "gambatte.h"
#ifdef AURORA_UNDEF_VIDEO_ABGR1555
#undef VIDEO_ABGR1555
#undef AURORA_UNDEF_VIDEO_ABGR1555
#endif
#ifdef AURORA_UNDEF_HAVE_STDINT_H
#undef HAVE_STDINT_H
#undef AURORA_UNDEF_HAVE_STDINT_H
#endif

extern "C" void AuroraSgbBootTrace(const char *pText);

static const Uint32 GBHOST_STATE_MAGIC = 0x424D4147U; /* "GAMB" LE */
static const Uint32 GBHOST_STATE_VERSION = 4U; /* AURORA_SGB_GAMBATTE_XPOS168_RASTER_V1_2_1_20260907 */
static const Uint32 AUDIO_FRAMES = 4096U;
static const Uint32 AUDIO_SCRATCH_FRAMES = 4096U;
static const Uint32 AUDIO_DECIMATE = 64U; /* raw 1/2 clocks -> FIFO 1/128 */
static const Uint32 LCD_LINE_CLOCKS = 456U;
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
          romBytes(0), romCRC(0), clockCredit(0),
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

static Uint8 AuroraGambatteDecodeShade(gambatte::video_pixel_t pixel)
{
    /* AURORA_SGB_GAMBATTE_DIRECT_SHADE_V1_2_2_20260907
     *
     * SGB ICD2 consumes the final DMG shade after BGP/OBP mapping.
     * Gambatte's DMG base colors are programmed to literal 0,1,2,3,
     * so the composited framebuffer itself is the required 2-bit signal. */
    return (Uint8)((Uint16)pixel & 3U);
}

extern "C" void AuroraGambatteSgbNewLy(unsigned line)
{
    GBHost::Impl *p = g_AuroraGambatteRasterHost;

    if (!p || !p->loaded || line >= SNSGBICD2::LCD_TOTAL_LINES)
        return;

    /* AURORA_SGB_GAMBATTE_BSNESPLUS_VIDEO_V1_2_4_20260907
     * bsnes-plus does not feed ICD2 one line at a time. Gambatte keeps a
     * complete 160x144 framebuffer; when LY enters a new 8-line row, ICD2
     * publishes the PREVIOUS completed row into its four-slot ring. */
    if (p->icd2FastPath)
        p->icd2FastPath->GambatteNewLy(
            line, (const Uint16 *)p->screen);
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
    Uint32 i;

    for (i = 0; i < nRawFrames; ++i)
    {
        Uint32 packed = (Uint32)p->audioScratch[i];
        Int16 left = (Int16)(packed & 0xffffU);
        Int16 right = (Int16)((packed >> 16) & 0xffffU);

        p->audioDecimLeft += (Int32)left;
        p->audioDecimRight += (Int32)right;
        ++p->audioDecimCount;

        if (p->audioDecimCount >= AUDIO_DECIMATE)
        {
            AuroraGambattePushAudio(
                p,
                (Int16)(p->audioDecimLeft / (Int32)AUDIO_DECIMATE),
                (Int16)(p->audioDecimRight / (Int32)AUDIO_DECIMATE));
            p->audioDecimCount = 0;
            p->audioDecimLeft = 0;
            p->audioDecimRight = 0;
        }
    }
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

    if (m_p->gb.load(
            pData, (unsigned)nBytes, gambatte::GB::FORCE_DMG) != 0)
    {
        m_p->loaded = FALSE;
        return FALSE;
    }

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
    m_p->gb.setSgbVideoBuffer(m_p->screen, SNSGBICD2::LCD_WIDTH);

    m_p->clockCredit = 0;
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

Uint32 GBHost::RunClocks(Uint32 nTargetClocks)
{
    Uint64 target, credit, need, advanced = 0;
    Uint32 guard = 0;

    if (!m_p || !m_p->loaded || !nTargetClocks)
        return 0;

    target = nTargetClocks;
    credit = m_p->clockCredit > 0 ? (Uint64)m_p->clockCredit : 0;

    if (credit >= target)
    {
        m_p->clockCredit = (Int64)(credit - target);
        return nTargetClocks;
    }

    need = target - credit;
    m_p->clockCredit = 0;

    while (advanced < need && guard++ < 0x10000U)
    {
        Uint64 left = need - advanced;
        unsigned long request =
            left > 4096ULL ? 4096UL : (unsigned long)left;

        /* AURORA_SGB_GAMBATTE_VIDEO_PERF_FIX_V1_1_3_20260907
         * Gambatte's normal API is designed for much larger chunks than
         * Aurora's ~52-clock grant. One GB line is a conservative batch:
         * large enough to cut call overhead ~9x, bounded to ~106 us lead. */
        if (request < LCD_LINE_CLOCKS)
            request = LCD_LINE_CLOCKS;

        unsigned samples = 0;
        unsigned long step;

        /* V1.1: DO NOT clear audioScratch here. Gambatte setSoundBuffer()
           resets its write position and fillSoundBuffer() returns the exact
           initialized count. The V1 memset would clear 16 KiB per grant. */
        step = m_p->gb.runForClocks(
            m_p->screen, SNSGBICD2::LCD_WIDTH,
            m_p->audioScratch, AUDIO_SCRATCH_FRAMES,
            request, samples);

        if (!step)
            break;

        if (samples > AUDIO_SCRATCH_FRAMES)
            samples = AUDIO_SCRATCH_FRAMES;

        AuroraGambatteConsumeAudio(m_p, (Uint32)samples);
        /* AURORA_SGB_GAMBATTE_XPOS168_RASTER_V1_2_1_20260907
         * Video/line phase is emitted inside Gambatte at real LY_COUNT. */
        advanced += (Uint64)step;
    }

    if (credit + advanced >= target)
    {
        m_p->clockCredit = (Int64)(credit + advanced - target);
        return nTargetClocks;
    }

    return (Uint32)(credit + advanced);
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
    pState->LCDClock = 0; /* V1.2: retired synthetic raster fields */
    pState->LCDLine = 0;
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
    m_p->gb.setSgbVideoBuffer(m_p->screen, SNSGBICD2::LCD_WIDTH);
    m_p->clockCredit = pState->ClockCredit;
    m_p->lcdClock = 0;
    m_p->lcdLine = 0;
    g_AuroraGambatteRasterHost = m_p;
    ClearAudio();
    m_p->gb.clearSavedataDirty();
    return TRUE;
}
