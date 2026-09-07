#include <string.h>
#include "snsgb_icd2.h"

/* AURORA_SGB_HANDSHAKE_TRACE_V0_6_10_20260905 */
extern "C" void AuroraSgbBootTrace(const char *pText);

/* AURORA_SGB_POST_FB_CLOCK_TRACE_V0_6_12_20260905 */
static Bool g_bAuroraSgbFBConsumed = FALSE;
/* AURORA_SGB_FB_6003_ORDER_TRACE_V0_6_13_20260905 */
static Uint8 g_uAuroraSgbPostFB6003Trace = 0;
/* AURORA_SGB_STICKY_FB_RESULT_V0_6_13_1_20260905
 * 1=FB/RUN no write, 2=FB/RESET no write,
 * 3=RUN->RUN, 4=RUN->RESET, 5=RESET->RUN, 6=RESET->RESET. */
static Uint8 g_uAuroraSgbPostFBCause = 0;
extern "C" Bool AuroraSgbDebugFBConsumed(void) { return g_bAuroraSgbFBConsumed; }
extern "C" Uint8 AuroraSgbDebugPostFBCause(void) { return g_uAuroraSgbPostFBCause; }

/* AURORA_SGB_ICD2_V0_2_20260904 */

SNSGBICD2::SNSGBICD2() { Reset(MODEL_NONE); }

void SNSGBICD2::Reset(ModelE eModel)
{
    g_bAuroraSgbFBConsumed = FALSE;
    g_uAuroraSgbPostFB6003Trace = 0;
    g_uAuroraSgbPostFBCause = 0;
    m_eModel = eModel;
    m_uIcdRevision = 0x21;
    m_uControl = 0;
    m_bPacketReady = FALSE;
    m_uReadBank = 0;
    m_uReadAddress = 0;
    m_uWriteBank = 0;
    m_nVCounter = 0;
    m_uHCounter = 0;

    m_uJoypID = 3;
    m_bPreviousP15 = FALSE;
    m_bPulseLock = TRUE;
    m_bStrobeLock = FALSE;
    m_bPacketLock = FALSE;
    m_uPacketOffset = 0;
    m_uBitData = 0;
    m_uBitOffset = 0;
    m_bResetRequested = FALSE;

    memset(m_uController, 0xff, sizeof(m_uController));
    memset(m_uPacket, 0, sizeof(m_uPacket));
    memset(m_uOutput, 0xff, sizeof(m_uOutput));
    m_uClockAccumulator = 0;
}

Bool SNSGBICD2::IsMappedAddress(Uint32 uAddr)
{
    Uint32 d = uAddr & 0x40f80fU;
    if (d == 0x6000U || d == 0x6001U || d == 0x6002U ||
        d == 0x6003U || d == 0x6004U || d == 0x6005U ||
        d == 0x6006U || d == 0x6007U || d == 0x600fU)
        return TRUE;
    if (d >= 0x7000U && d <= 0x700fU) return TRUE;
    if (d >= 0x7800U && d <= 0x780fU) return TRUE;
    return FALSE;
}

Uint8 SNSGBICD2::Read(Uint32 uAddr)
{
    if (!IsMappedAddress(uAddr)) return 0;
    return ReadDecoded(uAddr & 0x40f80fU);
}

void SNSGBICD2::Write(Uint32 uAddr, Uint8 uData)
{
    if (IsMappedAddress(uAddr))
        WriteDecoded(uAddr & 0x40f80fU, uData);
}

