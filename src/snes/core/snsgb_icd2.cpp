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

    m_uJoypID = 0;
    m_bJoyp15Lock = FALSE;
    m_bJoyp14Lock = FALSE; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */
    m_bPulseLock = TRUE;
    m_bStrobeLock = FALSE;
    m_bPacketLock = FALSE;
    m_uPacketOffset = 0;
    m_uBitData = 0;
    m_uBitOffset = 0;
    m_bResetRequested = FALSE;

    memset(m_uController, 0xff, sizeof(m_uController));
    memset(m_uPacket, 0, sizeof(m_uPacket));
    memset(m_uJoypPacket, 0, sizeof(m_uJoypPacket));
    m_uPacketQueueCount = 0;
    memset(m_uPacketQueue, 0, sizeof(m_uPacketQueue));
    memset(m_uOutput, 0x00, sizeof(m_uOutput)); /* bsnes-plus SGB ring power state */
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
        /* AURORA_SGB_GAMBATTE_BSNESPLUS_VIDEO_V1_2_4_20260907
         * Match bsnes-plus SuperGameBoy wrapper:
         * high bits = current 8-line vram_row;
         * low 2 bits = next ring slot being written. */
        return (Uint8)(((Uint8)m_nVCounter << 3) |
                       (m_uWriteBank & 3U));
    }

    if (d == 0x6002U) {
        /* AURORA_SGB_BSNES_PACKET_FIFO_V1_2_6_20260907
         * Match bsnes/libsupergameboy: polling $6002 dequeues the next
         * complete packet into the 16-byte $7000 register window. */
        if (m_uPacketQueueCount)
        {
            Uint32 i;
            memcpy(m_uPacket, m_uPacketQueue[0], PACKET_BYTES);
            --m_uPacketQueueCount;
            for (i = 0; i < m_uPacketQueueCount; ++i)
                memcpy(m_uPacketQueue[i], m_uPacketQueue[i + 1], PACKET_BYTES);
            memset(m_uPacketQueue[m_uPacketQueueCount], 0, PACKET_BYTES);
            m_bPacketReady = m_uPacketQueueCount ? TRUE : FALSE;
            return 1;
        }
        m_bPacketReady = FALSE;
        return 0;
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
            /* V1.2.6: FIFO advancement happens only at $6002. */
        }
        return m_uPacket[i];
    }

    if (d >= 0x7800U && d <= 0x780fU) {
        /* AURORA_SGB_GAMBATTE_BSNESPLUS_VIDEO_V1_2_4_20260907
         * 20 tiles * 16 bytes = exactly 320 bytes per tile row. */
        Uint8 v = m_uOutput[m_uReadBank & 3U][m_uReadAddress];
        ++m_uReadAddress;
        if (m_uReadAddress >= LCD_VISIBLE_BYTES)
            m_uReadAddress = 0;
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

    /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907
     * Later bsnes-plus joypad-ID lock sequence. */
    if (p15 && p14) {
        if (!m_bJoyp14Lock) {
            m_bJoyp14Lock = TRUE;
            m_uJoypID = (Uint8)((m_uJoypID + 1U) & ControllerMask());
        }
    }

    if (!p15) {
        if (m_bJoyp15Lock)
            m_bJoyp14Lock = m_bJoyp14Lock ? FALSE : TRUE;
    }
    m_bJoyp15Lock = p15;

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
            SubmitPacket(m_uJoypPacket);
            memset(m_uJoypPacket, 0, sizeof(m_uJoypPacket));
            m_bPacketLock = FALSE;
            m_bPulseLock = TRUE;
        }
        return input;
    }

    m_uBitData = (Uint8)((bit ? 0x80U : 0U) | (m_uBitData >> 1));
    m_uBitOffset = (Uint8)((m_uBitOffset + 1U) & 7U);
    if (m_uBitOffset) return input;

    m_uJoypPacket[m_uPacketOffset & 15U] = m_uBitData;
    m_uPacketOffset = (Uint8)((m_uPacketOffset + 1U) & 15U);
    if (!m_uPacketOffset) m_bPacketLock = TRUE;
    return input;
}

