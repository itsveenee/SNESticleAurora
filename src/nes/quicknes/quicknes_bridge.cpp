/* AURORA_VOLUME_TFA_N163_V4_20260913 */
/* SNESTICLE_QUICKNES_BRIDGE
 * SNESTICLE_QUICKNES_NATIVE_DIRECT_V1
 *
 * Direct QuickNES Nes_Emu integration for SNESticle/PS2.
 *
 * IMPORTANT: This intentionally does NOT call the libretro frontend at
 * runtime. The QuickNES archive still contains libretro.o, but normal static
 * archive linking only extracts objects referenced by SNESticle. Since this
 * file references Nes_Emu directly and no retro_* symbol, libretro.o stays
 * out of the final ELF.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <libpad.h> /* AURORA_QN_LIGHTGUN_TIMING_V7_2_20260829 */

#include "quicknes_bridge.h"
#include "types.h"
#include "emuinput.h"
#include "rendersurface.h"
#include "pixelformat.h"
#include "mixbuffer.h"
#include "snio.h"
#include "snppurender.h"
#include "input.h" /* AURORA_QN_ARKANOID_ANALOG_V2_20260828 */

/* AURORA_ALLCORES_PERF_V5_20260824 */
extern "C" {
#include "gs.h"
#include "gpprim.h"
}
/* SNESTICLE_QUICKNES_PS2_ALIGNMENT_V1
 * Keep the bridge under the same unaligned-access contract used by
 * QuickNES's PS2 objects. */
#ifndef NO_UNALIGNED_ACCESS
#define NO_UNALIGNED_ACCESS 1
#endif

#include "Nes_Emu.h"
#include "Nes_Buffer.h"
#include "Data_Reader.h"
#include "abstract_file.h"
#include "nes_ntsc.h" /* AURORA_FCEUMM_FDS_V4_TURBO_PAL_PERF_20260827 */

extern "C" void quicknes_snesticle_set_duty_swap(int enable);
extern "C" void quicknes_snesticle_set_microphone(int enable); /* AURORA_FAMICOM_MIC_CFG41_20260828 */

/* AURORA_QN_EXT_HOST_V2_20260828 */
extern "C" void quicknes_snesticle_ext_set_arkanoid(int enable);
extern "C" void quicknes_snesticle_ext_set_turbofile(int enable); /* AURORA_CD_AUDIO_STREAM_V3_NES_HOST_API_20260829 */
extern "C" void quicknes_snesticle_ext_set_battlebox(int enable); /* AURORA_QN_BATTLEBOX_V5_20260829 */
extern "C" void quicknes_snesticle_ext_set_lightgun(int mode); /* AURORA_QN_LIGHTGUN_CURSOR_V7_20260829 */
extern "C" void quicknes_snesticle_ext_set_lightgun_state(int x, int y, int trigger, int offscreen);
extern "C" void quicknes_snesticle_ext_set_arkanoid_state(unsigned int paddle, int fire);
extern "C" void quicknes_snesticle_ext_reset_bus(void);
extern "C" unsigned char *quicknes_snesticle_ext_turbofile_data(void);
extern "C" int quicknes_snesticle_ext_turbofile_dirty(void);
extern "C" void quicknes_snesticle_ext_turbofile_clear_dirty(void);
extern "C" unsigned char *quicknes_snesticle_ext_battlebox_data(void);
extern "C" int quicknes_snesticle_ext_battlebox_dirty(void);
extern "C" void quicknes_snesticle_ext_battlebox_clear_dirty(void);

static bool s_Initialized = false;
static bool s_GameLoaded  = false;
static bool s_DutySwap    = false;
static bool s_TurboPhase  = false;
static bool s_SkipVideoNext = false; /* AURORA_SAFE_FRAMESKIP_GG_ZOOM_V2_2 */
static bool s_ArkanoidVaus = false; /* AURORA_QN_EXT_HOST_V2_20260828 */
static bool s_TurboFileEnabled = false; /* AURORA_CD_AUDIO_STREAM_V3_NES_HOST_FLAG_20260829 */
static bool s_BattleBoxEnabled = false; /* AURORA_QN_BATTLEBOX_V5_20260829 */

/* AURORA_VOLUME_TFA_N163_V4_20260913
 * Namco 163 has 128 bytes of internal RAM which can itself be battery-backed,
 * independently of optional external 8 KiB WRAM.  Legacy iNES cannot express
 * that distinction.  Keep a tiny aligned persistence mirror and manipulate
 * only the MAPR snapshot at explicit save/load boundaries; no QuickNES
 * submodule modification and no per-frame state serialization. */
enum
{
    QN_N163_INTERNAL_RAM_BYTES = 0x80,
    QN_N163_MAPPER_STATE_BYTES = 192,
    QN_N163_INTERNAL_RAM_OFFSET = 20
};
static bool s_N163InternalBatterySave = false;
static Uint8 s_N163SaveMirror[QN_N163_INTERNAL_RAM_BYTES]
    __attribute__((aligned(64)));
static Uint8 s_N163StateScratch[QUICKNES_STATE_CAPACITY]
    __attribute__((aligned(64)));

/* AURORA_QN_LIGHTGUN_CURSOR_V7_20260829 */
enum
{
    QN_GUN_NONE = 0,
    QN_GUN_NES_PORT2 = 1,
    QN_GUN_FAMICOM_EXT = 2,
    QN_GUN_TWO_NES = 3
};
/* AURORA_PCE_SCALING_LIGHTGUN_TOGGLE_V2_20260830
 * User preference is separate from CRC detection so Off/On works live.
 * Default is On; only qLightGunModeForCrc() may detect a gun. */
static bool s_LightGunEnabled = true;
static int s_LightGunDetectedMode = QN_GUN_NONE;
static int s_LightGunMode = QN_GUN_NONE;
static Int32 s_GunX = 128 << 8;
static Int32 s_GunY = 120 << 8;
static Int32 s_GunVX = 0;
static Int32 s_GunVY = 0;
/* AURORA_CONTROLLER_OPTIONS_V2: Max/Half/Quarter cadence. */
static unsigned s_TurboSpeedShift = 0;
static uint32_t s_TurboFrame = 0;

/* AURORA_V18_SAFE_PERF_QUICKNES_LIMITER_CACHE_20260825
 * set_sprite_limits() only writes the two rendering-limit fields after
 * clamping. Cache the exact final arguments and touch the core only when
 * those derived budgets really change. */
static int s_LastSpriteScanlineLimit = -1;
static int s_LastSpriteScreenLimit = -1;

/* Reuse one native emulator for the process lifetime. Its constructor does
 * not allocate the cart/audio buffers; those are initialized lazily by
 * QuicknesBridge_Init()/LoadGame(). Reuse avoids libretro's global
 * current_buffer lifecycle entirely. */
static Nes_Emu    *s_pEmu = NULL;
static Nes_Buffer *s_pAudioBuffer = NULL;

enum
{
    QN_VIDEO_W = Nes_Emu::buffer_width,
    /* This matches QuickNES's own libretro software frontend allocation. */
    QN_VIDEO_H = Nes_Emu::image_height + 2,
    QN_AUDIO_MAX = 4096
};

/* QuickNES renders palette indices into this buffer. The +16 horizontal
 * padding and +2 vertical rows are required by its native PPU renderer. */
/* Keep eight bytes before the QuickNES allocation so the active
 * frame pointer (base + one 272-byte guard row + left 8) is
 * 16-byte aligned for the GIF DMA source. */
static Uint8 s_Video[QN_VIDEO_W * QN_VIDEO_H + 16]
    __attribute__((aligned(64)));

/* SNESticle's framebuffer is 256x256 RGBA8. Cache the 256-entry mapping
 * from QuickNES host-palette indices to native RGBA8 words. */
