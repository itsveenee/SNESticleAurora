#include "gba/system/gpspsystem.h"

#include <new>
#include <stdio.h>
#include <string.h>

#include "rendersurface.h"
#include "mixbuffer.h"
#include "audmixbuffer.h" /* AURORA_GPSP_GBA_V13_SAFE_PERF_20260911: interleaved libretro audio */
#include "../../platform/ps2/input/input.h" /* AURORA_GPSP_GBA_V13_BITMASK_TURBO_INPUT_20260911: raw PS2 chords */
#include <libpad.h> /* PAD_L1/PAD_L2/PAD_R2 */
#include "snio.h"

extern "C" {
#include "gs.h"
#include "gpprim.h"
#include "libretro.h"

/* These names are produced by tools/namespace_gpsp_archive.py. */
void GPSP_retro_set_environment(retro_environment_t cb);
void GPSP_retro_set_video_refresh(retro_video_refresh_t cb);
void GPSP_retro_set_audio_sample(retro_audio_sample_t cb);
void GPSP_retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
void GPSP_retro_set_input_poll(retro_input_poll_t cb);
void GPSP_retro_set_input_state(retro_input_state_t cb);
void GPSP_retro_set_controller_port_device(unsigned port, unsigned device);
void GPSP_retro_init(void);
void GPSP_retro_deinit(void);
bool GPSP_retro_load_game(const struct retro_game_info *info);
void GPSP_retro_unload_game(void);
void GPSP_retro_run(void);
void GPSP_retro_reset(void);
size_t GPSP_retro_serialize_size(void);
bool GPSP_retro_serialize(void *data, size_t size);
bool GPSP_retro_unserialize(const void *data, size_t size);
void *GPSP_retro_get_memory_data(unsigned id);
size_t GPSP_retro_get_memory_size(unsigned id);
void GPSP_retro_get_system_av_info(struct retro_system_av_info *info);
void GPSP_aurora_set_frontend_skip(unsigned skip); /* AURORA_GPSP_GBA_V13_SAFE_FRAMESKIP_BRIDGE_20260911 */

/* AURORA_GPSP_GBA_V2_TFA_BLEND_20260911: staged gpSP exports. */
void GPSP_aurora_tfa_set_storage(uint8_t *data, uint32_t bytes);
void GPSP_aurora_tfa_reset_protocol(void);
bool GPSP_aurora_tfa_active(void);
bool GPSP_aurora_tfa_dirty(void);
void GPSP_aurora_tfa_clear_dirty(void);
}

/* AURORA_GPSP_GBA_V14_VISIBLE_BIOS_SQUARE_20260911: incremental over v13; explicit frontend upload lives in mainloop_process.cpp. */
/* AURORA_GPSP_GBA_V1_20260911 */
struct GpSPSystem::Impl
{
    Bool initialized;
    Bool loaded;
    Uint16 pad;
    Bool turboShoulderL;
    Bool turboShoulderR;
    Uint32 sampleRate;
    CRenderSurface *target;
    CMixBuffer *mix;
    /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912 */
    const void *directVideoData;
    unsigned directVideoW;
    unsigned directVideoH;
    size_t directVideoPitch;
    Bool directVideoValid;
    Char systemDirectory[1024];
    /* AURORA_GPSP_GBA_V2_TFA_BLEND_20260911 */
    Uint8 *tfaData;
    Uint32 romCRC;

    Impl()
        : initialized(FALSE), loaded(FALSE), pad(0),
          turboShoulderL(FALSE), turboShoulderR(FALSE), sampleRate(32768),
          target(NULL), mix(NULL),
          directVideoData(NULL), directVideoW(0), directVideoH(0),
          directVideoPitch(0), directVideoValid(FALSE),
          tfaData(NULL), romCRC(0)
    {
        systemDirectory[0] = 0;
    }
    ~Impl() { delete [] tfaData; tfaData = NULL; }
};

static GpSPSystem::Impl *s_GpSPHost = NULL;

