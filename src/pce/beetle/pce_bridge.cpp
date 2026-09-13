/* AURORA_PCE_EXPERIMENTAL_V1
 * Beetle PCE Fast libretro bridge for PS2/SNESticle Aurora.
 * HuCard V1: in-memory load, RGB565 video, stereo PCM, five pads, SRAM/state.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include "pce_bridge.h"
#include "types.h"
#include "emuinput.h"
#include "rendersurface.h"
#include "pixelformat.h"
#include "mixbuffer.h"
/* AURORA_PS2_PERF_V4_20260824 */
#include "audmixbuffer.h"
extern AudMixBuffer *_AudMix;
#include "snio.h"
#include "snppurender.h"
extern "C" {
#include "gs.h"
#include "gpprim.h"
#include "gskit_backend.h"
}

extern "C" {
#include "../../third_party/beetle-pce-fast/libretro-common/include/libretro.h"
}

extern "C" {
unsigned PCE_retro_api_version(void);
void PCE_retro_set_environment(retro_environment_t);
void PCE_retro_set_video_refresh(retro_video_refresh_t);
void PCE_retro_set_audio_sample(retro_audio_sample_t);
void PCE_retro_set_audio_sample_batch(retro_audio_sample_batch_t);
void PCE_retro_set_input_poll(retro_input_poll_t);
void PCE_retro_set_input_state(retro_input_state_t);
void PCE_retro_init(void);
void PCE_retro_deinit(void);
bool PCE_retro_load_game(const struct retro_game_info *);
void PCE_retro_unload_game(void);
void PCE_retro_reset(void);
void PCE_retro_run(void);
size_t PCE_retro_serialize_size(void);
bool PCE_retro_serialize(void *, size_t);
bool PCE_retro_unserialize(const void *, size_t);
void *PCE_retro_get_memory_data(unsigned);
size_t PCE_retro_get_memory_size(unsigned);
void PCE_retro_get_system_av_info(struct retro_system_av_info *);
/* AURORA_V15_MULTICORE_SPRITE_LIMIT_20260824: PS2-only renderer hook inside the Beetle fork. */
void PCE_AuroraSetSpriteLimiter(int level, int mode);
void PCE_AuroraSetSkipNextVideoFrame(int skip); /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */
/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830 */
void PCE_AuroraSetCdAudioSafeWindow(int allowed);
int PCE_AuroraConsumeCdAudioRefillRequest(void);
/* AURORA_EXTREME_CD_VIDEO_FIRST_V2_20260830 */
int PCE_AuroraPrefetchCdAudio(void);
/* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
void PCE_AuroraSetCdMusicEnabled(int enabled);
int PCE_AuroraCdMusicEnabled(void);
/* AURORA_PCE_CD_MENU_IO_QUIESCE_V1_20260901 */
int PCE_AuroraCdAsyncPause(unsigned int timeout_ms); /* AURORA_PS2_THREE_BUG_FIX_V1_20260912_PCE_SOFT_PAUSE_BRIDGE */
int PCE_AuroraCdAsyncPauseReady(void); /* AURORA_SSF2_PCE_MENU_FIX_V2_20260913_PCE_ACK_POLL_BRIDGE */
int PCE_AuroraCdAsyncQuiesce(unsigned int timeout_ms);
void PCE_AuroraCdAsyncResume(void);
}

static bool s_Initialized = false;
static bool s_GameLoaded = false;
/* AURORA_V4_11_CD_REALTIME_PACING_PCE_TOC_OFFSETS_20260830 */
static bool s_DiscLoaded = false;
static Emu::SysInputT *s_pInput = NULL;
static CMixBuffer *s_pMix = NULL;
static const void *s_VideoData = NULL;
static unsigned s_VideoW = 0, s_VideoH = 0;
static size_t s_VideoPitch = 0;
static bool s_HaveVideo = false;
static bool s_SkipVideoNext = false;
static bool s_SkipVideoActive = false;
/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830 */
static bool s_CdAudioSafeWindowNext = false;
static bool s_PceDirectPixelsValid = false; /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */
static Uint8 *s_pSramData = NULL;
static Int32 s_SramBytes = 0;
static unsigned s_SampleRate = 32000; /* AURORA_PCE_EXPERIMENTAL_V3 */
/* AURORA_V17_SAFE_PERF_PCE_LIMITER_20260824
 * Limiter settings are global menu state. Avoid two getters plus a
 * core setter on every emulated frame when the values did not change. */
static int s_AuroraLimiterLevel = -1;
static int s_AuroraLimiterMode = -1;
static const void *s_ContentData = NULL;
static size_t s_ContentBytes = 0;
static char s_ContentName[1024] = "game.pce";
static char s_ContentBaseName[1024] = "game";
static char s_ContentExt[16] = "pce";
static struct retro_game_info_ext s_ContentInfoExt;
static const char s_DotPath[] = ".";
/* AURORA_CD_SRAM_NOTICES_20260824 -- user firmware under SNESticle/SYSTEM. */
static char s_SystemPath[1024] = ".";
static char s_ContentDir[1024] = ".";
enum { PCE_AUDIO_CHUNK = 1024 };
static Int16 s_AudioL[PCE_AUDIO_CHUNK], s_AudioR[PCE_AUDIO_CHUNK];

