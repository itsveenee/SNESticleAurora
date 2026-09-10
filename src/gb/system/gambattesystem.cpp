/* AURORA_V13_UNIFIED_GBC_AUDIO_32X_FRAMESKIP_20260910 */
/* AURORA_GAMBATTE_STANDALONE_V2_20260908
 *
 * Standalone Game Boy / Game Boy Color frontend for Aurora.
 *
 * The pinned Gambatte fork is consumed through its C++ API. The generic
 * libretro frontend is intentionally not linked: Aurora already owns timing,
 * controller mapping, save slots, battery routing and PS2 audio/video sinks.
 */

#include <string.h>
#include <stddef.h>
#include <new>

#include "gb/system/gambattesystem.h"
#include "emuinput.h"
#include "rendersurface.h"
#include "mixbuffer.h"
#include "snio.h"

#ifndef HAVE_STDINT_H
#define HAVE_STDINT_H 1
#define AURORA_GB_UNDEF_HAVE_STDINT_H 1
#endif
#include "gambatte.h"
#ifdef AURORA_GB_UNDEF_HAVE_STDINT_H
#undef HAVE_STDINT_H
#undef AURORA_GB_UNDEF_HAVE_STDINT_H
#endif

typedef char AuroraGbVideoPixelMustBe32Bit[
    (sizeof(gambatte::video_pixel_t) == 4U) ? 1 : -1];

/* AURORA_GB_SAFE_AUDIO_BIOS_OPT_R3_20260909
 * Gambatte reconstructs one stereo PSG frame per two 4.194304 MHz GB clocks:
 * 2,097,152 raw frames/s. Keep Aurora's current exact 32-frame box decimator:
 * 65,536 Hz with no fractional-rate conversion.
 *
 * Request 5120 raw frames per normal frame-aware runFor-style call. Relative
 * to the 8192-frame scratch and Gambatte's documented +2064 overrun allowance,
 * this still leaves 1008 raw frames of reserve. It reduces frontend/runFor()
 * dispatches without changing emulated clocks, frame-stop semantics or rate. */
static const Uint32 AURORA_GB_RAW_SAMPLE_RATE = 2097152U;
static const Uint32 AURORA_GB_RAW_SAMPLES_PER_FRAME = 35112U;
static const Uint32 AURORA_GB_RAW_SAMPLES_PER_RUN = 5120U; /* AURORA_GB_SAFE_AUDIO_BIOS_OPT_R3_20260909: nominal full-frame dispatches ceil(35112/4096)=9 -> ceil(35112/5120)=7; 1008 raw-frame reserve after documented +2064. */
static const Uint32 AURORA_GB_RAW_SAMPLES_PER_BIOS_RUN = 6000U; /* AURORA_V13: BIOS-only batching; 6000+2064=8064 < 8192 scratch. */
static const Uint32 AURORA_GB_AUDIO_DECIMATION = 32U; /* AURORA_GB_HOTFIX_R13F_STANDALONE_BOOT_AUDIO_Y_20260909_AUDIO: 65536-Hz intermediate; less box-filter harshness. */
static const Uint32 AURORA_GB_AUDIO_RATE =
    AURORA_GB_RAW_SAMPLE_RATE / AURORA_GB_AUDIO_DECIMATION;
static const Uint32 AURORA_GB_AUDIO_SCRATCH = 8192U; /* AURORA_GAMBATTE_LAZY_RAM_V2R6_20260908 */
static const Uint32 AURORA_GB_BIOS_AUDIO_QUEUE = 1536U; /* AURORA_GB_FINAL_R1_BIOS_HOST_FAST_20260909 */
typedef char AuroraGbAudioRateMustBe65536[
    (AURORA_GB_AUDIO_RATE == 65536U) ? 1 : -1];
typedef char AuroraGbAudioScratchMustCoverRunOverrun[
    (AURORA_GB_AUDIO_SCRATCH >= AURORA_GB_RAW_SAMPLES_PER_RUN + 2064U) ? 1 : -1];
typedef char AuroraGbBiosAudioScratchMustCoverRunOverrun[
    (AURORA_GB_AUDIO_SCRATCH >= AURORA_GB_RAW_SAMPLES_PER_BIOS_RUN + 2064U) ? 1 : -1];
typedef char AuroraGbBiosAudioQueueMustCoverOneFrame[
    (AURORA_GB_BIOS_AUDIO_QUEUE >=
     (AURORA_GB_RAW_SAMPLES_PER_FRAME + AURORA_GB_AUDIO_SCRATCH +
      AURORA_GB_AUDIO_DECIMATION - 1U) / AURORA_GB_AUDIO_DECIMATION) ? 1 : -1];
static const Uint32 AURORA_GB_STATE_PAYLOAD = 0x20000U;
static const Uint32 AURORA_GB_STATE_MAGIC = 0x32534247U; /* "GBS2" LE */
static const Uint32 AURORA_GB_STATE_VERSION = 2U; /* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909 */
/* AURORA_GB_ASCII_TURBO_FILE_R8_20260909 */
static const Uint32 AURORA_GB_TURBO_FILE_BYTES = 0x200000U;
static const Uint32 AURORA_GB_TURBO_FILE_BANK_BYTES = 0x2000U;
static const Uint32 AURORA_GB_TURBO_FILE_CRC_TSUKURU1 = 0x0B614307U;
static const Uint32 AURORA_GB_TURBO_FILE_CRC_TSUKURU2 = 0x219E42E3U;
static const Uint32 AURORA_GB_CGB_BOOT_BYTES = 0x900U;

class AuroraGbInputGetter : public gambatte::InputGetter
{
public:
    AuroraGbInputGetter() : m_State(0) {}
    void Set(unsigned state) { m_State = state; }
    unsigned State() const { return m_State; } /* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909 */
    virtual unsigned operator()() { return m_State; }
private:
    unsigned m_State;
};

struct AuroraGbSgbHostT
{
    Uint16 palette[16];
    Uint8 attr[20U * 18U];
    Uint8 palRam[0x1000U];
    Uint8 attrFiles[0x1000U];
    Uint8 command[16U * 7U];
    Uint8 joyPacket[16U];
    Int16 sgbBit;
    Uint8 currentBits;
    Uint8 commandPackets;
    Uint8 expectedPackets;
    Uint8 transferType;
    Uint8 transferDelay;
    Uint8 maskMode;
    Uint8 controllers;
    Uint8 currentController;
    Bool controllerIncrement;
    Bool hasDynamicPalette;
};

struct AuroraGbStateT
{
    Uint32 Magic;
    Uint32 Version;
    Uint32 CoreBytes;
    Uint32 TurboFrame;
    Int64 ClockCredit;
    Uint32 Reserved[4];
    Uint8 Core[AURORA_GB_STATE_PAYLOAD];
    /* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909: appended so v1 prefix size
     * remains exactly offsetof(AuroraGbStateT, Sgb). */
    AuroraGbSgbHostT Sgb;
};

/* AURORA_GB_ASCII_TURBO_FILE_R8_20260909
 * ASCII Turbo File GB, following the documented byte protocol used by
 * RPG Tsukuru GB / RPG Tsukuru GB 2. Gambatte's SerialIO::check() is the
 * external-clock endpoint; send() is mirrored defensively for games that
 * request internal clock. The physical unit is shared and not a per-ROM
 * savestate payload. */
class AuroraAsciiTurboFileGb : public gambatte::SerialIO
{
public:
    enum StateE { WAIT_SYNC = 0, PACKET_BODY, PACKET_END, DATA_RESPONSE };

    AuroraAsciiTurboFileGb()
        : m_pData(NULL), m_bDirty(FALSE), m_eState(WAIT_SYNC),
          m_uCounter(0), m_uCommand(0), m_uDeviceStatus(0x03),
          m_uCardStatus(0x05), m_uBank(0), m_bSync1(FALSE),
          m_bSync2(FALSE), m_uOutLength(0), m_uOutPos(0)
    {
        memset(m_uPacket, 0, sizeof(m_uPacket));
        memset(m_uOut, 0, sizeof(m_uOut));
    }

    ~AuroraAsciiTurboFileGb() { Shutdown(); }

    static Bool SupportsCRC(Uint32 crc)
    {
        return (crc == AURORA_GB_TURBO_FILE_CRC_TSUKURU1 ||
                crc == AURORA_GB_TURBO_FILE_CRC_TSUKURU2) ? TRUE : FALSE;
    }

    Bool Init(Uint32 crc)
    {
        Shutdown();
        if (!SupportsCRC(crc)) return TRUE;
        m_pData = new (std::nothrow) Uint8[AURORA_GB_TURBO_FILE_BYTES];
        if (!m_pData) return FALSE;
        memset(m_pData, 0xff, AURORA_GB_TURBO_FILE_BYTES);
        m_bDirty = FALSE;
        ResetProtocol();
        return TRUE;
    }

    void Shutdown()
    {
        delete [] m_pData;
        m_pData = NULL;
        m_bDirty = FALSE;
        ResetProtocol();
    }

    Bool Active() const { return m_pData ? TRUE : FALSE; }
    Uint8 *Data() { return m_pData; }
    const Uint8 *Data() const { return m_pData; }
    Uint32 Bytes() const { return m_pData ? AURORA_GB_TURBO_FILE_BYTES : 0U; }
    Bool Dirty() const { return (m_pData && m_bDirty) ? TRUE : FALSE; }
    void ClearDirty() { m_bDirty = FALSE; }