Uint8 SNSGBICD2::ReadDecoded(Uint32 d)
{
    if (d == 0x6000U) {
        /* ICD LY port exposes the real line rounded down to its 8-line
           tile row, including VBlank 144..153, ORed with writeBank. */
        Uint8 ly8 = (Uint8)((Uint8)m_nVCounter & (Uint8)~7U);
        return (Uint8)(ly8 | (m_uWriteBank & 3U));
    }

    if (d == 0x6002U) {
        /* Reading $6002 has no side effect; reading $7000 clears ready. */
        return m_bPacketReady ? 1 : 0;
    }

    if (d == 0x600fU) return m_uIcdRevision;

    if (d >= 0x7000U && d <= 0x700fU) {
        Uint8 i = (Uint8)(d & 15U);
        if (i == 0) {
            switch (m_uPacket[0])
            {
                case 0xF1: AuroraSgbBootTrace("SGB H60: F1 consumed"); break;
                case 0xF3: AuroraSgbBootTrace("SGB H61: F3 consumed"); break;
                case 0xF5: AuroraSgbBootTrace("SGB H62: F5 consumed"); break;
                case 0xF7: AuroraSgbBootTrace("SGB H63: F7 consumed"); break;
                case 0xF9: AuroraSgbBootTrace("SGB H64: F9 consumed"); break;
                case 0xFB:
                    g_bAuroraSgbFBConsumed = TRUE;
                    g_uAuroraSgbPostFB6003Trace = 0;
                    g_uAuroraSgbPostFBCause = (m_uControl & 0x80U) ? 1U : 2U;
                    AuroraSgbBootTrace("SGB H65: FB consumed");
                    AuroraSgbBootTrace((m_uControl & 0x80U)
                        ? "SGB H82: FB consumed while RUN"
                        : "SGB H83: FB consumed while RESET");
                    break;
                default: break;
            }
            m_bPacketReady = FALSE;
        }
        return m_uPacket[i];
    }

    if (d >= 0x7800U && d <= 0x780fU) {
        Uint8 v = m_uOutput[m_uReadBank & 3U][m_uReadAddress & 0x1ffU];
        m_uReadAddress = (Uint16)((m_uReadAddress + 1U) & 0x1ffU);
        return v;
    }

    return 0;
}

void SNSGBICD2::WriteDecoded(Uint32 d, Uint8 uData)
{
    if (d == 0x6001U) {
        m_uReadBank = uData & 3U;
        m_uReadAddress = 0;
        return;
    }

    if (d == 0x6003U) {
        Uint8 old = m_uControl;
        if (g_bAuroraSgbFBConsumed && !g_uAuroraSgbPostFB6003Trace) {
            g_uAuroraSgbPostFB6003Trace = 1;
            if (old & 0x80U) {
                if (uData & 0x80U) {
                    g_uAuroraSgbPostFBCause = 3U;
                    AuroraSgbBootTrace("SGB H84: first $6003 RUN->RUN");
                } else {
                    g_uAuroraSgbPostFBCause = 4U;
                    AuroraSgbBootTrace("SGB H85: first $6003 RUN->RESET");
                }
            } else {
                if (uData & 0x80U) {
                    g_uAuroraSgbPostFBCause = 5U;
                    AuroraSgbBootTrace("SGB H86: first $6003 RESET->RUN");
                } else {
                    g_uAuroraSgbPostFBCause = 6U;
                    AuroraSgbBootTrace("SGB H87: first $6003 RESET->RESET");
                }
            }
        }
        if (g_bAuroraSgbFBConsumed && ((old ^ uData) & 0x80U)) {
            AuroraSgbBootTrace((uData & 0x80U)
                ? "SGB H81: $6003 -> RUN"
                : "SGB H80: $6003 -> RESET");
        }
        if (!(old & 0x80U) && (uData & 0x80U)) {
            /* Rising RESET powers the ICD side too: packet/VRAM ports, JOYP
               parser and LCD banks return to power state before the freshly
               written control value takes effect. */
            ModelE model = m_eModel;
            Reset(model);
            m_bResetRequested = TRUE;
            old = 0;
        }
        if ((old & 3U) != (uData & 3U))
            m_uClockAccumulator = 0;
        m_uControl = uData;
        m_uJoypID &= ControllerMask();
        return;
    }

    if (d >= 0x6004U && d <= 0x6007U)
        m_uController[d - 0x6004U] = uData;
}

Bool SNSGBICD2::ConsumeResetRequest()
{
    Bool b = m_bResetRequested;
    m_bResetRequested = FALSE;
    return b;
}

Uint8 SNSGBICD2::ControllerMask() const
{
    Uint8 m = (m_uControl >> 4) & 3U;
    if (m == 0) return 0;
    if (m == 1) return 1;
    return 3;
}

Uint8 SNSGBICD2::GetControllerCount() const
{
    return (Uint8)(ControllerMask() + 1U);
}

Uint8 SNSGBICD2::GetControllerByte(Int32 iController) const
{
    return (iController >= 0 && iController < 4)
        ? m_uController[iController] : (Uint8)0xff;
}

Uint8 SNSGBICD2::JoypInput(Bool p14, Bool p15) const
{
    Uint8 joy, input = 0x0f;
    p14 = p14 ? TRUE : FALSE;
    p15 = p15 ? TRUE : FALSE;
    joy = m_uController[m_uJoypID & 3U];
    if (p14 && p15) input = (Uint8)(0x0fU - (m_uJoypID & 3U));
    if (!p14) input &= joy & 0x0fU;
    if (!p15) input &= (joy >> 4) & 0x0fU;
    return input;
}