/* AURORA_PCE_EXPERIMENTAL_V3
 * Beetle emits conventional RGB565 (R high, B low). Aurora's 32-bit
 * framebuffer packs R in the low byte and B in bits 16..23, like the
 * existing PicoDrive bridge. Keep three tiny expansion tables hot in cache
 * instead of doing repeated channel expansion arithmetic for every pixel. */
static Uint32 s_PceR[32], s_PceG[64], s_PceB[32];
static bool s_PceColorTablesReady = false;

static void pceInitColorTables(void)
{
    if (s_PceColorTablesReady) return;
    for (unsigned i = 0; i < 32; ++i)
    {
        Uint32 v = (i << 3) | (i >> 2);
        s_PceR[i] = v;
        s_PceB[i] = v << 16;
    }
    for (unsigned i = 0; i < 64; ++i)
    {
        Uint32 v = (i << 2) | (i >> 4);
        s_PceG[i] = v << 8;
    }
    s_PceColorTablesReady = true;
}

static void pceLog(enum retro_log_level level, const char *fmt, ...)
{
    (void)level;
    if (!fmt) return;
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
}

static const char *pceVariable(const char *key)
{
    if (!key) return NULL;
    if (!strcmp(key, "pce_fast_nospritelimit")) return "disabled";
    if (!strcmp(key, "pce_fast_ocmultiplier")) return "1";
    if (!strcmp(key, "pce_fast_frameskip")) return "disabled";
    if (!strcmp(key, "pce_fast_frameskip_threshold")) return "33";
    /* AURORA_PCE_352_WIDTH_BLACKSCREEN_FIX_V3_20260830
     * Beetle uses this numeric option directly as defined_width[1], i.e.
     * the output width for VCE dot-clock mode 1. Zero is not "no overscan":
     * it makes DisplayRect.w == 0, so Aurora rejects video_cb and games
     * using the 352-class mode (e.g. Aoi Blink) render black.
     * Keep Beetle PCE Fast's own documented/default width: 352 pixels. */
    /* AURORA_PCE_DOTCLOCK_342_4TO3_V4_20260830
     * 342 is the Beetle-family recommended 352-mode width when matching
     * 256-mode geometry: it removes only horizontal overscan and leaves a
     * width extremely close to the hardware 4:3 dot-clock relationship. */
    if (!strcmp(key, "pce_fast_hoverscan")) return "352"; /* AURORA_PCE_CRT_OVERSCAN_352_V9_20260830 */
    if (!strcmp(key, "pce_fast_initial_scanline")) return "0";
    if (!strcmp(key, "pce_fast_last_scanline")) return "239";
    if (!strcmp(key, "pce_fast_cdimagecache")) return "disabled";
    if (!strcmp(key, "pce_fast_cdbios")) return "System Card 3";
    if (!strcmp(key, "pce_fast_cddavolume")) return "100";
    if (!strcmp(key, "pce_fast_adpcmvolume")) return "100";
    if (!strcmp(key, "pce_fast_adpcmlp")) return "disabled";
    if (!strcmp(key, "pce_fast_cdpsgvolume")) return "100";
    if (!strcmp(key, "pce_fast_cdignoreerrors")) return "disabled";
    if (!strcmp(key, "pce_fast_cdspeed")) return "1";
    if (!strcmp(key, "pce_fast_turbo_toggling")) return "disabled";
    if (!strcmp(key, "pce_fast_turbo_delay")) return "3";
    if (!strcmp(key, "pce_fast_turbo_toggle_hotkey")) return "disabled";
    if (!strcmp(key, "pce_fast_disable_softreset")) return "disabled";
    if (!strcmp(key, "pce_fast_mouse_sensitivity")) return "1.0";
    if (!strcmp(key, "pce_fast_palette")) return "RGB";
    if (!strncmp(key, "pce_fast_default_joypad_type_p", 30)) return "2 Buttons";
    if (!strncmp(key, "pce_fast_sound_channel_", 23) && strstr(key, "_volume")) return "100";
    return NULL;
}

static bool pceEnvironment(unsigned cmd, void *data)
{
    switch (cmd)
    {
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            if (!data) return false;
            ((struct retro_log_callback *)data)->log = pceLog; return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
            return data && *(enum retro_pixel_format *)data == RETRO_PIXEL_FORMAT_RGB565;
        case RETRO_ENVIRONMENT_GET_VARIABLE:
        {
            struct retro_variable *v = (struct retro_variable *)data;
            if (!v || !v->key) return false;
            v->value = pceVariable(v->key); return v->value != NULL;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            if (!data) return false; *(bool *)data = false; return true;
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
            if (!data) return false; *(const char **)data = s_SystemPath; return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            if (!data) return false; *(const char **)data = s_DotPath; return true;
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            if (data) *(bool *)data = true; return true;
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            if (data) *(unsigned *)data = 0; return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
        case RETRO_ENVIRONMENT_GET_GAME_INFO_EXT:
            if (!data || !s_ContentInfoExt.full_path) return false;
            *(const struct retro_game_info_ext **)data = &s_ContentInfoExt; return true;
        case RETRO_ENVIRONMENT_GET_CURRENT_SOFTWARE_FRAMEBUFFER:
        case RETRO_ENVIRONMENT_GET_VFS_INTERFACE:
        case RETRO_ENVIRONMENT_GET_PERF_INTERFACE:
            return false;
        case RETRO_ENVIRONMENT_SET_VARIABLES:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
        case RETRO_ENVIRONMENT_SET_MESSAGE:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE:
        case RETRO_ENVIRONMENT_SET_MINIMUM_AUDIO_LATENCY:
        case RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK:
            return true;
        default: return false;
    }
}

static void pceVideoRefresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
    /* A skipped Emulate() may still issue libretro video_cb with the
     * old surface. Keep the last known frame/residency untouched. */
    if (s_SkipVideoActive) return;
    if (!data || !w || !h || pitch < w * 2) { s_HaveVideo = false; return; }
    s_VideoData = data; s_VideoW = w; s_VideoH = h; s_VideoPitch = pitch; s_HaveVideo = true;
    s_PceDirectPixelsValid = false;
}