void SNSGBICD2::SubmitPacket(const Uint8 *pPacket)
{
    if (!pPacket) return;

    /* AURORA_SGB_BSNES_PACKET_FIFO_V1_2_6_20260907 */
    if (m_uPacketQueueCount >= PACKET_QUEUE_CAPACITY)
        return;

    memcpy(m_uPacketQueue[m_uPacketQueueCount], pPacket, PACKET_BYTES);
    ++m_uPacketQueueCount;
    m_bPacketReady = TRUE;
}

/* AURORA_SGB_HOTPATH_V2_20260907
 * PPUWrite/PPUHReset/PPUVReset moved inline to snsgb_icd2.h. */

/* AURORA_SGB_GAMBATTE_BSNESPLUS_VIDEO_V1_2_4_20260907 */
/* AURORA_SGB_GAMBATTE_SHADE8_JOYP_SYNC_PERF_V3_20260908 */
static inline void AuroraSgbPack8Shade(const Uint8 *s, Uint8 *p0, Uint8 *p1)
{
    /* Eight literal 0..3 shades -> Game Boy tile bitplanes. The old nested
       x-loop tested two branches per pixel. This fixed expression is both
       exact and much friendlier to the R5900 compiler. */
    *p0 = (Uint8)(
        ((s[0] & 1U) << 7) | ((s[1] & 1U) << 6) |
        ((s[2] & 1U) << 5) | ((s[3] & 1U) << 4) |
        ((s[4] & 1U) << 3) | ((s[5] & 1U) << 2) |
        ((s[6] & 1U) << 1) |  (s[7] & 1U));
    *p1 = (Uint8)(
        (((s[0] >> 1) & 1U) << 7) | (((s[1] >> 1) & 1U) << 6) |
        (((s[2] >> 1) & 1U) << 5) | (((s[3] >> 1) & 1U) << 4) |
        (((s[4] >> 1) & 1U) << 3) | (((s[5] >> 1) & 1U) << 2) |
        (((s[6] >> 1) & 1U) << 1) |  ((s[7] >> 1) & 1U));
}

void SNSGBICD2::GambatteNewLy(Uint32 uNewLy, const Uint8 *pFrame)
{
    Uint32 newRow, oldRow, y, tile;
    Uint8 *pDest;

    if (!pFrame || uNewLy >= LCD_TOTAL_LINES)
        return;

    /* Same bsnes-plus phase as V1.2.4: row 17 is committed when the next
       frame enters LY 0; VBlank itself leaves vram_row at 17. */
    if (uNewLy >= LCD_VISIBLE_LINES)
        return;

    newRow = uNewLy >> 3;
    oldRow = (Uint32)m_nVCounter;
    if (newRow == oldRow)
        return;

    if (oldRow >= (LCD_VISIBLE_LINES >> 3))
        oldRow = (LCD_VISIBLE_LINES >> 3) - 1U;

    pDest = m_uOutput[m_uWriteBank & 3U];

    /* AURORA_V4_4_CUMULATIVE_20260908
     * The loop below writes 20 * 8 * 2 = all 320 visible bytes. Clearing the
     * same row first is pure EE memory bandwidth on this LY hot path. */
    for (y = 0; y < 8U; ++y) {
        const Uint8 *src = pFrame + (oldRow * 8U + y) * LCD_WIDTH;
        for (tile = 0; tile < 20U; ++tile) {
            Uint8 p0, p1;
            AuroraSgbPack8Shade(src + tile * 8U, &p0, &p1);
            pDest[tile * 16U + y * 2U + 0U] = p0;
            pDest[tile * 16U + y * 2U + 1U] = p1;
        }
    }

    m_nVCounter = (Int32)newRow;
    m_uWriteBank = (Uint8)((m_uWriteBank + 1U) & 3U);
    m_uHCounter = 0;
}