Uint8 SNSGBICD2::JoypRead(Bool p14, Bool p15) const
{
    return JoypInput(p14, p15);
}

Uint8 SNSGBICD2::JoypWrite(Bool p14, Bool p15)
{
    Uint8 input;
    Bool bit;

    p14 = p14 ? TRUE : FALSE;
    p15 = p15 ? TRUE : FALSE;

    if (p15 && !m_bPreviousP15) {
        m_uJoypID++;
        m_uJoypID &= ControllerMask();
    }
    m_bPreviousP15 = p15;
    /* Controller selection changes on the P15 edge before this transfer
       input nibble is sampled. */
    input = JoypInput(p14, p15);

    if (!p14 && !p15) {
        m_bPulseLock = FALSE;
        m_uPacketOffset = 0;
        m_uBitOffset = 0;
        m_bStrobeLock = TRUE;
        m_bPacketLock = FALSE;
        return input;
    }

    if (m_bPulseLock) return input;

    if (p14 && p15) {
        m_bStrobeLock = FALSE;
        return input;
    }

    if (m_bStrobeLock) {
        if (p14 || p15) {
            m_bPacketLock = FALSE;
            m_bPulseLock = TRUE;
            m_uBitOffset = 0;
            m_uPacketOffset = 0;
        }
        return input;
    }

    bit = !p15 ? TRUE : FALSE; /* p14=1,p15=0 => 1 */
    m_bStrobeLock = TRUE;

    if (m_bPacketLock) {
        if (!p14 && p15) {
            m_bPacketReady = TRUE;
            m_bPacketLock = FALSE;
            m_bPulseLock = TRUE;
        }
        return input;
    }

    m_uBitData = (Uint8)((bit ? 0x80U : 0U) | (m_uBitData >> 1));
    m_uBitOffset = (Uint8)((m_uBitOffset + 1U) & 7U);
    if (m_uBitOffset) return input;

    m_uPacket[m_uPacketOffset & 15U] = m_uBitData;
    m_uPacketOffset = (Uint8)((m_uPacketOffset + 1U) & 15U);
    if (!m_uPacketOffset) m_bPacketLock = TRUE;
    return input;
}

void SNSGBICD2::SubmitPacket(const Uint8 *pPacket)
{
    if (!pPacket) return;
    memcpy(m_uPacket, pPacket, PACKET_BYTES);
    m_bPacketReady = TRUE;
}

/* AURORA_SGB_HOTPATH_V2_20260907
 * PPUWrite/PPUHReset/PPUVReset moved inline to snsgb_icd2.h. */

void SNSGBICD2::PushLCDScanline(Int32 nLine, const Uint8 *pShade2Bit)
{
    Int32 tile, px;
    Uint32 rowBase;

    if (!pShade2Bit || nLine < 0 || nLine >= LCD_VISIBLE_LINES)
        return;


    m_nVCounter = nLine;
    rowBase = (Uint32)(nLine & 7) * 2U;

    for (tile = 0; tile < 20; ++tile) {
        Uint8 p0 = 0, p1 = 0;
        for (px = 0; px < 8; ++px) {
            Uint8 c = pShade2Bit[tile * 8 + px] & 3U;
            Uint8 b = (Uint8)(0x80U >> px);
            if (c & 1U) p0 |= b;
            if (c & 2U) p1 |= b;
        }
        m_uOutput[m_uWriteBank & 3U][(Uint32)tile * 16U + rowBase] = p0;
        m_uOutput[m_uWriteBank & 3U][(Uint32)tile * 16U + rowBase + 1U] = p1;
    }
}

void SNSGBICD2::EndLCDLine(Int32 nLine)
{
    if (nLine < 0 || nLine >= LCD_TOTAL_LINES) return;

    m_nVCounter = nLine;

    /* AURORA_SGB_ICD2_HRESET_BANK_V1_20260906
     * The ICD2 write-row advances only after eight VISIBLE GB LCD scanlines
     * have actually filled one 2bpp character row. VBlank lines 144..153 do
     * not push pixels into the four-row ring and therefore must not rotate it.
     * The row counter itself still advances through VBlank and resets on
     * VSync below; $6000 may consequently report >= $88, which the SGB
     * firmware intentionally rejects while waiting for visible data. */
    if (((nLine + 1) & 7) == 0)
        m_uWriteBank = (Uint8)((m_uWriteBank + 1U) & 3U);

    m_nVCounter = (nLine == LCD_TOTAL_LINES - 1) ? 0 : nLine + 1;
}

