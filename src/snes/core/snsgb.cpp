#include <string.h>
#include "snsgb.h"
#include <stdio.h> /* AURORA_SGB_V0_6_20_SNPRINTF_INCLUDE_RECOVERY_20260905 */

/* AURORA_SGB_ATTACH_TRACE_RECOVERY_V0_6_3_1_20260905 */
extern "C" void AuroraSgbBootTrace(const char *pText);
extern "C" Bool AuroraSgbDebugFBConsumed(void);
extern "C" Uint8 AuroraSgbDebugPostFBCause(void);

/* AURORA_SGB_HANDSHAKE_TRACE_V0_6_10_20260905 */
static Uint32 g_uAuroraSgbRuntimeResetCount = 0;
/* AURORA_SGB_FINAL_WAIT_TRACE_V0_6_11_20260905 */
static Uint8 g_uAuroraSgbFinalWaitTrace = 0;
static Uint8 g_uAuroraSgbFirstRunTrace = 0;
/* AURORA_SGB_POST_FB_CLOCK_TRACE_V0_6_12_20260905 */
static Uint8 g_uAuroraSgbClockBridgeTrace = 0;
/* AURORA_SGB_STICKY_FB_RESULT_V0_6_13_1_20260905 */
/* AURORA_SGB_HOLD_BEFORE_FIRST_GB_RUN_V0_6_15_20260905
 * V0.6.15 proved the full SGB handshake and isolated the freeze to real GB
 * runtime. V0.6.16 retires the hard hold and releases GBHost through a
 * cooperative SM83 stepping path. */
/* AURORA_SGB_COOPERATIVE_GB_RUNTIME_V0_6_16_20260905 */
static Bool g_bAuroraSgbRuntimeReleased = FALSE;
/* AURORA_SGB_POST_RUNTIME_REHANDSHAKE_PROBE_V0_6_22_1_20260905 */
static Uint32 g_uAuroraSgbPostRuntimeResetCount = 0;
/* AURORA_SGB_PRE_EVENT_HARD_PROBE_V0_6_17_20260905: hold after reading pre-SM83Tick state so OSD can render it. */
static Bool g_bAuroraSgbPreTickProbeHeld = FALSE;
/* AURORA_SGB_DUE_EVENT_IDENTITY_PROBE_V0_6_20_20260905
         * Due-event identity probe layered on top of
         * AURORA_SGB_ABSOLUTE_DEADLINE_PRIME_V0_6_19_20260905.
         *
         *
         * Corrected absolute-deadline prime layered on top of
         * AURORA_SGB_SCHEDULER_PRIME_V0_6_18_20260905.
         *
         *: hold only if primed scheduler remains unsafe. */
static Bool g_bAuroraSgbPreTickUnsafeHold = FALSE;

/* AURORA_SGB_RUNTIME_V0_4_20260904 */

SNSuperGameBoy::SNSuperGameBoy()
    : m_bActive(FALSE), m_eModel(MODEL_SGB1),
      m_uGameBytes(0), m_uGameCRC(0),
      m_uBootPacketIndex(0), m_uBootWaitClocks(0),
      m_bBootHandshake(FALSE), m_uBootLine(0), m_uBootLineClocks(0),
      m_uAudioPhase(0), m_iAudioSumLeft(0), m_iAudioSumRight(0), m_uAudioCount(0)
{
    memset(m_uBootHeader, 0, sizeof(m_uBootHeader));
    m_ICD2.Reset(SNSGBICD2::MODEL_NONE);
}

SNSuperGameBoy::~SNSuperGameBoy() { Detach(); }