static const char *AuroraGpSPVariable(const char *key)
{
    if (!key) return NULL;
    if (!strcmp(key, "gpsp_bios"))                return "auto";
    if (!strcmp(key, "gpsp_boot_mode"))           return "bios"; /* AURORA_GPSP_GBA_V14_BIOS_BOOT_20260911: execute BIOS before cartridge */
    if (!strcmp(key, "gpsp_drc"))                 return "enabled";
    if (!strcmp(key, "gpsp_sprlim"))              return "disabled";
    if (!strcmp(key, "gpsp_rtc"))                 return "auto";
    if (!strcmp(key, "gpsp_rtc_time_source"))     return "system"; /* AURORA_GPSP_GBA_V13_SAFE_PERF_20260911: real PS2 clock */
    if (!strcmp(key, "gpsp_serial"))              return "disabled";
    if (!strcmp(key, "gpsp_rumble"))              return "disabled";
    if (!strcmp(key, "gpsp_sound_rate"))          return "32768";
    if (!strcmp(key, "gpsp_frameskip"))           return "disabled";
    if (!strcmp(key, "gpsp_frameskip_threshold")) return "33";
    if (!strcmp(key, "gpsp_frameskip_interval"))  return "0";
    if (!strcmp(key, "gpsp_color_correction"))    return "enabled"; /* AURORA_GPSP_GBA_V13_PS2_COLOR_CORRECTION_20260911: native GBA LCD colour model */
    if (!strcmp(key, "gpsp_frame_mixing"))        return "enabled"; /* AURORA_GPSP_GBA_V13_BITMASK_TURBO_INPUT_20260911: default ON */
    if (!strcmp(key, "gpsp_turbo_period"))        return "4";
    return NULL;
}

static bool AuroraGpSPEnvironment(unsigned cmd, void *data)
{
    GpSPSystem::Impl *p = s_GpSPHost;

    if (cmd == RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY ||
        cmd == RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY)
    {
        if (!p || !data) return false;
        *(const char **)data = p->systemDirectory;
        return true;
    }

    if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE)
    {
        struct retro_variable *v = (struct retro_variable *)data;
        if (!v) return false;
        v->value = AuroraGpSPVariable(v->key);
        return v->value != NULL;
    }

    if (cmd == RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE)
    {
        if (!data) return false;
        *(bool *)data = false;
        return true;
    }

    if (cmd == RETRO_ENVIRONMENT_SET_PIXEL_FORMAT)
    {
        if (!data) return false;
        /* gpSP's current PS2 path requests RGB565. */
        return *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
    }

    if (cmd == RETRO_ENVIRONMENT_GET_CAN_DUPE)
    {
        if (!data) return false;
        *(bool *)data = true;
        return true;
    }

    if (cmd == RETRO_ENVIRONMENT_GET_LANGUAGE)
    {
        if (!data) return false;
        *(unsigned *)data = RETRO_LANGUAGE_ENGLISH;
        return true;
    }

    if (cmd == RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION)
    {
        if (!data) return false;
        *(unsigned *)data = 0;
        return true;
    }

    /* AURORA_GPSP_GBA_V13_BITMASK_INPUT_20260911
     * gpSP natively supports one packed joypad sample per frame. Aurora owns
     * the source bits, so accepting this capability removes a dozen-ish
     * frontend callbacks without giving the core ownership of PS2 R2/L2. */
    if (cmd == RETRO_ENVIRONMENT_GET_INPUT_BITMASKS)
        return true;

    /* Optional host services that gpSP can run without remain declined. */
    if (cmd == RETRO_ENVIRONMENT_GET_PERF_INTERFACE ||
        cmd == RETRO_ENVIRONMENT_GET_VFS_INTERFACE ||
        cmd == RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE ||
        cmd == RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE ||
        cmd == RETRO_ENVIRONMENT_SET_NETPACKET_INTERFACE)
        return false;

    /* Registration/notification calls: accepted, but Aurora owns their UI. */
    if (cmd == RETRO_ENVIRONMENT_SET_VARIABLES ||
        cmd == RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS ||
        cmd == RETRO_ENVIRONMENT_SET_MEMORY_MAPS ||
        cmd == RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY ||
        cmd == RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO ||
        cmd == RETRO_ENVIRONMENT_SET_MESSAGE ||
        cmd == RETRO_ENVIRONMENT_SET_MESSAGE_EXT ||
        cmd == RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY ||
        cmd == RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK)
        return true;

    return false;
}

static void AuroraGpSPInputPoll(void)
{
}

