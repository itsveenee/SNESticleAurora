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
#include "gambatte.h"

/* AURORA_SGB_CLASSIC_RGB32_V1_20260908
 * Keep the host ABI identical to the staged Gambatte archive. The historical
 * bsnes-plus/classic SGB bridge consumes Gambatte's normal 32-bit grayscale
 * framebuffer and converts it to ICD2 bitplanes only at the SGB boundary. */
#ifdef VIDEO_SGB_SHADE8
#error "Aurora SGB classic RGB32 host must not be built with VIDEO_SGB_SHADE8"
#endif
typedef char AuroraSgbVideoPixelMustBe32Bit[
    (sizeof(gambatte::video_pixel_t) == 4U) ? 1 : -1];

#ifdef AURORA_UNDEF_HAVE_STDINT_H
#undef HAVE_STDINT_H
#undef AURORA_UNDEF_HAVE_STDINT_H
#endif

extern "C" void AuroraSgbBootTrace(const char *pText);

static const Uint32 GBHOST_STATE_MAGIC = 0x424D4147U; /* "GAMB" LE */
static const Uint32 GBHOST_STATE_VERSION = 6U; /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908: real-boot state ABI */
static const Uint32 AUDIO_FRAMES = 4096U;
/* AURORA_SGB_GAMBATTE_SHADE8_JOYP_SYNC_PERF_V3_20260908
 * 24576 GB clocks normally produce 12288 raw stereo frames. The 16K scratch
 * leaves generous instruction/event headroom while cutting runForClocks()
 * call frequency by ~6x versus V2's 4096-clock batch. */
/* AURORA_V6_RUNTIME_EFFECT_ALL5_20260908
 * Gambatte V6 now performs the exact 64:1 box decimation while converting
 * PSG deltas to absolute samples.  Scratch still has to hold the RAW delta
 * stream while the CPU runs, hence 32768 entries for a 49152-clock batch. */
static const Uint32 AUDIO_SCRATCH_FRAMES = 32768U;
static const Uint32 RUN_BATCH_CLOCKS = 49152U; /* 64:1 audio contract remains inside Gambatte */
/* AURORA_GB_HOTFIX_R13F_STANDALONE_BOOT_AUDIO_Y_20260909_SGB_CLEANUP: standalone R13D follow-up does not retime/filter SGB. */
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
    Uint8 bootRom[0x100];
    Bool realBootRom; /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */

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
          realBootRom(FALSE), /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */
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
        memset(bootRom, 0, sizeof(bootRom)); /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */
    }
};

/* AURORA_SGB_CLASSIC_RGB32_V1_20260908
 * Match bsnes-plus/classic Gambatte at the video boundary: the DMG palette
 * is neutral 0xFFFFFF / 0xAAAAAA / 0x555555 / 0x000000 RGB32.
 * SGB colourization remains entirely on the SNES side. */
static void AuroraGambatteApplySgbShadePalette(GBHost::Impl *p)
{
    static const Uint32 s_DmgShade[4] = {
        0x00ffffffU, 0x00aaaaaaU, 0x00555555U, 0x00000000U
    }; /* AURORA_SGB_CLASSIC_RGB32_V1_20260908 */
    Uint32 pal, shade;

    if (!p)
        return;

    for (pal = 0; pal < 3U; ++pal)
        for (shade = 0; shade < 4U; ++shade)
            p->gb.setDmgPaletteColor(pal, shade, s_DmgShade[shade]);
}

/* AURORA_SGB_GAMBATTE_XPOS168_RASTER_V1_2_1_20260907
 * Aurora has one active SGB GBHost. Keep the staged callback ABI tiny and
 * avoid reintroducing a generic libretro frontend. */
static GBHost::Impl *g_AuroraGambatteRasterHost = NULL;

/* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908
 * Gambatte's BootloaderGetter receives its internal Bootloader pointer, not
 * caller userdata. Aurora has one active SGB, so bind that one host here. */
static GBHost::Impl *g_AuroraGambatteBootHost = NULL;

static bool AuroraGambatteSgbBootloaderGetter(
    void *ignored, bool isgbc, uint8_t *data, uint32_t bytes)
{
    GBHost::Impl *p = g_AuroraGambatteBootHost;
    (void)ignored;
    if (!p || !p->realBootRom || isgbc || !data || bytes < 0x100U)
        return false;
    memcpy(data, p->bootRom, 0x100U);
    return true;
}