Bool SNSuperGameBoy::AttachGame(
    const Uint8 *pData, Uint32 nBytes, ModelE eModel,
    const Uint8 *pBootRom, Uint32 nBootRomBytes) /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */
{
    AuroraSgbBootTrace("SGB 4A: detach old");
    Detach();
    AuroraSgbBootTrace("SGB 4B: detached");

    if (!pData || nBytes < (0x104U + BOOT_HEADER_BYTES))
        return FALSE; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */

    g_uAuroraSgbRuntimeResetCount = 0;
    {
        Uint8 h = 0;
        Uint32 i;
        for (i = 0x134U; i <= 0x14cU; ++i)
            h = (Uint8)(h - pData[i] - 1U);
        AuroraSgbBootTrace(h == pData[0x14dU]
            ? "SGB H00: header checksum OK"
            : "SGB H00: header checksum BAD");
    }

    m_eModel = (eModel == MODEL_SGB2) ? MODEL_SGB2 : MODEL_SGB1;
    memcpy(m_uBootHeader, pData + 0x104U, BOOT_HEADER_BYTES);
    ResetBootHandshake();

    AuroraSgbBootTrace("SGB 4C: GBHost init");
    if (!m_GB.Init())
    {
        m_GB.Shutdown();
        return FALSE;
    }

    AuroraSgbBootTrace("SGB 4D: GBHost load ROM");
    if (!m_GB.LoadROM(
            pData, nBytes,
            m_eModel == MODEL_SGB2 ? GBHost::MODEL_SGB2 : GBHost::MODEL_SGB1,
            pBootRom, nBootRomBytes))
    {
        m_GB.Shutdown();
        return FALSE;
    }

    AuroraSgbBootTrace("SGB 4E: ROM identity");
    m_uGameBytes = m_GB.GetROMBytes();
    m_uGameCRC = m_GB.GetROMCRC();
    if (!m_uGameBytes)
    {
        m_GB.UnloadROM();
        return FALSE;
    }

    AuroraSgbBootTrace("SGB 4F: ICD2 reset");
    m_ICD2.Reset(m_eModel == MODEL_SGB2
        ? SNSGBICD2::MODEL_SGB2 : SNSGBICD2::MODEL_SGB1);
    AuroraSgbBootTrace("SGB 4G: install hooks");
    /* AURORA_SGB_HOTPATH_V2_20260907 */
    m_GB.SetICD2FastPath(&m_ICD2);
    m_GB.SetHooks(&SNSuperGameBoy::JoypHook,
                  &SNSuperGameBoy::PixelHook,
                  &SNSuperGameBoy::HResetHook,
                  &SNSuperGameBoy::VResetHook, this);
    ResetAudioPipeline();
    m_bActive = TRUE;
    AuroraSgbBootTrace("SGB 4H: attach ready");
    return TRUE;
}

void SNSuperGameBoy::Detach()
{
    m_GB.SetICD2FastPath(NULL);
    m_GB.SetHooks(NULL, NULL, NULL, NULL, NULL);
    m_GB.UnloadROM();
    m_ICD2.Reset(SNSGBICD2::MODEL_NONE);
    m_bActive = FALSE;
    m_uGameBytes = 0;
    m_uGameCRC = 0;
    memset(m_uBootHeader, 0, sizeof(m_uBootHeader));
    ResetBootHandshake();
    ResetAudioPipeline();
}

void SNSuperGameBoy::ResetBootHandshake()
{
    m_uBootPacketIndex = 0;
    m_uBootWaitClocks = 0;
    m_bBootHandshake = FALSE;
    m_uBootLine = 0;
    m_uBootLineClocks = 0;
}

