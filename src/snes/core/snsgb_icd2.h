#ifndef _SNSGB_ICD2_H
#define _SNSGB_ICD2_H

#include "types.h"

/* AURORA_SGB_ICD2_V0_2_20260904
 * Isolated ICD2 hardware bridge. V0.2 does not map it into SnesSystem yet.
 */
class SNSGBICD2
{
public:
    enum ModelE { MODEL_NONE = 0, MODEL_SGB1 = 1, MODEL_SGB2 = 2 };

    enum {
        PACKET_BYTES = 16,
        PACKET_QUEUE_CAPACITY = 64,
        LCD_BANKS = 4,
        LCD_BANK_BYTES = 512,
        LCD_VISIBLE_BYTES = 320,
        LCD_WIDTH = 160,
        LCD_VISIBLE_LINES = 144,
        LCD_TOTAL_LINES = 154
    };

    struct StateT {
        Uint32 magic, version, model, icdRevision;
        Uint32 control, packetReady, readBank, readAddress, writeBank;
        Int32 vcounter;
        Uint32 hcounter;
        Uint32 joypID, joyp15Lock, joyp14Lock, pulseLock, strobeLock, packetLock; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */
        Uint32 joypPreviousSelector; /* AURORA_SGB_JOYP_TRANSITION_R11_20260909 */
        Uint32 packetOffset, bitData, bitOffset, resetRequested;
        Uint8 controller[4];
        Uint8 packet[PACKET_BYTES];
        Uint8 joypPacket[PACKET_BYTES];
        Uint32 packetQueueCount;
        Uint8 packetQueue[PACKET_QUEUE_CAPACITY][PACKET_BYTES];
        Uint8 output[LCD_BANKS][LCD_BANK_BYTES];
        Uint64 clockAccumulator;
    };

    SNSGBICD2();
    void Reset(ModelE eModel);

    ModelE GetModel() const { return m_eModel; }
    Bool IsEnabled() const { return m_eModel != MODEL_NONE; }
    Bool IsRunning() const { return (m_uControl & 0x80U) ? TRUE : FALSE; }
    Bool ConsumeResetRequest();

    Uint8 Read(Uint32 uAddr);
    void Write(Uint32 uAddr, Uint8 uData);

    /* Feed raw GB JOYP P14/P15 levels; returns active-low ICD input nibble. */
    Uint8 JoypRead(Bool bP14, Bool bP15) const;
    Uint8 JoypWrite(Bool bP14, Bool bP15);
    void SubmitPacket(const Uint8 *pPacket);

    /* AURORA_SGB_HOTPATH_V2_20260907
     * AURORA_SGB_ICD2_BSNES_RING_R7_20260909
     * Hardware phase from current bsnes: pixels write the current bank;
     * HReset advances LY, then rotates the ring whenever low 3 LY bits become
     * zero (including VBlank LY 144 and 152). */
    void PPUWrite(Uint8 uColor)
    {
        Uint16 x = m_uHCounter++;
        Uint8 y;
        Uint32 off;
        Uint8 *pBank;

        if (x >= LCD_WIDTH || m_nVCounter < 0 ||
            m_nVCounter >= LCD_VISIBLE_LINES)
            return;

        y = (Uint8)(m_nVCounter & 7);
        off = (Uint32)y * 2U + ((Uint32)x >> 3) * 16U;
        pBank = m_uOutput[m_uWriteBank & 3U];

        pBank[off + 0U] =
            (Uint8)((pBank[off + 0U] << 1) | ((uColor & 1U) ? 1U : 0U));
        pBank[off + 1U] =
            (Uint8)((pBank[off + 1U] << 1) | ((uColor & 2U) ? 1U : 0U));
    }

    void PPUHReset()
    {
        m_uHCounter = 0;
        if (m_nVCounter < LCD_TOTAL_LINES - 1)
        {
            ++m_nVCounter;
            if ((m_nVCounter & 7) == 0)
                m_uWriteBank = (Uint8)((m_uWriteBank + 1U) & 3U);
        }
        else
            m_nVCounter = 0;
    }

    void PPUVReset()
    {
        m_uHCounter = 0;
        m_nVCounter = 0;
    }

    /* AURORA_SGB_CLASSIC_RGB32_V1_20260908
     * Called with NEW LY after Gambatte advances its LY counter.
     * pFrame is the persistent 160x144 RGB32 grayscale framebuffer, matching
     * the historical bsnes-plus/classic Gambatte SGB bridge. */
    void GambatteNewLy(Uint32 uNewLy, const Uint32 *pFrame);