    Bool Attach(const Uint8 *pData, Uint32 nBytes)
    {
        if (!m_pData) return nBytes == 0 ? TRUE : FALSE;
        if (!pData && nBytes == 0)
            memset(m_pData, 0xff, AURORA_GB_TURBO_FILE_BYTES);
        else if (pData && nBytes == AURORA_GB_TURBO_FILE_BYTES)
            memcpy(m_pData, pData, AURORA_GB_TURBO_FILE_BYTES);
        else
            return FALSE;
        m_bDirty = FALSE;
        ResetProtocol();
        return TRUE;
    }

    void ResetProtocol()
    {
        m_eState = WAIT_SYNC;
        m_uCounter = 0;
        m_uCommand = 0;
        m_uDeviceStatus = 0x03;
        m_uCardStatus = 0x05; /* Aurora presents the 1 MiB card inserted. */
        m_uBank = 0;
        m_bSync1 = m_bSync2 = FALSE;
        m_uOutLength = m_uOutPos = 0;
        memset(m_uPacket, 0, sizeof(m_uPacket));
        memset(m_uOut, 0, sizeof(m_uOut));
    }

    virtual bool check(unsigned char out, unsigned char& in, bool& fastCgb)
    {
        if (!m_pData) return false;
        fastCgb = false; /* documented Turbo File GB external clock */
        in = Transfer((Uint8)out);
        return true;
    }

    virtual unsigned char send(unsigned char data, bool fastCgb)
    {
        (void)fastCgb;
        return m_pData ? Transfer((Uint8)data) : 0xffU;
    }

    /* AURORA_GB_STANDALONE_R5_ROUTE_BIOS_TURBO_20260909
     * The physical Turbo File GB is the external serial-clock source.  Tell
     * Gambatte that an accepted check() byte may complete the current SC.7
     * request immediately instead of waiting for the generic link scheduler. */
    virtual bool drivesExternalClockImmediately() const { return true; }

private:
    Uint8 *m_pData;
    Bool m_bDirty;
    StateE m_eState;
    Uint32 m_uCounter;
    Uint8 m_uCommand;
    Uint8 m_uDeviceStatus;
    Uint8 m_uCardStatus;
    Uint16 m_uBank;
    Bool m_bSync1, m_bSync2;
    Uint8 m_uPacket[70];
    Uint8 m_uOut[70];
    Uint32 m_uOutLength, m_uOutPos;

    static Uint32 BodyFinalCounter(Uint8 command)
    {
        switch (command)
        {
            case 0x10: return 2U;  /* 5A 10 checksum */
            case 0x20: return 3U;  /* 5A 20 param checksum */
            case 0x22:
            case 0x23: return 4U;  /* AURORA_GB_FINAL_R2_TURBO_PROTOCOL_20260909: 5A cmd bankBit7 bankLo7 checksum */
            case 0x24: return 2U;
            case 0x30: return 68U; /* 5A 30 offHi offLo + 64 + checksum */
            case 0x40: return 4U;  /* 5A 40 offHi offLo checksum */
            default:   return 2U;
        }
    }

    Bool PacketChecksumOK() const
    {
        Uint32 sum = 0, i;
        Uint32 n = m_uCounter + 1U;
        if (n > sizeof(m_uPacket)) return FALSE;
        for (i = 0; i < n; ++i) sum += m_uPacket[i];
        return ((sum & 0xffU) == 0U) ? TRUE : FALSE;
    }

    void FinishResponse(Uint32 nWithoutChecksum)
    {
        Uint32 i;
        Uint8 sum = 0x5bU; /* 0x100 - second-sync response A5 */
        if (nWithoutChecksum + 1U > sizeof(m_uOut))
        {
            m_uOutLength = 0;
            return;
        }
        for (i = 0; i < nWithoutChecksum; ++i)
            sum = (Uint8)(sum - m_uOut[i]);
        m_uOut[nWithoutChecksum] = sum;
        m_uOutLength = nWithoutChecksum + 1U;
        m_uOutPos = 0;
    }

    void BuildShort(Uint8 command)
    {
        m_uOut[0] = command;
        m_uOut[1] = 0x00;
        m_uOut[2] = m_uDeviceStatus;
        FinishResponse(3U);
    }

    Uint32 StorageOffset() const
    {
        return (Uint32)m_uBank * AURORA_GB_TURBO_FILE_BANK_BYTES +
               ((Uint32)m_uPacket[2] & 0x1fU) * 256U +
               (Uint32)m_uPacket[3];
    }

    void ProcessCommand()
    {
        Uint32 i, off;
        /* AURORA_GB_FINAL_R2_TURBO_PROTOCOL_20260909
         * Every command packet includes its trailing checksum. BodyFinalCounter
         * now consumes it for 0x22/0x23 as it already did for the other known
         * commands. Keep malformed-checksum handling permissive until the real
         * device's error response is established; reject only a missing 5A. */
        if (m_uPacket[0] != 0x5aU)
        {
            BuildShort(m_uCommand ? m_uCommand : 0x10U);
            return;
        }

        switch (m_uCommand)
        {
            case 0x10:
                m_uOut[0] = 0x10; m_uOut[1] = 0x00;
                m_uOut[2] = m_uDeviceStatus;
                m_uOut[3] = m_uCardStatus;
                /* Hardware reports the 8-bit bank as bit 7 in byte 4
                 * and bits 0-6 in byte 5. Bytes 6/7 are unknown/zero. */
                m_uOut[4] = (Uint8)((m_uBank >> 7) & 0x01U);
                m_uOut[5] = (Uint8)(m_uBank & 0x7fU);
                m_uOut[6] = 0x00;
                m_uOut[7] = 0x00;
                FinishResponse(8U);
                break;

            case 0x20:
                BuildShort(0x20);
                break;

            case 0x22:
            case 0x23:
                m_uBank = (Uint16)((((Uint16)m_uPacket[2] & 0x01U) << 7) |
                                    ((Uint16)m_uPacket[3] & 0x7fU));
                m_uDeviceStatus |= 0x08U;
                BuildShort(m_uCommand);
                break;

            case 0x24:
                BuildShort(0x24);
                break;

            case 0x30:
                off = StorageOffset();
                if (off <= AURORA_GB_TURBO_FILE_BYTES - 64U)
                {
                    for (i = 0; i < 64U; ++i)
                    {
                        Uint8 v = m_uPacket[4U + i];
                        if (m_pData[off + i] != v)
                        {
                            m_pData[off + i] = v;
                            m_bDirty = TRUE;
                        }
                    }
                }
                BuildShort(0x30);
                break;

            case 0x40:
                m_uOut[0] = 0x40; m_uOut[1] = 0x00;
                m_uOut[2] = m_uDeviceStatus;
                off = StorageOffset();
                for (i = 0; i < 64U; ++i)
                    m_uOut[3U + i] =
                        (off <= AURORA_GB_TURBO_FILE_BYTES - 64U)
                            ? m_pData[off + i] : 0xffU;
                FinishResponse(67U);
                break;

            default:
                BuildShort(m_uCommand);
                break;
        }
    }

    Uint8 Transfer(Uint8 out)
    {
        Uint8 in = 0x00;
        switch (m_eState)
        {
            case WAIT_SYNC:
                if (out == 0x6cU)
                {
                    in = 0xc6U;
                    m_eState = PACKET_BODY;
                    m_uCounter = 0;
                    m_uCommand = 0;
                    memset(m_uPacket, 0, sizeof(m_uPacket));
                }
                break;

            case PACKET_BODY:
                if (m_uCounter < sizeof(m_uPacket))
                    m_uPacket[m_uCounter] = out;
                if (m_uCounter == 1U)
                    m_uCommand = out;
                if (m_uCounter >= 1U &&
                    m_uCounter == BodyFinalCounter(m_uCommand))
                {
                    ProcessCommand();
                    m_eState = PACKET_END;
                    m_bSync1 = m_bSync2 = FALSE;
                }
                ++m_uCounter;
                break;

            case PACKET_END:
                if (out == 0xf1U)
                {
                    in = 0xe7U;
                    m_bSync1 = TRUE;
                }
                else if (out == 0x7eU)
                {
                    in = 0xa5U;
                    if (m_bSync1) m_bSync2 = TRUE;
                }
                if (m_bSync1 && m_bSync2)
                {
                    m_eState = DATA_RESPONSE;
                    m_uOutPos = 0;
                }
                break;

            case DATA_RESPONSE:
                /* Games normally transmit F2 here; hardware returns the
                 * response stream regardless of the filler byte. */
                if (m_uOutPos < m_uOutLength)
                    in = m_uOut[m_uOutPos++];
                else
                    in = 0xffU;
                if (m_uOutPos >= m_uOutLength)
                {
                    m_eState = WAIT_SYNC;
                    m_uCounter = 0;
                }
                break;
        }
        return in;
    }
};