static void pceAudioSample(int16_t l, int16_t r)
{
    if (!s_pMix) return;
    Int16 L = (Int16)l, R = (Int16)r; s_pMix->OutputSamplesStereo(&L, &R, 1);
}

static size_t pceAudioBatch(const int16_t *data, size_t frames)
{
    if (!data || !frames || !s_pMix) return frames;
    if (_AudMix && s_pMix == _AudMix && frames <= 0x7fffffffU)
    {
        size_t directPos = 0;
        while (directPos < frames)
        {
            size_t n = frames - directPos;
            if (n > PCE_AUDIO_CHUNK) n = PCE_AUDIO_CHUNK;
            _AudMix->OutputLibretroInterleaved(
                (const Int16 *)(data + directPos * 2), (Int32)n);
            directPos += n;
        }
        return frames;
    }
    size_t pos = 0;
    while (pos < frames)
    {
        size_t n = frames - pos; if (n > PCE_AUDIO_CHUNK) n = PCE_AUDIO_CHUNK;
        for (size_t i = 0; i < n; ++i)
        {
            s_AudioL[i] = (Int16)data[(pos + i) * 2 + 0];
            s_AudioR[i] = (Int16)data[(pos + i) * 2 + 1];
        }
        s_pMix->OutputSamplesStereo(s_AudioL, s_AudioR, (Int32)n); pos += n;
    }
    return frames;
}

static void pceInputPoll(void) {}
static bool pcePadHas(Uint16 p, Uint16 bit) { return p != EMUSYS_DEVICE_DISCONNECTED && (p & bit); }

static int16_t pceJoyMask(unsigned port)
{
    if (!s_pInput || port >= EMUSYS_DEVICE_NUM) return 0;
    Uint16 p = s_pInput->uPad[port]; if (p == EMUSYS_DEVICE_DISCONNECTED) return 0;
    unsigned m = 0;
    if (pcePadHas(p, SNESIO_JOY_UP))     m |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
    if (pcePadHas(p, SNESIO_JOY_DOWN))   m |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
    if (pcePadHas(p, SNESIO_JOY_LEFT))   m |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
    if (pcePadHas(p, SNESIO_JOY_RIGHT))  m |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
    if (pcePadHas(p, SNESIO_JOY_Y))      m |= 1u << RETRO_DEVICE_ID_JOYPAD_A; /* I */
    if (pcePadHas(p, SNESIO_JOY_B))      m |= 1u << RETRO_DEVICE_ID_JOYPAD_B; /* II */
    if (pcePadHas(p, SNESIO_JOY_SELECT)) m |= 1u << RETRO_DEVICE_ID_JOYPAD_SELECT;
    if (pcePadHas(p, SNESIO_JOY_START))  m |= 1u << RETRO_DEVICE_ID_JOYPAD_START;
    return (int16_t)m;
}

static int16_t pceInputState(unsigned port, unsigned device, unsigned index, unsigned id)
{
    (void)index;
    if (device != RETRO_DEVICE_JOYPAD || port >= EMUSYS_DEVICE_NUM) return 0;
    int16_t mask = pceJoyMask(port);
    if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return mask;
    if (id > RETRO_DEVICE_ID_JOYPAD_R3) return 0;
    return (mask & (1u << id)) ? 1 : 0;
}

static inline Uint32 pce565(Uint16 c)
{
    /* RGB565: RRRRRGGGGGGBBBBB. Aurora RGBA word: AABBGGRR. */
    return 0xff000000u |
           s_PceR[(c >> 11) & 31u] |
           s_PceG[(c >> 5) & 63u] |
           s_PceB[c & 31u];
}

static inline void pce565parts(Uint16 c, Uint32 *r, Uint32 *g, Uint32 *b)
{
    *r = s_PceR[(c >> 11) & 31u];
    *g = s_PceG[(c >> 5) & 63u] >> 8;
    *b = s_PceB[c & 31u] >> 16;
}