static Uint32 s_RgbaPalette[256];
static short  s_LastFramePalette[Nes_Emu::max_palette_size];
static bool   s_PaletteValid = false;

/* AURORA_FCEUMM_FDS_V4_TURBO_PAL_PERF_20260827: raw 64xRGB .pal expanded by QuickNES's own nes_ntsc. */
static bool s_CustomPaletteValid = false;
static Uint8 s_CustomBasePalette[64 * 3];
static Uint8 s_CustomExpandedPalette[Nes_Emu::color_table_size * 3];
static void qGetRgb(unsigned ci, Uint8 *r, Uint8 *g, Uint8 *b)
{
    if (ci >= (unsigned)Nes_Emu::color_table_size) ci = 0;
    if (s_CustomPaletteValid)
    {
        const Uint8 *p = s_CustomExpandedPalette + ci * 3;
        *r=p[0]; *g=p[1]; *b=p[2];
    }
    else
    {
        const Nes_Emu::rgb_t &rgb = Nes_Emu::nes_colors[ci];
        *r=rgb.red; *g=rgb.green; *b=rgb.blue;
    }
}


/* AURORA_ALLCORES_PERF_V5_20260824
 * Direct indexed-video state. Palette entries are kept in GS
 * CSM1 order; texture and CLUT live in Aurora's reserved output slab. */
static Uint32 s_GsPalette[256] __attribute__((aligned(64)));
static short s_DirectLastPalette[Nes_Emu::max_palette_size];
static bool s_DirectPaletteValid = false;
static bool s_DirectClutResident = false;
static bool s_DirectReady = false;
static Uint32 s_DirectFrameSerial = 0;
static Uint32 s_DirectUploadSerial = 0;
/* Audio scratch is BSS/static, never EE thread stack. QuickNES's default
 * non-linear Nes_Buffer emits mono; SNESticle's AudMixBuffer duplicates it
 * to stereo and performs the existing 32 -> 48 kHz conversion. */
/* AURORA_V3_SAFE_QN_DIRECT_AUDIO_DECL_20260828 */
static Int16 s_AudioOut[QN_AUDIO_MAX + 4];
static Int16 s_Pending[4];
static int   s_PendingCount = 0;



/* AURORA_QN_EXT_HOST_V2_20260828
 * Mednafen/NesCartDB identify the Japanese Famicom Arkanoid payload as
 * CRC32 D89E5A67. Compute the CRC over PRG+CHR (not the 16-byte iNES header),
 * matching the combined cartridge-ROM CRC convention used by that database.
 */
static Uint32 qCrc32(const Uint8 *pData, size_t nBytes)
{
    /* AURORA_CD_AUDIO_STREAM_V3_NES_CRC_NIBBLE_20260829
     * Same reflected CRC-32, but two 4-bit table steps per byte instead of
     * eight bit-at-a-time branches. This runs only at legacy-iNES load. */
    static const Uint32 table[16] =
    {
        0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
        0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
        0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
        0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU
    };
    Uint32 crc = 0xFFFFFFFFU;

    while (nBytes--)
    {
        crc ^= *pData++;
        crc = (crc >> 4) ^ table[crc & 0x0FU];
        crc = (crc >> 4) ^ table[crc & 0x0FU];
    }
    return crc ^ 0xFFFFFFFFU;
}

static Uint32 qNesPayloadCrc32(const void *pData, size_t nBytes)
{
    const Uint8 *rom = (const Uint8 *)pData;
    size_t offset, payload;

    if (!rom || nBytes < 16 ||
        rom[0] != 'N' || rom[1] != 'E' ||
        rom[2] != 'S' || rom[3] != 0x1A)
        return 0;

    offset = 16U + ((rom[6] & 0x04U) ? 512U : 0U);
    payload = (size_t)rom[4] * 16384U +
              (size_t)rom[5] * 8192U;
    if (payload == 0 || offset > nBytes || payload > nBytes - offset)
        return 0;

    return qCrc32(rom + offset, payload);
}

/* AURORA_VOLUME_TFA_N163_V4_20260913 */
static Uint32 qReadLe32(const Uint8 *p)
{
    return (Uint32)p[0] |
           ((Uint32)p[1] << 8) |
           ((Uint32)p[2] << 16) |
           ((Uint32)p[3] << 24);
}

static int qNesMapperCode(const void *pData, size_t nBytes, bool *pNes2)
{
    const Uint8 *rom = (const Uint8 *)pData;
    bool nes2;
    int mapper;

    if (pNes2) *pNes2 = false;
    if (!rom || nBytes < 16 ||
        rom[0] != 'N' || rom[1] != 'E' ||
        rom[2] != 'S' || rom[3] != 0x1A)
        return -1;

    nes2 = (rom[7] & 0x0CU) == 0x08U;
    mapper = ((int)rom[6] >> 4) | ((int)rom[7] & 0xF0);
    if (nes2)
        mapper |= ((int)rom[8] & 0x0F) << 8;
    if (pNes2) *pNes2 = nes2;
    return mapper;
}

static bool qN163UsesInternalBatteryRam(const void *pData, size_t nBytes,
                                        Uint32 payloadCrc)
{
    const Uint8 *rom = (const Uint8 *)pData;
    bool nes2 = false;
    int mapper = qNesMapperCode(pData, nBytes, &nes2);

    if (!rom || mapper != 19 || !(rom[6] & 0x02U))
        return false;

    if (nes2)
    {
        /* NES 2.0 byte 10 high nibble describes external PRG-NVRAM only.
         * The N163's private 128-byte battery RAM is intentionally not
         * represented there.  Zero external NVRAM + battery means internal. */
        const unsigned prgNvShift = ((unsigned)rom[10] >> 4) & 0x0FU;
        return prgNvShift == 0U;
    }

    /* Legacy iNES is ambiguous.  Isolate the known clean Hydlide III image
     * by PRG+CHR CRC32 (NesCartDB convention) instead of guessing every M19. */
    return payloadCrc == 0x47C2020BU;
}

static Uint8 *qN163FindMapperPayload(Uint8 *state, int bytes)
{
    int pos;

    if (!state || bytes < 16 ||
        state[0] != 'N' || state[1] != 'E' ||
        state[2] != 'S' || state[3] != 'S')
        return NULL;

    /* Nes_File_Writer stores a top-level NESS group followed by 8-byte
     * block headers. Tags are byte-readable ASCII; block size is little-endian. */
    pos = 8;
    while (pos + 8 <= bytes)
    {
        Uint32 size = qReadLe32(state + pos + 4);

        if (state[pos + 0] == 'M' && state[pos + 1] == 'A' &&
            state[pos + 2] == 'P' && state[pos + 3] == 'R')
        {
            if (size != QN_N163_MAPPER_STATE_BYTES ||
                size > (Uint32)(bytes - pos - 8))
                return NULL;
            return state + pos + 8;
        }

        if (size == 0xFFFFFFFFU || size > (Uint32)(bytes - pos - 8))
            return NULL;
        pos += 8 + (int)size;
    }
    return NULL;
}

static int qN163SerializeCurrentState(void)
{
    Mem_Writer writer(s_N163StateScratch, QUICKNES_STATE_CAPACITY);
    const char *err;
    long written;

    if (!s_GameLoaded || !s_pEmu)
        return 0;
    err = s_pEmu->save_state(writer);
    written = writer.size();
    if (err || written <= 0 || written > QUICKNES_STATE_CAPACITY)
    {
        printf("[QuickNES/N163] snapshot failed: %s (%ld)\n",
               err ? err : "invalid size", written);
        return 0;
    }
    return (int)written;
}