extern "C" void AuroraGambatteSgbNewLy(unsigned line)
{
    GBHost::Impl *p = g_AuroraGambatteRasterHost;

    if (!p || !p->loaded || line >= SNSGBICD2::LCD_TOTAL_LINES)
        return;

    /* AURORA_SGB_CLASSIC_RGB32_V1_20260908
     * Same data boundary as bsnes-plus/classic: Gambatte draws normal RGB32;
     * ICD2 converts the previous completed 8-line row to 2bpp. */
    if (p->icd2FastPath)
        p->icd2FastPath->GambatteNewLy(
            line, (const Uint32 *)p->screen);
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
    if (g_AuroraGambatteBootHost == m_p)
        g_AuroraGambatteBootHost = NULL; /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */

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

Bool GBHost::HasRealBootROM() const
{
    return (m_p && m_p->loaded && m_p->realBootRom) ? TRUE : FALSE;
} /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */

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

static void AuroraGambatteConsumeAudio(GBHost::Impl *p, Uint32 nFrames)
{
    /* AURORA_V6_RUNTIME_EFFECT_ALL5_20260908
     * audioScratch already contains exact 64-raw-frame box averages emitted
     * in-place by Gambatte::PSG::fillBufferSgb64(). The SGB host only queues
     * those final frames; standalone GB audio quality belongs to GambatteSystem. */
    const gambatte::uint_least32_t *src = p->audioScratch;
    while (nFrames--)
    {
        Uint32 packed = (Uint32)*src++;
        AuroraGambattePushAudio(
            p,
            (Int16)(packed & 0xffffU),
            (Int16)((packed >> 16) & 0xffffU));
    }
}



Bool GBHost::LoadROM(
    const Uint8 *pData, Uint32 nBytes, ModelE eModel,
    const Uint8 *pBootRom, Uint32 nBootRomBytes)
{
    if (!pData || nBytes < 0x150U)
        return FALSE;

    if (!Init())
        return FALSE;

    m_p->model = (eModel == MODEL_SGB2) ? MODEL_SGB2 : MODEL_SGB1;
    if (!pBootRom || nBootRomBytes != 0x100U)
        return FALSE; /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908: sgb_bios.* is mandatory; never enter HLE. */
    m_p->realBootRom = TRUE;
    memcpy(m_p->bootRom, pBootRom, 0x100U);
    g_AuroraGambatteBootHost = m_p;
    m_p->gb.setBootloaderGetter(&AuroraGambatteSgbBootloaderGetter);

    /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908: SM83 boot ROM owns FF00/header protocol; HLE is unreachable. */
    m_p->gb.setSgbJoypCallback(&GBHost::GambatteJoypCallback, m_p);
    m_p->gb.setScanlineCallback(&AuroraGambatteSgbNewLy);

    if (m_p->gb.load(
            pData, (unsigned)nBytes, gambatte::GB::FORCE_DMG) != 0)
    {
        if (g_AuroraGambatteBootHost == m_p) g_AuroraGambatteBootHost = NULL;
        m_p->realBootRom = FALSE;
        memset(m_p->bootRom, 0, sizeof(m_p->bootRom));
        m_p->loaded = FALSE;
        return FALSE;
    }


    /* AURORA_SGB_GAMBATTE_VIDEO_PERF_FIX_V1_1_3_20260907
     * Attach once after load/full_init. Never reset the active fbline_ on every
     * small RunClocks() grant. */
    m_p->gb.setSgbVideoBuffer(m_p->screen, SNSGBICD2::LCD_WIDTH);

    /* Preserve neutral classic DMG grayscale; SGB color attributes are SNES-side. */
    AuroraGambatteApplySgbShadePalette(m_p);

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
    if (g_AuroraGambatteBootHost == m_p)
        g_AuroraGambatteBootHost = NULL; /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */

    m_p->realBootRom = FALSE;
    memset(m_p->bootRom, 0, sizeof(m_p->bootRom));
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

    /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908: mandatory sgb_bios.*; reset always executes the real boot ROM. */
    g_AuroraGambatteBootHost = m_p;
    m_p->gb.setBootloaderGetter(&AuroraGambatteSgbBootloaderGetter);
    m_p->gb.reset();

    /* AURORA_SGB_CLASSIC_RGB32_V1_20260908
     * full_init() behind GB::reset() restores ordinary DMG video state.
     * Reassert every external SGB binding and the classic grayscale contract
     * before the first post-reset pixel can reach ICD2. */
    m_p->gb.setSgbJoypCallback(&GBHost::GambatteJoypCallback, m_p);
    m_p->gb.setScanlineCallback(&AuroraGambatteSgbNewLy);
    m_p->gb.setSgbVideoBuffer(m_p->screen, SNSGBICD2::LCD_WIDTH);
    AuroraGambatteApplySgbShadePalette(m_p);
    memset(m_p->screen, 0, sizeof(m_p->screen));

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

        /* AURORA_V6_RUNTIME_EFFECT_ALL5_20260908:
         * one Gambatte pass converts + box-decimates SGB PSG. */
        step = p->gb.runForClocksSgb64(
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

    /* AURORA_V6_RUNTIME_EFFECT_ALL5_20260908:
     * transient 64:1 box phase lives inside Gambatte PSG. */
    m_p->gb.clearSgbAudioDecimator();
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
    pState->Reserved2 = m_p->realBootRom ? 1 : 0; /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */
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

    /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908: do not restore a real-boot state under HLE or vice versa. */
    if (pState->Reserved2 != 1U || !m_p->realBootRom)
        return FALSE;

    if (!m_p->gb.loadState(
            pState->Serialized, (size_t)pState->Reserved))
        return FALSE;

    m_p->model =
        pState->Model == MODEL_SGB2 ? MODEL_SGB2 : MODEL_SGB1;
    /* AURORA_V4_4_CUMULATIVE_20260908
     * Callbacks/pixel encoding are host policy, not savestate policy. */
    m_p->gb.setSgbJoypCallback(&GBHost::GambatteJoypCallback, m_p);
    m_p->gb.setScanlineCallback(&AuroraGambatteSgbNewLy);
    m_p->gb.setSgbVideoBuffer(m_p->screen, SNSGBICD2::LCD_WIDTH);
    AuroraGambatteApplySgbShadePalette(m_p);
    m_p->clockCredit = pState->ClockCredit;
    m_p->pendingClocks = pState->PendingClocks;
    m_p->lcdClock = 0;
    m_p->lcdLine = 0;
    g_AuroraGambatteRasterHost = m_p;
    g_AuroraGambatteBootHost = m_p;
    m_p->gb.setBootloaderGetter(&AuroraGambatteSgbBootloaderGetter); /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */
    ClearAudio();
    m_p->gb.clearSavedataDirty();
    return TRUE;
}