struct GambatteSystem::Impl
{
    gambatte::GB gb;
    AuroraGbInputGetter input;
    AuroraAsciiTurboFileGb turboFile;
    Bool loaded;
    StandaloneModeE mode;
    AuroraGbSgbHostT sgb; /* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909 */
    Bool hasCgbBootRom;
    Uint32 romBytes;
    Uint32 romCRC;
    Int64 clockCredit;
    Uint32 turboFrame;
    /* AURORA_GB_HOTFIX_R13D_COLOR_AUDIO_LIFECYCLE_20260909: normal runFor() 32:1 carry state; keeps box-filter phase across calls. */
    Int64 audioSumL;
    Int64 audioSumR;
    Uint32 audioPhase;
    Bool biosHostFast; /* AURORA_GB_FINAL_R1_BIOS_HOST_FAST_20260909: host-only, never savestated */
    gambatte::uint_least32_t biosAudio[AURORA_GB_BIOS_AUDIO_QUEUE];
    Uint8 cgbBootRom[AURORA_GB_CGB_BOOT_BYTES];
    gambatte::video_pixel_t screen[160U * 144U];
    gambatte::uint_least32_t audioScratch[AURORA_GB_AUDIO_SCRATCH];

    Impl()
        : loaded(FALSE), mode(STANDALONE_CGB),
          hasCgbBootRom(FALSE), romBytes(0), romCRC(0),
          clockCredit(0), turboFrame(0),
          audioSumL(0), audioSumR(0), audioPhase(0), biosHostFast(FALSE)
    {
        memset(biosAudio, 0, sizeof(biosAudio));
        memset(&sgb, 0, sizeof(sgb));
        memset(cgbBootRom, 0, sizeof(cgbBootRom));
        memset(screen, 0, sizeof(screen));
        memset(audioScratch, 0, sizeof(audioScratch));
    }
};

/* Gambatte's BootloaderGetter receives its internal Bootloader pointer rather
 * than caller userdata. Aurora has exactly one standalone GB core, so bind the
 * currently loaded lazy Impl here, mirroring the SGB GBHost solution. */
static GambatteSystem::Impl *g_AuroraGbBootHost = NULL;

static bool AuroraGbCgbBootloaderGetter(
    void *ignored, bool isgbc, uint8_t *data, uint32_t bytes)
{
    GambatteSystem::Impl *p = g_AuroraGbBootHost;
    (void)ignored;
    if (!p || !p->loaded || !p->hasCgbBootRom || !isgbc ||
        !data || bytes < AURORA_GB_CGB_BOOT_BYTES)
        return false;
    memcpy(data, p->cgbBootRom, AURORA_GB_CGB_BOOT_BYTES);
    return true;
}

/* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909
 * Standalone SGB command host.
 *
 * Gambatte itself remains a DMG CPU/PPU/APU/MBC emulator.  Aurora advertises
 * the SGB post-boot registers, observes FF00 P14/P15 transitions through the
 * existing Gambatte callback, and applies the resulting four SGB palettes and
 * 20x18 attribute map when copying Gambatte's framebuffer to the PS2 surface.
 * No SNES, SGB ROM, or ICD2 object participates in this path.
 *
 * The private Gambatte DMG palette is deliberately four neutral marker values.
 * This preserves the final 2-bit DMG shade in RGB32.  Aurora can therefore
 * recover shade 0..3 losslessly for both dynamic recoloring and the standard
 * 4 KiB PAL_TRN / ATTR_TRN screen transfer. */
static const Uint32 AURORA_GB_SGB_TILES_X = 20U;
static const Uint32 AURORA_GB_SGB_TILES_Y = 18U;
static const Uint32 AURORA_GB_SGB_ATTR_CELLS = 20U * 18U;
static const Uint32 AURORA_GB_SGB_TRANSFER_BYTES = 0x1000U;
static const Uint32 AURORA_GB_SGB_PACKET_BYTES = 16U;
static const Uint32 AURORA_GB_SGB_MAX_PACKETS = 7U;
static const Uint32 AURORA_GB_SGB_COMMAND_BYTES = 16U * 7U;

enum AuroraGbSgbTransferE {
    AURORA_GB_SGB_TRANSFER_NONE = 0,
    AURORA_GB_SGB_TRANSFER_PAL = 1,
    AURORA_GB_SGB_TRANSFER_ATTR = 2
};

static Uint16 AuroraGbSgbRead16(const Uint8 *p)
{
    return (Uint16)((Uint16)p[0] | ((Uint16)p[1] << 8));
}

static void AuroraGbSgbSetAttr(GambatteSystem::Impl *p,
                               Uint32 x, Uint32 y, Uint8 pal)
{
    if (!p || x >= AURORA_GB_SGB_TILES_X || y >= AURORA_GB_SGB_TILES_Y)
        return;
    p->sgb.attr[y * AURORA_GB_SGB_TILES_X + x] = (Uint8)(pal & 3U);
}

static void AuroraGbSgbLoadAttrFile(GambatteSystem::Impl *p, Uint8 index)
{
    Uint32 i;
    Uint32 base;
    if (!p || index > 0x2cU)
        return;
    base = (Uint32)index * 90U;
    if (base + 90U > AURORA_GB_SGB_TRANSFER_BYTES)
        return;
    for (i = 0; i < AURORA_GB_SGB_ATTR_CELLS; ++i)
    {
        Uint8 packed = p->sgb.attrFiles[base + (i >> 2)];
        Uint8 shift = (Uint8)(6U - ((i & 3U) << 1));
        p->sgb.attr[i] = (Uint8)((packed >> shift) & 3U);
    }
}

static void AuroraGbSgbResetHost(GambatteSystem::Impl *p)
{
    static const Uint16 gray[4] = {
        0x7fffU, 0x56b5U, 0x294aU, 0x0000U
    };
    Uint32 pal, shade;
    if (!p) return;
    memset(&p->sgb, 0, sizeof(p->sgb));
    p->sgb.sgbBit = -1;
    /* Gambatte's DMG post-boot JOYP is 0xCF (P14/P15 both low), so the
     * standalone SGB parser must begin at selector state 00. The first
     * 00->30 edge then clocks packet bit 0 exactly like real SGB hosts. */
    p->sgb.currentBits = 0U;
    p->sgb.controllers = 0U;
    p->sgb.currentController = 0U;
    p->sgb.controllerIncrement = FALSE;
    for (pal = 0; pal < 4U; ++pal)
        for (shade = 0; shade < 4U; ++shade)
            p->sgb.palette[pal * 4U + shade] = gray[shade];
}

static void AuroraGbSgbDirectPalette(GambatteSystem::Impl *p,
                                     Uint8 command, const Uint8 *d)
{
    Uint32 i;
    Uint16 common;
    if (!p || !d) return;

    if (command == 0U || command == 2U)
    {
        for (i = 0; i < 4U; ++i)
            p->sgb.palette[i] = AuroraGbSgbRead16(d + 1U + i * 2U);
        common = p->sgb.palette[0];
        p->sgb.palette[4] = common;
        p->sgb.palette[8] = common;
        p->sgb.palette[12] = common;
        if (command == 0U)
        {
            for (i = 1; i < 4U; ++i)
                p->sgb.palette[4U + i] = AuroraGbSgbRead16(d + 7U + i * 2U);
        }
        else
        {
            for (i = 1; i < 4U; ++i)
                p->sgb.palette[12U + i] = AuroraGbSgbRead16(d + 7U + i * 2U);
        }
    }
    else if (command == 1U)
    {
        for (i = 1; i < 4U; ++i)
        {
            p->sgb.palette[8U + i] = AuroraGbSgbRead16(d + 1U + i * 2U);
            p->sgb.palette[12U + i] = AuroraGbSgbRead16(d + 7U + i * 2U);
        }
    }
    else if (command == 3U)
    {
        for (i = 1; i < 4U; ++i)
        {
            p->sgb.palette[4U + i] = AuroraGbSgbRead16(d + 1U + i * 2U);
            p->sgb.palette[8U + i] = AuroraGbSgbRead16(d + 7U + i * 2U);
        }
    }
    p->sgb.hasDynamicPalette = TRUE;
}

static void AuroraGbSgbAttrBlock(GambatteSystem::Impl *p,
                                 const Uint8 *d, Uint32 bytes)
{
    Uint32 pos = 2U;
    Uint32 sets;
    if (!p || !d || bytes < 2U) return;
    sets = d[1];
    while (sets-- && pos + 5U < bytes)
    {
        Uint8 control = d[pos + 0U];
        Uint8 pals = d[pos + 1U];
        Uint8 x0 = d[pos + 2U], y0 = d[pos + 3U];
        Uint8 x1 = d[pos + 4U], y1 = d[pos + 5U];
        Uint8 pIn = pals & 3U;
        Uint8 pPerim = (pals >> 2) & 3U;
        Uint8 pOut = (pals >> 4) & 3U;
        Uint32 x, y;
        for (y = 0; y < AURORA_GB_SGB_TILES_Y; ++y)
        {
            for (x = 0; x < AURORA_GB_SGB_TILES_X; ++x)
            {
                if (y > y0 && y < y1 && x > x0 && x < x1)
                {
                    if (control & 1U) AuroraGbSgbSetAttr(p, x, y, pIn);
                }
                else if (y < y0 || y > y1 || x < x0 || x > x1)
                {
                    if (control & 4U) AuroraGbSgbSetAttr(p, x, y, pOut);
                }
                else
                {
                    if (control & 2U) AuroraGbSgbSetAttr(p, x, y, pPerim);
                    else if (control & 1U) AuroraGbSgbSetAttr(p, x, y, pIn);
                    else if (control & 4U) AuroraGbSgbSetAttr(p, x, y, pOut);
                }
            }
        }
        pos += 6U;
    }
}