static bool qN163SnapshotInternalRam(void)
{
    int bytes;
    Uint8 *mapper;

    if (!s_N163InternalBatterySave)
        return false;
    bytes = qN163SerializeCurrentState();
    if (bytes <= 0)
        return false;
    mapper = qN163FindMapperPayload(s_N163StateScratch, bytes);
    if (!mapper)
    {
        printf("[QuickNES/N163] MAPR block/state layout not found\n");
        return false;
    }
    memcpy(s_N163SaveMirror,
           mapper + QN_N163_INTERNAL_RAM_OFFSET,
           QN_N163_INTERNAL_RAM_BYTES);
    return true;
}

/* AURORA_CD_AUDIO_STREAM_V3_NES_DETECT_20260829 */
static int qNes2DefaultExpansionDevice(const void *pData, size_t nBytes)
{
    const Uint8 *rom = (const Uint8 *)pData;

    if (!rom || nBytes < 16 ||
        rom[0] != 'N' || rom[1] != 'E' ||
        rom[2] != 'S' || rom[3] != 0x1A)
        return -1;

    /* NES 2.0 signature: header byte 7 bits 2..3 == 2. */
    if ((rom[7] & 0x0CU) != 0x08U)
        return -1;

    return (int)(rom[15] & 0x3FU);
}

static bool qLegacyTurboFileCrc(Uint32 crc)
{
    /* Mesen/NesCartDB entries whose GameInputType == TurboFile (0x21).
     * CRC is PRG+CHR, matching qNesPayloadCrc32(). */
    switch (crc)
    {
        case 0x012E12E3U: case 0x0719E982U: case 0x149C0EC3U:
        case 0x1A5CE587U: case 0x21DD2174U: case 0x2A3CA509U:
        case 0x30D00EF7U: case 0x3469B714U: case 0x392E268CU:
        case 0x39A18397U: case 0x3BBFF3A6U: case 0x47C0935BU:
        case 0x48C66CEBU: case 0x498187B6U: case 0x55397DB3U:
        case 0x58600A77U: case 0x5C123EF7U: case 0x5CC1E2C6U:
        case 0x65AA77CEU: case 0x6AF3DEF8U: case 0x7FA2CC55U:
        case 0x83EAF3B1U: case 0x8507A4F9U: case 0x8C4D59D6U:
        case 0x96BE8381U: case 0x974E8840U: case 0xA1A33B85U:
        case 0xAEF00A33U: case 0xB811C054U: case 0xB8747ABFU:
        case 0xB9AB06AAU: case 0xC2EF3422U: case 0xC3DE7C69U:
        case 0xCA503F32U: case 0xCDC69231U: case 0xCEE5857BU:
        case 0xD68A6F33U: case 0xDC33AED8U: case 0xE19293A2U:
        case 0xE46C7D6BU: case 0xE5620994U: case 0xEEC7995AU:
            return true;
        default:
            return false;
    }
}

/* AURORA_QN_BATTLEBOX_V5_20260829
 * Mesen/NesCartDB GameInputType == BattleBox (0x22), PRG+CHR CRC32.
 *
 * Clean retail identities:
 * 61A852EA Battle Stadium - Senbatsu Pro Yakyuu
 * 78B657AC Armadillo
 * 803B9979 J-League Fighting Soccer - The King of Ace Strikers
 * C22BC87B Seiryaku Simulation - Inbou no Wakusei - Shancara
 */
static bool qBattleBoxCrc(Uint32 crc)
{
    switch (crc)
    {
        case 0x58B2FE44U:
        case 0x5B3B5BC1U:
        case 0x61A852EAU:
        case 0x78B657ACU:
        case 0x803B9979U:
        case 0xB3D92E78U:
        case 0xC22BC87BU:
        case 0xC4F9251AU:
        case 0xD8015A7AU:
            return true;
        default:
            return false;
    }
}

/* Mesen2 GameInputType::Zapper/TwoZappers PRG+CHR CRC32.
 * Vs. System, PlayChoice and Dendy rows are intentionally excluded. */
static bool qFamicomLightGunCrc(Uint32 crc)
{
    switch (crc)
    {
        case 0x0AFB395EU: case 0x24598791U: case 0x2A6559A1U:
        case 0x4FBBFA74U: case 0x5112DC21U: case 0x61061352U:
        case 0x73FB55ACU: case 0x74BEA652U: case 0xAA9F9765U:
        case 0xDDCBDA16U: case 0xDF31B364U: case 0xDF3E45D2U:
        case 0xFF24D794U:
            return true;
        default: return false;
    }
}

static bool qTwoNesZapperCrc(Uint32 crc)
{
    switch (crc)
    {
        case 0x231BC76EU: case 0xB79F2651U: case 0xD15009CCU:
        case 0xE85B4D3DU: case 0xFD9E5DD6U:
            return true;
        default: return false;
    }
}

static bool qNesZapperCrc(Uint32 crc)
{
    switch (crc)
    {
        case 0x01B87025U: case 0x04A6B46DU: case 0x051E60C6U:
        case 0x090568DFU: case 0x0E9428FBU: case 0x0F263E59U:
        case 0x143DF524U: case 0x19B0A9F1U: case 0x1BFE42ABU:
        case 0x1CA9C322U: case 0x1CAE8DEFU: case 0x1DA88F85U:
        case 0x1E6C3344U: case 0x1EC1DFEBU: case 0x1F6660E6U:
        case 0x23D17F5EU: case 0x27ACE333U: case 0x283D7727U:
        case 0x2CE31186U: case 0x2F72A0BEU: case 0x327DDDECU:
        case 0x3488A174U: case 0x3C04E8EFU: case 0x3E58A87EU:
        case 0x3E85BA0FU: case 0x3F8BB92DU: case 0x407449DAU:
        case 0x4318A2F8U: case 0x431A5F59U: case 0x44340DA6U:
        case 0x44BEB4B0U: case 0x497C6A69U: case 0x4A3D4790U:
        case 0x4B143FB6U: case 0x4D3982BCU: case 0x4D68CFB1U:
        case 0x4E959173U: case 0x522EE20FU: case 0x524BC479U:
        case 0x5529431FU: case 0x5D4574E0U: case 0x5E8C77DBU:
        case 0x5EE6008EU: case 0x62AF1BC4U: case 0x6332E4CAU:
        case 0x63506FB4U: case 0x64594DA3U: case 0x6519CB3BU:
        case 0x67751094U: case 0x70DF0D3DU: case 0x73CCDAE0U:
        case 0x790B295BU: case 0x7A018E1FU: case 0x7BAF8142U:
        case 0x7C4EBDACU: case 0x7C899CFAU: case 0x7CDF51D5U:
        case 0x7D01D4E0U: case 0x7FC220F7U: case 0x82908FF7U:
        case 0x8373021EU: case 0x851EB9BEU: case 0x8A7D9467U:
        case 0x8B7DA8B8U: case 0x91467F41U: case 0x93216279U:
        case 0xA0FBF02EU: case 0xA1430EEBU: case 0xA39A8063U:
        case 0xA671DA25U: case 0xA7C6C842U: case 0xA7F8BBC8U:
        case 0xAA65ADBFU: case 0xB037246DU: case 0xB0480AE9U:
        case 0xB133CFA7U: case 0xB8B9ACA3U: case 0xBBE40DC4U:
        case 0xBC9BFFCBU: case 0xBCFDD7DEU: case 0xBEB8AB01U:
        case 0xC0F0D838U: case 0xC267D861U: case 0xC3C9D852U:
        case 0xC49F6407U: case 0xC616BAD5U: case 0xCA2C23E2U:
        case 0xCFD8D4A5U: case 0xD0FBE052U: case 0xD5BCF1E5U:
        case 0xD7CD7E8EU: case 0xDE8FD935U: case 0xE145B441U:
        case 0xE18CD9AAU: case 0xE615C8DFU: case 0xEDC3662BU:
        case 0xF24C0B66U: case 0xF27F9E88U: case 0xF4E7A58CU:
        case 0xF5E62944U: case 0xFA08CCBFU:
            return true;
        default: return false;
    }
}