static void pceRender(CRenderSurface *target)
{
    if (!target || !s_HaveVideo || !s_VideoData) return;

    PixelFormatT *fmt = target->GetFormat();
    if (!fmt || fmt->uBitDepth != 32) return;

    pceInitColorTables();

    const int tw = (int)target->GetWidth();
    const int th = (int)target->GetHeight();
    if (tw <= 0 || th <= 0) return;

    const int visH = th < 240 ? th : 240;
    const int srcH = (int)s_VideoH;
    const int cropY = srcH > visH ? (srcH - visH) / 2 : 0;
    const int dstH = srcH > visH ? visH : srcH;
    const int dstY = (visH - dstH) / 2;
    const int sw = (int)s_VideoW;
    const Uint32 black = 0xff000000u;

    /* Fast path for the overwhelmingly common PCE mode (256x240).
     * Avoid the old full-row black clear + generic geometry checks, which
     * nearly doubled EE framebuffer writes on every frame. */
    if (sw == tw && dstH == visH && dstY == 0)
    {
        for (int y = 0; y < visH; ++y)
        {
            Uint32 *dst = (Uint32 *)target->GetLinePtr(y);
            if (!dst) continue;
            const Uint16 *src = (const Uint16 *)(
                (const Uint8 *)s_VideoData +
                (size_t)(cropY + y) * s_VideoPitch);

            int x = 0;
            for (; x + 3 < tw; x += 4)
            {
                dst[x + 0] = pce565(src[x + 0]);
                dst[x + 1] = pce565(src[x + 1]);
                dst[x + 2] = pce565(src[x + 2]);
                dst[x + 3] = pce565(src[x + 3]);
            }
            for (; x < tw; ++x)
                dst[x] = pce565(src[x]);
        }
        return;
    }

    /* Generic path for narrow/wide PCE modes. Only clear visible rows. */
    for (int y = 0; y < visH; ++y)
    {
        Uint32 *dst = (Uint32 *)target->GetLinePtr(y);
        if (!dst) continue;

        if (y < dstY || y >= dstY + dstH)
        {
            for (int x = 0; x < tw; ++x) dst[x] = black;
            continue;
        }

        const int sy = cropY + (y - dstY);
        const Uint16 *src = (const Uint16 *)(
            (const Uint8 *)s_VideoData + (size_t)sy * s_VideoPitch);

        if (sw <= tw)
        {
            const int dx = (tw - sw) / 2;
            for (int x = 0; x < dx; ++x) dst[x] = black;
            for (int x = 0; x < sw; ++x) dst[dx + x] = pce565(src[x]);
            for (int x = dx + sw; x < tw; ++x) dst[x] = black;
        }
        else
        {
            /* AURORA_PCE_DOTCLOCK_342_4TO3_V4_20260830
             *
             * PC Engine dot-clock mode 1 is ~4/3 the sampling rate of the
             * common 256-class mode. Once Beetle has cropped the nominal
             * 352-wide line to its recommended 342-pixel visible window,
             * map it with a fixed 4-source -> 3-destination cadence.
             *
             * This avoids the long-period column-width wobble produced by
             * generic 342/256 nearest scaling. No RGB interpolation: source
             * colours remain exact. The normal 256-wide fast path is intact.
             */
            if (sw == 342 && tw == 256)
            {
                for (int x = 0; x < 256; ++x)
                {
                    /* Centre-sampled exact 4:3 dot-clock phase:
                     * sx = floor((x + 0.5) * 4 / 3). */
                    int sx = (x * 4 + 2) / 3;
                    if (sx >= sw) sx = sw - 1;
                    dst[x] = pce565(src[sx]);
                }
            }
            else
            {
                /* Fallback for any other unusual width. */
                for (int x = 0; x < tw; ++x)
                {
                    const uint64_t num =
                        ((uint64_t)(unsigned)x * 2u + 1u) *
                        (uint64_t)(unsigned)sw;
                    int sx = (int)(num /
                        ((uint64_t)(unsigned)tw * 2u));
                    if (sx >= sw) sx = sw - 1;
                    dst[x] = pce565(src[sx]);
                }
            }
        }
    }
}

static void pceBuildInfo(const char *name, const void *data, size_t bytes)
{
    const char *base;
    const char *base2;
    const char *dot;
    size_t n;
    size_t dirBytes;

    if (name && *name)
    {
        strncpy(s_ContentName, name, sizeof(s_ContentName) - 1);
        s_ContentName[sizeof(s_ContentName) - 1] = 0;
    }
    else
        strcpy(s_ContentName, "game.pce");

    base = strrchr(s_ContentName, '/');
    base2 = strrchr(s_ContentName, '\\');
    if (!base || (base2 && base2 > base))
        base = base2;

    if (base)
    {
        dirBytes = (size_t)((base + 1) - s_ContentName);
        if (dirBytes >= sizeof(s_ContentDir))
            dirBytes = sizeof(s_ContentDir) - 1;
        memcpy(s_ContentDir, s_ContentName, dirBytes);
        s_ContentDir[dirBytes] = 0;
        base++;
    }
    else
    {
        strcpy(s_ContentDir, ".");
        base = s_ContentName;
    }

    dot = strrchr(base, '.');
    n = (dot && dot > base) ? (size_t)(dot - base) : strlen(base);
    if (n >= sizeof(s_ContentBaseName))
        n = sizeof(s_ContentBaseName) - 1;
    memcpy(s_ContentBaseName, base, n);
    s_ContentBaseName[n] = 0;

    if (dot && dot[1])
    {
        strncpy(s_ContentExt, dot + 1, sizeof(s_ContentExt) - 1);
        s_ContentExt[sizeof(s_ContentExt) - 1] = 0;
    }
    else
        strcpy(s_ContentExt, "pce");

    s_ContentData = data;
    s_ContentBytes = bytes;
    memset(&s_ContentInfoExt, 0, sizeof(s_ContentInfoExt));
    s_ContentInfoExt.full_path = s_ContentName;
    s_ContentInfoExt.dir = s_ContentDir;
    s_ContentInfoExt.name = s_ContentBaseName;
    s_ContentInfoExt.ext = s_ContentExt;
    s_ContentInfoExt.data = data;
    s_ContentInfoExt.size = bytes;
    s_ContentInfoExt.file_in_archive = false;
    s_ContentInfoExt.persistent_data = data && bytes;
}