Uint32 SNSGBICD2::GetClockDivider() const
{
    static const Uint8 s_Divider[4] = {4, 5, 7, 9};
    return s_Divider[m_uControl & 3U];
}

Uint32 SNSGBICD2::AdvanceMasterClocks(Uint32 clocks, Uint32 snesHz)
{
    Uint64 sourceHz, den, total, steps;

    if (m_eModel == MODEL_NONE || !IsRunning() || !clocks || !snesHz)
        return 0;

    sourceHz = (m_eModel == MODEL_SGB2) ? (Uint64)SGB2_OSC_HZ : (Uint64)snesHz;
    den = (Uint64)snesHz * (Uint64)GetClockDivider();
    total = m_uClockAccumulator + (Uint64)clocks * sourceHz;
    steps = total / den;
    m_uClockAccumulator = total % den;

    return steps > 0xffffffffULL ? 0xffffffffU : (Uint32)steps;
}

Bool SNSGBICD2::SaveState(StateT *s) const
{
    if (!s) return FALSE;
    memset(s, 0, sizeof(*s));

    s->magic = STATE_MAGIC;
    s->version = STATE_VERSION;
    s->model = (Uint32)m_eModel;
    s->icdRevision = m_uIcdRevision;
    s->control = m_uControl;
    s->packetReady = m_bPacketReady;
    s->readBank = m_uReadBank;
    s->readAddress = m_uReadAddress;
    s->writeBank = m_uWriteBank;
    s->vcounter = m_nVCounter;
    s->hcounter = m_uHCounter;
    s->joypID = m_uJoypID;
    s->previousP15 = m_bPreviousP15;
    s->pulseLock = m_bPulseLock;
    s->strobeLock = m_bStrobeLock;
    s->packetLock = m_bPacketLock;
    s->packetOffset = m_uPacketOffset;
    s->bitData = m_uBitData;
    s->bitOffset = m_uBitOffset;
    s->resetRequested = m_bResetRequested;
    memcpy(s->controller, m_uController, sizeof(m_uController));
    memcpy(s->packet, m_uPacket, sizeof(m_uPacket));
    memcpy(s->output, m_uOutput, sizeof(m_uOutput));
    s->clockAccumulator = m_uClockAccumulator;
    return TRUE;
}

Bool SNSGBICD2::RestoreState(const StateT *s)
{
    if (!s || s->magic != STATE_MAGIC || s->version != STATE_VERSION ||
        s->model > (Uint32)MODEL_SGB2 ||
        s->readBank >= LCD_BANKS || s->writeBank >= LCD_BANKS ||
        s->readAddress >= LCD_BANK_BYTES || s->joypID >= 4 ||
        s->packetOffset >= PACKET_BYTES || s->bitOffset >= 8 ||
        s->vcounter < 0 || s->vcounter > 255 || s->hcounter > 0xffffU)
        return FALSE;

    m_eModel = (ModelE)s->model;
    m_uIcdRevision = (Uint8)s->icdRevision;
    m_uControl = (Uint8)s->control;
    m_bPacketReady = s->packetReady ? TRUE : FALSE;
    m_uReadBank = (Uint8)s->readBank;
    m_uReadAddress = (Uint16)s->readAddress;
    m_uWriteBank = (Uint8)s->writeBank;
    m_nVCounter = s->vcounter;
    m_uHCounter = (Uint16)s->hcounter;
    m_uJoypID = (Uint8)s->joypID;
    m_bPreviousP15 = s->previousP15 ? TRUE : FALSE;
    m_bPulseLock = s->pulseLock ? TRUE : FALSE;
    m_bStrobeLock = s->strobeLock ? TRUE : FALSE;
    m_bPacketLock = s->packetLock ? TRUE : FALSE;
    m_uPacketOffset = (Uint8)s->packetOffset;
    m_uBitData = (Uint8)s->bitData;
    m_uBitOffset = (Uint8)s->bitOffset;
    m_bResetRequested = s->resetRequested ? TRUE : FALSE;
    memcpy(m_uController, s->controller, sizeof(m_uController));
    memcpy(m_uPacket, s->packet, sizeof(m_uPacket));
    memcpy(m_uOutput, s->output, sizeof(m_uOutput));
    m_uClockAccumulator = s->clockAccumulator;
    return TRUE;
}