    /* Legacy helpers retained for old source/state compatibility. */
    void PushLCDScanline(Int32 nLine, const Uint8 *pShade2Bit);
    void EndLCDLine(Int32 nLine);

    /* AURORA_SGB_GAMBATTE_SELECTIVE_SYNC_V2_20260908
     * AURORA_SGB_ICD2_REAL_PACKET_MIRROR_R10_20260909
     * Hardware-verified ICD register mirroring uses 0x40f80f. All
     * $7800-$780f aliases feed the same sequential VRAM port. */
    Bool NeedsGBSyncBeforeRead(Uint32 uAddr) const
    {
        Uint32 d = uAddr & 0x40f80fU;
        if (d == 0x6000U || d == 0x6002U)
            return TRUE;
        if (d >= 0x7800U && d <= 0x780fU && m_uReadAddress == 0U)
            return TRUE;
        return FALSE;
    }

    Bool NeedsGBSyncBeforeWrite(Uint32 uAddr) const
    {
        /* AURORA_SGB_ICD2_RW_DECODE_R12_20260909
         * Hardware mirrors READ decode only. Writes remain exact low-16. */
        Uint32 d = uAddr & 0xffffU;
        return (d == 0x6001U || d == 0x6003U ||
                (d >= 0x6004U && d <= 0x6007U)) ? TRUE : FALSE;
    }

    Uint8 GetControllerByte(Int32 iController) const;
    Uint8 GetControllerCount() const;
    /* AURORA_SGB_ICD2_REAL_PACKET_MIRROR_R10_20260909
     * Real ICD exposes one current 16-byte command window, not a FIFO. */
    Bool PacketReady() const { return m_bPacketReady ? TRUE : FALSE; }
    Uint8 GetControl() const { return m_uControl; }
    Uint8 GetICDRevision() const { return m_uIcdRevision; }
    void SetICDRevision(Uint8 uRevision) { m_uIcdRevision = uRevision; }

    Uint32 GetClockDivider() const;
    Uint32 AdvanceMasterClocks(Uint32 uSnesMasterClocks, Uint32 uSnesMasterHz);

    Bool SaveState(StateT *pState) const;
    Bool RestoreState(const StateT *pState);

    static Bool IsMappedAddress(Uint32 uAddr);
    static Bool IsMappedWriteAddress(Uint32 uAddr); /* AURORA_SGB_ICD2_RW_DECODE_R12_20260909 */

private:
    static const Uint32 STATE_MAGIC = 0x32424753U; /* "SGB2" LE */
    static const Uint32 STATE_VERSION = 8U; /* AURORA_SGB_JOYP_TRANSITION_R11_20260909 */
    static const Uint32 SGB2_OSC_HZ = 20971520U;

    ModelE m_eModel;
    Uint8 m_uIcdRevision;
    Uint8 m_uControl;
    Bool m_bPacketReady;
    Uint8 m_uReadBank;
    Uint16 m_uReadAddress;
    Uint8 m_uWriteBank;
    Int32 m_nVCounter;
    Uint16 m_uHCounter;

    Uint8 m_uJoypID;
    Bool m_bJoyp15Lock, m_bJoyp14Lock, m_bPulseLock, m_bStrobeLock, m_bPacketLock; /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */
    Uint8 m_uJoypPreviousSelector; /* AURORA_SGB_JOYP_TRANSITION_R11_20260909 */
    Uint8 m_uPacketOffset, m_uBitData, m_uBitOffset;
    Bool m_bResetRequested;

    Uint8 m_uController[4];
    Uint8 m_uPacket[PACKET_BYTES];
    Uint8 m_uJoypPacket[PACKET_BYTES];
    Uint8 m_uPacketQueueCount;
    Uint8 m_uPacketQueue[PACKET_QUEUE_CAPACITY][PACKET_BYTES];
    Uint8 m_uOutput[LCD_BANKS][LCD_BANK_BYTES];
    Uint64 m_uClockAccumulator;

    Uint8 ReadDecoded(Uint32 uDecoded);
    void WriteDecoded(Uint32 uDecoded, Uint8 uData);
    Uint8 ControllerMask() const;
    Uint8 JoypInput(Bool bP14, Bool bP15) const;
};

#endif