void SNSuperGameBoy::SubmitBootPacket()
{
    Uint8 packet[SNSGBICD2::PACKET_BYTES];
    Uint8 checksum = 0;
    Uint32 i, base;
    if (!m_bBootHandshake || m_uBootPacketIndex >= BOOT_PACKET_COUNT) return;

    memset(packet, 0, sizeof(packet));
    packet[0] = (Uint8)(0xF1U + (m_uBootPacketIndex << 1));
    base = (Uint32)m_uBootPacketIndex * BOOT_PACKET_DATA_BYTES;
    for (i = 0; i < BOOT_PACKET_DATA_BYTES; ++i)
    {
        Uint8 v = m_uBootHeader[base + i]; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */
        packet[2U + i] = v;
        checksum = (Uint8)(checksum + v);
    }
    packet[1] = checksum;
    switch (m_uBootPacketIndex)
    {
        case 0: AuroraSgbBootTrace("SGB H50: F1 queued"); break;
        case 1: AuroraSgbBootTrace("SGB H51: F3 queued"); break;
        case 2: AuroraSgbBootTrace("SGB H52: F5 queued"); break;
        case 3: AuroraSgbBootTrace("SGB H53: F7 queued"); break;
        case 4: AuroraSgbBootTrace("SGB H54: F9 queued"); break;
        case 5: AuroraSgbBootTrace("SGB H55: FB queued"); break;
        default: break;
    }
    m_ICD2.SubmitPacket(packet);
    ++m_uBootPacketIndex;

    /* AURORA_SGB_BSNES_BOOT_CADENCE_V1_2_5_20260907
     * bsnes/libsupergameboy exposes the six internal-bootstrap packets
     * without an artificial four-frame gap. Aurora currently has one ICD2
     * packet slot, so AdvanceBootHandshake() refills it immediately after
     * the SNES BIOS consumes the current packet. */
    m_uBootWaitClocks = 0;
}

void SNSuperGameBoy::ResetAudioPipeline()
{
    m_uAudioPhase = 0;
    m_iAudioSumLeft = 0;
    m_iAudioSumRight = 0;
    m_uAudioCount = 0;
    m_GB.ClearAudio();
}

Int16 SNSuperGameBoy::Saturate16(Int32 value)
{
    if (value > 32767) return 32767;
    if (value < -32768) return -32768;
    return (Int16)value;
}

void SNSuperGameBoy::BeginBootHandshake()
{
    /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908
     * Real SGB/SGB2 SM83 boot ROM is mandatory. Legacy packet-HLE helpers
     * remain compiled for historical diagnostics, but this entry point never
     * queues F1/F3/F5/F7/F9/FB itself. The boot ROM drives FF00/ICD2. */
    ResetBootHandshake();

    g_uAuroraSgbFinalWaitTrace = 0;
    g_uAuroraSgbFirstRunTrace = 0;
    g_uAuroraSgbClockBridgeTrace = 0;
    g_bAuroraSgbRuntimeReleased = FALSE;
    g_bAuroraSgbPreTickProbeHeld = FALSE;
    g_bAuroraSgbPreTickUnsafeHold = FALSE;

    ResetAudioPipeline();
    if (!m_GB.HasRealBootROM())
    {
        AuroraSgbBootTrace("SGB H12: ERROR real SM83 bootstrap missing");
        return;
    }

    AuroraSgbBootTrace("SGB H12: real SM83 bootstrap owns handshake");
}

void SNSuperGameBoy::AdvanceBootLCD(Uint32 nGBClocks)
{
    Uint32 total = (Uint32)m_uBootLineClocks + nGBClocks;
    while (total >= BOOT_LCD_LINE_CLOCKS)
    {
        total -= BOOT_LCD_LINE_CLOCKS;
        m_ICD2.EndLCDLine((Int32)m_uBootLine);
        ++m_uBootLine;
        if (m_uBootLine >= SNSGBICD2::LCD_TOTAL_LINES) m_uBootLine = 0;
    }
    m_uBootLineClocks = (Uint16)total;
}

Uint32 SNSuperGameBoy::AdvanceBootHandshake(Uint32 nGBClocks)
{
    /* AURORA_SGB_BSNES_PACKET_FIFO_V1_2_6_20260907
     * Bootstrap packets are already queued; do not stall Gambatte. */
    return nGBClocks;
}