static int qLightGunModeForCrc(Uint32 crc)
{
    if (qFamicomLightGunCrc(crc)) return QN_GUN_FAMICOM_EXT;
    if (qTwoNesZapperCrc(crc)) return QN_GUN_TWO_NES;
    if (qNesZapperCrc(crc)) return QN_GUN_NES_PORT2;
    return QN_GUN_NONE;
}

static void qResetLightGunAim(void)
{
    s_GunX = 128 << 8;
    s_GunY = 120 << 8;
    s_GunVX = s_GunVY = 0;
}

/* AURORA_QN_LIGHTGUN_TIMING_V7_2_20260829
 * 8.8 fixed point. V7 was 0.5..4.0 px/frame with 0.5 px/frame^2 braking.
 * V7.2 is deliberately only a little faster (0.625..4.75), but braking is
 * much stronger so the reticle stops instead of skating past the target. */
static Int32 qGunTargetVelocity(unsigned axis)
{
    const Int32 dead = 20;
    const Int32 minSpeed = 160;  /* 0.625 px/frame */
    const Int32 maxSpeed = 1216; /* 4.75 px/frame */
    Int32 d = (Int32)axis - 128;
    Int32 sign = d < 0 ? -1 : 1;
    Int32 mag = d < 0 ? -d : d;
    if (mag <= dead) return 0;

    mag -= dead;
    Int32 speed = minSpeed +
        (mag * (maxSpeed - minSpeed) + 53) / 107;
    if (speed > maxSpeed) speed = maxSpeed;
    return sign * speed;
}

static Int32 qGunApproach(Int32 value, Int32 target, Int32 step)
{
    if (value < target)
    {
        value += step;
        if (value > target) value = target;
    }
    else if (value > target)
    {
        value -= step;
        if (value < target) value = target;
    }
    return value;
}

static Int32 qGunVelocityStep(Int32 current, Int32 target)
{
    if (!target)
        return 320; /* 1.25 px/frame^2: stop quickly at stick center. */

    if ((current < 0 && target > 0) ||
        (current > 0 && target < 0))
        return 256; /* 1.00 px/frame^2 when reversing direction. */

    return 80;      /* 0.3125 px/frame^2 ordinary acceleration. */
}

static void qUpdateLightGunAim(Emu::SysInputT *pInput)
{
    unsigned ax = 0x80U, ay = 0x80U;
    bool trigger = false;
    bool offscreen = false;
    Uint32 rawPad = 0;

    if (InputIsPadConnected(0))
    {
        const Uint32 packed = InputGetPadAnalog(0);
        ax = (packed >> 16) & 0xFFU;
        ay = (packed >> 24) & 0xFFU;
        rawPad = InputGetPadData(0);

        /* L2 is frontend-only and therefore never appears in SysInputT.
         * Read the raw PS2 pad only here, while a Zapper/Light Gun is active.
         * L2+Square is otherwise unassigned in Aurora's runtime hotkeys. */
        offscreen =
            (rawPad & (PAD_L2 | PAD_SQUARE)) == (PAD_L2 | PAD_SQUARE);
    }

    if (pInput && pInput->uPad[0] != EMUSYS_DEVICE_DISCONNECTED)
        trigger = (pInput->uPad[0] & SNESIO_JOY_B) != 0; /* Cross / X */

    /* Off-screen shot is itself a trigger action; no Cross is required. */
    if (offscreen)
        trigger = true;

    const Int32 tx = qGunTargetVelocity(ax);
    const Int32 ty = qGunTargetVelocity(ay);
    s_GunVX = qGunApproach(
        s_GunVX, tx, qGunVelocityStep(s_GunVX, tx));
    s_GunVY = qGunApproach(
        s_GunVY, ty, qGunVelocityStep(s_GunVY, ty));

    s_GunX += s_GunVX;
    s_GunY += s_GunVY;

    const Int32 xmin = 6 << 8, xmax = 249 << 8;
    const Int32 ymin = 6 << 8, ymax = 233 << 8;
    if (s_GunX < xmin) { s_GunX = xmin; s_GunVX = 0; }
    if (s_GunX > xmax) { s_GunX = xmax; s_GunVX = 0; }
    if (s_GunY < ymin) { s_GunY = ymin; s_GunVY = 0; }
    if (s_GunY > ymax) { s_GunY = ymax; s_GunVY = 0; }

    quicknes_snesticle_ext_set_lightgun_state(
        (int)(s_GunX >> 8), (int)(s_GunY >> 8),
        trigger ? 1 : 0, offscreen ? 1 : 0);
}

static void qUpdateArkanoidVaus(Emu::SysInputT *pInput)
{
    unsigned axis = 0x80U;
    unsigned paddle;
    int fire = 0;

    if (!s_ArkanoidVaus)
        return;

    /* Absolute left-stick X -> authentic Vaus working range 0x54..0xF4. */
    if (InputIsPadConnected(0))
        axis = (InputGetPadAnalog(0) >> 16) & 0xFFU;

    paddle = 0x54U +
        (axis * (0xF4U - 0x54U) + 127U) / 255U;

    /* PS2 X/Cross is sampled directly as Vaus Fire. RunFrame removes its
     * ordinary NES-A mirror while Arkanoid J is active. */
    if (pInput && pInput->uPad[0] != EMUSYS_DEVICE_DISCONNECTED &&
        (pInput->uPad[0] & SNESIO_JOY_B))
        fire = 1;

    quicknes_snesticle_ext_set_arkanoid_state(paddle, fire);
}

static void qResetDirectVideo(void)
{
    memset(s_DirectLastPalette, 0, sizeof(s_DirectLastPalette));
    s_DirectPaletteValid = false;
    s_DirectClutResident = false;
    s_DirectReady = false;
    s_DirectFrameSerial = 0;
    s_DirectUploadSerial = 0;
}

static void qResetTransient(void)
{
    memset(s_Video, 0, sizeof(s_Video));
    memset(s_LastFramePalette, 0, sizeof(s_LastFramePalette));
    memset(s_Pending, 0, sizeof(s_Pending));
    s_PendingCount = 0;
    s_PaletteValid = false;
    qResetDirectVideo();
    s_TurboPhase = false;
    s_TurboFrame = 0;
    s_SkipVideoNext = false;
    s_LastSpriteScanlineLimit = -1;
    s_LastSpriteScreenLimit = -1;
}

static Uint8 qMapPad(Uint16 pad)
{
    /* AURORA_CODEPATH_SIMPLIFY_V5_20260927 */
    if (pad == EMUSYS_DEVICE_DISCONNECTED)
        return 0;

    Uint8 nes = SnesIOPadToNes8(pad);

    if (s_TurboPhase)
    {
        nes |= (Uint8)(
            ((pad & SNESIO_JOY_A) >> 7) |   /* Circle   -> turbo A */
            ((pad & SNESIO_JOY_X) >> 5));   /* Triangle -> turbo B */
    }

    /* Same policy as the old QuickNES option "up_down_allowed=disabled". */
    return SnesIOFilterOppositeDirections8(nes);
}