static void AuroraGbSgbAttrLine(GambatteSystem::Impl *p,
                                const Uint8 *d, Uint32 bytes)
{
    Uint32 pos = 2U;
    Uint32 sets;
    if (!p || !d || bytes < 2U) return;
    sets = d[1];
    while (sets-- && pos < bytes)
    {
        Uint8 v = d[pos++];
        Uint8 line = v & 0x1fU;
        Uint8 pal = (v >> 5) & 3U;
        Uint32 i;
        if (v & 0x80U)
        {
            if (line < AURORA_GB_SGB_TILES_Y)
                for (i = 0; i < AURORA_GB_SGB_TILES_X; ++i)
                    AuroraGbSgbSetAttr(p, i, line, pal);
        }
        else if (line < AURORA_GB_SGB_TILES_X)
        {
            for (i = 0; i < AURORA_GB_SGB_TILES_Y; ++i)
                AuroraGbSgbSetAttr(p, line, i, pal);
        }
    }
}

static void AuroraGbSgbAttrDiv(GambatteSystem::Impl *p, const Uint8 *d)
{
    Uint8 pAfter, pBefore, pDiv, line;
    Uint32 x, y;
    if (!p || !d) return;
    pAfter = d[1] & 3U;
    pBefore = (d[1] >> 2) & 3U;
    pDiv = (d[1] >> 4) & 3U;
    line = d[2];

    for (y = 0; y < AURORA_GB_SGB_TILES_Y; ++y)
    {
        for (x = 0; x < AURORA_GB_SGB_TILES_X; ++x)
        {
            Uint8 pal;
            if (d[1] & 0x40U)
                pal = (y < line) ? pBefore : ((y == line) ? pDiv : pAfter);
            else
                pal = (x < line) ? pBefore : ((x == line) ? pDiv : pAfter);
            AuroraGbSgbSetAttr(p, x, y, pal);
        }
    }
}

static void AuroraGbSgbAttrChr(GambatteSystem::Impl *p,
                               const Uint8 *d, Uint32 bytes)
{
    Uint32 x, y, count, pos = 6U;
    Bool vertical;
    if (!p || !d || bytes < 6U) return;
    x = d[1];
    y = d[2];
    if (x >= AURORA_GB_SGB_TILES_X) x = 0;
    if (y >= AURORA_GB_SGB_TILES_Y) y = 0;
    count = (Uint32)d[3] | ((Uint32)d[4] << 8);
    vertical = d[5] ? TRUE : FALSE;

    while (count && pos < bytes)
    {
        Uint8 packed = d[pos++];
        Uint32 j;
        for (j = 0; j < 4U && count; ++j, --count)
        {
            AuroraGbSgbSetAttr(p, x, y,
                (Uint8)((packed >> (6U - j * 2U)) & 3U));
            if (vertical)
            {
                if (++y >= AURORA_GB_SGB_TILES_Y)
                {
                    y = 0;
                    if (++x >= AURORA_GB_SGB_TILES_X) x = 0;
                }
            }
            else
            {
                if (++x >= AURORA_GB_SGB_TILES_X)
                {
                    x = 0;
                    if (++y >= AURORA_GB_SGB_TILES_Y) y = 0;
                }
            }
        }
    }
}

static void AuroraGbSgbPaletteSet(GambatteSystem::Impl *p, const Uint8 *d)
{
    Uint32 pal, shade;
    if (!p || !d) return;
    for (pal = 0; pal < 4U; ++pal)
    {
        Uint32 index = AuroraGbSgbRead16(d + 1U + pal * 2U) & 0x1ffU;
        Uint32 base = index * 8U;
        if (base + 7U >= AURORA_GB_SGB_TRANSFER_BYTES)
            continue;
        for (shade = 0; shade < 4U; ++shade)
            p->sgb.palette[pal * 4U + shade] =
                AuroraGbSgbRead16(p->sgb.palRam + base + shade * 2U);
    }
    p->sgb.hasDynamicPalette = TRUE;
    if (d[9] & 0x80U)
        AuroraGbSgbLoadAttrFile(p, d[9] & 0x3fU);
    if (d[9] & 0x40U)
        p->sgb.maskMode = 0U;
}

static void AuroraGbSgbCapture4K(const GambatteSystem::Impl *p, Uint8 *dst);

static void AuroraGbSgbProcessCommand(GambatteSystem::Impl *p)
{
    Uint8 command;
    Uint32 bytes;
    const Uint8 *d;
    if (!p || !p->sgb.commandPackets) return;
    d = p->sgb.command;
    command = (Uint8)(d[0] >> 3);
    bytes = (Uint32)p->sgb.commandPackets * AURORA_GB_SGB_PACKET_BYTES;

    switch (command)
    {
        case 0x00U: case 0x01U: case 0x02U: case 0x03U:
            AuroraGbSgbDirectPalette(p, command, d);
            break;
        case 0x04U:
            AuroraGbSgbAttrBlock(p, d, bytes);
            break;
        case 0x05U:
            AuroraGbSgbAttrLine(p, d, bytes);
            break;
        case 0x06U:
            AuroraGbSgbAttrDiv(p, d);
            break;
        case 0x07U:
            AuroraGbSgbAttrChr(p, d, bytes);
            break;
        case 0x0aU: /* PAL_SET */
            /* If the game consumes PAL_TRN immediately at the next boundary,
             * finish from the last completed marker framebuffer before using
             * palette RAM. This makes correctness independent of runFor()
             * host chunk size. */
            if (p->sgb.transferType == AURORA_GB_SGB_TRANSFER_PAL)
            {
                AuroraGbSgbCapture4K(p, p->sgb.palRam);
                p->sgb.transferType = AURORA_GB_SGB_TRANSFER_NONE;
                p->sgb.transferDelay = 0U;
            }
            AuroraGbSgbPaletteSet(p, d);
            break;
        case 0x0bU: /* PAL_TRN */
            p->sgb.transferType = AURORA_GB_SGB_TRANSFER_PAL;
            p->sgb.transferDelay = 1U;
            break;
        case 0x15U: /* ATTR_TRN */
            p->sgb.transferType = AURORA_GB_SGB_TRANSFER_ATTR;
            p->sgb.transferDelay = 1U;
            break;
        case 0x16U: /* ATTR_SET */
            if (p->sgb.transferType == AURORA_GB_SGB_TRANSFER_ATTR)
            {
                AuroraGbSgbCapture4K(p, p->sgb.attrFiles);
                p->sgb.transferType = AURORA_GB_SGB_TRANSFER_NONE;
                p->sgb.transferDelay = 0U;
            }
            AuroraGbSgbLoadAttrFile(p, d[1] & 0x3fU);
            if (d[1] & 0x40U) p->sgb.maskMode = 0U;
            break;
        case 0x11U: /* MLT_REQ */
            /* SGB software (including Pokemon Red/Blue) uses this response
             * path to prove that an SGB is present before enabling PAL/ATTR
             * commands. Mirror the mature host behavior: request value is
             * controller-mask 0/1/3, with the value 2 quirk advancing once. */
            if ((d[1] & 3U) == 2U)
                ++p->sgb.currentController;
            p->sgb.controllers = d[1] & 3U;
            p->sgb.currentController &= p->sgb.controllers;
            break;
        case 0x17U: /* MASK_EN */
            p->sgb.maskMode = d[1] & 3U;
            break;
        default:
            /* SOUND/DATA/border transfers do not affect the 160x144
             * standalone color plane and are intentionally ignored here. */
            break;
    }
}

static void AuroraGbSgbCommitPacket(GambatteSystem::Impl *p)
{
    Uint32 dst;
    if (!p) return;

    if (!p->sgb.commandPackets)
    {
        p->sgb.expectedPackets = p->sgb.joyPacket[0] & 7U;
        if (!p->sgb.expectedPackets ||
            p->sgb.expectedPackets > AURORA_GB_SGB_MAX_PACKETS)
        {
            p->sgb.expectedPackets = 0;
            return;
        }
        memset(p->sgb.command, 0, sizeof(p->sgb.command));
    }

    if (p->sgb.commandPackets >= p->sgb.expectedPackets ||
        p->sgb.commandPackets >= AURORA_GB_SGB_MAX_PACKETS)
    {
        p->sgb.commandPackets = 0;
        p->sgb.expectedPackets = 0;
        return;
    }

    dst = (Uint32)p->sgb.commandPackets * AURORA_GB_SGB_PACKET_BYTES;
    memcpy(p->sgb.command + dst, p->sgb.joyPacket,
           AURORA_GB_SGB_PACKET_BYTES);
    ++p->sgb.commandPackets;

    if (p->sgb.commandPackets == p->sgb.expectedPackets)
    {
        AuroraGbSgbProcessCommand(p);
        p->sgb.commandPackets = 0;
        p->sgb.expectedPackets = 0;
    }
}

static Uint8 AuroraGbSgbJoyLow(const GambatteSystem::Impl *p, Uint8 selector)
{
    unsigned input;
    Uint8 low = 0x0fU;
    if (!p) return low;

    /* MLT_REQ is part of SGB presence detection. With both select lines high,
     * hardware reports the active controller ID in active-low form. Nonzero
     * virtual controllers have no physical pad connected in this standalone
     * single-pad frontend, matching the usual SGB host behavior. */
    if (selector == 0x30U)
        return (Uint8)(0x0fU ^ (p->sgb.currentController & 3U));
    if (p->sgb.currentController != 0U)
        return 0x0fU;

    input = p->input.State();

    /* Active-low JOYP. P14 low selects directions, P15 low buttons. */
    if (!(selector & 0x10U))
        low &= (Uint8)(0x0fU ^ ((input >> 4) & 0x0fU));
    if (!(selector & 0x20U))
        low &= (Uint8)(0x0fU ^ (input & 0x0fU));
    return low;
}