bool PceBridge_Init(void)
{
    if (s_Initialized) return true;
    PCE_retro_set_environment(pceEnvironment); PCE_retro_set_video_refresh(pceVideoRefresh);
    PCE_retro_set_audio_sample(pceAudioSample); PCE_retro_set_audio_sample_batch(pceAudioBatch);
    PCE_retro_set_input_poll(pceInputPoll); PCE_retro_set_input_state(pceInputState);
    if (PCE_retro_api_version()!=RETRO_API_VERSION) { printf("[PCE] libretro API mismatch\n"); return false; }
    PCE_retro_init(); s_Initialized=true; return true;
}

void PceBridge_Shutdown(void)
{
    if (!s_Initialized) return; PceBridge_UnloadGame(); PCE_retro_deinit(); s_Initialized=false;
}

bool PceBridge_LoadGame(const void *data, size_t bytes, size_t capacity, const char *name)
{
    (void)capacity;
    if (!data || !bytes || bytes > (size_t)(4U*1024U*1024U+512U) || !PceBridge_Init()) return false;
    if (s_GameLoaded)
        PceBridge_UnloadGame();
    s_DiscLoaded = false;
    pceBuildInfo(name,data,bytes);
    struct retro_game_info info; memset(&info,0,sizeof(info)); info.path=s_ContentName; info.data=data; info.size=bytes;
    if (!PCE_retro_load_game(&info)) { printf("[PCE] retro_load_game failed: %s (%u bytes)\n",s_ContentName,(unsigned)bytes); s_ContentData=NULL;s_ContentBytes=0;return false; }
    s_GameLoaded=true; size_t sb=PCE_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM); void *sp=PCE_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    s_pSramData=(sp&&sb&&sb<=0x7fffffffU)?(Uint8*)sp:NULL; s_SramBytes=s_pSramData?(Int32)sb:0;
    struct retro_system_av_info av; memset(&av,0,sizeof(av)); PCE_retro_get_system_av_info(&av);
    if (av.timing.sample_rate>=8000.0 && av.timing.sample_rate<=96000.0) s_SampleRate=(unsigned)(av.timing.sample_rate+0.5);
    printf("[PCE] loaded %s; SRAM=%d; audio=%u Hz\n",s_ContentName,(int)s_SramBytes,s_SampleRate); return true;
}

/* AURORA_CD_SRAM_NOTICES_20260824 */
bool PceBridge_LoadDisc(const char *path, const char *systemPath)
{
    struct retro_game_info info;
    struct retro_system_av_info av;
    size_t sb;
    void *sp;

    if (!path || !*path || !systemPath || !*systemPath ||
        strlen(path) >= sizeof(s_ContentName) ||
        strlen(systemPath) >= sizeof(s_SystemPath))
        return false;

    if (s_GameLoaded)
        PceBridge_UnloadGame();
    s_DiscLoaded = false;

    /* Beetle caches RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY in retro_init. */
    if (s_Initialized)
    {
        PCE_retro_deinit();
        s_Initialized = false;
    }
    strcpy(s_SystemPath, systemPath);
    if (!PceBridge_Init())
        return false;

    pceBuildInfo(path, NULL, 0);
    memset(&info, 0, sizeof(info));
    info.path = s_ContentName;
    info.data = NULL;
    info.size = 0;
    if (!PCE_retro_load_game(&info))
    {
        printf("[PCE/CD] load failed: %s (SYSTEM=%s)\n",
               s_ContentName, s_SystemPath);
        s_ContentData = NULL;
        s_ContentBytes = 0;
        return false;
    }

    s_GameLoaded = true;
    s_DiscLoaded = true;
    sb = PCE_retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
    sp = PCE_retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
    s_pSramData = (sp && sb && sb <= 0x7fffffffU) ? (Uint8 *)sp : NULL;
    s_SramBytes = s_pSramData ? (Int32)sb : 0;

    memset(&av, 0, sizeof(av));
    PCE_retro_get_system_av_info(&av);
    if (av.timing.sample_rate >= 8000.0 && av.timing.sample_rate <= 96000.0)
        s_SampleRate = (unsigned)(av.timing.sample_rate + 0.5);

    printf("[PCE/CD] loaded %s; SRAM=%d; audio=%u Hz; SYSTEM=%s\n",
           s_ContentName, (int)s_SramBytes, s_SampleRate, s_SystemPath);
    return true;
}