static void qRenderFrame(CRenderSurface *pTarget)
{
    if (!pTarget)
        return;

    PixelFormatT *fmt = pTarget->GetFormat();
    if (!fmt || fmt->uBitDepth != 32)
        return;

    Uint32 width  = pTarget->GetWidth();
    Uint32 height = pTarget->GetHeight();
    if (width < (Uint32)Nes_Emu::image_width ||
        height < (Uint32)Nes_Emu::image_height)
        return;

    const Nes_Emu::frame_t &frame = s_pEmu->frame();
    if (!frame.pixels || frame.pitch <= 0)
        return;

    if (!s_PaletteValid ||
        memcmp(s_LastFramePalette, frame.palette,
               sizeof(s_LastFramePalette)) != 0)
    {
        for (unsigned i = 0; i < 256; ++i)
        {
            unsigned ci = (unsigned)(unsigned short)frame.palette[i];
            if (ci >= (unsigned)Nes_Emu::color_table_size)
                ci = 0;

            Uint8 r, g, b;
            qGetRgb(ci, &r, &g, &b);

            /* CRenderSurface PIXELFORMAT_RGBA8 on little-endian EE is
             * bytes R,G,B,A -> integer 0xAABBGGRR. */
            s_RgbaPalette[i] = 0xff000000u | ((Uint32)b << 16) | ((Uint32)g << 8) | (Uint32)r;
        }

        memcpy(s_LastFramePalette, frame.palette,
               sizeof(s_LastFramePalette));
        s_PaletteValid = true;
    }

    for (int y = 0; y < Nes_Emu::image_height; ++y)
    {
        const Uint8 *src = frame.pixels + (long)y * frame.pitch;
        Uint32 *dst = (Uint32 *)pTarget->GetLinePtr(y);
        if (!dst)
            continue;

        for (int x = 0; x < Nes_Emu::image_width; ++x)
            dst[x] = s_RgbaPalette[src[x]];
    }

    /* SNESticle uploads a 256x256 texture; NES only owns rows 0..239. */
    Uint32 clearWidth = width;
    if (clearWidth > (Uint32)Nes_Emu::image_width)
        clearWidth = (Uint32)Nes_Emu::image_width;
    for (Uint32 y = Nes_Emu::image_height; y < height && y < 256; ++y)
    {
        Uint8 *dst = pTarget->GetLinePtr((Int32)y);
        if (dst)
            memset(dst, 0, clearWidth * 4);
    }
}

enum
{
    QN_GS_TEX_TBP_OFFSET = 0x400,
    QN_GS_CLUT_TBP_OFFSET = 0x580,
    QN_GS_T8_TBW = 320
};

void QuicknesBridge_InvalidateGsResources(void)
{
    s_DirectUploadSerial = 0;
    s_DirectClutResident = false;
}

bool QuicknesBridge_CanDirectGsVideo(void)
{
    return s_GameLoaded && s_DirectReady && s_DirectFrameSerial != 0;
}

static void qRefreshGsPalette(const Nes_Emu::frame_t &frame)
{
    if (s_DirectPaletteValid &&
        memcmp(s_DirectLastPalette, frame.palette,
               sizeof(s_DirectLastPalette)) == 0)
        return;

    for (unsigned i = 0; i < 256; ++i)
    {
        unsigned ci = (unsigned)(unsigned short)frame.palette[i];
        unsigned dest = i;
        unsigned block = i & 0x3fU;
        if (ci >= (unsigned)Nes_Emu::color_table_size)
            ci = 0;

        /* GS CSM1 swaps each 8-entry 0x08/0x10 CLUT block. */
        if ((block & 0x18U) == 0x08U)
            dest += 8U;
        else if ((block & 0x18U) == 0x10U)
            dest -= 8U;

        Uint8 r, g, b;
        qGetRgb(ci, &r, &g, &b);
        s_GsPalette[dest] = 0x80000000u | ((Uint32)b << 16) | ((Uint32)g << 8) | (Uint32)r;
    }

    memcpy(s_DirectLastPalette, frame.palette,
           sizeof(s_DirectLastPalette));
    s_DirectPaletteValid = true;
    s_DirectClutResident = false;
}

bool QuicknesBridge_DrawDirectGs(Uint32 auroraOutBaseTBP,
                                 Int32 logicalY,
                                 Float32 intensity)
{
    Uint32 texTBP, clutTBP, mod, modColor;
    const Nes_Emu::frame_t &frame = s_pEmu->frame();

    if (!auroraOutBaseTBP || !QuicknesBridge_CanDirectGsVideo() ||
        !frame.pixels || frame.pitch != QN_VIDEO_W)
        return false;

    texTBP = auroraOutBaseTBP + QN_GS_TEX_TBP_OFFSET;
    clutTBP = auroraOutBaseTBP + QN_GS_CLUT_TBP_OFFSET;

    if (s_DirectUploadSerial != s_DirectFrameSerial)
    {
        /* Transfer the native 272-byte stride. UV 0..256 selects only the
         * active image; the final 16 bytes are QuickNES's guard/padding. */
        GPPrimUploadTexture((int)texTBP, QN_GS_T8_TBW,
                            0, 0, GS_PSMT8,
                            frame.pixels, QN_VIDEO_W,
                            Nes_Emu::image_height);
        s_DirectUploadSerial = s_DirectFrameSerial;
    }

    qRefreshGsPalette(frame);
    if (!s_DirectClutResident)
    {
        GPPrimUploadTexture((int)clutTBP, 64,
                            0, 0, GS_PSMCT32,
                            s_GsPalette, 16, 16);
        s_DirectClutResident = true;
    }

    GPPrimSetTex(texTBP, QN_GS_T8_TBW, 9, 8, GS_PSMT8,
                 clutTBP, 64, GS_PSMCT32, 0);

    if (intensity < 0.0f) intensity = 0.0f;
    if (intensity > 1.0f) intensity = 1.0f;
    mod = (Uint32)(128.0f * intensity + 0.5f);
    if (mod > 128u) mod = 128u;
    modColor = 0x80000000u | (mod << 16) | (mod << 8) | mod;

    /* Same logical rectangles and half-texel UVs as the old generic branch:
       Y=2 in 240p, Y=4 in 480i/1080i, always native 256x240. */
    GPPrimTexRect(0, (Uint32)logicalY << 4, 8, 8,
                  256u << 4, (Uint32)(logicalY + 240) << 4,
                  (256u << 4) + 8u, (240u << 4) + 8u,
                  10u << 4, modColor, 0);
    return true;
}


static void qDrainAudio(CMixBuffer *pMix)
{
    long count;

    if (!pMix)
    {
        /* QuickNES explicitly supports NULL here; drain the complete frame so
         * the internal Blip buffer never accumulates into the next frame. */
        s_pEmu->read_samples(NULL, QN_AUDIO_MAX);
        s_PendingCount = 0;
        return;
    }

    /* AURORA_V3_SAFE_QN_DIRECT_AUDIO_DRAIN_20260828:
     * preserve the 0..3-sample prefix and let QuickNES write directly after it. */
    if (s_PendingCount > 0)
        memcpy(s_AudioOut, s_Pending,
               (size_t)s_PendingCount * sizeof(s_Pending[0]));

    count = s_pEmu->read_samples(
        (short *)(s_AudioOut + s_PendingCount), QN_AUDIO_MAX);
    if (count <= 0)
    {
        pMix->Flush();
        return;
    }
    if (count > QN_AUDIO_MAX)
        count = QN_AUDIO_MAX;

    int n = s_PendingCount + (int)count;

    /* AudMixBuffer's existing 32->48 kHz path expects input counts divisible
     * by four. Keep only the tail (0..3 samples) for the next frame. */
    int flush = n & ~3;
    s_PendingCount = n - flush;
    if (s_PendingCount > 0)
        memcpy(s_Pending, s_AudioOut + flush,
               (size_t)s_PendingCount * sizeof(s_Pending[0]));

    if (flush > 0)
        pMix->OutputSamplesMono(s_AudioOut, flush);
    pMix->Flush();
}