static int16_t AuroraGpSPInputState(unsigned port, unsigned device,
                                    unsigned index, unsigned id)
{
    GpSPSystem::Impl *p = s_GpSPHost;
    Uint16 pad;
    (void)index;

    if (!p || port != 0 || device != RETRO_DEVICE_JOYPAD)
        return 0;
    pad = p->pad;

    if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
    {
        unsigned mask = 0; /* AURORA_GPSP_GBA_V13_BITMASK_INPUT_20260911 */
        if (pad & SNESIO_JOY_B)      mask |= 1U << RETRO_DEVICE_ID_JOYPAD_A;
        if (pad & SNESIO_JOY_Y)      mask |= 1U << RETRO_DEVICE_ID_JOYPAD_B;
        if (pad & SNESIO_JOY_A)      mask |= 1U << RETRO_DEVICE_ID_JOYPAD_X;
        if (pad & SNESIO_JOY_X)      mask |= 1U << RETRO_DEVICE_ID_JOYPAD_Y;
        if (pad & SNESIO_JOY_L)      mask |= 1U << RETRO_DEVICE_ID_JOYPAD_L;
        if (pad & SNESIO_JOY_R)      mask |= 1U << RETRO_DEVICE_ID_JOYPAD_R;
        if (pad & SNESIO_JOY_SELECT) mask |= 1U << RETRO_DEVICE_ID_JOYPAD_SELECT;
        if (pad & SNESIO_JOY_START)  mask |= 1U << RETRO_DEVICE_ID_JOYPAD_START;
        if (pad & SNESIO_JOY_UP)     mask |= 1U << RETRO_DEVICE_ID_JOYPAD_UP;
        if (pad & SNESIO_JOY_DOWN)   mask |= 1U << RETRO_DEVICE_ID_JOYPAD_DOWN;
        if (pad & SNESIO_JOY_LEFT)   mask |= 1U << RETRO_DEVICE_ID_JOYPAD_LEFT;
        if (pad & SNESIO_JOY_RIGHT)  mask |= 1U << RETRO_DEVICE_ID_JOYPAD_RIGHT;
        if (p->turboShoulderL)        mask |= 1U << RETRO_DEVICE_ID_JOYPAD_L3;
        if (p->turboShoulderR)        mask |= 1U << RETRO_DEVICE_ID_JOYPAD_R3;
        return (int16_t)(mask & 0xffffU);
    }

    switch (id)
    {
        case RETRO_DEVICE_ID_JOYPAD_A:      return (pad & SNESIO_JOY_B) ? 1 : 0; /* Cross -> A */
        case RETRO_DEVICE_ID_JOYPAD_B:      return (pad & SNESIO_JOY_Y) ? 1 : 0; /* Square -> B */
        case RETRO_DEVICE_ID_JOYPAD_X:      return (pad & SNESIO_JOY_A) ? 1 : 0; /* Circle -> Turbo A */
        case RETRO_DEVICE_ID_JOYPAD_Y:      return (pad & SNESIO_JOY_X) ? 1 : 0; /* Triangle -> Turbo B */
        case RETRO_DEVICE_ID_JOYPAD_L:      return (pad & SNESIO_JOY_L) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_R:      return (pad & SNESIO_JOY_R) ? 1 : 0;
        /* AURORA_GPSP_GBA_V13_BITMASK_TURBO_INPUT_20260911
         * L3/R3 are private virtual transport bits; physical stick clicks are
         * not exposed. Staged gpSP consumes them only as Turbo L/Turbo R. */
        case RETRO_DEVICE_ID_JOYPAD_L3:     return p->turboShoulderL ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_R3:     return p->turboShoulderR ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return (pad & SNESIO_JOY_SELECT) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_START:  return (pad & SNESIO_JOY_START) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_UP:     return (pad & SNESIO_JOY_UP) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   return (pad & SNESIO_JOY_DOWN) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   return (pad & SNESIO_JOY_LEFT) ? 1 : 0;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return (pad & SNESIO_JOY_RIGHT) ? 1 : 0;
        default: return 0;
    }
}

/* AURORA_GPSP_GBA_V13_SCALER_LUT_20260911
 * gpSP's PS2-native framebuffer is 0BGR555: B=10..14, G=5..9, R=0..4.
 * Three tiny 32-entry tables expand those channels directly into Aurora's
 * 0xAARRGGBB surface. Total lookup footprint is only 384 bytes and output is
 * bit-identical to the working V1/V2 bridge. */
static Uint32 s_GpSPBlueToSurfaceLut[32];
static Uint32 s_GpSPGreenToSurfaceLut[32];
static Uint32 s_GpSPRedToSurfaceLut[32];
static Bool s_GpSPColorLutReady = FALSE;

static void AuroraGpSPInitColorLut(void)
{
    unsigned i;
    if (s_GpSPColorLutReady) return;
    for (i = 0; i < 32U; ++i)
    {
        Uint32 c8 = (i << 3) | (i >> 2);
        /* AURORA_GPSP_GBA_V14_BGR555_SURFACE_20260911
         * gpSP PS2 0BGR555 -> Aurora software surface 0xAABBGGRR. */
        s_GpSPRedToSurfaceLut[i] = 0xff000000U | c8;
        s_GpSPGreenToSurfaceLut[i] = c8 << 8;
        s_GpSPBlueToSurfaceLut[i] = c8 << 16;
    }
    s_GpSPColorLutReady = TRUE;
}