void SNSuperGameBoy::Reset()
{
    if (!m_bActive) return;
    m_ICD2.Reset(m_eModel == MODEL_SGB2
        ? SNSGBICD2::MODEL_SGB2 : SNSGBICD2::MODEL_SGB1);
    ResetBootHandshake();
    m_GB.Reset(m_eModel == MODEL_SGB2 ? GBHost::MODEL_SGB2 : GBHost::MODEL_SGB1);
    ResetAudioPipeline();
    /* AURORA_SGB_HOTPATH_V2_20260907 */
    m_GB.SetICD2FastPath(&m_ICD2);
    m_GB.SetHooks(&SNSuperGameBoy::JoypHook,
                  &SNSuperGameBoy::PixelHook,
                  &SNSuperGameBoy::HResetHook,
                  &SNSuperGameBoy::VResetHook, this);
}

Uint8 SNSuperGameBoy::Read(Uint32 uAddr, Uint8 uOpenBus)
{
    if (!m_bActive || !SNSGBICD2::IsMappedAddress(uAddr))
        return uOpenBus;
    return m_ICD2.Read(uAddr);
}

void SNSuperGameBoy::Write(Uint32 uAddr, Uint8 uData)
{
    if (!m_bActive || !SNSGBICD2::IsMappedAddress(uAddr))
        return;
    m_ICD2.Write(uAddr, uData);
    if (m_ICD2.ConsumeResetRequest())
    {
        if (g_bAuroraSgbRuntimeReleased)
        {
            Char msg[80];
            ++g_uAuroraSgbPostRuntimeResetCount;
            snprintf(
                msg, sizeof(msg),
                "SGB HD0: POST-RUNTIME $6003 RESET #%lu",
                (unsigned long)g_uAuroraSgbPostRuntimeResetCount
            );
            AuroraSgbBootTrace(msg);
        }

        ++g_uAuroraSgbRuntimeResetCount;
        switch (g_uAuroraSgbRuntimeResetCount)
        {
            case 1: AuroraSgbBootTrace("SGB H40: runtime reset #1"); break;
            case 2: AuroraSgbBootTrace("SGB H41: runtime reset #2"); break;
            case 3: AuroraSgbBootTrace("SGB H42: runtime reset #3"); break;
            default: AuroraSgbBootTrace("SGB H43: runtime reset #4+"); break;
        }

        m_GB.Reset(m_eModel == MODEL_SGB2 ? GBHost::MODEL_SGB2 : GBHost::MODEL_SGB1);
        AuroraSgbBootTrace("SGB H44: GB reset returned");
        BeginBootHandshake();
        AuroraSgbBootTrace(
            m_GB.HasRealBootROM()
                ? "SGB H45: real boot ROM running"
                : "SGB H45: HLE handshake armed"); /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */
    }
}

void SNSuperGameBoy::AdvanceMasterClocks(Uint32 nClocks, Uint32 uSnesMasterHz)
{
    /* AURORA_SGB_NATIVE_RUNTIME_BOOT_ATTEMPT_V0_6_29_20260906
     *
     * Diagnostic probes V0.6.17..V0.6.28 intentionally surrounded the first
     * runtime transition with holds, event inspection, audio-event skipping
     * and status spam. The hardware tests have already established:
     *   - ICD2 RUN is reached
     *   - F1..FB handshake reaches packet 6
     *   - final wait consumes clocks
     *   - SNES master -> GB ratio is correct (260 -> 52)
     *
     * Return to the minimal production clock path. No event is skipped and no
     * diagnostic hold can prevent GBHost::RunClocks from being entered.
     */
    if (!m_bActive || !nClocks)
        return;

    Uint32 nGB = m_ICD2.AdvanceMasterClocks(nClocks, uSnesMasterHz);

    if (nGB && m_bBootHandshake)
        nGB = AdvanceBootHandshake(nGB);

    if (nGB)
        (void)m_GB.RunClocks(nGB);
}

void SNSuperGameBoy::FlushClocks()
{
    if (m_bActive)
        m_GB.FlushClocks();
} /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */

/* AURORA_SGB_AUDIO_V0_5_20260904
 * Gambatte's GB PSG FIFO is sampled once per 32 logical GB clocks. Convert that
 * exact rational clock relationship to the SNES mixer's output domain with
 * a box-decimation accumulator. This is intentionally cartridge-local: the
 * normal SNES/NES/Sega/PCE host mixer never sees an SGB-specific mode.
 */
void SNSuperGameBoy::MixAudio(Int16 *pLeft, Int16 *pRight, Int32 nSamples, Uint32 uOutputHz)
{
    Int16 raw[256 * 2];
    Uint32 rawPos = 0, rawCount = 0;
    Uint32 produced = 0;
    Uint32 sourceHz;
    Uint64 denominator;

    if (!m_bActive || !pLeft || nSamples <= 0 || !uOutputHz || m_bBootHandshake)
        return;

    m_GB.FlushClocks(); /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */

    sourceHz = m_GB.GetClockHz();
    if (!sourceHz)
        return;

    /*
     * AURORA_SGB_GAMBATTE_RUNTIME_CURE_V1_20260906
     *
     * AURORA_SGB_GAMBATTE_BACKEND_V1_1_20260907
     * GBHost box-decimates Gambatte's native 1-sample/2-clock stream by 64,
     * preserving Aurora's existing one-FIFO-frame/128-GB-clocks contract.
     *
     * The old Aurora mixer was downsample-only and assumed 32 clocks/sample.
     * A PS2/SNES output domain such as 48 kHz therefore could not be represented
     * correctly. Use a rational zero-order hold resampler instead. Gambatte has
     * already band-limited the source; this stage only changes sample cadence.
     */
    denominator = (Uint64)uOutputHz * (Uint64)AUDIO_SOURCE_CLOCKS_PER_SAMPLE;
    if (!denominator)
        return;

    /* m_iAudioSum{Left,Right} are the persistent held source sample.
       m_uAudioCount is 0/1 validity in STATE_VERSION 4. */
    if (!m_uAudioCount)
    {
        if (m_GB.ReadAudioFrames(raw, 1) != 1)
            return;
        m_iAudioSumLeft = raw[0];
        m_iAudioSumRight = raw[1];
        m_uAudioCount = 1;
    }

    while (produced < (Uint32)nSamples)
    {
        Int32 gbL = m_iAudioSumLeft * AUDIO_GAIN_NUM / AUDIO_GAIN_DEN;
        Int32 gbR = m_iAudioSumRight * AUDIO_GAIN_NUM / AUDIO_GAIN_DEN;

        if (pRight)
        {
            pLeft[produced] =
                Saturate16((Int32)pLeft[produced] + gbL);
            pRight[produced] =
                Saturate16((Int32)pRight[produced] + gbR);
        }
        else
        {
            pLeft[produced] =
                Saturate16((Int32)pLeft[produced] + (gbL + gbR) / 2);
        }

        ++produced;
        m_uAudioPhase += sourceHz;

        while ((Uint64)m_uAudioPhase >= denominator)
        {
            if (rawPos >= rawCount)
            {
                Uint64 future;
                Uint64 need64;
                Uint32 request;

                /* Exact upper bound of source transitions still needed by
                   this output call. Never consume FIFO frames speculatively. */
                future = (Uint64)m_uAudioPhase +
                    (Uint64)((Uint32)nSamples - produced) * sourceHz;
                need64 = future / denominator;
                if (!need64)
                    need64 = 1;
                request = need64 > 256ULL ? 256U : (Uint32)need64;

                rawCount = m_GB.ReadAudioFrames(raw, request);
                rawPos = 0;
                if (!rawCount)
                    return;
            }

            m_iAudioSumLeft = raw[rawPos * 2U + 0U];
            m_iAudioSumRight = raw[rawPos * 2U + 1U];
            ++rawPos;
            m_uAudioPhase =
                (Uint32)((Uint64)m_uAudioPhase - denominator);
        }
    }
}