static unsigned char AuroraGbSgbJoypCallback(void *userdata,
                                             unsigned char p14p15,
                                             bool write)
{
    GambatteSystem::Impl *p = (GambatteSystem::Impl *)userdata;
    Uint8 selector = (Uint8)(p14p15 & 0x30U);
    Uint8 bits = (Uint8)(selector >> 4);

    if (!p)
        return 0x0fU;
    if (p->mode != GambatteSystem::STANDALONE_SGB1_DYNAMIC &&
        p->mode != GambatteSystem::STANDALONE_SGB2_DYNAMIC)
        return AuroraGbSgbJoyLow(p, selector);
    if (!write)
        return AuroraGbSgbJoyLow(p, selector);

    /* AURORA_GB_STANDALONE_R6_JOYP_RESET_20260909
     * Match mature SGB parser ordering exactly:
     *   1) 00 always resets packet assembly, even if it is a repeated level;
     *   2) repeated non-reset selector levels are ignored;
     *   3) only real selector edges advance MLT_REQ controller sequencing.
     * The old ICD-oriented bridge delivered transitions only, which made a
     * repeated reset disappear before the standalone parser could see it. */
    if (bits == 0U)
    {
        p->sgb.sgbBit = -1;
        memset(p->sgb.joyPacket, 0, sizeof(p->sgb.joyPacket));
    }
    if (bits == p->sgb.currentBits)
        return AuroraGbSgbJoyLow(p, selector);

    if (bits & 2U)
    {
        if (p->sgb.controllerIncrement)
        {
            p->sgb.controllerIncrement = FALSE;
            p->sgb.currentController =
                (Uint8)((p->sgb.currentController + 1U) &
                        p->sgb.controllers);
        }
    }
    else if (p->sgb.currentBits & 2U)
    {
        p->sgb.controllerIncrement =
            p->sgb.controllerIncrement ? FALSE : TRUE;
    }

    /* 10 represents data 1, 20 data 0, 30 clocks a bit; the 20 transition
     * following bit 127 commits the completed 16-byte packet. */
    p->sgb.currentBits = bits;

    if (p->sgb.sgbBit == 128 && bits == 2U)
    {
        AuroraGbSgbCommitPacket(p);
        ++p->sgb.sgbBit;
    }
    if (p->sgb.sgbBit < 128)
    {
        if (bits == 1U)
        {
            if (p->sgb.sgbBit >= 0)
                p->sgb.joyPacket[(Uint32)p->sgb.sgbBit >> 3] |=
                    (Uint8)(1U << ((Uint32)p->sgb.sgbBit & 7U));
        }
        else if (bits == 3U)
        {
            ++p->sgb.sgbBit;
        }
    }
    return AuroraGbSgbJoyLow(p, selector);
}

static Uint8 AuroraGbSgbMarkerShade(Uint32 rgb)
{
    /* Marker bytes are FF/AA/55/00, so bits 7:6 distinguish all four. */
    return (Uint8)((((rgb >> 6) & 3U) ^ 3U) & 3U);
}

static void AuroraGbSgbCapture4K(const GambatteSystem::Impl *p, Uint8 *dst)
{
    Uint32 y, x;
    if (!p || !dst) return;
    memset(dst, 0, AURORA_GB_SGB_TRANSFER_BYTES);

    /* Layout matches the SGB 4 KiB screen transfer: 20 sequential 2bpp tiles
     * per 8-line tile row, truncated at 0x1000 bytes. */
    for (y = 0; y < 144U; ++y)
    {
        Uint32 offset = 2U * ((y & 7U) + (y >> 3) * 160U);
        if (offset >= AURORA_GB_SGB_TRANSFER_BYTES)
            break;
        for (x = 0; x < 160U; x += 8U)
        {
            Uint32 pos = offset + (x << 1);
            Uint8 lo = 0, hi = 0;
            Uint32 k;
            if (pos + 1U >= AURORA_GB_SGB_TRANSFER_BYTES)
                break;
            for (k = 0; k < 8U; ++k)
            {
                Uint8 shade = AuroraGbSgbMarkerShade(
                    (Uint32)p->screen[y * 160U + x + k]);
                Uint8 bit = (Uint8)(0x80U >> k);
                if (shade & 1U) lo |= bit;
                if (shade & 2U) hi |= bit;
            }
            dst[pos + 0U] = lo;
            dst[pos + 1U] = hi;
        }
    }
}

static void AuroraGbSgbAdvanceTransfer(GambatteSystem::Impl *p)
{
    if (!p || p->sgb.transferType == AURORA_GB_SGB_TRANSFER_NONE)
        return;
    if (p->sgb.transferDelay)
    {
        --p->sgb.transferDelay;
        return;
    }

    if (p->sgb.transferType == AURORA_GB_SGB_TRANSFER_PAL)
        AuroraGbSgbCapture4K(p, p->sgb.palRam);
    else if (p->sgb.transferType == AURORA_GB_SGB_TRANSFER_ATTR)
        AuroraGbSgbCapture4K(p, p->sgb.attrFiles);
    p->sgb.transferType = AURORA_GB_SGB_TRANSFER_NONE;
}

static void AuroraGbConfigureVideo(GambatteSystem::Impl *p)
{
    if (!p) return;

    /* AURORA_GB_HOTFIX_R13D_COLOR_AUDIO_LIFECYCLE_20260909
     * GBC keeps colour correction, but uses Gambatte's integer fast path.
     * This removes the pow()-heavy gold-standard palette conversion from the
     * CGB boot animation while preserving correction. Brightness/dark filter
     * remain disabled. */
    if (p->mode == GambatteSystem::STANDALONE_CGB)
    {
        p->gb.setColorCorrectionMode(1U);
        p->gb.setColorCorrection(true);
        p->gb.setColorCorrectionBrightness(0.0f);
    }
    else
    {
        static const Uint32 marker[4] = {
            0x00ffffffU, 0x00aaaaaaU, 0x00555555U, 0x00000000U
        };
        Uint32 pal, shade;
        p->gb.setColorCorrection(false);
        /* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909
         * Neutral markers only; never install 1-A/2-A/etc.  The final host
         * renderer recolors each pixel from the game's SGB palette+ATTR map. */
        for (pal = 0; pal < 3U; ++pal)
            for (shade = 0; shade < 4U; ++shade)
                p->gb.setDmgPaletteColor(pal, shade, marker[shade]);
    }
    p->gb.setDarkFilterLevel(0U);
}

static unsigned AuroraGbMapInput(const Emu::SysInputT *pInput,
                                 Uint32 turboFrame)
{
    Uint16 pad = pInput ? pInput->uPad[0] : EMUSYS_DEVICE_DISCONNECTED;
    unsigned out = 0;
    const Bool turboOn = ((turboFrame & 1U) == 0U) ? TRUE : FALSE;

    if (pad == EMUSYS_DEVICE_DISCONNECTED)
        return 0;

    if (pad & SNESIO_JOY_B)      out |= 0x01U; /* Cross -> A */
    if (pad & SNESIO_JOY_Y)      out |= 0x02U; /* Square -> B */
    if (pad & SNESIO_JOY_SELECT) out |= 0x04U;
    if (pad & SNESIO_JOY_START)  out |= 0x08U;
    if (pad & SNESIO_JOY_RIGHT)  out |= 0x10U;
    if (pad & SNESIO_JOY_LEFT)   out |= 0x20U;
    if (pad & SNESIO_JOY_UP)     out |= 0x40U;
    if (pad & SNESIO_JOY_DOWN)   out |= 0x80U;
    if (turboOn && (pad & SNESIO_JOY_A)) out |= 0x01U; /* Circle */
    if (turboOn && (pad & SNESIO_JOY_X)) out |= 0x02U; /* Triangle */

    if ((out & 0x30U) == 0x30U) out &= ~0x30U;
    if ((out & 0xC0U) == 0xC0U) out &= ~0xC0U;
    return out;
}

static void AuroraGbOutputAudio(CMixBuffer *pMix,
                                const gambatte::uint_least32_t *pPacked,
                                Uint32 nFrames)
{
    Int16 left[512];
    Int16 right[512];
    Uint32 pos = 0;

    if (!pMix || !pPacked)
        return;

    while (pos < nFrames)
    {
        Uint32 i;
        Uint32 batch = nFrames - pos;
        if (batch > 512U) batch = 512U;
        for (i = 0; i < batch; ++i)
        {
            Uint32 packed = (Uint32)pPacked[pos + i];
            Int16 l = (Int16)(packed & 0xffffU);
            Int16 r = (Int16)((packed >> 16) & 0xffffU);
            Int16 mono = (Int16)(((Int32)l + (Int32)r) / 2);
            /* GB/GBC internal speaker: mono signal replicated to host L/R. */
            left[i] = mono;
            right[i] = mono;
        }
        pMix->OutputSamplesStereo(left, right, (Int32)batch);
        pos += batch;
    }
}

/* AURORA_GB_HOTFIX_R13D_COLOR_AUDIO_LIFECYCLE_20260909
 * Reuse the normal Gambatte full-rate PCM path that was already clean on PS2.
 * Carry partial 32-frame groups across runFor() calls to avoid boundary clicks. */