static inline Uint32 AuroraGpSPPs2PixelToSurface(Uint16 c)
{
    return s_GpSPRedToSurfaceLut[c & 31U] |
           s_GpSPGreenToSurfaceLut[(c >> 5) & 31U] |
           s_GpSPBlueToSurfaceLut[(c >> 10) & 31U];
}

static void AuroraGpSPVideo(const void *data, unsigned width,
                            unsigned height, size_t pitch)
{
    GpSPSystem::Impl *p = s_GpSPHost;
    CRenderSurface *dstSurf;
    unsigned y, x;
    const unsigned nativeW = 240U;
    const unsigned nativeH = 160U;
    const unsigned offX = (256U - nativeW) / 2U; /* 8 + 8 */
    const unsigned offY = (240U - nativeH) / 2U; /* 40 + 40 */

    if (!p || !data || !width || !height || !pitch)
        return;

    /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912
     * gpSP's PS2 build uses USE_XBGR1555_FORMAT. That buffer is already
     * 0BGR1555: R=0..4, G=5..9, B=10..14, which is exactly the RGB channel
     * ordering consumed by GS PSMCT16. For the normal 240x160 geometry keep
     * the libretro framebuffer pointer and let MainLoopRender upload it
     * directly. No 16->32 expansion and no 256x240 RGBA staging surface.
     *
     * The pointer only has to live until the next retro_run(), and Aurora
     * draws immediately after ExecuteFrame. Safe Frameskip passes target=NULL
     * and therefore never publishes a new direct frame.
     */
    if (p->target &&
        width == nativeW && height == nativeH &&
        pitch >= nativeW * 2U && (pitch & 1U) == 0U &&
        (pitch >> 1) <= 512U)
    {
        p->directVideoData = data;
        p->directVideoW = width;
        p->directVideoH = height;
        p->directVideoPitch = pitch;
        p->directVideoValid = TRUE;
        return;
    }

    if (!p->target)
        return; /* Safe Frameskip: retain the previously resident GS image. */

    p->directVideoValid = FALSE;
    dstSurf = p->target;
    if (dstSurf->GetWidth() < 256U || dstSurf->GetHeight() < 240U)
        return;

    AuroraGpSPInitColorLut();

    /* AURORA_GPSP_GBA_V14_NATIVE_SQUARE_20260911
     * Keep all native GBA pixels and do not pre-distort them for Aurora's
     * generic 4:3 canvas. mainloop_render applies the same square-pixel host
     * presentation used by standalone Gambatte: exact 2x2 in 480i/1080i and
     * uniform PCRTC pixel width in 240p. Normal gpSP geometry is 240x160, so
     * the hot path is a direct source-pixel -> surface-pixel conversion. */
    if (width == nativeW && height == nativeH && pitch >= nativeW * 2U)
    {
        /* Opaque black top/bottom bars. */
        for (y = 0; y < offY; ++y)
        {
            Uint32 *row = (Uint32 *)dstSurf->GetLinePtr((Int32)y);
            for (x = 0; x < 256U; ++x) row[x] = 0xff000000U;
        }
        for (y = offY + nativeH; y < 240U; ++y)
        {
            Uint32 *row = (Uint32 *)dstSurf->GetLinePtr((Int32)y);
            for (x = 0; x < 256U; ++x) row[x] = 0xff000000U;
        }

        for (y = 0; y < nativeH; ++y)
        {
            const Uint16 *src = (const Uint16 *)
                ((const Uint8 *)data + (size_t)y * pitch);
            Uint32 *row = (Uint32 *)dstSurf->GetLinePtr((Int32)(offY + y));
            Uint32 *dst = row + offX;

            for (x = 0; x < offX; ++x) row[x] = 0xff000000U;
            for (x = 0; x < nativeW; ++x)
                dst[x] = AuroraGpSPPs2PixelToSurface(src[x]);
            for (x = offX + nativeW; x < 256U; ++x)
                row[x] = 0xff000000U;
        }
        return;
    }

    /* Defensive generic fallback for any future core geometry change. Keep a
     * 240x160 square-pixel destination and nearest-neighbour sample into it;
     * this branch is not used by the pinned gpSP core. */
    for (y = 0; y < 240U; ++y)
    {
        Uint32 *row = (Uint32 *)dstSurf->GetLinePtr((Int32)y);
        for (x = 0; x < 256U; ++x) row[x] = 0xff000000U;
    }
    for (y = 0; y < nativeH; ++y)
    {
        const unsigned sy = (unsigned)(((unsigned long long)y * height) / nativeH);
        const Uint16 *src = (const Uint16 *)
            ((const Uint8 *)data + (size_t)sy * pitch);
        Uint32 *dst = (Uint32 *)dstSurf->GetLinePtr((Int32)(offY + y)) + offX;
        for (x = 0; x < nativeW; ++x)
        {
            const unsigned sx =
                (unsigned)(((unsigned long long)x * width) / nativeW);
            dst[x] = AuroraGpSPPs2PixelToSurface(src[sx]);
        }
    }
}

/* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912 */
Bool GpSPSystem::CanDirectGsVideo() const
{
    if (!m_p || !m_p->loaded || !m_p->directVideoValid ||
        !m_p->directVideoData)
        return FALSE;

    if (m_p->directVideoW != 240U || m_p->directVideoH != 160U ||
        m_p->directVideoPitch < 240U * 2U ||
        (m_p->directVideoPitch & 1U) != 0U ||
        (m_p->directVideoPitch >> 1) > 512U)
        return FALSE;

    return TRUE;
}

Bool GpSPSystem::DrawDirectGs(Uint32 auroraOutBaseTBP, Float32 intensity)
{
    unsigned pitchPixels;
    unsigned texTBW;
    unsigned texLog2;
    Uint32 black;
    Uint32 mod;
    Uint32 modColor;

    if (!auroraOutBaseTBP || !CanDirectGsVideo())
        return FALSE;

    pitchPixels = (unsigned)(m_p->directVideoPitch >> 1);
    texTBW = (pitchPixels + 63U) & ~63U;

    texLog2 = 5U;
    while ((1U << texLog2) < pitchPixels && texLog2 < 9U)
        ++texLog2;

    /* Upload only the gpSP 16-bit backing rows. With the ordinary
     * 240-pixel pitch this is 240*160*2 = 76,800 bytes instead of the
     * old 256*256*4 = 262,144-byte RGBA texture upload. */
    GPPrimUploadTexture(
        (int)auroraOutBaseTBP, (int)texTBW,
        0, 0, GS_PSMCT16,
        (void *)m_p->directVideoData,
        (int)pitchPixels, 160);

    GPPrimSetTex(
        auroraOutBaseTBP, texTBW, texLog2, 8,
        GS_PSMCT16, 0, 0, GS_PSMCT16, 0);

    /* Match the old 256x240 staging surface exactly:
     * 8-pixel black bars left/right, 40 lines top/bottom, 240x160 image.
     * GPPrimTexRect uses Aurora's active logical->physical transform, so
     * GSK_SetGbSquarePixelDraw() still gives exact 2x2 pixels in 480i/1080i.
     */
    black = 0x80000000U;
    GPPrimRect(0, 0, black,
               256U << 4, 240U << 4, black,
               0, 0);

    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    mod = (Uint32)(128.0f * intensity + 0.5f);
    if (mod > 128U) mod = 128U;
    modColor = 0x80000000U | (mod << 16) | (mod << 8) | mod;

    /* Half-texel UVs mirror Aurora's proven direct-GS paths and keep
     * NEAREST sampling on texel centres. */
    GPPrimTexRect(
        8U << 4, 40U << 4,
        8U, 8U,
        (8U + 240U) << 4, (40U + 160U) << 4,
        (240U << 4) + 8U, (160U << 4) + 8U,
        0, modColor, 0);

    return TRUE;
}

static size_t AuroraGpSPAudioBatch(const int16_t *data, size_t frames)
{
    GpSPSystem::Impl *p = s_GpSPHost;

    if (!p || !p->mix || !data || !frames)
        return frames;

    /* AURORA_GPSP_GBA_V13_SAFE_PERF_20260911
     * V1 split every interleaved gpSP batch into two stack arrays, then fed
     * those arrays back into AudMixBuffer. Aurora already provides a native
     * libretro interleaved path with arbitrary-rate -> 48 kHz resampling. */
    {
        AudMixBuffer *aud = (AudMixBuffer *)p->mix;
        if (aud->OutputLibretroInterleaved(data, (Int32)frames))
            return frames;
    }

    /* Conservative fallback if the specialised path ever declines a batch. */
    {
        size_t pos = 0;
        Int16 left[512];
        Int16 right[512];

        while (pos < frames)
        {
            size_t i;
            size_t batch = frames - pos;
            if (batch > 512U) batch = 512U;
            for (i = 0; i < batch; ++i)
            {
                left[i] = data[(pos + i) * 2U + 0U];
                right[i] = data[(pos + i) * 2U + 1U];
            }
            p->mix->OutputSamplesStereo(left, right, (Int32)batch);
            pos += batch;
        }
    }
    return frames;
}