bool QuicknesBridge_Init(void)
{
    if (s_Initialized)
        return true;

    s_pEmu = new Nes_Emu();
    if (!s_pEmu)
        return false;

    s_pAudioBuffer = new Nes_Buffer();
    if (!s_pAudioBuffer)
    {
        delete s_pEmu;
        s_pEmu = NULL;
        return false;
    }

    const char *err = s_pEmu->set_sample_rate(32000, s_pAudioBuffer);
    if (err)
    {
        printf("[QuickNES/native] set_sample_rate failed: %s\n", err);
        delete s_pAudioBuffer;
        s_pAudioBuffer = NULL;
        delete s_pEmu;
        s_pEmu = NULL;
        return false;
    }

    s_pEmu->set_palette_range(0);
    s_pEmu->set_sprite_mode(Nes_Emu::sprites_visible);
    s_pEmu->set_pixels(s_Video + 8, QN_VIDEO_W);

    quicknes_snesticle_set_duty_swap(s_DutySwap ? 1 : 0);
    qResetTransient();
    s_Initialized = true;
    return true;
}

void QuicknesBridge_Shutdown(void)
{
    if (s_pEmu)
        s_pEmu->close();

    delete s_pAudioBuffer;
    s_pAudioBuffer = NULL;

    delete s_pEmu;
    s_pEmu = NULL;

    s_GameLoaded = false;
    qResetTransient();
    s_Initialized = false;
}

/* AURORA_QN_MISSING_MAPPER_DIAGNOSTIC_V1 */
static int s_QuicknesMissingMapper = -1;
int QuicknesBridge_LastUnsupportedMapper(void) { return s_QuicknesMissingMapper; }

bool QuicknesBridge_LoadGame(const void *pData, size_t nBytes, const char *pName)
{
    (void)pName;
    s_QuicknesMissingMapper = -1;

    if (!pData || nBytes < 16 || !QuicknesBridge_Init())
        return false;

    if (s_GameLoaded)
        QuicknesBridge_UnloadGame();

    qResetTransient();
    s_pEmu->set_pixels(s_Video + 8, QN_VIDEO_W);

    Mem_File_Reader reader(pData, (long)nBytes);
    const char *err = s_pEmu->load_ines(reader);
    if (err)
    {
        if (!strcmp(err, "Unsupported mapper"))
            s_QuicknesMissingMapper = qNesMapperCode(pData, nBytes, NULL);
        printf("[QuickNES/native] load_ines failed: %s\n", err);
        s_pEmu->close();
        s_GameLoaded = false;
        return false;
    }

    quicknes_snesticle_set_duty_swap(s_DutySwap ? 1 : 0);

    /* AURORA_CD_AUDIO_STREAM_V3_NES_SELECT_20260829
     * No expansion device is attached by default. NES 2.0 explicitly wins;
     * legacy iNES falls back to the known PRG+CHR CRC database. */
    {
        const int nes2Ext = qNes2DefaultExpansionDevice(pData, nBytes);
        const Uint32 payloadCrc = qNesPayloadCrc32(pData, nBytes);
        s_N163InternalBatterySave =
            qN163UsesInternalBatteryRam(pData, nBytes, payloadCrc);

        /* AURORA_PCE_SCALING_LIGHTGUN_TOGGLE_V2_20260830
         * Light Gun detection is deliberately CRC32-only. */
        s_LightGunDetectedMode = qLightGunModeForCrc(payloadCrc);
        s_LightGunMode = s_LightGunEnabled
            ? s_LightGunDetectedMode
            : QN_GUN_NONE;

        s_ArkanoidVaus = false;
        s_TurboFileEnabled = false;
        s_BattleBoxEnabled = false;

        /* A CRC-known gun game does not fall through to another accessory
         * when the user's Light Gun toggle is Off: Off means plain pads. */
        if (s_LightGunDetectedMode == QN_GUN_NONE)
        {
            s_BattleBoxEnabled = qBattleBoxCrc(payloadCrc);
            if (!s_BattleBoxEnabled)
            {
                if (nes2Ext == 0x10)
                    s_ArkanoidVaus = true;
                else if (nes2Ext == 0x21)
                    s_TurboFileEnabled = true;
                else if (nes2Ext <= 0)
                {
                    s_ArkanoidVaus = (payloadCrc == 0xD89E5A67U);
                    if (!s_ArkanoidVaus)
                        s_TurboFileEnabled = qLegacyTurboFileCrc(payloadCrc);
                }
            }
        }

        quicknes_snesticle_ext_set_turbofile(
            s_TurboFileEnabled ? 1 : 0);
        quicknes_snesticle_ext_set_arkanoid(
            s_ArkanoidVaus ? 1 : 0);
        quicknes_snesticle_ext_set_battlebox(
            s_BattleBoxEnabled ? 1 : 0);
        quicknes_snesticle_ext_set_lightgun(s_LightGunMode);
        quicknes_snesticle_ext_reset_bus();

        if (s_LightGunMode != QN_GUN_NONE)
        {
            qResetLightGunAim();
            const char *where =
                s_LightGunMode == QN_GUN_FAMICOM_EXT ? "Famicom EXT" :
                s_LightGunMode == QN_GUN_TWO_NES ? "NES ports 1+2" :
                "NES port 2";
            printf("[QuickNES/GUN] enabled: %s; CRC=%08X%s\n",
                   where, (unsigned)payloadCrc,
                   (nes2Ext == 0x08 || nes2Ext == 0x09) ? "; NES2" : "");
        }
        else if (s_BattleBoxEnabled)
            printf("[QuickNES/EXT] IGS Battle Box enabled; CRC=%08X\n",
                   (unsigned)payloadCrc);
        else if (s_ArkanoidVaus)
            printf("[QuickNES/EXT] Famicom Vaus enabled%s%08X\n",
                   payloadCrc ? "; CRC=" : "; NES2 device=",
                   (unsigned)(payloadCrc ? payloadCrc : (Uint32)nes2Ext));
        else if (s_TurboFileEnabled)
            printf("[QuickNES/EXT] ASCII Turbo File enabled%s%08X\n",
                   payloadCrc ? "; CRC=" : "; NES2 device=",
                   (unsigned)(payloadCrc ? payloadCrc : (Uint32)nes2Ext));

        if (s_N163InternalBatterySave)
            printf("[QuickNES/N163] internal battery RAM selected: 128 bytes; CRC=%08X\n",
                   (unsigned)payloadCrc);
    }

    s_GameLoaded = true;

    /* Do not serialize merely to discover state size during ROM load. */
    printf("[QuickNES/native] loaded %u bytes; SRAM=%d\n",
           (unsigned)nBytes, QuicknesBridge_GetSRAMBytes());
    return true;
}

void QuicknesBridge_UnloadGame(void)
{
    if (s_Initialized && s_pEmu)
        s_pEmu->close();
    s_GameLoaded = false;
    s_ArkanoidVaus = false;
    s_TurboFileEnabled = false; /* AURORA_CD_AUDIO_STREAM_V3_NES_UNLOAD_20260829 */
    s_BattleBoxEnabled = false; /* AURORA_QN_BATTLEBOX_V5_20260829 */
    s_N163InternalBatterySave = false; /* AURORA_VOLUME_TFA_N163_V4_20260913 */
    s_LightGunDetectedMode = QN_GUN_NONE; /* preference survives ROM unload */
    s_LightGunMode = QN_GUN_NONE; /* AURORA_QN_LIGHTGUN_CURSOR_V7_20260829 */
    quicknes_snesticle_ext_set_turbofile(0);
    quicknes_snesticle_ext_set_arkanoid(0);
    quicknes_snesticle_ext_set_battlebox(0);
    quicknes_snesticle_ext_set_lightgun(QN_GUN_NONE);
    qResetLightGunAim();
    qResetTransient();
}