/* AURORA_GB_SAFE_AUDIO_BIOS_OPT_R3_20260909
 * Bit-equivalent 32:1 box decimator for the current 65,536-Hz standalone path.
 *
 * A carried partial block still uses the existing Int64 state. Complete
 * 32-sample blocks use local Int32 sums (32 * signed-16-bit is safely inside
 * Int32), then use normal signed C++ division exactly like the old path.
 * Do not replace the division with >> 5: negative rounding would differ.
 *
 * Output is written in-place only after every input of that block was read,
 * and 'out' always trails 'i', so no unread raw sample can be overwritten. */
static Uint32 AuroraGbDecimateRawAudio(GambatteSystem::Impl *p, Uint32 nRaw)
{
    Uint32 i = 0, out = 0;
    if (!p) return 0;
    if (nRaw > AURORA_GB_AUDIO_SCRATCH) nRaw = AURORA_GB_AUDIO_SCRATCH;

    /* Finish the partial group carried from the previous runFor() call. */
    if (p->audioPhase)
    {
        Uint32 need = AURORA_GB_AUDIO_DECIMATION - p->audioPhase;
        Uint32 take = (nRaw < need) ? nRaw : need;
        Uint32 end = i + take;

        for (; i < end; ++i)
        {
            Uint32 packed = (Uint32)p->audioScratch[i];
            p->audioSumL += (Int16)(packed & 0xffffU);
            p->audioSumR += (Int16)((packed >> 16) & 0xffffU);
        }

        p->audioPhase += take;
        if (p->audioPhase == AURORA_GB_AUDIO_DECIMATION)
        {
            Int32 l = (Int32)(p->audioSumL / (Int64)AURORA_GB_AUDIO_DECIMATION);
            Int32 r = (Int32)(p->audioSumR / (Int64)AURORA_GB_AUDIO_DECIMATION);
            p->audioScratch[out++] =
                (gambatte::uint_least32_t)((Uint16)l | ((Uint32)(Uint16)r << 16));
            p->audioSumL = 0;
            p->audioSumR = 0;
            p->audioPhase = 0;
        }
    }

    /* Fast path: whole 32-sample groups, no per-sample phase branch/Int64. */
    while (i + AURORA_GB_AUDIO_DECIMATION <= nRaw)
    {
        Int32 sumL = 0;
        Int32 sumR = 0;
        Uint32 end = i + AURORA_GB_AUDIO_DECIMATION;

        do
        {
            Uint32 packed = (Uint32)p->audioScratch[i++];
            sumL += (Int16)(packed & 0xffffU);
            sumR += (Int16)((packed >> 16) & 0xffffU);
        } while (i < end);

        {
            Int32 l = sumL / (Int32)AURORA_GB_AUDIO_DECIMATION;
            Int32 r = sumR / (Int32)AURORA_GB_AUDIO_DECIMATION;
            p->audioScratch[out++] =
                (gambatte::uint_least32_t)((Uint16)l | ((Uint32)(Uint16)r << 16));
        }
    }

    /* Carry an incomplete tail exactly as before. */
    if (i < nRaw)
    {
        Uint32 start = i;
        for (; i < nRaw; ++i)
        {
            Uint32 packed = (Uint32)p->audioScratch[i];
            p->audioSumL += (Int16)(packed & 0xffffU);
            p->audioSumR += (Int16)((packed >> 16) & 0xffffU);
        }
        p->audioPhase += nRaw - start;
    }

    return out;
}


/* AURORA_GB_SAFE_AUDIO_BIOS_OPT_R3_20260909
 * Standalone GB/GBC keeps Gambatte's normal full-rate runFor() PCM path.
 * Aurora decimates that packed stereo stream 32:1 to 65,536 Hz below.
 * PSG::fillBufferSgb64() belongs to the separate SGB/native64 path. */

static inline Uint32 AuroraGbRgb32ToSurface(Uint32 rgb, Bool cgbFiveBit)
{
    Uint32 r = (rgb >> 16) & 0xffU;
    Uint32 g = (rgb >> 8) & 0xffU;
    Uint32 b = rgb & 0xffU;

    /* AURORA_GB_HOTFIX_R13D_COLOR_AUDIO_LIFECYCLE_20260909: Gambatte gbcToRgb32()'s RGB32 branch returns 5-bit channel
     * magnitudes in each byte. Expand 0..31 -> 0..255 only for CGB output;
     * standalone SGB palettes are already true 8-bit RGB. */
    if (cgbFiveBit)
    {
        r = (r << 3) | (r >> 2);
        g = (g << 3) | (g >> 2);
        b = (b << 3) | (b >> 2);
    }
    return 0xff000000U | (b << 16) | (g << 8) | r;
}

static void AuroraGbRender(const GambatteSystem::Impl *p,
                           CRenderSurface *pTarget)
{
    /* AURORA_GB_HOTFIX_R13D_COLOR_AUDIO_LIFECYCLE_20260909
     * Keep native integer 1x. R13D moved centred dy 56 -> 28.
     * R13F moves 14 pixels back down: dy 28 -> 42, with no clipping. */
    const Int32 dx = 48;
    const Int32 dy = 42; /* AURORA_GB_HOTFIX_R13F_STANDALONE_BOOT_AUDIO_Y_20260909_Y: R13D 28 + 14 (25% of original 56). */
    const Bool cgbFiveBit =
        (p && p->mode == GambatteSystem::STANDALONE_CGB) ? TRUE : FALSE;
    const Bool sgbDynamic =
        (p && (p->mode == GambatteSystem::STANDALONE_SGB1_DYNAMIC ||
               p->mode == GambatteSystem::STANDALONE_SGB2_DYNAMIC)) ? TRUE : FALSE;
    Int32 y, x;

    if (!p || !pTarget ||
        pTarget->GetWidth() < 256U || pTarget->GetHeight() < 240U)
        return;

    /* MASK_EN=1 freezes the previous host image. */
    if (sgbDynamic && p->sgb.maskMode == 1U)
        return;

    for (y = 0; y < 144; ++y)
    {
        const gambatte::video_pixel_t *src = p->screen + y * 160;
        Uint32 *dst = (Uint32 *)pTarget->GetLinePtr(dy + y) + dx;
        for (x = 0; x < 160; ++x)
        {
            if (sgbDynamic)
            {
                Uint16 c;
                if (p->sgb.maskMode == 2U)
                    c = 0U;
                else if (p->sgb.maskMode == 3U)
                    c = p->sgb.palette[0];
                else
                {
                    Uint8 shade = AuroraGbSgbMarkerShade((Uint32)src[x]);
                    Uint8 pal = p->sgb.attr[(Uint32)(y >> 3) * 20U +
                                            (Uint32)(x >> 3)] & 3U;
                    c = p->sgb.palette[(Uint32)pal * 4U + shade];
                }
                {
                    Uint32 r = c & 31U;
                    Uint32 g = (c >> 5) & 31U;
                    Uint32 b = (c >> 10) & 31U;
                    r = (r << 3) | (r >> 2);
                    g = (g << 3) | (g >> 2);
                    b = (b << 3) | (b >> 2);
                    dst[x] = 0xff000000U | (b << 16) | (g << 8) | r;
                }
            }
            else
                dst[x] = AuroraGbRgb32ToSurface((Uint32)src[x], cgbFiveBit);
        }
    }
}

GambatteSystem::GambatteSystem() : m_p(NULL)
{
    /* AURORA_GAMBATTE_LAZY_RAM_V2R6_20260908 */
}

GambatteSystem::~GambatteSystem()
{
    UnloadGame();
}

