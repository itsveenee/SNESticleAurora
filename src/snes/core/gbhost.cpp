#include <string.h>
#include <new>

#include "gbhost.h"
#include "snsgb_icd2.h"
#include "sameboy_sgb_boot.h"

extern "C" {
#include "../../third_party/sameboy/Core/gb.h"

/* AURORA_SGB_SCANLINE_BATCH_V4_1_20260907
 * Staged SameBoy helper: keep GB_gameboy_t internals on the C side.
 * gbhost.cpp only consumes an opaque row pointer + completed line index. */
void *AuroraSameBoyGetCompletedSGBScanline(GB_gameboy_t *gb, int *pLine);
}

extern "C" void AuroraSgbBootTrace(const char *pText);

static const Uint32 GBHOST_STATE_MAGIC = 0x42534753U; /* "SGSB" LE */
static const Uint32 GBHOST_STATE_VERSION = 1U;
static const Uint32 AUDIO_FRAMES = 4096U;

static Uint32 AuroraSameBoyCRC32(const Uint8 *pData, Uint32 nBytes)
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
    GB_gameboy_t gb;
    Bool gbInited;
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

    /* AURORA_SGB_HOTPATH_V2_20260907 */
    SNSGBICD2 *icd2FastPath;

    Uint32 *screen;

    Int16 audio[AUDIO_FRAMES * 2U];
    Uint32 audioRead;
    Uint32 audioWrite;
    Uint32 audioCount;
};

static GB_model_t AuroraSameBoyModel(GBHost::ModelE model)
{
    return model == GBHost::MODEL_SGB2
        ? GB_MODEL_SGB2_NO_SFC
        : GB_MODEL_SGB_NO_SFC;
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

    memset(m_p, 0, sizeof(*m_p));
    m_p->model = MODEL_SGB1;
    m_p->initialized = TRUE;
    AuroraSgbBootTrace("SGB SB1: SameBoy host ready");
    return TRUE;
}

