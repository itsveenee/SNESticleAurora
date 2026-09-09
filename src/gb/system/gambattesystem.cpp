/* AURORA_GAMBATTE_STANDALONE_V2_20260908
 *
 * Standalone Game Boy / Game Boy Color frontend for Aurora.
 *
 * The pinned Gambatte fork is consumed through its C++ API. The generic
 * libretro frontend is intentionally not linked: Aurora already owns timing,
 * controller mapping, save slots, battery routing and PS2 audio/video sinks.
 */

#include <string.h>
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
static const Uint32 AURORA_GB_AUDIO_DECIMATION = 32U; /* AURORA_GB_HOTFIX_R13F_STANDALONE_BOOT_AUDIO_Y_20260909_AUDIO: 65536-Hz intermediate; less box-filter harshness. */
static const Uint32 AURORA_GB_AUDIO_RATE =
    AURORA_GB_RAW_SAMPLE_RATE / AURORA_GB_AUDIO_DECIMATION;
static const Uint32 AURORA_GB_AUDIO_SCRATCH = 8192U; /* AURORA_GAMBATTE_LAZY_RAM_V2R6_20260908 */
typedef char AuroraGbAudioRateMustBe65536[
    (AURORA_GB_AUDIO_RATE == 65536U) ? 1 : -1];
typedef char AuroraGbAudioScratchMustCoverRunOverrun[
    (AURORA_GB_AUDIO_SCRATCH >= AURORA_GB_RAW_SAMPLES_PER_RUN + 2064U) ? 1 : -1];
static const Uint32 AURORA_GB_STATE_PAYLOAD = 0x20000U;
static const Uint32 AURORA_GB_STATE_MAGIC = 0x32534247U; /* "GBS2" LE */
static const Uint32 AURORA_GB_STATE_VERSION = 1U;
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
    virtual unsigned operator()() { return m_State; }