Bool GambatteSystem::LoadGame(const Uint8 *pData, Uint32 nBytes, Uint32 uCRC,
                              StandaloneModeE eMode,
                              const Uint8 *pCgbBootRom,
                              Uint32 nCgbBootRomBytes)
{
    unsigned flags;

    if (!pData || nBytes < 0x150U)
        return FALSE;
    if (eMode != STANDALONE_CGB &&
        eMode != STANDALONE_SGB1_DYNAMIC &&
        eMode != STANDALONE_SGB2_DYNAMIC)
        return FALSE;

    UnloadGame();
    m_p = new (std::nothrow) Impl;
    if (!m_p)
        return FALSE;

    m_p->mode = eMode;
    AuroraGbSgbResetHost(m_p); /* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909 */
    m_p->loaded = TRUE; /* boot getter is allowed during GB::load/full_init */

    m_p->gb.setSgbJoypCallback(NULL, NULL);
    m_p->gb.setScanlineCallback(NULL);
    m_p->gb.setInputGetter(&m_p->input);
    m_p->gb.setDarkFilterLevel(0U);

    if (eMode == STANDALONE_CGB)
    {
        if (!pCgbBootRom || nCgbBootRomBytes != AURORA_GB_CGB_BOOT_BYTES)
        {
            delete m_p;
            m_p = NULL;
            return FALSE;
        }
        memcpy(m_p->cgbBootRom, pCgbBootRom, AURORA_GB_CGB_BOOT_BYTES);
        m_p->hasCgbBootRom = TRUE;
        g_AuroraGbBootHost = m_p;
        m_p->gb.setBootloaderGetter(&AuroraGbCgbBootloaderGetter);
        flags = gambatte::GB::FORCE_CGB;
    }
    else
    {
        m_p->hasCgbBootRom = FALSE;
        g_AuroraGbBootHost = NULL;
        m_p->gb.setBootloaderGetter(NULL);
        flags = gambatte::GB::FORCE_DMG;
    }

    if (!m_p->turboFile.Init(uCRC))
    {
        if (g_AuroraGbBootHost == m_p) g_AuroraGbBootHost = NULL;
        delete m_p; m_p = NULL;
        return FALSE;
    }
    m_p->gb.setSerialIO(m_p->turboFile.Active() ? &m_p->turboFile : NULL);

    if (m_p->gb.load(pData, (unsigned)nBytes, flags) != 0)
    {
        if (g_AuroraGbBootHost == m_p)
            g_AuroraGbBootHost = NULL;
        delete m_p;
        m_p = NULL;
        return FALSE;
    }

    /* AURORA_GB_HOTFIX_R13D_COLOR_AUDIO_LIFECYCLE_20260909: boot ROM has already been copied by full_init(); do not leave
     * a live global pointer to the cartridge for the rest of gameplay. */
    if (g_AuroraGbBootHost == m_p) g_AuroraGbBootHost = NULL;

    m_p->gb.setInputGetter(&m_p->input);
    if (eMode == STANDALONE_SGB1_DYNAMIC ||
        eMode == STANDALONE_SGB2_DYNAMIC)
    {
        /* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909
         * GB::load() has just installed the ordinary DMG post-boot state.
         * Override A/C before the first instruction and connect FF00 packets. */
        m_p->gb.setSgbPostBootState(eMode == STANDALONE_SGB2_DYNAMIC);
        m_p->gb.setSgbJoypCallback(&AuroraGbSgbJoypCallback, m_p);
    }
    else
        m_p->gb.setSgbJoypCallback(NULL, NULL);
    AuroraGbConfigureVideo(m_p);

    m_p->romBytes = nBytes;
    m_p->romCRC = uCRC;
    m_p->clockCredit = 0;
    m_p->turboFrame = 0;
    m_p->audioSumL = m_p->audioSumR = 0;
    m_p->audioPhase = 0;
    m_p->biosHostFast = FALSE; /* AURORA_GB_FINAL_R1_BIOS_HOST_FAST_20260909 */
    m_p->input.Set(0);
    memset(m_p->screen, 0, sizeof(m_p->screen));
    m_p->gb.clearSavedataDirty();
    m_uLine = 0;
    m_uFrame = 0;
    return TRUE;
}

void GambatteSystem::UnloadGame()
{
    if (!m_p)
    {
        g_AuroraGbBootHost = NULL;
        m_uLine = 0;
        m_uFrame = 0;
        return;
    }

    /* AURORA_GB_HOTFIX_R13D_COLOR_AUDIO_LIFECYCLE_20260909
     * Do not leave a previous cartridge connected to any frontend callback.
     * The boot-host global is valid only while boot bytes are being fetched. */
    if (g_AuroraGbBootHost == m_p) g_AuroraGbBootHost = NULL;
    m_p->loaded = FALSE;
    m_p->input.Set(0);
    m_p->gb.setSerialIO(NULL);
    m_p->gb.setBootloaderGetter(NULL);
    m_p->gb.setInputGetter(NULL);
    m_p->gb.setSgbJoypCallback(NULL, NULL);
    m_p->gb.setScanlineCallback(NULL);
    m_p->turboFile.ResetProtocol();
    m_p->audioSumL = m_p->audioSumR = 0;
    m_p->audioPhase = 0;
    m_p->biosHostFast = FALSE; /* AURORA_GB_FINAL_R1_BIOS_HOST_FAST_20260909 */

    delete m_p;
    m_p = NULL;
    m_uLine = 0;
    m_uFrame = 0;
}

Bool GambatteSystem::IsGameLoaded() const
{
    return (m_p && m_p->loaded) ? TRUE : FALSE;
}

Uint32 GambatteSystem::GetGameCRC() const
{
    return m_p ? m_p->romCRC : 0;
}

Uint32 GambatteSystem::GetGameBytes() const
{
    return m_p ? m_p->romBytes : 0;
}

void GambatteSystem::SetRom(Emu::Rom *pRom)
{
    if (!pRom)
        UnloadGame();
}

void GambatteSystem::Reset()
{
    if (!m_p || !m_p->loaded)
        return;

    if (m_p->mode == STANDALONE_CGB)
    {
        g_AuroraGbBootHost = m_p;
        m_p->gb.setBootloaderGetter(&AuroraGbCgbBootloaderGetter);
    }
    else
    {
        g_AuroraGbBootHost = NULL;
        m_p->gb.setBootloaderGetter(NULL);
    }

    m_p->gb.setSerialIO(m_p->turboFile.Active() ? &m_p->turboFile : NULL);
    m_p->turboFile.ResetProtocol();
    m_p->gb.reset();
    if (g_AuroraGbBootHost == m_p) g_AuroraGbBootHost = NULL;
    /* AURORA_GB_HOTFIX_R13D_COLOR_AUDIO_LIFECYCLE_20260909: boot-host lifetime ends after reset full_init. */
    AuroraGbSgbResetHost(m_p);
    if (m_p->mode == STANDALONE_SGB1_DYNAMIC ||
        m_p->mode == STANDALONE_SGB2_DYNAMIC)
    {
        m_p->gb.setSgbPostBootState(m_p->mode == STANDALONE_SGB2_DYNAMIC);
        m_p->gb.setSgbJoypCallback(&AuroraGbSgbJoypCallback, m_p);
    }
    else
        m_p->gb.setSgbJoypCallback(NULL, NULL);
    m_p->gb.setScanlineCallback(NULL);
    m_p->gb.setInputGetter(&m_p->input);
    AuroraGbConfigureVideo(m_p);
    m_p->clockCredit = 0;
    m_p->turboFrame = 0;
    m_p->audioSumL = m_p->audioSumR = 0;
    m_p->audioPhase = 0;
    m_p->biosHostFast = FALSE; /* AURORA_GB_FINAL_R1_BIOS_HOST_FAST_20260909 */
    m_p->input.Set(0);
    memset(m_p->screen, 0, sizeof(m_p->screen));
}

void GambatteSystem::SoftReset()
{
    Reset();
}

void GambatteSystem::ExecuteFrame(Emu::SysInputT *pInput,
                                  CRenderSurface *pTarget,
                                  CMixBuffer *pMixBuf,
                                  Emu::System::ModeE eMode)
{
    Uint32 rawTotal = 0;
    Uint32 guard = 0;
    Uint32 biosAudioFrames = 0;
    Bool frameDone = FALSE;
    Bool biosAtFrameStart;
    Bool biosAtFrameEnd;
    (void)eMode;

    if (!m_p || !m_p->loaded)
        return;

    /* AURORA_GB_FINAL_R1_BIOS_HOST_FAST_20260909
     * "Async" here is deliberately host-side only. Gambatte still executes
     * CPU, PPU and APU synchronously through the exact normal runFor() path.
     * While the real CGB boot ROM is mapped, avoid expensive display colour
     * correction and batch all decimated PCM into one mixer delivery at frame
     * end. The frontend framebuffer copy is also done every second BIOS frame.
     * FF50 is the exact boundary: gameplay immediately restores the normal
     * colour path, every-frame copy and ordinary per-run mixer delivery. */
    biosAtFrameStart =
        (m_p->mode == STANDALONE_CGB && m_p->gb.isBootloaderActive())
            ? TRUE : FALSE;

    if (biosAtFrameStart && !m_p->biosHostFast)
    {
        m_p->gb.setColorCorrection(false);
        m_p->biosHostFast = TRUE;
    }
    else if (!biosAtFrameStart && m_p->biosHostFast)
    {
        AuroraGbConfigureVideo(m_p);
        m_p->biosHostFast = FALSE;
    }

    m_p->input.Set(AuroraGbMapInput(pInput, m_p->turboFrame));
    ++m_p->turboFrame;

    while (!frameDone && rawTotal < AURORA_GB_RAW_SAMPLES_PER_FRAME &&
           guard++ < 32U)
    {
        unsigned samples = biosAtFrameStart
            ? AURORA_GB_RAW_SAMPLES_PER_BIOS_RUN
            : AURORA_GB_RAW_SAMPLES_PER_RUN; /* AURORA_V13_UNIFIED_GBC_AUDIO_32X_FRAMESKIP_20260910 */
        long frameAt = m_p->gb.runFor(
            m_p->screen, 160,
            m_p->audioScratch, AURORA_GB_AUDIO_SCRATCH,
            samples);
        Uint32 outFrames;

        if (samples > AURORA_GB_AUDIO_SCRATCH)
            samples = AURORA_GB_AUDIO_SCRATCH;
        rawTotal += (Uint32)samples;

        outFrames = AuroraGbDecimateRawAudio(m_p, (Uint32)samples);
        if (biosAtFrameStart && outFrames)
        {
            Uint32 room = AURORA_GB_BIOS_AUDIO_QUEUE - biosAudioFrames;
            Uint32 copyFrames = outFrames < room ? outFrames : room;
            if (copyFrames)
            {
                memcpy(m_p->biosAudio + biosAudioFrames,
                       m_p->audioScratch,
                       copyFrames * sizeof(m_p->biosAudio[0]));
                biosAudioFrames += copyFrames;
            }
            /* Defensive fallback only for a pathological runFor overrun.
             * Normal one-frame output fits the compile-time-checked queue. */
            if (copyFrames < outFrames)
                AuroraGbOutputAudio(
                    pMixBuf, m_p->audioScratch + copyFrames,
                    outFrames - copyFrames);
        }
        else
        {
            AuroraGbOutputAudio(pMixBuf, m_p->audioScratch, outFrames);
        }

        if (frameAt >= 0) frameDone = TRUE;
        if (!samples && frameAt < 0) break;
    }

    if (biosAtFrameStart && biosAudioFrames)
        AuroraGbOutputAudio(pMixBuf, m_p->biosAudio, biosAudioFrames);

    biosAtFrameEnd =
        (m_p->mode == STANDALONE_CGB && m_p->gb.isBootloaderActive())
            ? TRUE : FALSE;
    if (m_p->biosHostFast && !biosAtFrameEnd)
    {
        AuroraGbConfigureVideo(m_p);
        m_p->biosHostFast = FALSE;
    }

    if (m_p->mode == STANDALONE_SGB1_DYNAMIC ||
        m_p->mode == STANDALONE_SGB2_DYNAMIC)
        AuroraGbSgbAdvanceTransfer(m_p); /* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909 */

    /* AURORA_GB_STANDALONE_R5_ROUTE_BIOS_TURBO_20260909
     * BIOS audio batching stays enabled, but presentation must be every
     * frontend frame.  Alternating this copy produced an obvious ~30 Hz
     * blink during the CGB boot ROM/fade. */
    if (pTarget)
        AuroraGbRender(m_p, pTarget);

    if (pMixBuf) pMixBuf->Flush();
    m_p->clockCredit = 0;
    m_uLine = 0;
    ++m_uFrame;
}