static void AuroraGpSPAudioSample(int16_t left, int16_t right)
{
    GpSPSystem::Impl *p = s_GpSPHost;
    if (!p || !p->mix) return;
    Int16 l[1] = { left };
    Int16 r[1] = { right };
    p->mix->OutputSamplesStereo(l, r, 1);
}

GpSPSystem::GpSPSystem() : m_p(NULL)
{
    m_uLine = 0;
    m_uFrame = 0;
}

GpSPSystem::~GpSPSystem()
{
    UnloadGame();
}

/* AURORA_GPSP_GBA_V2_TFA_BLEND_20260911
 * Strict No-Intro CRC whitelist.  Do not enable the accessory from title/game
 * code alone: hacks, overdumps and translations must not accidentally get a
 * physical Turbo File attached. */
static const Uint32 AURORA_GBA_TFA_CRC_DERBY = 0x9746EF12U;
static const Uint32 AURORA_GBA_TFA_CRC_TSUKURU = 0xE7FC81D0U;
static const Uint32 AURORA_GBA_TFA_BYTES = 0x200000U;

static Bool AuroraGpSPTurboFileAdvanceCRC(Uint32 crc)
{
    return (crc == AURORA_GBA_TFA_CRC_DERBY ||
            crc == AURORA_GBA_TFA_CRC_TSUKURU) ? TRUE : FALSE;
}

static Bool AuroraGpSPCRC32File(const Char *pPath, Uint32 *pCRC)
{
    static Uint32 table[256];
    static Bool tableReady = FALSE;
    Uint8 buf[32768];
    FILE *fp;
    Uint32 crc = 0xffffffffU;
    size_t got, i;

    if (pCRC) *pCRC = 0;
    if (!pPath || !*pPath || !pCRC) return FALSE;
    if (!tableReady)
    {
        Uint32 n, k;
        for (n = 0; n < 256U; ++n)
        {
            Uint32 c = n;
            for (k = 0; k < 8U; ++k)
                c = (c >> 1) ^ ((c & 1U) ? 0xedb88320U : 0U);
            table[n] = c;
        }
        tableReady = TRUE;
    }

    fp = fopen(pPath, "rb");
    if (!fp) return FALSE;
    while ((got = fread(buf, 1, sizeof(buf), fp)) != 0)
        for (i = 0; i < got; ++i)
            crc = table[(crc ^ buf[i]) & 0xffU] ^ (crc >> 8);
    if (ferror(fp)) { fclose(fp); return FALSE; }
    fclose(fp);
    *pCRC = crc ^ 0xffffffffU;
    return TRUE;
}

Bool GpSPSystem::LoadGame(const Char *pPath, const Char *pSystemDirectory)
{
    struct retro_game_info info;
    struct retro_system_av_info av;

    if (!pPath || !*pPath || !pSystemDirectory || !*pSystemDirectory)
        return FALSE;

    UnloadGame();
    m_p = new (std::nothrow) Impl;
    if (!m_p) return FALSE;

    snprintf(m_p->systemDirectory, sizeof(m_p->systemDirectory), "%s",
             pSystemDirectory);
    s_GpSPHost = m_p;

    /* AURORA_GPSP_GBA_V21_TFA_PREALLOC_20260912
     * Reserve the accessory before gpSP retro_init(). retro_init()
     * allocates the ROM LRU in 1 MiB chunks; allocating TFA later can
     * fail after the LRU has consumed the last contiguous 2 MiB.
     * Normal games still pay zero TFA RAM cost. */
    {
        Uint32 crc = 0;
        if (AuroraGpSPCRC32File(pPath, &crc))
            m_p->romCRC = crc;
        if (AuroraGpSPTurboFileAdvanceCRC(m_p->romCRC))
        {
            m_p->tfaData =
                new (std::nothrow) Uint8[AURORA_GBA_TFA_BYTES];
            if (m_p->tfaData)
                memset(m_p->tfaData, 0xff, AURORA_GBA_TFA_BYTES);
            else
                printf("[gpSP] Turbo File Advance disabled: "
                       "2 MiB pre-allocation failed\n");
        }
    }

    GPSP_retro_set_environment(AuroraGpSPEnvironment);
    GPSP_retro_set_video_refresh(AuroraGpSPVideo);
    GPSP_retro_set_audio_sample(AuroraGpSPAudioSample);
    GPSP_retro_set_audio_sample_batch(AuroraGpSPAudioBatch);
    GPSP_retro_set_input_poll(AuroraGpSPInputPoll);
    GPSP_retro_set_input_state(AuroraGpSPInputState);
    GPSP_retro_init();
    m_p->initialized = TRUE;
    GPSP_retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    GPSP_aurora_tfa_set_storage(
        m_p->tfaData,
        m_p->tfaData ? AURORA_GBA_TFA_BYTES : 0U); /* AURORA_GPSP_GBA_V21_TFA_PREALLOC_20260912 */

    memset(&info, 0, sizeof(info));
    info.path = pPath;
    if (!GPSP_retro_load_game(&info))
    {
        UnloadGame();
        return FALSE;
    }

    m_p->loaded = TRUE;

    /* AURORA_GPSP_GBA_V21_TFA_PREALLOC_20260912: CRC/storage were reserved before core init. */

    memset(&av, 0, sizeof(av));
    GPSP_retro_get_system_av_info(&av);
    if (av.timing.sample_rate > 1000.0)
        m_p->sampleRate = (Uint32)(av.timing.sample_rate + 0.5);
    else
        m_p->sampleRate = 32768U;

    m_p->pad = 0;
    m_uLine = 0;
    m_uFrame = 0;
    return TRUE;
}

