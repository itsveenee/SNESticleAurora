#ifndef _AURORA_SNSGB_H
#define _AURORA_SNSGB_H

#include "types.h"
#include "snsgb_icd2.h"
#include "gbhost.h"

/* AURORA_SGB_RUNTIME_V0_4_20260904
 * Cartridge-side SGB runtime. Native SNESticle remains the SNES CPU/PPU/APU;
 * GBHost owns only the embedded Game Boy CPU side behind this ICD2 bridge.
 */
class SNSuperGameBoy
{
public:
    enum ModelE { MODEL_SGB1 = 1, MODEL_SGB2 = 2 };

    struct StateHeaderT
    {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Model;
        Uint32 GameBytes;
        Uint32 GameCRC;
        Uint32 ICDBytes;
        Uint32 GBBytes;
        Uint32 SaveBytes;
        Uint32 BootPacketIndex;
        Uint32 BootWaitClocks;
        Uint32 BootHandshake;
        Uint32 BootLine;
        Uint32 BootLineClocks;
        /* AURORA_SGB_AUDIO_V0_5_20260904 */
        Uint32 AudioPhase;
        Int32 AudioSumLeft;
        Int32 AudioSumRight;
        Uint32 AudioCount;
        Uint32 Reserved;
    };

    SNSuperGameBoy();
    ~SNSuperGameBoy();

    Bool AttachGame(const Uint8 *pData, Uint32 nBytes, ModelE eModel);
    void Detach();
    void Reset();
    Bool IsActive() const { return m_bActive; }
    ModelE GetModel() const { return m_eModel; }

    Uint8 Read(Uint32 uAddr, Uint8 uOpenBus);
    void Write(Uint32 uAddr, Uint8 uData);
    void AdvanceMasterClocks(Uint32 nClocks, Uint32 uSnesMasterHz);
    /* Mix cartridge GB PSG into the SNES PCM domain before host resampling. */
    void MixAudio(Int16 *pLeft, Int16 *pRight, Int32 nSamples, Uint32 uOutputHz);

    Bool AttachSavedata(const Uint8 *pData, Uint32 nBytes);
    Uint32 GetSavedataBytes();
    Bool ExportSavedata(Uint8 *pData, Uint32 nCapacity, Uint32 *pActualBytes);
    Bool SavedataDirty() const;
    void ClearSavedataDirty();

    Uint32 GetGameBytes() const { return m_uGameBytes; }
    Uint32 GetGameCRC() const { return m_uGameCRC; }

    Uint32 GetStateBytes();
    Bool SaveState(void *pData, Uint32 nBytes);
    Bool RestoreState(const void *pData, Uint32 nBytes);

private:
    static const Uint32 STATE_MAGIC = 0x35424753U; /* SGB5 */
    /* AURORA_SGB_GAMBATTE_BACKEND_V1_1_20260907:
       backend/state payload changed; reject old Gambatte SGB states cleanly. */
    static const Uint32 STATE_VERSION = 6U; /* AURORA_SGB_BSNES_PACKET_FIFO_V1_2_6_20260907 */
    enum {
        BOOT_HEADER_BYTES = 0x4c, /* GB $0104-$014f */
        BOOT_PACKET_COUNT = 6,
        BOOT_PACKET_DATA_BYTES = 14,
        BOOT_LCD_LINE_CLOCKS = 456,
        BOOT_WAIT_CLOCKS = 70224U * 4U,
        MAX_SAVEDATA_BYTES = 1024U * 1024U,
        AUDIO_SOURCE_CLOCKS_PER_SAMPLE = 128U,
        AUDIO_GAIN_NUM = 1U,
        AUDIO_GAIN_DEN = 2U
    };

    Bool m_bActive;
    ModelE m_eModel;
    Uint32 m_uGameBytes;
    Uint32 m_uGameCRC;
    Uint8 m_uBootHeader[BOOT_HEADER_BYTES];
    Uint8 m_uBootPacketIndex;
    Uint32 m_uBootWaitClocks;
    Bool m_bBootHandshake;
    Uint8 m_uBootLine;
    Uint16 m_uBootLineClocks;
    Uint32 m_uAudioPhase;
    Int32 m_iAudioSumLeft;
    Int32 m_iAudioSumRight;
    Uint32 m_uAudioCount;
    SNSGBICD2 m_ICD2;
    GBHost m_GB;

    void ResetBootHandshake();
    void BeginBootHandshake();
    void SubmitBootPacket();
    void AdvanceBootLCD(Uint32 nGBClocks);
    Uint32 AdvanceBootHandshake(Uint32 nGBClocks);
    void ResetAudioPipeline();
    static Int16 Saturate16(Int32 value);

    static Uint8 JoypHook(void *pContext, Bool bP14, Bool bP15, Bool bWrite);
    static void PixelHook(void *pContext, Uint8 uColor);
    static void HResetHook(void *pContext);
    static void VResetHook(void *pContext);
};

#endif