void SNSGBICD2::PushLCDScanline(Int32 nLine, const Uint8 *pShade2Bit)
{
    Int32 tile, px;
    Uint32 rowBase;

    if (!pShade2Bit || nLine < 0 || nLine >= LCD_VISIBLE_LINES)
        return;


    /* AURORA_SGB_ICD2_RING_PHASE_V1_2_3_20260907
     * The ICD2 advances to the next write-row at the START of each
     * visible 8-line character row, including LY=0. */
    if ((nLine & 7) == 0)
        m_uWriteBank = (Uint8)((m_uWriteBank + 1U) & 3U);

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

    /* AURORA_SGB_ICD2_RING_PHASE_V1_2_3_20260907
     * Ring rotation now happens before PushLCDScanline() writes LY
     * 0,8,16,... . HBlank/VBlank only advances the line counter here. */

    m_nVCounter = (nLine == LCD_TOTAL_LINES - 1) ? 0 : nLine + 1;
}

Uint32 SNSGBICD2::GetClockDivider() const
{
    static const Uint8 s_Divider[4] = {4, 5, 7, 9};
    return s_Divider[m_uControl & 3U];
}

Uint32 SNSGBICD2::AdvanceMasterClocks(Uint32 clocks, Uint32 snesHz)
{
    Uint64 total, den, steps;
    Uint32 divider;

    if (m_eModel == MODEL_NONE || !IsRunning() || !clocks || !snesHz)
        return 0;

    divider = GetClockDivider();

    /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907
     * SGB1 derives its GB clock from the same SNES master oscillator, so the
     * frequency ratio cancels exactly. SGB2 keeps its independent oscillator. */
    if (m_eModel == MODEL_SGB1)
    {
        total = m_uClockAccumulator + (Uint64)clocks;
        steps = total / (Uint64)divider;
        m_uClockAccumulator = total % (Uint64)divider;
    }
    else
    {
        den = (Uint64)snesHz * (Uint64)divider;
        total = m_uClockAccumulator +
                (Uint64)clocks * (Uint64)SGB2_OSC_HZ;
        steps = total / den;
        m_uClockAccumulator = total % den;
    }

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
    s->joyp15Lock = m_bJoyp15Lock;
    s->joyp14Lock = m_bJoyp14Lock;
    s->pulseLock = m_bPulseLock;
    s->strobeLock = m_bStrobeLock;
    s->packetLock = m_bPacketLock;
    s->packetOffset = m_uPacketOffset;
    s->bitData = m_uBitData;
    s->bitOffset = m_uBitOffset;
    s->resetRequested = m_bResetRequested;
    memcpy(s->controller, m_uController, sizeof(m_uController));
    memcpy(s->packet, m_uPacket, sizeof(m_uPacket));
    memcpy(s->joypPacket, m_uJoypPacket, sizeof(m_uJoypPacket));
    s->packetQueueCount = m_uPacketQueueCount;
    memcpy(s->packetQueue, m_uPacketQueue, sizeof(m_uPacketQueue));
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
        s->packetQueueCount > PACKET_QUEUE_CAPACITY ||
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
    m_bJoyp15Lock = s->joyp15Lock ? TRUE : FALSE;
    m_bJoyp14Lock = s->joyp14Lock ? TRUE : FALSE;
    m_bPulseLock = s->pulseLock ? TRUE : FALSE;
    m_bStrobeLock = s->strobeLock ? TRUE : FALSE;
    m_bPacketLock = s->packetLock ? TRUE : FALSE;
    m_uPacketOffset = (Uint8)s->packetOffset;
    m_uBitData = (Uint8)s->bitData;
    m_uBitOffset = (Uint8)s->bitOffset;
    m_bResetRequested = s->resetRequested ? TRUE : FALSE;
    memcpy(m_uController, s->controller, sizeof(m_uController));
    memcpy(m_uPacket, s->packet, sizeof(m_uPacket));
    memcpy(m_uJoypPacket, s->joypPacket, sizeof(m_uJoypPacket));
    m_uPacketQueueCount = (Uint8)s->packetQueueCount;
    memcpy(m_uPacketQueue, s->packetQueue, sizeof(m_uPacketQueue));
    memcpy(m_uOutput, s->output, sizeof(m_uOutput));
    m_uClockAccumulator = s->clockAccumulator;
    return TRUE;
}