/* AURORA_SSF2_PCECD_LAZY_GPSP_JIT_V1_20260912_PCE_TEARDOWN
 * A CD worker may still be finishing an 8 KiB read when the frontend asks
 * Beetle to unload/swap content.  Teardown is different from opening the
 * menu: it must not resume and free the old disc underneath an in-flight
 * read.  Keep requesting the core's real quiesce barrier until it acknowledges
 * idle, unload, then release the pause for the next content. */
static void pceQuiesceDiscForTeardown(void)
{
    unsigned waits = 0;
    if (!s_Initialized || !s_GameLoaded || !s_DiscLoaded)
        return;

    while (!PCE_AuroraCdAsyncQuiesce(250U))
    {
        ++waits;
        if (waits == 4U || (waits > 4U && (waits & 15U) == 0U))
            printf("[PCE/CD] waiting for async worker before teardown (%u)\n", waits);
    }
}

void PceBridge_UnloadGame(void)
{
    const bool wasDisc = s_Initialized && s_GameLoaded && s_DiscLoaded;
    if (s_Initialized && s_GameLoaded)
    {
        if (wasDisc)
            pceQuiesceDiscForTeardown();
        PCE_retro_unload_game();
        if (wasDisc)
            PCE_AuroraCdAsyncResume();
    }
    s_GameLoaded=false;s_DiscLoaded=false;s_AuroraLimiterLevel=s_AuroraLimiterMode=-1;s_pInput=NULL;s_pMix=NULL;s_pSramData=NULL;s_SramBytes=0;s_VideoData=NULL;s_VideoW=s_VideoH=0;s_VideoPitch=0;s_HaveVideo=false;s_ContentData=NULL;s_ContentBytes=0;
    s_SkipVideoNext=false;s_SkipVideoActive=false;s_PceDirectPixelsValid=false;
}
bool PceBridge_IsDiscLoaded(void)
{
    return s_GameLoaded && s_DiscLoaded;
}

/* AURORA_CD_STATE_V1_SAFE_20260903 */
const char *PceBridge_GetDiscPath(void)
{
    return PceBridge_IsDiscLoaded() ? s_ContentName : NULL;
}

/* AURORA_SSF2_PCE_MENU_FIX_V2_20260913_PCE_ACK_POLL_BRIDGE
 * Normal-menu entry must not wait on storage. Arm the pause request and let
 * the frontend publish the menu immediately. Filesystem work in the menu is
 * separately gated on PceBridge_DiscIOPaused(). Hard teardown still waits. */
bool PceBridge_QuiesceDiscIO(void)
{
    if (!s_GameLoaded || !s_DiscLoaded) return true;
    (void)PCE_AuroraCdAsyncPause(0U);
    return true;
}

bool PceBridge_DiscIOPaused(void)
{
    if (!s_GameLoaded || !s_DiscLoaded) return true;
    return PCE_AuroraCdAsyncPauseReady() != 0;
}

void PceBridge_ResumeDiscIO(void)
{
    PCE_AuroraCdAsyncResume();
}

void PceBridge_Reset(void){if(s_GameLoaded)PCE_retro_reset();}
void PceBridge_SoftReset(void){if(s_GameLoaded)PCE_retro_reset();}
/* AURORA_PCE_EXPERIMENTAL_V10_DIRECT_GS
 * AURORA_PCE_V10_DIRECT_GS_FORMAT_FIX_R1
 * AURORA_PCE_EXPERIMENTAL_V11_DIRECT_GS_SAFE
 * AURORA_PCE_EXPERIMENTAL_V12_NATIVE_GS_COLOR
 *
 * V12 keeps V11's private GS allocation, but removes V11's full-frame
 * RGB565->GS staging conversion.  Under AURORA_PS2_PCE_FAST the Beetle VCE
 * palette now emits GS-native A1B5G5R5 directly, so the core framebuffer can
 * be DMA-uploaded exactly like V10, without corrupting other Aurora VRAM.
 *
 * Vertical presentation is also corrected: a 240-line PCE frame maps to all
 * 240 logical output lines.  V10/V11 incorrectly preserved the old generic
 * y=4 coordinate by shrinking 240 source rows into 236 destination rows,
 * which necessarily dropped rows throughout the picture.
 */
enum
{
    PCE_GS_MAX_W = 512,
    PCE_GS_MAX_H = 240,
    PCE_GS_DIRECT_BYTES = PCE_GS_MAX_W * PCE_GS_MAX_H * 2
};

static Uint32 s_PceDirectTexTBP = 0;

void PceBridge_InvalidateGsResources(void)
{
    s_PceDirectTexTBP = 0;
    s_PceDirectPixelsValid = false;
}

/* AURORA_PCE_NATIVE_GS_RASTER_V5_20260830
 * Beetle V4 presents common PCE as 256, high dot-clock as 342, and may
 * expose the 512-dot class. Return only GS framebuffer widths Aurora owns. */
int PceBridge_GetNative240pRasterWidth(void)
{
    if (!s_HaveVideo || !s_VideoData || !s_VideoW)
        return 256;
    if (s_VideoW > 352u)
        return 512;
    if (s_VideoW > 256u)
        return 352;
    return 256;
}