Int32 GambatteSystem::GetStateSize()
{
    return (Int32)sizeof(AuroraGbStateT);
}

Bool GambatteSystem::SaveStateChecked(void *pState, Int32 nStateBytes)
{
    AuroraGbStateT *s = (AuroraGbStateT *)pState;
    size_t coreBytes;
    if (!m_p || !m_p->loaded || !s || nStateBytes < (Int32)sizeof(*s))
        return FALSE;

    memset(s, 0, sizeof(*s));
    coreBytes = m_p->gb.stateSize();
    if (!coreBytes || coreBytes > AURORA_GB_STATE_PAYLOAD)
        return FALSE;

    m_p->gb.saveState(s->Core);
    s->Version = AURORA_GB_STATE_VERSION;
    s->CoreBytes = (Uint32)coreBytes;
    s->TurboFrame = m_p->turboFrame;
    s->ClockCredit = m_p->clockCredit;
    s->Sgb = m_p->sgb; /* AURORA_GB_STANDALONE_DYNAMIC_R4_20260909 */
    s->Magic = AURORA_GB_STATE_MAGIC;
    return TRUE;
}

Bool GambatteSystem::RestoreStateChecked(const void *pState, Int32 nStateBytes)
{
    const AuroraGbStateT *s = (const AuroraGbStateT *)pState;
    const Int32 v1Bytes = (Int32)offsetof(AuroraGbStateT, Sgb);
    if (!m_p || !m_p->loaded || !s || nStateBytes < v1Bytes ||
        s->Magic != AURORA_GB_STATE_MAGIC ||
        (s->Version != 1U && s->Version != AURORA_GB_STATE_VERSION) ||
        (s->Version == AURORA_GB_STATE_VERSION &&
         nStateBytes < (Int32)sizeof(*s)) ||
        !s->CoreBytes || s->CoreBytes > AURORA_GB_STATE_PAYLOAD)
        return FALSE;

    if (!m_p->gb.loadState(s->Core, (size_t)s->CoreBytes))
        return FALSE;

    if (m_p->mode == STANDALONE_CGB)
    {
        g_AuroraGbBootHost = m_p;
        m_p->gb.setBootloaderGetter(&AuroraGbCgbBootloaderGetter);
    }
    else
    {
        g_AuroraGbBootHost = NULL;
        m_p->gb.setBootloaderGetter(NULL);
    }
    m_p->gb.setSerialIO(m_p->turboFile.Active() ? &m_p->turboFile : NULL);
    m_p->turboFile.ResetProtocol();
    if (s->Version == AURORA_GB_STATE_VERSION)
        m_p->sgb = s->Sgb;
    else
        AuroraGbSgbResetHost(m_p);
    if (m_p->mode == STANDALONE_SGB1_DYNAMIC ||
        m_p->mode == STANDALONE_SGB2_DYNAMIC)
        m_p->gb.setSgbJoypCallback(&AuroraGbSgbJoypCallback, m_p);
    else
        m_p->gb.setSgbJoypCallback(NULL, NULL);
    m_p->gb.setScanlineCallback(NULL);
    m_p->gb.setInputGetter(&m_p->input);
    AuroraGbConfigureVideo(m_p);
    m_p->biosHostFast = FALSE; /* AURORA_GB_FINAL_R1_BIOS_HOST_FAST_20260909: rebuilt lazily from boot-ROM mapping */
    m_p->clockCredit = 0;
    m_p->turboFrame = s->TurboFrame;
    m_p->input.Set(0);
    return TRUE;
}

void GambatteSystem::SaveState(void *pState, Int32 nStateBytes)
{
    (void)SaveStateChecked(pState, nStateBytes);
}

void GambatteSystem::RestoreState(void *pState, Int32 nStateBytes)
{
    (void)RestoreStateChecked(pState, nStateBytes);
}

Uint32 GambatteSystem::GetSavedataBytes() const
{
    if (!m_p || !m_p->loaded)
        return 0;
    return (Uint32)m_p->gb.savedata_size() +
           (Uint32)m_p->gb.rtcdata_size();
}

Bool GambatteSystem::AttachSavedata(const Uint8 *pData, Uint32 nBytes)
{
    Uint32 sramBytes, rtcBytes, total;
    Uint8 *dst;
    if (!m_p || !m_p->loaded || (nBytes && !pData))
        return FALSE;

    sramBytes = (Uint32)m_p->gb.savedata_size();
    rtcBytes = (Uint32)m_p->gb.rtcdata_size();
    total = sramBytes + rtcBytes;

    /* AURORA_GB_RTC_TIMESTAMP_R8_20260909
     * Gambatte persists RTC::baseTime (Unix timestamp). Never copy a partial
     * timestamp: exact current bundle is accepted; old SRAM-only files are
     * accepted without touching the freshly initialized RTC baseTime. */
    if (nBytes != 0U && nBytes != total && nBytes != sramBytes)
        return FALSE;

    if (nBytes && sramBytes)
    {
        dst = (Uint8 *)m_p->gb.savedata_ptr();
        if (!dst) return FALSE;
        memcpy(dst, pData, sramBytes);
    }

    if (nBytes == total && rtcBytes)
    {
        dst = (Uint8 *)m_p->gb.rtcdata_ptr();
        if (!dst) return FALSE;
        memcpy(dst, pData + sramBytes, rtcBytes);
    }

    m_p->gb.clearSavedataDirty();
    return TRUE;
}

Bool GambatteSystem::ExportSavedata(Uint8 *pData, Uint32 nCapacity,
                                    Uint32 *pActual) const
{
    Uint32 sramBytes, rtcBytes, total, pos = 0;
    const Uint8 *src;
    if (pActual) *pActual = 0;
    if (!m_p || !m_p->loaded)
        return FALSE;

    sramBytes = (Uint32)m_p->gb.savedata_size();
    rtcBytes = (Uint32)m_p->gb.rtcdata_size();
    total = sramBytes + rtcBytes;
    if (pActual) *pActual = total;
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
        memcpy(pData + pos, src, rtcBytes);
    return TRUE;
}

Bool GambatteSystem::SavedataDirty() const
{
    return (m_p && m_p->loaded && m_p->gb.savedataDirty()) ? TRUE : FALSE;
}

void GambatteSystem::ClearSavedataDirty()
{
    if (m_p && m_p->loaded)
        m_p->gb.clearSavedataDirty();
}

Bool GambatteSystem::HasTurboFile() const
{
    return (m_p && m_p->loaded && m_p->turboFile.Active()) ? TRUE : FALSE;
}

Uint32 GambatteSystem::GetTurboFileBytes() const
{
    return HasTurboFile() ? m_p->turboFile.Bytes() : 0U;
}

Uint8 *GambatteSystem::GetTurboFileData()
{
    return HasTurboFile() ? m_p->turboFile.Data() : NULL;
}

const Uint8 *GambatteSystem::GetTurboFileData() const
{
    return HasTurboFile() ? m_p->turboFile.Data() : NULL;
}

Bool GambatteSystem::AttachTurboFile(const Uint8 *pData, Uint32 nBytes)
{
    return HasTurboFile() ? m_p->turboFile.Attach(pData, nBytes)
                          : (nBytes == 0U ? TRUE : FALSE);
}

Bool GambatteSystem::TurboFileDirty() const
{
    return HasTurboFile() ? m_p->turboFile.Dirty() : FALSE;
}

void GambatteSystem::ClearTurboFileDirty()
{
    if (HasTurboFile()) m_p->turboFile.ClearDirty();
}

const char *GambatteSystem::GetString(Emu::System::StringE eString)
{
    switch (eString)
    {
        case Emu::System::STRING_SHORTNAME: return "GB";
        case Emu::System::STRING_FULLNAME:  return "Gambatte Game Boy / Color";
        case Emu::System::STRING_SRAMEXT:   return "sav";
        case Emu::System::STRING_STATEEXT:  return "gst";
        default: return "";
    }
}

Uint32 GambatteSystem::GetSampleRate()
{
    /* AURORA_GB_AUDIO_NATIVE64_R9_20260909: exact 2097152 / 32. */
    return AURORA_GB_AUDIO_RATE;
}
