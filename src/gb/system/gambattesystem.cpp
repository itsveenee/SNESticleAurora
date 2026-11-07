/* AURORA_V13_UNIFIED_GBC_AUDIO_32X_FRAMESKIP_20260910 */
/* AURORA_VOLUME_TFA_N163_V4_20260913 */
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
#include "platform/ps2/system/mainloop_safe_frameskip.h"

extern Int32 VideoGetGbcVolume(void);

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
static const Uint32 AURORA_GB_STATE_VERSION = 3U; /* AURORA_NO32X_SGB_AUDIT_V4_1_20260921: short CGB-only state ABI */
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

struct AuroraGbStateT
{
    Uint32 Magic;
    Uint32 Version;
    Uint32 CoreBytes;
    Uint32 TurboFrame;
    Int64 ClockCredit; /* legacy V1/V2 ABI slot; V4_1 always writes zero */
    Uint32 Reserved[4];
    Uint8 Core[AURORA_GB_STATE_PAYLOAD];
}; /* AURORA_NO_SGB_ELF_V4_20260921: no SGB tail/state allocation. */

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
    Bool hasCgbBootRom;
    Uint32 romBytes;
    Uint32 romCRC;
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
    /* AURORA_GAMBATTE_TARGET_INIT_V13_20260911
     * Host-only bookkeeping; never serialized into emulation state. */
    mutable CRenderSurface *clearedTarget0;
    mutable CRenderSurface *clearedTarget1;

    Impl()
        : loaded(FALSE),
          hasCgbBootRom(FALSE), romBytes(0), romCRC(0),
          turboFrame(0),
          audioSumL(0), audioSumR(0), audioPhase(0), biosHostFast(FALSE),
          clearedTarget0(NULL), clearedTarget1(NULL)
    {
        memset(biosAudio, 0, sizeof(biosAudio));
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

/* AURORA_NO_SGB_ELF_V4_20260921: dynamic SGB frontend removed. */
/* AURORA_NO32X_SGB_AUDIT_V4_1_20260921: lifecycle/state audit only. */
static void AuroraGbConfigureVideo(GambatteSystem::Impl *p)
{
    if (!p) return;
    p->gb.setColorCorrectionMode(1U);
    p->gb.setColorCorrection(false);
    p->gb.setColorCorrectionBrightness(0.0f);
    p->gb.setDarkFilterLevel(0U);
}

static unsigned AuroraGbMapInput(const Emu::SysInputT *pInput,
                                 Uint32 turboFrame)
{
    Uint16 pad = pInput ? pInput->uPad[0] : EMUSYS_DEVICE_DISCONNECTED;
    const Bool turboOn = ((turboFrame & 1U) == 0U) ? TRUE : FALSE;

    if (pad == EMUSYS_DEVICE_DISCONNECTED)
        return 0;

    /* AURORA_CODEPATH_SIMPLIFY_V5_20260927
     * Start from the common A/B/Select/Start/U/D/L/R byte, then permute only
     * the four direction bits into Gambatte's R/L/U/D ordering. */
    const Uint8 nes = SnesIOPadToNes8(pad);
    Uint8 out = (Uint8)(
        (nes & 0x0FU) |
        ((nes & 0x80U) >> 3) |
        ((nes & 0x40U) >> 1) |
        ((nes & 0x30U) << 2));

    if (turboOn)
    {
        out |= (Uint8)(
            ((pad & SNESIO_JOY_A) >> 7) | /* Circle   -> turbo A */
            ((pad & SNESIO_JOY_X) >> 5)); /* Triangle -> turbo B */
    }

    return SnesIOFilterOppositeDirections8(out);
}

/* AURORA_VOLUME_TFA_N163_V4_20260913 */
static Int16 AuroraGbScaleVolume(Int16 sample, Int32 gain)
{
    Int32 v;
    if (gain <= 0) return 0;
    if (gain == 200) return sample;
    v = ((Int32)sample * gain) / 200;
    if (v > 32767) v = 32767;
    if (v < -32768) v = -32768;
    return (Int16)v;
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

    const Int32 gain = VideoGetGbcVolume();

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
            mono = AuroraGbScaleVolume(mono, gain);
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

/* AURORA_GAMBATTE_TARGET_INIT_V13_20260911
 * Aurora alternates two shared 256x240 software surfaces. Gambatte updates
 * only 160x144 pixels, so a newly encountered target must have its margins
 * initialized before the complete surface is uploaded to GS. The cost is paid
 * once per target per loaded handheld session, not once per frame. */
static void AuroraGbEnsureTargetBlack(const GambatteSystem::Impl *p,
                                      CRenderSurface *pTarget)
{
    Int32 y, x;
    if (!p || !pTarget)
        return;
    if (p->clearedTarget0 == pTarget || p->clearedTarget1 == pTarget)
        return;

    for (y = 0; y < 240; ++y)
    {
        Uint32 *dst = (Uint32 *)pTarget->GetLinePtr(y);
        for (x = 0; x < 256; ++x)
            dst[x] = 0xff000000U;
    }

    if (!p->clearedTarget0)
        p->clearedTarget0 = pTarget;
    else if (!p->clearedTarget1)
        p->clearedTarget1 = pTarget;
    else
    {
        /* Defensive fallback if a future frontend rotates >2 surfaces. */
        p->clearedTarget0 = p->clearedTarget1;
        p->clearedTarget1 = pTarget;
    }
}

static void AuroraGbRender(const GambatteSystem::Impl *p,
                           CRenderSurface *pTarget)
{
    const Int32 dx = 48;
    const Int32 dy = 42;
    Int32 y, x;

    if (!p || !pTarget ||
        pTarget->GetWidth() < 256U || pTarget->GetHeight() < 240U)
        return;

    AuroraGbEnsureTargetBlack(p, pTarget);
    for (y = 0; y < 144; ++y)
    {
        const gambatte::video_pixel_t *src = p->screen + y * 160;
        Uint32 *dst = (Uint32 *)pTarget->GetLinePtr(dy + y) + dx;
        for (x = 0; x < 160; ++x)
            dst[x] = AuroraGbRgb32ToSurface((Uint32)src[x], TRUE);
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
    if (!pData || nBytes < 0x150U || eMode != STANDALONE_CGB)
        return FALSE;

    UnloadGame();
    m_p = new (std::nothrow) Impl;
    if (!m_p)
        return FALSE;

    if (!pCgbBootRom || nCgbBootRomBytes != AURORA_GB_CGB_BOOT_BYTES)
    {
        delete m_p;
        m_p = NULL;
        return FALSE;
    }

    m_p->loaded = TRUE;
    memcpy(m_p->cgbBootRom, pCgbBootRom, AURORA_GB_CGB_BOOT_BYTES);
    m_p->hasCgbBootRom = TRUE;
    m_p->gb.setInputGetter(&m_p->input);
    m_p->gb.setDarkFilterLevel(0U);
    g_AuroraGbBootHost = m_p;
    m_p->gb.setBootloaderGetter(&AuroraGbCgbBootloaderGetter);

    if (!m_p->turboFile.Init(AuroraRomIdentityCRCForDetection(uCRC)))
    {
        if (g_AuroraGbBootHost == m_p) g_AuroraGbBootHost = NULL;
        m_p->loaded = FALSE;
        m_p->gb.setSerialIO(NULL);
        m_p->gb.setBootloaderGetter(NULL);
        m_p->gb.setInputGetter(NULL);
        delete m_p;
        m_p = NULL;
        return FALSE;
    }
    m_p->gb.setSerialIO(m_p->turboFile.Active() ? &m_p->turboFile : NULL);

    if (m_p->gb.load(pData, (unsigned)nBytes, gambatte::GB::FORCE_CGB) != 0)
    {
        if (g_AuroraGbBootHost == m_p) g_AuroraGbBootHost = NULL;
        m_p->loaded = FALSE;
        m_p->gb.setSerialIO(NULL);
        m_p->gb.setBootloaderGetter(NULL);
        m_p->gb.setInputGetter(NULL);
        delete m_p;
        m_p = NULL;
        return FALSE;
    }
    if (g_AuroraGbBootHost == m_p) g_AuroraGbBootHost = NULL;

    m_p->gb.setInputGetter(&m_p->input);
    AuroraGbConfigureVideo(m_p);
    m_p->romBytes = nBytes;
    m_p->romCRC = uCRC;
    m_p->turboFrame = 0;
    m_p->audioSumL = m_p->audioSumR = 0;
    m_p->audioPhase = 0;
    m_p->biosHostFast = FALSE;
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

    if (g_AuroraGbBootHost == m_p) g_AuroraGbBootHost = NULL;
    m_p->loaded = FALSE;
    m_p->input.Set(0);
    m_p->gb.setSerialIO(NULL);
    m_p->gb.setBootloaderGetter(NULL);
    m_p->gb.setInputGetter(NULL);
    m_p->turboFile.ResetProtocol();
    m_p->audioSumL = m_p->audioSumR = 0;
    m_p->audioPhase = 0;
    m_p->biosHostFast = FALSE;

    delete m_p;
    m_p = NULL;
    m_uLine = 0;
    m_uFrame = 0;
}

Bool GambatteSystem::IsGameLoaded() const
{
    return (m_p && m_p->loaded) ? TRUE : FALSE;
}

/* AURORA_GAMBATTE_SQUARE_ASPECT_V13_20260911
 * The standalone CGB path is also Aurora's normal GB/GBC handheld path.
 * Its LCD pixels are square. Keep dynamic SGB out: SGB presentation belongs
 * to the SNES/TV pixel-aspect domain rather than the handheld LCD domain. */
Bool GambatteSystem::UsesSquarePixelPresentation() const
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

    g_AuroraGbBootHost = m_p;
    m_p->gb.setBootloaderGetter(&AuroraGbCgbBootloaderGetter);
    m_p->gb.setSerialIO(m_p->turboFile.Active() ? &m_p->turboFile : NULL);
    m_p->turboFile.ResetProtocol();
    m_p->gb.reset();
    if (g_AuroraGbBootHost == m_p) g_AuroraGbBootHost = NULL;
    m_p->gb.setInputGetter(&m_p->input);
    AuroraGbConfigureVideo(m_p);
    m_p->turboFrame = 0;
    m_p->audioSumL = m_p->audioSumR = 0;
    m_p->audioPhase = 0;
    m_p->biosHostFast = FALSE;
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
        m_p->gb.isBootloaderActive()
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
        m_p->gb.isBootloaderActive()
            ? TRUE : FALSE;
    if (m_p->biosHostFast && !biosAtFrameEnd)
    {
        AuroraGbConfigureVideo(m_p);
        m_p->biosHostFast = FALSE;
    }

    /* AURORA_GB_STANDALONE_R5_ROUTE_BIOS_TURBO_20260909
     * BIOS audio batching stays enabled, but presentation must be every
     * frontend frame.  Alternating this copy produced an obvious ~30 Hz
     * blink during the CGB boot ROM/fade. */
    if (pTarget)
        AuroraGbRender(m_p, pTarget);

    if (pMixBuf) pMixBuf->Flush();
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
    s->Magic = AURORA_GB_STATE_MAGIC;
    return TRUE;
}

Bool GambatteSystem::RestoreStateChecked(const void *pState, Int32 nStateBytes)
{
    const AuroraGbStateT *s = (const AuroraGbStateT *)pState;
    if (!m_p || !m_p->loaded || !s ||
        nStateBytes < (Int32)sizeof(*s) ||
        s->Magic != AURORA_GB_STATE_MAGIC ||
        (s->Version != 1U && s->Version != 2U &&
         s->Version != AURORA_GB_STATE_VERSION) ||
        !s->CoreBytes || s->CoreBytes > AURORA_GB_STATE_PAYLOAD)
        return FALSE;

    if (!m_p->gb.loadState(s->Core, (size_t)s->CoreBytes))
        return FALSE;

    /* No long-lived boot-host pointer: reset/load establishes it only while
     * boot bytes are actually being fetched. */
    g_AuroraGbBootHost = NULL;
    m_p->gb.setBootloaderGetter(&AuroraGbCgbBootloaderGetter);
    m_p->gb.setSerialIO(m_p->turboFile.Active() ? &m_p->turboFile : NULL);
    m_p->turboFile.ResetProtocol();
    m_p->gb.setInputGetter(&m_p->input);
    AuroraGbConfigureVideo(m_p);
    m_p->biosHostFast = FALSE;
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