/* AURORA_PCE_KRAZY_RUNTIME_DIAG_V11R3_20260830 */
void PceBridge_GetVideoDebug(unsigned *w, unsigned *h, unsigned *pitchPixelsOut,
                             int *fbw, int *nativeClass)
{
    const unsigned pp = (unsigned)(s_VideoPitch >> 1);
    const int physicalW = (int)(256.0f * GPPrimGetScaleX() + 0.5f);
    const int native =
        (GSK_GetActiveVideoMode() == GSK_VIDMODE_240P &&
         physicalW == 512 &&
         (s_VideoW == 256u || s_VideoW == 352u || s_VideoW == 512u)) ? 1 : 0;

    if (w)              *w = s_VideoW;
    if (h)              *h = s_VideoH;
    if (pitchPixelsOut) *pitchPixelsOut = pp;
    if (fbw)            *fbw = physicalW;
    if (nativeClass)    *nativeClass = native;
}

bool PceBridge_CanDirectGsVideo(void)
{
    size_t pitchPixels;

    if (!s_GameLoaded || !s_HaveVideo || !s_VideoData ||
        !s_VideoW || !s_VideoH || (s_VideoPitch & 1u))
        return false;

    pitchPixels = s_VideoPitch >> 1;

    if (pitchPixels < s_VideoW || pitchPixels > PCE_GS_MAX_W ||
        s_VideoW > PCE_GS_MAX_W || s_VideoH > 256u)
        return false;

    return true;
}

bool PceBridge_DrawDirectGs(Uint32 auroraOutBaseTBP, Float32 intensity)
{
    const Uint16 *src;
    unsigned pitchPixels, cropY, srcH, srcW;
    unsigned texLog2, texTBW;
    int logicalX, logicalY, logicalW, logicalH;
    int fbW, fbH;
    int dstX, dstY, dstW, dstH;
    Uint32 black, modColor, mod;

    /* AURORA_ALLCORES_PERF_V5_20260824 -- PCE reuses Aurora's reserved base slab. */
    if (!auroraOutBaseTBP)
        return false;

    if (!PceBridge_CanDirectGsVideo())
        return false;

    pitchPixels = (unsigned)(s_VideoPitch >> 1);
    srcW = s_VideoW;
    srcH = s_VideoH;

    cropY = srcH > 240u ? (srcH - 240u) / 2u : 0u;
    if (srcH > 240u)
        srcH = 240u;

    if (s_PceDirectTexTBP != auroraOutBaseTBP)
        s_PceDirectPixelsValid = false;
    s_PceDirectTexTBP = auroraOutBaseTBP;

    /* libretro's PCE surface is pitchPixels wide in memory.  Uploading the
     * native stride avoids any EE repack/copy even when the active image is
     * only 256 pixels inside a 512-pixel backing surface. */
    src = (const Uint16 *)((const Uint8 *)s_VideoData +
                           (size_t)cropY * s_VideoPitch);

    texTBW = (pitchPixels + 63u) & ~63u;
    texLog2 = 5u;
    while ((1u << texLog2) < pitchPixels && texLog2 < 9u)
        ++texLog2;

    if (!s_PceDirectPixelsValid)
    {
        GPPrimUploadTexture(
            (int)s_PceDirectTexTBP, (int)texTBW,
            0, 0, GS_PSMCT16,
            (void *)src, (int)pitchPixels, (int)srcH);
        s_PceDirectPixelsValid = true;
    }

    GPPrimSetTex(
        s_PceDirectTexTBP, texTBW, texLog2, 8,
        GS_PSMCT16, 0, 0, GS_PSMCT16, 0);

    logicalH = (int)srcH;
    logicalY = (240 - logicalH) / 2;

    fbW = (int)(256.0f * GPPrimGetScaleX() + 0.5f);
    fbH = (int)(240.0f * GPPrimGetScaleY() + 0.5f);
    if (fbW <= 0 || fbH <= 0)
        return false;

    /* AURORA_PCE_FIXED512_DBX0_CUMULATIVE_V8_20260830
     * PCE 240p lives in one 512-wide aligned GS framebuffer. Draw the source
     * 1:1, centred, and crop scanout to exactly that source window.
     *
     * No column is duplicated, discarded or interpolated. A mode switch only
     * changes dstX/DBX/DISPLAY geometry; VRAM/framebuffers stay intact.
     */
    if (GSK_GetActiveVideoMode() == GSK_VIDMODE_240P &&
        fbW == 512 &&
        (srcW == 256u || srcW == 352u || srcW == 512u))
    {
        logicalW = (int)srcW;
        logicalX = 0;

        /* AURORA_PCE_FIXED512_DBX0_CUMULATIVE_V8_20260830
         * Keep native PCE samples at framebuffer X=0.  Physical centering
         * belongs to PCRTC DISPLAY, not DISPFB panning.  Keeping DBX=0 also
         * avoids the live right-edge echo observed with 256/342 windows. */
        dstX = 0;
        dstW = (int)srcW;

        GSK_Set240pVisibleWindow(0, (int)srcW);
    }
    else
    {
        if (srcW <= 256u)
        {
            logicalW = (int)srcW;
            logicalX = (256 - logicalW) / 2;
        }
        else
        {
            logicalW = 256;
            logicalX = 0;
        }

        dstX = (fbW * logicalX + 128) / 256;
        dstW = (fbW * logicalW + 128) / 256;
    }

    /* V12 scaling fix: use the complete 240-line destination domain.
     * For a 240-line source this is exactly 1:1 at a 240p framebuffer,
     * so no periodically missing source rows can occur. */
    dstY = (fbH * logicalY + 120) / 240;
    dstH = (fbH * logicalH + 120) / 240;

    black = 0x80000000u;
    GPPrimRect(0, 0, black,
               256u << 4, 240u << 4, black,
               0, 0);

    if (intensity < 0.0f)
        intensity = 0.0f;
    if (intensity > 1.0f)
        intensity = 1.0f;

    mod = (Uint32)(128.0f * intensity + 0.5f);
    if (mod > 128u)
        mod = 128u;

    modColor = 0x80000000u | (mod << 16) | (mod << 8) | mod;

    /* AURORA_PCE_MODECLOCK_APERTURE_HALFTEXEL_V10_20260830
     * Sample texel centres, matching Aurora's proven QuickNES direct-GS path.
     * With NEAREST this removes boundary ambiguity without scaling/repacking:
     * native 1:1 modes still map one source texel to one framebuffer pixel. */
    GPPrimTexRectAbs(
        (Uint32)(dstX << 4), (Uint32)(dstY << 4),
        8u, 8u,
        (Uint32)((dstX + dstW) << 4),
        (Uint32)((dstY + dstH) << 4),
        (Uint32)(srcW << 4) + 8u, (Uint32)(srcH << 4) + 8u,
        0, modColor, 0);

    return true;
}