Uint8 SNSuperGameBoy::JoypHook(void *pContext, Bool bP14, Bool bP15, Bool bWrite)
{
    SNSuperGameBoy *p = (SNSuperGameBoy *)pContext;
    if (!p || !p->m_bActive) return 0x0f;
    return bWrite ? p->m_ICD2.JoypWrite(bP14, bP15)
                  : p->m_ICD2.JoypRead(bP14, bP15);
}

void SNSuperGameBoy::PixelHook(void *pContext, Uint8 uColor)
{
    SNSuperGameBoy *p = (SNSuperGameBoy *)pContext;
    if (p && p->m_bActive)
        p->m_ICD2.PPUWrite(uColor);
}

void SNSuperGameBoy::HResetHook(void *pContext)
{
    SNSuperGameBoy *p = (SNSuperGameBoy *)pContext;
    if (p && p->m_bActive)
        p->m_ICD2.PPUHReset();
}

void SNSuperGameBoy::VResetHook(void *pContext)
{
    SNSuperGameBoy *p = (SNSuperGameBoy *)pContext;
    if (p && p->m_bActive)
        p->m_ICD2.PPUVReset();
}

Bool SNSuperGameBoy::AttachSavedata(const Uint8 *pData, Uint32 nBytes)
{
    return m_bActive ? m_GB.AttachSavedata(pData, nBytes) : FALSE;
}
Uint32 SNSuperGameBoy::GetSavedataBytes()
{
    return m_bActive ? m_GB.GetSavedataBytes() : 0;
}
Bool SNSuperGameBoy::ExportSavedata(Uint8 *pData, Uint32 nCapacity, Uint32 *pActualBytes)
{
    return m_bActive ? m_GB.ExportSavedata(pData, nCapacity, pActualBytes) : FALSE;
}
Bool SNSuperGameBoy::SavedataDirty() const
{
    return m_bActive ? m_GB.SavedataDirty() : FALSE;
}
void SNSuperGameBoy::ClearSavedataDirty()
{
    if (m_bActive) m_GB.ClearSavedataDirty();
}

Uint32 SNSuperGameBoy::GetStateBytes()
{
    Uint32 nSave;
    Uint64 total;
    if (!m_bActive) return 0;
    nSave = m_GB.GetSavedataBytes();
    total = (Uint64)sizeof(StateHeaderT) +
            (Uint64)sizeof(SNSGBICD2::StateT) +
            (Uint64)sizeof(GBHost::StateT) + nSave;
    return total <= 0xffffffffULL ? (Uint32)total : 0;
}

Bool SNSuperGameBoy::SaveState(void *pData, Uint32 nBytes)
{
    StateHeaderT h;
    SNSGBICD2::StateT icd;
    GBHost::StateT gb;
    Uint8 *p;
    Uint32 nSave, actual = 0, need;
    if (!m_bActive || !pData) return FALSE;
    nSave = m_GB.GetSavedataBytes();
    need = (Uint32)(sizeof(h) + sizeof(icd) + sizeof(gb)) + nSave;
    if (nBytes != need) return FALSE;
    if (!m_ICD2.SaveState(&icd) || !m_GB.SaveState(&gb)) return FALSE;
    memset(&h, 0, sizeof(h));
    h.Magic = STATE_MAGIC; h.Version = STATE_VERSION;
    h.Model = (Uint32)m_eModel; h.GameBytes = m_uGameBytes; h.GameCRC = m_uGameCRC;
    h.ICDBytes = (Uint32)sizeof(icd); h.GBBytes = (Uint32)sizeof(gb); h.SaveBytes = nSave;
    h.BootPacketIndex = m_uBootPacketIndex;
    h.BootWaitClocks = m_uBootWaitClocks;
    h.BootHandshake = m_bBootHandshake ? 1U : 0U;
    h.BootLine = m_uBootLine;
    h.BootLineClocks = m_uBootLineClocks;
    h.AudioPhase = m_uAudioPhase;
    h.AudioSumLeft = m_iAudioSumLeft;
    h.AudioSumRight = m_iAudioSumRight;
    h.AudioCount = m_uAudioCount;
    p = (Uint8 *)pData;
    memcpy(p, &h, sizeof(h)); p += sizeof(h);
    memcpy(p, &icd, sizeof(icd)); p += sizeof(icd);
    memcpy(p, &gb, sizeof(gb)); p += sizeof(gb);
    if (nSave && !m_GB.ExportSavedata(p, nSave, &actual)) return FALSE;
    return actual == nSave ? TRUE : FALSE;
}