void GBHost::Shutdown()
{
    if (!m_p)
        return;

    if (m_p->gbInited)
    {
        GB_free(&m_p->gb);
        m_p->gbInited = FALSE;
    }

    delete [] m_p->screen;
    m_p->screen = NULL;
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

void GBHost::BootRomThunk(void *pOpaque, Int32 eBootType)
{
    Impl *p = (Impl *)pOpaque;
    Uint8 boot[256];

    if (!p || !p->gbInited)
        return;

    memcpy(boot, g_AuroraSameBoySgbBoot, sizeof(boot));
    boot[253] = (eBootType == (Int32)GB_BOOT_ROM_SGB2) ? 0xffU : 0x01U;
    GB_load_boot_rom_from_buffer(&p->gb, boot, sizeof(boot));
}

void GBHost::JoypThunk(void *pOpaque, Uint8 value)
{
    Impl *p = (Impl *)pOpaque;
    Uint8 input = 0x0fU;
    Bool p14, p15;

    if (!p || !p->gbInited)
        return;

    p14 = (value & 0x10U) ? TRUE : FALSE;
    p15 = (value & 0x20U) ? TRUE : FALSE;

    if (p->joypHook)
        input = p->joypHook(p->hookContext, p14, p15, TRUE) & 0x0fU;

    GB_icd_set_joyp(&p->gb, input);
}

void GBHost::PixelThunk(void *pOpaque, Uint8 pixel)
{
    Impl *p = (Impl *)pOpaque;
    if (!p)
        return;

    /* AURORA_SGB_HOTPATH_V2_20260907
     * SameBoy NO_SFC already emits the exact ICD color stream. When this host
     * belongs to SNSuperGameBoy, bypass the extra function-pointer + wrapper
     * hop and feed ICD2 directly. */
    if (p->icd2FastPath)
    {
        p->icd2FastPath->PPUWrite(pixel & 3U);
        return;
    }

    if (p->pixelHook)
        p->pixelHook(p->hookContext, pixel & 3U);
}

/* AURORA_SGB_SCANLINE_BATCH_V4_20260907
 * SameBoy NO_SFC has already produced exactly 160 2-bit LCD pixel values.
 * Convert the completed scratch row to the existing ICD2 scanline helper.
 * No pixel is skipped and no GB/SNES clock is changed.
 */
void GBHost::LineThunk(void *pOpaque, const Uint32 *pPixels, Int32 nLine)
{
    Impl *p = (Impl *)pOpaque;
    Uint8 shade[SNSGBICD2::LCD_WIDTH];
    Int32 x;

    if (!p || !pPixels ||
        nLine < 0 || nLine >= SNSGBICD2::LCD_VISIBLE_LINES)
        return;

    for (x = 0; x < SNSGBICD2::LCD_WIDTH; ++x)
        shade[x] = (Uint8)(pPixels[x] & 3U);

    if (p->icd2FastPath)
    {
        p->icd2FastPath->PushLCDScanline(nLine, shade);
        return;
    }

    if (p->pixelHook)
    {
        for (x = 0; x < SNSGBICD2::LCD_WIDTH; ++x)
            p->pixelHook(p->hookContext, shade[x]);
    }
}

void GBHost::HResetThunk(void *pOpaque)
{
    Impl *p = (Impl *)pOpaque;
    if (!p)
        return;
    if (p->icd2FastPath)
    {
        p->icd2FastPath->PPUHReset();
        return;
    }
    if (p->hresetHook)
        p->hresetHook(p->hookContext);
}

void GBHost::VResetThunk(void *pOpaque)
{
    Impl *p = (Impl *)pOpaque;
    if (!p)
        return;
    if (p->icd2FastPath)
    {
        p->icd2FastPath->PPUVReset();
        return;
    }
    if (p->vresetHook)
        p->vresetHook(p->hookContext);
}

void GBHost::SampleThunk(void *pOpaque, Int16 left, Int16 right)
{
    Impl *p = (Impl *)pOpaque;
    Uint32 w;

    if (!p)
        return;

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

static void AuroraSameBoyBootCallback(GB_gameboy_t *gb, GB_boot_rom_t type)
{
    GBHost::BootRomThunk(GB_get_user_data(gb), (Int32)type);
}

static void AuroraSameBoyJoypCallback(GB_gameboy_t *gb, uint8_t value)
{
    GBHost::JoypThunk(GB_get_user_data(gb), (Uint8)value);
}

static void AuroraSameBoyPixelCallback(GB_gameboy_t *gb, uint8_t pixel)
{
    GBHost::PixelThunk(GB_get_user_data(gb), (Uint8)pixel);
}

static void AuroraSameBoyHResetCallback(GB_gameboy_t *gb)
{
    void *pUser = GB_get_user_data(gb);
    int line = -1;
    const void *pRow = AuroraSameBoyGetCompletedSGBScanline(gb, &line);

    /* AURORA_SGB_SCANLINE_BATCH_V4_1_20260907
     * Same semantics as V4, but GB_gameboy_t layout stays private to SameBoy.
     * One C helper call per completed LCD row replaces all C++ field peeks. */
    if (pRow && line >= 0 &&
        line < (int)SNSGBICD2::LCD_VISIBLE_LINES)
    {
        GBHost::LineThunk(
            pUser,
            (const Uint32 *)pRow,
            (Int32)line);
    }

    GBHost::HResetThunk(pUser);
}

static void AuroraSameBoyVResetCallback(GB_gameboy_t *gb)
{
    GBHost::VResetThunk(GB_get_user_data(gb));
}

static void AuroraSameBoySampleCallback(GB_gameboy_t *gb, GB_sample_t *sample)
{
    GBHost::SampleThunk(
        GB_get_user_data(gb), (Int16)sample->left, (Int16)sample->right);
}

Bool GBHost::LoadROM(const Uint8 *pData, Uint32 nBytes, ModelE eModel)
{
    GB_model_t model;
    Uint8 boot[256];

    if (!pData || nBytes < 0x150U)
        return FALSE;

    if (!Init())
        return FALSE;

    if (m_p->gbInited)
    {
        GB_free(&m_p->gb);
        m_p->gbInited = FALSE;
    }
    delete [] m_p->screen;
    m_p->screen = NULL;

    memset(&m_p->gb, 0, sizeof(m_p->gb));
    m_p->model = (eModel == MODEL_SGB2) ? MODEL_SGB2 : MODEL_SGB1;
    model = AuroraSameBoyModel(m_p->model);

    GB_init(&m_p->gb, model);
    m_p->gbInited = TRUE;
    GB_set_user_data(&m_p->gb, m_p);

    GB_set_boot_rom_load_callback(&m_p->gb, AuroraSameBoyBootCallback);
    GB_set_joyp_write_callback(&m_p->gb, AuroraSameBoyJoypCallback);
    GB_set_icd_pixel_callback(&m_p->gb, AuroraSameBoyPixelCallback);
    GB_set_icd_hreset_callback(&m_p->gb, AuroraSameBoyHResetCallback);
    GB_set_icd_vreset_callback(&m_p->gb, AuroraSameBoyVResetCallback);
    GB_apu_set_sample_callback(&m_p->gb, AuroraSameBoySampleCallback);

    GB_set_sample_rate_by_clocks(&m_p->gb, 256.0);
    GB_set_highpass_filter_mode(&m_p->gb, GB_HIGHPASS_ACCURATE);
    GB_set_border_mode(&m_p->gb, GB_BORDER_NEVER);

    m_p->screen = new (std::nothrow) Uint32[160U * 144U];
    if (!m_p->screen)
    {
        GB_free(&m_p->gb);
        m_p->gbInited = FALSE;
        return FALSE;
    }
    memset(m_p->screen, 0, sizeof(Uint32) * 160U * 144U);
    /* AURORA_SGB_SCANLINE_BATCH_V4_1_20260907
     * PS2 toolchain typedefs Uint32 and uint32_t to distinct C++ base types
     * even though both are 32-bit. The storage is exactly 160*144 32-bit
     * words, so make the ABI conversion explicit at the SameBoy boundary. */
    GB_set_pixels_output(&m_p->gb, (uint32_t *)m_p->screen);

    GB_load_rom_from_buffer(&m_p->gb, pData, (size_t)nBytes);

    memcpy(boot, g_AuroraSameBoySgbBoot, sizeof(boot));
    boot[253] = (m_p->model == MODEL_SGB2) ? 0xffU : 0x01U;
    GB_load_boot_rom_from_buffer(&m_p->gb, boot, sizeof(boot));

    m_p->romBytes = nBytes;
    m_p->romCRC = AuroraSameBoyCRC32(pData, nBytes);
    m_p->clockCredit = 0;
    m_p->audioRead = m_p->audioWrite = m_p->audioCount = 0;
    m_p->loaded = TRUE;

    GB_reset(&m_p->gb);

    AuroraSgbBootTrace(
        m_p->model == MODEL_SGB2
            ? "SGB SB2: SameBoy SGB2 NO_SFC boot"
            : "SGB SB2: SameBoy SGB1 NO_SFC boot");
    return TRUE;
}

void GBHost::UnloadROM()
{
    if (!m_p || !m_p->initialized)
        return;

    if (m_p->gbInited)
    {
        GB_free(&m_p->gb);
        m_p->gbInited = FALSE;
    }

    delete [] m_p->screen;
    m_p->screen = NULL;

    m_p->loaded = FALSE;
    m_p->romBytes = 0;
    m_p->romCRC = 0;
    m_p->clockCredit = 0;
    ClearAudio();
}

void GBHost::Reset(ModelE eModel)
{
    if (!m_p || !m_p->loaded || !m_p->gbInited)
        return;

    m_p->model = (eModel == MODEL_SGB2) ? MODEL_SGB2 : MODEL_SGB1;

    if (GB_get_model(&m_p->gb) != AuroraSameBoyModel(m_p->model))
        GB_switch_model_and_reset(&m_p->gb, AuroraSameBoyModel(m_p->model));
    else
        GB_reset(&m_p->gb);

    m_p->clockCredit = 0;
    ClearAudio();
}

Bool GBHost::AttachSavedata(const Uint8 *pData, Uint32 nBytes)
{
    if (!m_p || !m_p->loaded || !m_p->gbInited || (nBytes && !pData))
        return FALSE;

    if (nBytes)
        GB_load_battery_from_buffer(&m_p->gb, pData, (size_t)nBytes);

    GB_clear_battery_dirty(&m_p->gb);
    return TRUE;
}

Uint32 GBHost::GetSavedataBytes()
{
    int n;
    if (!m_p || !m_p->loaded || !m_p->gbInited)
        return 0;
    n = GB_save_battery_size(&m_p->gb);
    return n > 0 ? (Uint32)n : 0;
}

Bool GBHost::ExportSavedata(
    Uint8 *pData, Uint32 nCapacity, Uint32 *pActualBytes)
{
    int n;

    if (pActualBytes)
        *pActualBytes = 0;

    if (!m_p || !m_p->loaded || !m_p->gbInited)
        return FALSE;

    n = GB_save_battery_size(&m_p->gb);
    if (n < 0)
        return FALSE;

    if (pActualBytes)
        *pActualBytes = (Uint32)n;

    if (!n)
        return TRUE;

    if (!pData || nCapacity < (Uint32)n)
        return FALSE;

    return GB_save_battery_to_buffer(&m_p->gb, pData, (size_t)n) == 0
        ? TRUE : FALSE;
}

Bool GBHost::SavedataDirty() const
{
    return m_p && m_p->loaded && m_p->gbInited &&
           GB_get_battery_dirty(&m_p->gb) ? TRUE : FALSE;
}

void GBHost::ClearSavedataDirty()
{
    if (m_p && m_p->gbInited)
        GB_clear_battery_dirty(&m_p->gb);
}

Uint32 GBHost::RunClocks(Uint32 nTargetClocks)
{
    Uint64 targetTicks, needTicks, advanced = 0, credit = 0;
    Uint32 guard = 0;
    Uint32 guardLimit;

    if (!m_p || !m_p->loaded || !m_p->gbInited || !nTargetClocks)
        return 0;

    /* SameBoy GB_run() reports 8 MHz ticks. bsnes integrates it by stepping
       the SGB thread with clocks >> 1; Aurora's ICD grant is the ~4 MHz side. */
    targetTicks = (Uint64)nTargetClocks * 2ULL;

    if (m_p->clockCredit > 0)
        credit = (Uint64)m_p->clockCredit;

    if (credit >= targetTicks)
    {
        m_p->clockCredit = (Int64)(credit - targetTicks);
        return nTargetClocks;
    }

    needTicks = targetTicks - credit;
    m_p->clockCredit = 0;
    guardLimit = nTargetClocks > 0x0fffffffU
        ? 0x7fffffffU : nTargetClocks * 8U + 4096U;

    while (advanced < needTicks && guard++ < guardLimit)
    {
        unsigned step = GB_run(&m_p->gb);
        if (!step)
            break;
        advanced += (Uint64)step;
    }

    if (advanced >= needTicks)
    {
        m_p->clockCredit = (Int64)(advanced - needTicks);
        return nTargetClocks;
    }

    return (Uint32)((credit + advanced) >> 1);
}

Uint32 GBHost::DebugPreTickState() const { return 0; }
const char *GBHost::DebugPreEventName() const { return "SameBoy GB_run"; }
Int32 GBHost::DebugPreEventDelta() const { return 0; }
Bool GBHost::DebugSkipDueAudioSample() { return FALSE; }

Uint32 GBHost::GetClockHz() const
{
    return m_p && m_p->gbInited
        ? (Uint32)GB_get_clock_rate(&m_p->gb) : 0U;
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
    if (!m_p)
        return;
    m_p->icd2FastPath = pICD2;
}

Bool GBHost::SaveState(StateT *pState) const
{
    size_t n;

    if (!m_p || !m_p->loaded || !m_p->gbInited || !pState)
        return FALSE;

    n = GB_get_save_state_size(&m_p->gb);
    if (!n || n > SERIALIZED_BYTES)
        return FALSE;

    memset(pState, 0, sizeof(*pState));
    pState->Magic = GBHOST_STATE_MAGIC;
    pState->Version = GBHOST_STATE_VERSION;
    pState->Model = (Uint32)m_p->model;
    pState->Reserved = (Uint32)n;
    pState->ClockCredit = m_p->clockCredit;
    GB_save_state_to_buffer(&m_p->gb, pState->Serialized);
    return TRUE;
}

Bool GBHost::RestoreState(const StateT *pState)
{
    if (!m_p || !m_p->loaded || !m_p->gbInited || !pState)
        return FALSE;

    if (pState->Magic != GBHOST_STATE_MAGIC ||
        pState->Version != GBHOST_STATE_VERSION ||
        pState->Reserved == 0 ||
        pState->Reserved > SERIALIZED_BYTES)
        return FALSE;

    if (GB_load_state_from_buffer(
            &m_p->gb, pState->Serialized, (size_t)pState->Reserved) != 0)
        return FALSE;

    m_p->model = pState->Model == MODEL_SGB2 ? MODEL_SGB2 : MODEL_SGB1;
    m_p->clockCredit = pState->ClockCredit;
    ClearAudio();
    return TRUE;
}