void PceBridge_SetSkipVideo(bool skip)
{
    s_SkipVideoNext = skip;
}

/* AURORA_EXTREME_CD_VIDEO_FIRST_V1_20260830 */
void PceBridge_SetCdAudioSafeWindow(bool allowed)
{
    s_CdAudioSafeWindowNext = allowed;
}

bool PceBridge_ConsumeCdAudioRefillRequest(void)
{
    return PCE_AuroraConsumeCdAudioRefillRequest() != 0;
}

/* AURORA_CD_MUSIC_REDBOOK_V3_20260830 */
void PceBridge_SetCdMusicEnabled(bool enabled)
{
    PCE_AuroraSetCdMusicEnabled(enabled ? 1 : 0);
}

bool PceBridge_GetCdMusicEnabled(void)
{
    return PCE_AuroraCdMusicEnabled() != 0;
}

void PceBridge_RunFrame(Emu::SysInputT *input, CRenderSurface *target, CMixBuffer *mix)
{
    const bool requestedSkip = s_SkipVideoNext;
    s_SkipVideoNext = false;
    s_CdAudioSafeWindowNext = false;

    if (!s_GameLoaded)
        return;

    /* Never skip before a valid previous image exists. Beetle itself
     * normally protects its first frame for exactly this reason. */
    const bool skipVideo = requestedSkip && s_HaveVideo && s_VideoData;
    s_pInput = input;
    s_pMix = mix;
    if (!skipVideo)
        s_HaveVideo = false;
    s_SkipVideoActive = skipVideo;
    /* AURORA_V3_SAFE_PCE_ONESHOT_SKIP_20260828 */
    if (skipVideo)
        PCE_AuroraSetSkipNextVideoFrame(1);

    {
        const int limiterLevel = (int)SNPPURenderGetObjLimitLevel();
        const int limiterMode = (int)SNPPURenderGetObjLimitMode();
        if (limiterLevel != s_AuroraLimiterLevel ||
            limiterMode != s_AuroraLimiterMode)
        {
            PCE_AuroraSetSpriteLimiter(limiterLevel, limiterMode);
            s_AuroraLimiterLevel = limiterLevel;
            s_AuroraLimiterMode = limiterMode;
        }
    }

    /* AURORA_ASYNC_CDDA_VIDEO_ABSOLUTE_V4_20260830
     * CDDA has no main-thread prefetch path anymore. */
    PCE_AuroraSetCdAudioSafeWindow(0);
    PCE_retro_run();
    PCE_AuroraSetCdAudioSafeWindow(0);
    s_SkipVideoActive = false;

    /* V10: ordinary Beetle RGB565 geometry bypasses the RGBA32 surface. */
    if (!skipVideo && s_HaveVideo && !PceBridge_CanDirectGsVideo())
        pceRender(target);

    if (s_pMix)
        s_pMix->Flush();

    s_pMix = NULL;
    s_pInput = NULL;
}
int PceBridge_GetStateSize(void){if(!s_GameLoaded)return 0;size_t n=PCE_retro_serialize_size();return n&&n<=0x7fffffffU?(int)n:0;}
int PceBridge_SaveState(void *data,int bytes){int n=PceBridge_GetStateSize();return data&&n>0&&bytes>=n&&PCE_retro_serialize(data,(size_t)n)?n:0;}
bool PceBridge_LoadState(const void *data,int bytes){return data&&bytes>0&&PCE_retro_unserialize(data,(size_t)bytes);}
int PceBridge_GetSRAMBytes(void){return s_GameLoaded?s_SramBytes:0;}
Uint8 *PceBridge_GetSRAMData(void){return s_GameLoaded?s_pSramData:NULL;}
unsigned PceBridge_GetSampleRate(void){return s_SampleRate;}

/* AURORA_V4_4_BUILD_FIX_32X_VIDEO_FIRST_20260830 */

/* AURORA_V4_11_CD_REALTIME_PACING_PCE_TOC_OFFSETS_20260830 */