void QuicknesBridge_Reset(void)
{
    if (s_GameLoaded)
        s_pEmu->reset(true, false);       /* power-cycle style reset */
    s_PendingCount = 0;
    s_PaletteValid = false;
    qResetDirectVideo();
    s_TurboPhase = false;
    s_TurboFrame = 0;
    s_LastSpriteScanlineLimit = -1;
    s_LastSpriteScreenLimit = -1;
    quicknes_snesticle_ext_reset_bus();
}

void QuicknesBridge_SoftReset(void)
{
    if (s_GameLoaded)
        s_pEmu->reset(false, false);      /* NES RESET button */
    s_PendingCount = 0;
    s_PaletteValid = false;
    qResetDirectVideo();
    s_TurboPhase = false;
    s_TurboFrame = 0;
    s_LastSpriteScanlineLimit = -1;
    s_LastSpriteScreenLimit = -1;
    quicknes_snesticle_ext_reset_bus();
}

void QuicknesBridge_SetDutySwap(bool enabled)
{
    s_DutySwap = enabled;
    quicknes_snesticle_set_duty_swap(enabled ? 1 : 0);
}

/* AURORA_FCEUMM_FDS_V4_TURBO_PAL_PERF_20260827: exact QuickNES libretro-style custom palette expansion. */
bool QuicknesBridge_SetPalette(const Uint8 *rgb192)
{
    nes_ntsc_setup_t setup;
    if (!rgb192) return false;
    memcpy(s_CustomBasePalette, rgb192, sizeof(s_CustomBasePalette));
    setup = nes_ntsc_rgb;
    setup.palette = NULL;
    setup.base_palette = s_CustomBasePalette;
    setup.palette_out = s_CustomExpandedPalette;
    nes_ntsc_init(NULL, &setup);
    s_CustomPaletteValid = true;
    s_PaletteValid = false;
    s_DirectPaletteValid = false;
    s_DirectClutResident = false;
    return true;
}

void QuicknesBridge_SetTurboSpeed(unsigned speedShift)
{
    /* AURORA_TURBO_SPEED_REARM_V1 */
    unsigned next = (speedShift <= 2U) ? speedShift : 0U;
    if (s_TurboSpeedShift != next)
    {
        s_TurboSpeedShift = next;
        s_TurboFrame = 0;
        s_TurboPhase = true;
    }
}

void QuicknesBridge_SetSkipVideo(bool skip)
{
    s_SkipVideoNext = skip;
}

void QuicknesBridge_RunFrame(Emu::SysInputT *pInput,
                             CRenderSurface *pTarget,
                             CMixBuffer *pMixBuf)
{
    const bool skipVideo =
        (s_LightGunMode == QN_GUN_NONE) ? s_SkipVideoNext : false;
    s_SkipVideoNext = false;

    if (!s_GameLoaded || !s_pEmu)
        return;

    /* AURORA_V15_MULTICORE_SPRITE_LIMIT_20260824
     * Off keeps the NES hardware renderer at 8 sprites/scanline and all 64
     * OAM entries. Scanline levels are the SNES severity ratios scaled to 8.
     * Screen mode keeps the native 8/line rule and caps the first distinct
     * OAM entries for the whole picture, matching the intent of SNES mode. */
    {
        static const int scanlineBudget[SNPPU_OBJ_LIMIT_NUM] =
            { 8, 7, 6, 5, 4, 3, 2 };
        static const int screenBudget[SNPPU_OBJ_LIMIT_NUM] =
            { 64, 28, 24, 20, 16, 12, 8 };
        int level = (int)SNPPURenderGetObjLimitLevel();
        int mode = (int)SNPPURenderGetObjLimitMode();
        int scanline = 8;
        int screen = 64;

        if (level > SNPPU_OBJ_LIMIT_OFF && level < SNPPU_OBJ_LIMIT_NUM)
        {
            if (mode == SNPPU_OBJ_LIMIT_MODE_SCREEN)
                screen = screenBudget[level];
            else
                scanline = scanlineBudget[level];
        }
        if (scanline != s_LastSpriteScanlineLimit ||
            screen != s_LastSpriteScreenLimit)
        {
            s_pEmu->set_sprite_limits(scanline, screen);
            s_LastSpriteScanlineLimit = scanline;
            s_LastSpriteScreenLimit = screen;
        }
    }

    Uint8 p1 = 0;
    Uint8 p2 = 0;
    s_TurboPhase = (((s_TurboFrame >> s_TurboSpeedShift) & 1U) == 0U);
    ++s_TurboFrame;

    /* AURORA_FAMICOM_MIC_CFG41_20260828:
     * SNESIO_JOY_L is a host-only NES carrier; qMapPad intentionally ignores
     * it. The QuickNES core exposes it as the Famicom pad-II mic on $4016 D2. */
    quicknes_snesticle_set_microphone(
        (pInput &&
         pInput->uPad[0] != EMUSYS_DEVICE_DISCONNECTED &&
         (pInput->uPad[0] & SNESIO_JOY_L)) ? 1 : 0);

    if (pInput)
    {
        p1 = qMapPad(pInput->uPad[0]);
        p2 = qMapPad(pInput->uPad[1]);
    }

    if (s_LightGunMode != QN_GUN_NONE)
    {
        qUpdateLightGunAim(pInput);

        if (s_LightGunMode == QN_GUN_NES_PORT2)
            p2 = 0;
        else if (s_LightGunMode == QN_GUN_TWO_NES)
            p1 = p2 = 0;
    }

    /* AURORA_QN_EXT_HOST_V2_20260828: Vaus is an absolute analog device.
     * Cross belongs to the paddle Fire line for Arkanoid J; do not mirror
     * that same press onto the ordinary controller's NES-A bit. */
    if (s_ArkanoidVaus)
    {
        /* AURORA_CD_AUDIO_STREAM_V3_NES_VAUS_FRAME_20260829:
         * zero analog/peripheral calls on ordinary NES frames. */
        p1 &= (Uint8)~0x01U;
        qUpdateArkanoidVaus(pInput);
    }

    const char *err = skipVideo
        ? s_pEmu->emulate_skip_frame((int)p1, (int)p2)
        : s_pEmu->emulate_frame((int)p1, (int)p2);
    if (err)
    {
        printf("[QuickNES/native] emulate_frame failed: %s\n", err);
        qDrainAudio(pMixBuf);
        return;
    }

    if (!skipVideo)
    {
        const Nes_Emu::frame_t &frame = s_pEmu->frame();
        s_DirectReady =
            frame.pixels && frame.pitch == QN_VIDEO_W &&
            (((uintptr_t)frame.pixels & 15u) == 0u);
        if (s_DirectReady)
        {
            if (++s_DirectFrameSerial == 0)
            {
                s_DirectFrameSerial = 1;
                s_DirectUploadSerial = 0;
            }
        }
        else
            qRenderFrame(pTarget);
    }
    /* On skip, DirectReady/serial and the fallback surface intentionally
     * remain the last displayed frame. Audio is still drained. */
    qDrainAudio(pMixBuf);
}

int QuicknesBridge_GetStateSize(void)
{
    /*
     * Capacity only. Do NOT invoke save_state() here.
     *
     * The previous implementation serialized once to determine the
     * size and then serialized a second time for the actual save.
     */
    return s_GameLoaded ? QUICKNES_STATE_CAPACITY : 0;
}