private:
    unsigned m_State;
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
            case 0x23: return 3U;  /* 5A cmd bankHi bankLo checksum */
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
               (Uint32)m_uPacket[2] * 256U + (Uint32)m_uPacket[3];
    }

    void ProcessCommand()
    {
        Uint32 i, off;
        /* Match the documented/GBE+ device behavior: 0x22/0x23 finish at
         * counter 3 and therefore have no separate trailing checksum byte.
         * Reject only a missing 5A packet marker here. */
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
                m_uOut[4] = 0x00;
                m_uOut[5] = 0x00;
                m_uOut[6] = (Uint8)(m_uBank & 0xffU);
                m_uOut[7] = 0x00;
                FinishResponse(8U);
                break;

            case 0x20:
                BuildShort(0x20);
                break;

            case 0x22:
            case 0x23:
                m_uBank = (Uint16)((((Uint16)m_uPacket[2] << 7) |
                                    (Uint16)m_uPacket[3]) & 0xffU);
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
    Uint8 sgbPaletteIndex; /* AURORA_GB_SGB_TITLE_PALETTES_R13_20260909 */
    Bool hasCgbBootRom;
    Uint32 romBytes;
    Uint32 romCRC;
    Int64 clockCredit;
    Uint32 turboFrame;
    /* AURORA_GB_HOTFIX_R13D_COLOR_AUDIO_LIFECYCLE_20260909: normal runFor() 32:1 carry state; keeps box-filter phase across calls. */
    Int64 audioSumL;
    Int64 audioSumR;
    Uint32 audioPhase;
    Uint8 cgbBootRom[AURORA_GB_CGB_BOOT_BYTES];
    gambatte::video_pixel_t screen[160U * 144U];
    gambatte::uint_least32_t audioScratch[AURORA_GB_AUDIO_SCRATCH];

    Impl()
        : loaded(FALSE), mode(STANDALONE_CGB), sgbPaletteIndex(0xffU),
          hasCgbBootRom(FALSE), romBytes(0), romCRC(0),
          clockCredit(0), turboFrame(0),
          audioSumL(0), audioSumR(0), audioPhase(0)
    {
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

/* AURORA_SGB_GAMBATTE_R13_TITLEPAL_20260909 / AURORA_GB_SGB_TITLE_PALETTES_R13_20260909
 * Compact per-title SGB palette database derived at patch time from
 * pinned Gambatte gbcpalettes.h. Only referenced sgb1A..sgb4H
 * palettes are embedded; SGB 1-A is fallback for unmatched titles. */
struct AuroraGbSgbPaletteDef { Uint32 rgb[12]; };
struct AuroraGbSgbTitleEntry { const char *title; Uint8 palette; };

static const AuroraGbSgbPaletteDef s_AuroraGbSgbPalettes[] = {
    { { /* sgb1A */
        0x00F8E8C8U, 0x00D89048U, 0x00A82820U, 0x00301850U,
        0x00F8E8C8U, 0x00D89048U, 0x00A82820U, 0x00301850U,
        0x00F8E8C8U, 0x00D89048U, 0x00A82820U, 0x00301850U
    } },
    { { /* sgb1B */
        0x00D8D8C0U, 0x00C8B070U, 0x00B05010U, 0x00000000U,
        0x00D8D8C0U, 0x00C8B070U, 0x00B05010U, 0x00000000U,
        0x00D8D8C0U, 0x00C8B070U, 0x00B05010U, 0x00000000U
    } },
    { { /* sgb1C */
        0x00F8C0F8U, 0x00E89850U, 0x00983860U, 0x00383898U,
        0x00F8C0F8U, 0x00E89850U, 0x00983860U, 0x00383898U,
        0x00F8C0F8U, 0x00E89850U, 0x00983860U, 0x00383898U
    } },
    { { /* sgb1D */
        0x00F8F8A8U, 0x00C08048U, 0x00F80000U, 0x00501800U,
        0x00F8F8A8U, 0x00C08048U, 0x00F80000U, 0x00501800U,
        0x00F8F8A8U, 0x00C08048U, 0x00F80000U, 0x00501800U
    } },
    { { /* sgb1E */
        0x00F8D8B0U, 0x0078C078U, 0x00688840U, 0x00583820U,
        0x00F8D8B0U, 0x0078C078U, 0x00688840U, 0x00583820U,
        0x00F8D8B0U, 0x0078C078U, 0x00688840U, 0x00583820U
    } },
    { { /* sgb1F */
        0x00D8E8F8U, 0x00E08850U, 0x00A80000U, 0x00004010U,
        0x00D8E8F8U, 0x00E08850U, 0x00A80000U, 0x00004010U,
        0x00D8E8F8U, 0x00E08850U, 0x00A80000U, 0x00004010U
    } },
    { { /* sgb1G */
        0x00000050U, 0x0000A0E8U, 0x00787800U, 0x00F8F858U,
        0x00000050U, 0x0000A0E8U, 0x00787800U, 0x00F8F858U,
        0x00000050U, 0x0000A0E8U, 0x00787800U, 0x00F8F858U
    } },
    { { /* sgb2A */
        0x00F0C8A0U, 0x00C08848U, 0x00287800U, 0x00000000U,
        0x00F0C8A0U, 0x00C08848U, 0x00287800U, 0x00000000U,
        0x00F0C8A0U, 0x00C08848U, 0x00287800U, 0x00000000U
    } },
    { { /* sgb2B */
        0x00F8F8F8U, 0x00F8E850U, 0x00F83000U, 0x00500058U,
        0x00F8F8F8U, 0x00F8E850U, 0x00F83000U, 0x00500058U,
        0x00F8F8F8U, 0x00F8E850U, 0x00F83000U, 0x00500058U
    } },
    { { /* sgb2C */
        0x00F8C0F8U, 0x00E88888U, 0x007830E8U, 0x00282898U,
        0x00F8C0F8U, 0x00E88888U, 0x007830E8U, 0x00282898U,
        0x00F8C0F8U, 0x00E88888U, 0x007830E8U, 0x00282898U
    } },
    { { /* sgb2D */
        0x00F8F8A0U, 0x0000F800U, 0x00F83000U, 0x00000050U,
        0x00F8F8A0U, 0x0000F800U, 0x00F83000U, 0x00000050U,
        0x00F8F8A0U, 0x0000F800U, 0x00F83000U, 0x00000050U
    } },
    { { /* sgb2F */
        0x00D0F8F8U, 0x00F89050U, 0x00A00000U, 0x00180000U,
        0x00D0F8F8U, 0x00F89050U, 0x00A00000U, 0x00180000U,
        0x00D0F8F8U, 0x00F89050U, 0x00A00000U, 0x00180000U
    } },
    { { /* sgb2G */
        0x0068B838U, 0x00E05040U, 0x00E0B880U, 0x00001800U,
        0x0068B838U, 0x00E05040U, 0x00E0B880U, 0x00001800U,
        0x0068B838U, 0x00E05040U, 0x00E0B880U, 0x00001800U
    } },
    { { /* sgb3A */
        0x00F8D098U, 0x0070C0C0U, 0x00F86028U, 0x00304860U,
        0x00F8D098U, 0x0070C0C0U, 0x00F86028U, 0x00304860U,
        0x00F8D098U, 0x0070C0C0U, 0x00F86028U, 0x00304860U
    } },
    { { /* sgb3B */
        0x00D8D8C0U, 0x00E08020U, 0x00005000U, 0x00001010U,
        0x00D8D8C0U, 0x00E08020U, 0x00005000U, 0x00001010U,
        0x00D8D8C0U, 0x00E08020U, 0x00005000U, 0x00001010U
    } },
    { { /* sgb3C */
        0x00E0A8C8U, 0x00F8F878U, 0x0000B8F8U, 0x00202058U,
        0x00E0A8C8U, 0x00F8F878U, 0x0000B8F8U, 0x00202058U,
        0x00E0A8C8U, 0x00F8F878U, 0x0000B8F8U, 0x00202058U
    } },
    { { /* sgb3D */
        0x00F0F8B8U, 0x00E0A878U, 0x0008C800U, 0x00000000U,
        0x00F0F8B8U, 0x00E0A878U, 0x0008C800U, 0x00000000U,
        0x00F0F8B8U, 0x00E0A878U, 0x0008C800U, 0x00000000U
    } },
    { { /* sgb3E */
        0x00F8F8C0U, 0x00E0B068U, 0x00B07820U, 0x00504870U,
        0x00F8F8C0U, 0x00E0B068U, 0x00B07820U, 0x00504870U,
        0x00F8F8C0U, 0x00E0B068U, 0x00B07820U, 0x00504870U
    } },
    { { /* sgb3F */
        0x007878C8U, 0x00F868F8U, 0x00F8D000U, 0x00404040U,
        0x007878C8U, 0x00F868F8U, 0x00F8D000U, 0x00404040U,
        0x007878C8U, 0x00F868F8U, 0x00F8D000U, 0x00404040U
    } },
    { { /* sgb3G */
        0x0060D850U, 0x00F8F8F8U, 0x00C83038U, 0x00380000U,
        0x0060D850U, 0x00F8F8F8U, 0x00C83038U, 0x00380000U,
        0x0060D850U, 0x00F8F8F8U, 0x00C83038U, 0x00380000U
    } },
    { { /* sgb3H */
        0x00E0F8A0U, 0x0078C838U, 0x00488818U, 0x00081800U,
        0x00E0F8A0U, 0x0078C838U, 0x00488818U, 0x00081800U,
        0x00E0F8A0U, 0x0078C838U, 0x00488818U, 0x00081800U
    } },
    { { /* sgb4A */
        0x00F0A868U, 0x0078A8F8U, 0x00D000D0U, 0x00000078U,
        0x00F0A868U, 0x0078A8F8U, 0x00D000D0U, 0x00000078U,
        0x00F0A868U, 0x0078A8F8U, 0x00D000D0U, 0x00000078U
    } },
    { { /* sgb4B */
        0x00F0E8F0U, 0x00E8A060U, 0x00407838U, 0x00180808U,
        0x00F0E8F0U, 0x00E8A060U, 0x00407838U, 0x00180808U,
        0x00F0E8F0U, 0x00E8A060U, 0x00407838U, 0x00180808U
    } },
    { { /* sgb4C */
        0x00F8E0E0U, 0x00D8A0D0U, 0x0098A0E0U, 0x00080000U,
        0x00F8E0E0U, 0x00D8A0D0U, 0x0098A0E0U, 0x00080000U,
        0x00F8E0E0U, 0x00D8A0D0U, 0x0098A0E0U, 0x00080000U
    } },
    { { /* sgb4D */
        0x00F8F8B8U, 0x0090C8C8U, 0x00486878U, 0x00082048U,
        0x00F8F8B8U, 0x0090C8C8U, 0x00486878U, 0x00082048U,
        0x00F8F8B8U, 0x0090C8C8U, 0x00486878U, 0x00082048U
    } },
    { { /* sgb4G */
        0x00B0E018U, 0x00B82058U, 0x00281000U, 0x00008060U,
        0x00B0E018U, 0x00B82058U, 0x00281000U, 0x00008060U,
        0x00B0E018U, 0x00B82058U, 0x00281000U, 0x00008060U
    } },
    { { /* sgb4H */
        0x00F8F8C8U, 0x00B8C058U, 0x00808840U, 0x00405028U,
        0x00F8F8C8U, 0x00B8C058U, 0x00808840U, 0x00405028U,
        0x00F8F8C8U, 0x00B8C058U, 0x00808840U, 0x00405028U
    } },
};

static const AuroraGbSgbTitleEntry s_AuroraGbSgbTitles[] = {
    { "ALLEY WAY", 18U },
    { "BALLOON KID", 0U },
    { "BASEBALL", 12U },
    { "CASINO FUNPAK", 0U },
    { "CONTRA ALIEN WAR", 5U },
    { "CONTRA SPIRITS", 5U },
    { "CUTTHROAT ISLAND", 17U },
    { "DMG FOOTBALL", 22U },
    { "DR.MARIO", 14U },
    { "F1RACE", 22U },
    { "FRANK THOMAS BB", 1U },
    { "GBWARS", 17U },
    { "GBWARST", 17U },
    { "GOLF", 20U },
    { "HOSHINOKA-BI", 9U },
    { "ITCHY & SCRATCHY", 22U },
    { "JEOPARDY", 11U },
    { "JEOPARDY SPORTS", 11U },
    { "JUNGLE BOOK", 22U },
    { "KAERUNOTAMENI", 7U },
    { "KID ICARUS", 11U },
    { "KIRBY BLOCKBALL", 24U },
    { "KIRBY DREAM LAND", 9U },
    { "KIRBY'S PINBALL", 2U },
    { "MARIO & YOSHI", 10U },
    { "MARIOLAND2", 16U },
    { "METROID2", 25U },
    { "MORTAL KOMBAT", 24U },
    { "MORTAL KOMBAT 3", 1U },
    { "MORTAL KOMBAT II", 24U },
    { "MORTALKOMBAT DUO", 24U },
    { "MORTALKOMBATI&II", 24U },
    { "NBA JAM", 11U },
    { "NBA JAM TE", 11U },
    { "PLAT JEOPARDY!", 11U },
    { "POCAHONTAS", 24U },
    { "PROBOTECTOR 2", 5U },
    { "QIX", 21U },
    { "RADARMISSION", 22U },
    { "RVT             ", 22U },
    { "SOLARSTRIKER", 6U },
    { "SPACE INVADERS", 24U },
    { "SUPER MARIOLAND", 5U },
    { "SUPERMARIOLAND3", 1U },
    { "TARZAN", 7U },
    { "TAZ-MANIA", 0U },
    { "TEEN JEOPARDY!", 11U },
    { "TENNIS", 19U },
    { "TETRIS", 13U },
    { "TETRIS FLASH", 8U },
    { "TETRIS2", 8U },
    { "THE GETAWAY     ", 1U },
    { "TOPRANKINGTENNIS", 22U },
    { "TOPRANKTENNIS", 22U },
    { "WAVERACE", 23U },
    { "WORLD CUP", 26U },
    { "X", 24U },
    { "YAKUMAN", 15U },
    { "YOGIS GOLDRUSH", 14U },
    { "YOSHI'S COOKIE", 3U },
    { "YOSSY NO COOKIE", 3U },
    { "YOSSY NO TAMAGO", 10U },
    { "ZELDA", 4U },
};

static const Uint8 AURORA_GB_SGB_FALLBACK_PALETTE = 0U;

static Uint8 AuroraGbFindSgbTitlePalette(const Uint8 *pRom, Uint32 nBytes)
{
    char title[17];
    Uint32 i;
    if (!pRom || nBytes < 0x144U)
        return AURORA_GB_SGB_FALLBACK_PALETTE;
    memcpy(title, pRom + 0x134U, 16U);
    title[16] = 0;
    for (i = 0; i < 16U; ++i)
        if ((unsigned char)title[i] < 0x20U ||
            (unsigned char)title[i] >= 0x80U) { title[i] = 0; break; }
    for (i = 0; i < (Uint32)(sizeof(s_AuroraGbSgbTitles) /
                             sizeof(s_AuroraGbSgbTitles[0])); ++i)
        if (strcmp(title, s_AuroraGbSgbTitles[i].title) == 0)
            return s_AuroraGbSgbTitles[i].palette;
    return AURORA_GB_SGB_FALLBACK_PALETTE;
}

static void AuroraGbApplySgbTitlePalette(GambatteSystem::Impl *p)
{
    Uint32 pal, shade;
    Uint8 index;
    if (!p) return;
    index = p->sgbPaletteIndex;
    if (index >= (Uint8)(sizeof(s_AuroraGbSgbPalettes) /
                         sizeof(s_AuroraGbSgbPalettes[0])))
        index = AURORA_GB_SGB_FALLBACK_PALETTE;
    for (pal = 0; pal < 3U; ++pal)
        for (shade = 0; shade < 4U; ++shade)
            p->gb.setDmgPaletteColor(
                pal, shade, s_AuroraGbSgbPalettes[index].rgb[pal * 4U + shade]);
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
        p->gb.setColorCorrection(false);
        AuroraGbApplySgbTitlePalette(p);
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
            left[i] = (Int16)(packed & 0xffffU);
            right[i] = (Int16)((packed >> 16) & 0xffffU);
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
    Int32 y, x;

    if (!p || !pTarget ||
        pTarget->GetWidth() < 256U || pTarget->GetHeight() < 240U)
        return;

    for (y = 0; y < 144; ++y)
    {
        const gambatte::video_pixel_t *src = p->screen + y * 160;
        Uint32 *dst = (Uint32 *)pTarget->GetLinePtr(dy + y) + dx;
        for (x = 0; x < 160; ++x)
            dst[x] = AuroraGbRgb32ToSurface((Uint32)src[x], cgbFiveBit);
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
    if (eMode != STANDALONE_CGB && eMode != STANDALONE_SGB_PALETTE)
        return FALSE;

    UnloadGame();
    m_p = new (std::nothrow) Impl;
    if (!m_p)
        return FALSE;

    m_p->mode = eMode;
    m_p->sgbPaletteIndex =
        (eMode == STANDALONE_SGB_PALETTE)
            ? AuroraGbFindSgbTitlePalette(pData, nBytes)
            : 0xffU; /* AURORA_GB_SGB_TITLE_PALETTES_R13_20260909 */
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
    AuroraGbConfigureVideo(m_p);

    m_p->romBytes = nBytes;
    m_p->romCRC = uCRC;
    m_p->clockCredit = 0;
    m_p->turboFrame = 0;
    m_p->audioSumL = m_p->audioSumR = 0;
    m_p->audioPhase = 0;
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
    m_p->gb.setSgbJoypCallback(NULL, NULL);
    m_p->gb.setScanlineCallback(NULL);
    m_p->gb.setInputGetter(&m_p->input);
    AuroraGbConfigureVideo(m_p);
    m_p->clockCredit = 0;
    m_p->turboFrame = 0;
    m_p->audioSumL = m_p->audioSumR = 0;
    m_p->audioPhase = 0;
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
    Bool frameDone = FALSE;
    (void)eMode;

    if (!m_p || !m_p->loaded)
        return;

    m_p->input.Set(AuroraGbMapInput(pInput, m_p->turboFrame));
    ++m_p->turboFrame;

    /* AURORA_GB_HOTFIX_R13D_COLOR_AUDIO_LIFECYCLE_20260909
     * Return to Gambatte's normal runFor() path, which was already clean at
     * full speed on PS2. Decimate 2.097152 MHz -> 65536 Hz in the frontend,
     * preserving filter phase across calls. */
    while (!frameDone && rawTotal < AURORA_GB_RAW_SAMPLES_PER_FRAME &&
           guard++ < 32U)
    {
        unsigned samples = AURORA_GB_RAW_SAMPLES_PER_RUN;
        long frameAt = m_p->gb.runFor(
            m_p->screen, 160,
            m_p->audioScratch, AURORA_GB_AUDIO_SCRATCH,
            samples);
        Uint32 outFrames;

        if (samples > AURORA_GB_AUDIO_SCRATCH)
            samples = AURORA_GB_AUDIO_SCRATCH;
        rawTotal += (Uint32)samples;

        outFrames = AuroraGbDecimateRawAudio(m_p, (Uint32)samples);
        AuroraGbOutputAudio(pMixBuf, m_p->audioScratch, outFrames);

        if (frameAt >= 0) frameDone = TRUE;
        if (!samples && frameAt < 0) break;
    }

    if (pTarget) AuroraGbRender(m_p, pTarget);
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
    s->Magic = AURORA_GB_STATE_MAGIC;
    return TRUE;
}

Bool GambatteSystem::RestoreStateChecked(const void *pState, Int32 nStateBytes)
{
    const AuroraGbStateT *s = (const AuroraGbStateT *)pState;
    if (!m_p || !m_p->loaded || !s || nStateBytes < (Int32)sizeof(*s) ||
        s->Magic != AURORA_GB_STATE_MAGIC ||
        s->Version != AURORA_GB_STATE_VERSION ||
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
    m_p->gb.setSgbJoypCallback(NULL, NULL);
    m_p->gb.setScanlineCallback(NULL);
    m_p->gb.setInputGetter(&m_p->input);
    AuroraGbConfigureVideo(m_p);
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