void GpSPSystem::UnloadGame()
{
    if (!m_p)
    {
        if (s_GpSPHost) s_GpSPHost = NULL;
        m_uLine = 0;
        m_uFrame = 0;
        return;
    }

    s_GpSPHost = m_p;
    /* Detach before Impl releases the shared physical backing pointer. */
    GPSP_aurora_tfa_set_storage(NULL, 0); /* AURORA_GPSP_GBA_V2_TFA_BLEND_20260911 */
    m_p->target = NULL;
    m_p->mix = NULL;
    m_p->pad = 0;
    m_p->directVideoData = NULL;
    m_p->directVideoW = 0;
    m_p->directVideoH = 0;
    m_p->directVideoPitch = 0;
    m_p->directVideoValid = FALSE; /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912 */
    if (m_p->loaded)
    {
        GPSP_retro_unload_game();
        m_p->loaded = FALSE;
    }
    if (m_p->initialized)
    {
        GPSP_retro_deinit();
        m_p->initialized = FALSE;
    }
    if (s_GpSPHost == m_p) s_GpSPHost = NULL;
    delete m_p;
    m_p = NULL;
    m_uLine = 0;
    m_uFrame = 0;
}

Bool GpSPSystem::IsGameLoaded() const
{
    return (m_p && m_p->loaded) ? TRUE : FALSE;
}

void GpSPSystem::SetRom(Emu::Rom *pRom)
{
    if (!pRom) UnloadGame();
}

void GpSPSystem::Reset()
{
    if (!m_p || !m_p->loaded) return;
    s_GpSPHost = m_p;
    GPSP_retro_reset();
    if (m_p->tfaData) GPSP_aurora_tfa_reset_protocol(); /* AURORA_GPSP_GBA_V2_TFA_BLEND_20260911 */
    m_p->directVideoValid = FALSE; /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912 */
    m_uLine = 0;
    m_uFrame = 0;
}

void GpSPSystem::SoftReset()
{
    Reset();
}

void GpSPSystem::ExecuteFrame(Emu::SysInputT *pInput,
                              CRenderSurface *pTarget,
                              CMixBuffer *pMixBuf,
                              Emu::System::ModeE eMode)
{
    Uint16 pad;
    (void)eMode;
    if (!m_p || !m_p->loaded) return;

    pad = pInput ? pInput->uPad[0] : EMUSYS_DEVICE_DISCONNECTED;
    m_p->pad = (pad == EMUSYS_DEVICE_DISCONNECTED) ? 0 : pad;
    {
        const Uint32 rawPad = InputGetPadData(0);
        const Bool r2 = (rawPad & PAD_R2) ? TRUE : FALSE;
        m_p->turboShoulderL = (r2 && (rawPad & PAD_L1)) ? TRUE : FALSE;
        m_p->turboShoulderR = (r2 && (rawPad & PAD_L2)) ? TRUE : FALSE;
    } /* AURORA_GPSP_GBA_V13_BITMASK_TURBO_INPUT_20260911:
       * R2+L1 = Turbo L; R2+L2 = Turbo R. */
    m_p->target = pTarget;
    m_p->mix = pMixBuf;
    s_GpSPHost = m_p;

    /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912
     * A visible host tick must receive a fresh video callback. A skipped tick
     * deliberately leaves the prior GS image resident and does not invalidate
     * the last direct pointer merely for bookkeeping. */
    if (pTarget)
        m_p->directVideoValid = FALSE;

    /* AURORA_GPSP_GBA_V13_SAFE_FRAMESKIP_BRIDGE_20260911: Aurora Safe Frameskip passes a NULL target.
     * Tell gpSP to skip only scanline/video work; retro_run still advances
     * CPU/timers/audio. The staged core resynchronizes blend history. */
    GPSP_aurora_set_frontend_skip(pTarget ? 0U : 1U);
    GPSP_retro_run();

    if (pMixBuf) pMixBuf->Flush();
    m_p->target = NULL;
    m_p->mix = NULL;
    m_uLine = 0;
    ++m_uFrame;
}