int QuicknesBridge_SaveState(void *pData, int nBytes)
{
    if (!s_GameLoaded || !s_pEmu || !pData || nBytes <= 0)
        return 0;

    if (nBytes > QUICKNES_STATE_CAPACITY)
        nBytes = QUICKNES_STATE_CAPACITY;

    /*
     * Exactly one native serialization.
     *
     * Mem_Writer is fixed-size here: no realloc, no expanding heap
     * buffer, and an oversized state fails cleanly.
     */
    Mem_Writer writer(pData, (long)nBytes);

    printf("[QuickNES/native] save_state begin; capacity=%d\n", nBytes);

    const char *err = s_pEmu->save_state(writer);
    long written = writer.size();

    if (err || written <= 0 || written > nBytes)
    {
        printf("[QuickNES/native] save_state failed: %s (%ld/%d)\n",
               err ? err : "invalid size", written, nBytes);
        return 0;
    }

    printf("[QuickNES/native] save_state OK; bytes=%ld\n", written);
    return (int)written;
}

bool QuicknesBridge_LoadState(const void *pData, int nBytes)
{
    if (!s_GameLoaded || !pData || nBytes <= 0)
        return false;

    Mem_File_Reader reader(pData, (long)nBytes);
    const char *err = s_pEmu->load_state(reader);
    if (err)
    {
        printf("[QuickNES/native] load_state failed: %s\n", err);
        return false;
    }

    /* Nes_Emu::load_state() clears/fades its sound buffer itself. Do not
     * restore a process-local audio snapshot that is not part of the state
     * file (it could belong to a different save slot). */
    s_PendingCount = 0;
    s_PaletteValid = false;
    qResetDirectVideo();
    /* EXT serial position is host peripheral state, not part of the legacy
     * QuickNES snapshot. Restart the transfer bus deterministically. */
    quicknes_snesticle_ext_reset_bus();
    return true;
}

int QuicknesBridge_GetSRAMBytes(void)
{
    if (!s_GameLoaded || !s_pEmu || !s_pEmu->cart())
        return 0;
    if (s_N163InternalBatterySave)
        return QN_N163_INTERNAL_RAM_BYTES;
    if (!s_pEmu->has_battery_ram())
        return 0;
    return (int)s_pEmu->battery_ram_size();
}

uint8_t *QuicknesBridge_GetSRAMData(void)
{
    if (s_N163InternalBatterySave)
        return qN163SnapshotInternalRam() ? s_N163SaveMirror : NULL;
    if (QuicknesBridge_GetSRAMBytes() <= 0)
        return NULL;
    return s_pEmu->high_mem();
}

bool QuicknesBridge_UsesN163InternalSave(void)
{
    return s_GameLoaded && s_N163InternalBatterySave;
}

bool QuicknesBridge_CommitSRAMData(void)
{
    int bytes;
    Uint8 *mapper;

    if (!s_N163InternalBatterySave)
        return true;
    bytes = qN163SerializeCurrentState();
    if (bytes <= 0)
        return false;
    mapper = qN163FindMapperPayload(s_N163StateScratch, bytes);
    if (!mapper)
    {
        printf("[QuickNES/N163] cannot apply battery RAM: MAPR block missing\n");
        return false;
    }

    memcpy(mapper + QN_N163_INTERNAL_RAM_OFFSET,
           s_N163SaveMirror,
           QN_N163_INTERNAL_RAM_BYTES);

    /* Reuse the proven native state restore path. The snapshot was made from
     * this same freshly-loaded machine, so only the 128-byte N163 RAM differs. */
    if (!QuicknesBridge_LoadState(s_N163StateScratch, bytes))
    {
        printf("[QuickNES/N163] cannot apply 128-byte battery RAM\n");
        return false;
    }
    printf("[QuickNES/N163] 128-byte battery RAM committed\n");
    return true;
}

bool QuicknesBridge_IsArkanoidVaus(void)
{
    return s_ArkanoidVaus;
}

bool QuicknesBridge_TurboFileEnabled(void)
{
    return s_TurboFileEnabled; /* AURORA_CD_AUDIO_STREAM_V3_NES_TF_QUERY_20260829 */
}

/* AURORA_QN_EXT_HOST_V2_20260828: 8 KiB ASCII Turbo File battery RAM. */
int QuicknesBridge_GetTurboFileBytes(void)
{
    return 0x2000;
}

uint8_t *QuicknesBridge_GetTurboFileData(void)
{
    return quicknes_snesticle_ext_turbofile_data();
}

bool QuicknesBridge_TurboFileDirty(void)
{
    return quicknes_snesticle_ext_turbofile_dirty() != 0;
}

void QuicknesBridge_ClearTurboFileDirty(void)
{
    quicknes_snesticle_ext_turbofile_clear_dirty();
}

/* AURORA_QN_BATTLEBOX_V5_20260829 */
/* AURORA_PCE_SCALING_LIGHTGUN_TOGGLE_V2_20260830 */
void QuicknesBridge_SetLightGunEnabled(bool enabled)
{
    const int newMode = enabled ? s_LightGunDetectedMode : QN_GUN_NONE;

    s_LightGunEnabled = enabled;
    if (newMode == s_LightGunMode)
        return;

    s_LightGunMode = newMode;
    quicknes_snesticle_ext_set_lightgun(s_LightGunMode);
    quicknes_snesticle_ext_reset_bus();

    if (s_LightGunMode != QN_GUN_NONE)
        qResetLightGunAim();
}

bool QuicknesBridge_GetLightGunEnabled(void)
{
    return s_LightGunEnabled;
}

/* AURORA_V6_1G_QN_KRAZY_DEBUG_CONSUMER_CURE_20260831
 * Dead QN79/Krazy runtime-debug bridge retired; gameplay path unchanged. */

bool QuicknesBridge_LightGunActive(void)
{
    return s_GameLoaded && s_LightGunMode != QN_GUN_NONE;
}

void QuicknesBridge_GetLightGunCursor(Int32 *x, Int32 *y)
{
    if (x) *x = s_GunX >> 8;
    if (y) *y = s_GunY >> 8;
}

void QuicknesBridge_DrawLightGunCursor(Int32 logicalY)
{
    if (!QuicknesBridge_LightGunActive())
        return;

    Int32 x = s_GunX >> 8;
    Int32 y = (s_GunY >> 8) + logicalY;
    const Uint32 black = 0x80000000u;
    const Uint32 white = 0x80FFFFFFu;
    const Uint32 z = 9u << 4;

    GPPrimRect((Uint32)(x - 6) << 4, (Uint32)y << 4, black,
               (Uint32)(x + 7) << 4, (Uint32)(y + 1) << 4, black, z, 0);
    GPPrimRect((Uint32)x << 4, (Uint32)(y - 6) << 4, black,
               (Uint32)(x + 1) << 4, (Uint32)(y + 7) << 4, black, z, 0);
    GPPrimRect((Uint32)(x - 5) << 4, (Uint32)y << 4, white,
               (Uint32)(x + 6) << 4, (Uint32)(y + 1) << 4, white, z, 0);
    GPPrimRect((Uint32)x << 4, (Uint32)(y - 5) << 4, white,
               (Uint32)(x + 1) << 4, (Uint32)(y + 6) << 4, white, z, 0);
}

bool QuicknesBridge_BattleBoxEnabled(void)
{
    return s_BattleBoxEnabled;
}

int QuicknesBridge_GetBattleBoxBytes(void)
{
    return 0x0200;
}

uint8_t *QuicknesBridge_GetBattleBoxData(void)
{
    return quicknes_snesticle_ext_battlebox_data();
}

bool QuicknesBridge_BattleBoxDirty(void)
{
    return quicknes_snesticle_ext_battlebox_dirty() != 0;
}

void QuicknesBridge_ClearBattleBoxDirty(void)
{
    quicknes_snesticle_ext_battlebox_clear_dirty();
}

unsigned QuicknesBridge_GetSampleRate(void)
{
    return 32000;
}