Bool SNSuperGameBoy::RestoreState(const void *pData, Uint32 nBytes)
{
    StateHeaderT h;
    SNSGBICD2::StateT icd;
    GBHost::StateT gb;
    const Uint8 *p;
    Uint64 need;
    if (!m_bActive || !pData || nBytes < sizeof(h)) return FALSE;
    p = (const Uint8 *)pData;
    memcpy(&h, p, sizeof(h)); p += sizeof(h);
    if (h.Magic != STATE_MAGIC || h.Version != STATE_VERSION ||
        (h.Model != MODEL_SGB1 && h.Model != MODEL_SGB2) ||
        h.Model != (Uint32)m_eModel || /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908: never cross-restore SGB1/SGB2 */
        h.GameBytes != m_uGameBytes || h.GameCRC != m_uGameCRC ||
        h.ICDBytes != sizeof(icd) || h.GBBytes != sizeof(gb) ||
        h.SaveBytes > MAX_SAVEDATA_BYTES ||
        h.BootPacketIndex > BOOT_PACKET_COUNT ||
        h.BootWaitClocks > BOOT_WAIT_CLOCKS ||
        h.BootHandshake > 1U ||
        h.BootLine >= SNSGBICD2::LCD_TOTAL_LINES ||
        h.BootLineClocks >= BOOT_LCD_LINE_CLOCKS ||
        h.AudioPhase >= m_GB.GetClockHz() ||
        h.AudioCount > 8U) return FALSE;
    need = (Uint64)sizeof(h) + h.ICDBytes + h.GBBytes + h.SaveBytes;
    if (need != nBytes) return FALSE;
    memcpy(&icd, p, sizeof(icd)); p += sizeof(icd);
    memcpy(&gb, p, sizeof(gb)); p += sizeof(gb);
    if (icd.model != h.Model || gb.Model != h.Model) return FALSE;
    if (!m_GB.AttachSavedata(p, h.SaveBytes)) return FALSE;
    if (!m_GB.RestoreState(&gb) || !m_ICD2.RestoreState(&icd)) return FALSE;
    m_eModel = (ModelE)h.Model;
    m_uBootPacketIndex = (Uint8)h.BootPacketIndex;
    m_uBootWaitClocks = h.BootWaitClocks;
    m_bBootHandshake = h.BootHandshake ? TRUE : FALSE;
    m_uBootLine = (Uint8)h.BootLine;
    m_uBootLineClocks = (Uint16)h.BootLineClocks;
    m_uAudioPhase = h.AudioPhase;
    m_iAudioSumLeft = h.AudioSumLeft;
    m_iAudioSumRight = h.AudioSumRight;
    m_uAudioCount = h.AudioCount;
    /* AURORA_SGB_HOTPATH_V2_20260907 */
    m_GB.SetICD2FastPath(&m_ICD2);
    m_GB.SetHooks(&SNSuperGameBoy::JoypHook,
                  &SNSuperGameBoy::PixelHook,
                  &SNSuperGameBoy::HResetHook,
                  &SNSuperGameBoy::VResetHook, this);
    return TRUE;
}