Int32 GpSPSystem::GetStateSize()
{
    size_t n = GPSP_retro_serialize_size();
    return n > 0x7fffffffU ? 0 : (Int32)n;
}

Bool GpSPSystem::SaveStateChecked(void *pState, Int32 nStateBytes)
{
    size_t need;
    if (!m_p || !m_p->loaded || !pState || nStateBytes <= 0) return FALSE;
    need = GPSP_retro_serialize_size();
    if (!need || (size_t)nStateBytes != need) return FALSE;
    return GPSP_retro_serialize(pState, need) ? TRUE : FALSE;
}

Bool GpSPSystem::RestoreStateChecked(const void *pState, Int32 nStateBytes)
{
    size_t need;
    Bool ok;
    if (!m_p || !m_p->loaded || !pState || nStateBytes <= 0) return FALSE;
    need = GPSP_retro_serialize_size();
    if (!need || (size_t)nStateBytes != need) return FALSE;
    ok = GPSP_retro_unserialize(pState, need) ? TRUE : FALSE;
    if (ok && m_p->tfaData)
        GPSP_aurora_tfa_reset_protocol(); /* external physical media */
    return ok;
}

void GpSPSystem::SaveState(void *pState, Int32 nStateBytes)
{
    (void)SaveStateChecked(pState, nStateBytes);
}

void GpSPSystem::RestoreState(void *pState, Int32 nStateBytes)
{
    (void)RestoreStateChecked(pState, nStateBytes);
}

Int32 GpSPSystem::GetSRAMBytes()
{
    if (!m_p || !m_p->loaded) return 0;
    size_t n = GPSP_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    return n > 0x7fffffffU ? 0 : (Int32)n;
}

Uint8 *GpSPSystem::GetSRAMData()
{
    if (!m_p || !m_p->loaded) return NULL;
    return (Uint8 *)GPSP_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
}

Bool GpSPSystem::HasTurboFileAdvance() const
{
    return (m_p && m_p->loaded && m_p->tfaData &&
            AuroraGpSPTurboFileAdvanceCRC(m_p->romCRC)) ? TRUE : FALSE;
}

Uint8 *GpSPSystem::GetTurboFileAdvanceData()
{
    return HasTurboFileAdvance() ? m_p->tfaData : NULL;
}

const Uint8 *GpSPSystem::GetTurboFileAdvanceData() const
{
    return HasTurboFileAdvance() ? m_p->tfaData : NULL;
}

Uint32 GpSPSystem::GetTurboFileAdvanceBytes() const
{
    return HasTurboFileAdvance() ? AURORA_GBA_TFA_BYTES : 0U;
}

Bool GpSPSystem::AttachTurboFileAdvance(const Uint8 *pData, Uint32 nBytes)
{
    if (!HasTurboFileAdvance()) return nBytes == 0U ? TRUE : FALSE;
    if (!pData && nBytes == 0U)
        memset(m_p->tfaData, 0xff, AURORA_GBA_TFA_BYTES);
    else if (pData && nBytes == AURORA_GBA_TFA_BYTES)
        memcpy(m_p->tfaData, pData, AURORA_GBA_TFA_BYTES);
    else
        return FALSE;
    GPSP_aurora_tfa_set_storage(m_p->tfaData, AURORA_GBA_TFA_BYTES);
    return TRUE;
}

Bool GpSPSystem::TurboFileAdvanceDirty() const
{
    return HasTurboFileAdvance() && GPSP_aurora_tfa_dirty() ? TRUE : FALSE;
}

void GpSPSystem::ClearTurboFileAdvanceDirty()
{
    if (HasTurboFileAdvance()) GPSP_aurora_tfa_clear_dirty();
}

Uint32 GpSPSystem::GetGameCRC() const
{
    return m_p ? m_p->romCRC : 0U;
}

const char *GpSPSystem::GetString(Emu::System::StringE eString)
{
    switch (eString)
    {
        case Emu::System::STRING_SHORTNAME: return "GBA";
        case Emu::System::STRING_FULLNAME:  return "gpSP Game Boy Advance";
        case Emu::System::STRING_SRAMEXT:   return "sav";
        case Emu::System::STRING_STATEEXT:  return "gst";
        default: return "";
    }
}

Uint32 GpSPSystem::GetSampleRate()
{
    return m_p ? m_p->sampleRate : 32768U;
}
