
#ifndef _SNES_H
#define _SNES_H

#include "emusys.h"

extern "C" {
#include "sndisasm.h"
#include "sncpu.h"
#include "snspcdisasm.h"
#include "snspc.h"
}
#include "snspcio.h"
#include "snspcdsp.h"
#include "snspcmix.h"
#include "snio.h"
#include "snppu.h"
#include "sndma.h"
#include "snrom.h"
#include "snppurender.h"
#include "sndebug.h"

#include "sndsp1.h"
#include "sndsp2.h"
#include "sndsp4.h"
#include "snobc1.h"
#include "sncx4.h"
#include "sngsu.h"
#include "snsdd1.h"
#include "snsrtc.h"
#include "snswc.h" /* AURORA_SWC_FLOPPY_V1_20260831 */
#include "snsa1.h" /* AURORA_SA1_V1_REFERENCE_LOGIC_20260902 */
#include "snsgb.h" /* AURORA_SGB_RUNTIME_V0_4_20260904 */

#define SNES_RAMSIZE  0x20000
#define SNES_SRAMSIZE (256 * 1024)

#define SNES_DSP1 1

/* AURORA_SNES_NATIVE_32K_V1_20260822
 * SNES S-DSP synthesis is fixed to its native 32 kHz domain. The setter is
 * retained only so existing config/call sites remain source-compatible. */
void SnesAudioSetRate(Uint32 hz);
Uint32 SnesAudioGetRate(void);


/* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNES_H
 * Nintendo 8M Memory Pack Type 1: 8 Mbit / 1 MiB flash.  It is intentionally
 * separate from Game Pak SRAM so slotted cartridges can own both saves. */
#define SNES_BSX_MEMORY_PACK_BYTES (1024 * 1024)

class SNBSXMemoryPack
{
public:
    SNBSXMemoryPack();
    ~SNBSXMemoryPack();

    Bool AttachBlank();
    void Detach();
    void ResetProtocol();
    Bool IsAttached() const { return m_bAttached; }

    Uint8 Read(Uint32 uAddr);
    void Write(Uint32 uAddr, Uint8 uData);
    Bool Load(const Uint8 *pData, Uint32 nBytes);

    Uint8 *GetData() { return m_bAttached ? m_pData : NULL; }
    Uint32 GetBytes() const
        { return m_bAttached ? (Uint32)SNES_BSX_MEMORY_PACK_BYTES : 0; }
    Bool Dirty() const { return m_bAttached && m_bDirty; }
    void ClearDirty() { m_bDirty = FALSE; }

    /* AURORA_BSXSLOT_MEMORY_PACK_V1_2_IO_WATCH_SGB_STATUS_20260906
     * Cheap counters only. Read()/Write() never log or touch storage. */
    void ResetIOStats();
    Uint32 ReadCount() const { return m_uReadCount; }
    Uint32 WriteCount() const { return m_uWriteCount; }
    Uint32 ProgramCount() const { return m_uProgramCount; }
    Uint32 BlockEraseCount() const { return m_uBlockEraseCount; }
    Uint32 ChipEraseCount() const { return m_uChipEraseCount; }
    Uint32 StatusReadCount() const { return m_uStatusReadCount; }
    Uint32 VendorReadCount() const { return m_uVendorReadCount; }

private:
    Uint8 *m_pData;
    Bool m_bAttached;
    Bool m_bDirty;

    /* AURORA_BSXSLOT_FLASH_PROTOCOL_V2_20260907
     * Sharp LH28F800SU-compatible 8-bit command state.
     * Page buffers are volatile protocol state; the 1 MiB flash array itself
     * remains the only .mpk payload persisted by the frontend. */
    enum
    {
        BSX_FLASH_MODE_ARRAY = 0,
        BSX_FLASH_MODE_CHIP,
        BSX_FLASH_MODE_PAGE,
        BSX_FLASH_MODE_COMPAT_STATUS,
        BSX_FLASH_MODE_EXT_STATUS
    };

    Uint8 m_uFlashMode;
    Uint8 m_uPageIndex;
    Uint8 m_PageBuffer[2][256];

    Uint8 m_uPendingCommand;
    Uint8 m_uPendingStep;
    Uint8 m_uPendingData;
    Uint8 m_uPendingAddrOdd;
    Uint16 m_uPendingCount;

    void ProgramFlashByte(Uint32 uAddr, Uint8 uData);
    void EraseFlashBlock(Uint32 uAddr);
    void EraseFlashAll();

    Uint32 m_uReadCount;
    Uint32 m_uWriteCount;
    Uint32 m_uProgramCount;
    Uint32 m_uBlockEraseCount;
    Uint32 m_uChipEraseCount;
    Uint32 m_uStatusReadCount;
    Uint32 m_uVendorReadCount;
};

class SnesSystem : public Emu::System
{
public:
    SnesSystem();
    ~SnesSystem();
    SNCpuT *GetCpu() {return &m_Cpu;}
    SNSpcT *GetSpc() {return &m_Spc;}
    SnesPPU *GetPPU() {return &m_PPU;}

    Uint32	GetFrame() {return m_uFrame;}
    Uint8	*GetSRAM() {return m_SRam;}

    /* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNES_H */
    Bool HasBSXMemoryPack() const { return m_BSXMemory.IsAttached(); }
    Int32 GetBSXMemoryPackBytes() const { return (Int32)m_BSXMemory.GetBytes(); }
    Uint8 *GetBSXMemoryPackData() { return m_BSXMemory.GetData(); }
    Bool LoadBSXMemoryPack(const Uint8 *pData, Uint32 nBytes)
        { return m_BSXMemory.Load(pData, nBytes); }
    Bool IsBSXMemoryPackDirty() const { return m_BSXMemory.Dirty(); }
    void ClearBSXMemoryPackDirty() { m_BSXMemory.ClearDirty(); }
    Uint8 ReadBSXMemoryPack(Uint32 uOffset) { return m_BSXMemory.Read(uOffset); }
    void WriteBSXMemoryPack(Uint32 uOffset, Uint8 uData)
        { m_BSXMemory.Write(uOffset, uData); }
    Uint32 GetBSXMemoryPackReadCount() const { return m_BSXMemory.ReadCount(); }
    Uint32 GetBSXMemoryPackWriteCount() const { return m_BSXMemory.WriteCount(); }
    Uint32 GetBSXMemoryPackProgramCount() const { return m_BSXMemory.ProgramCount(); }
    Uint32 GetBSXMemoryPackBlockEraseCount() const { return m_BSXMemory.BlockEraseCount(); }
    Uint32 GetBSXMemoryPackChipEraseCount() const { return m_BSXMemory.ChipEraseCount(); }
    Uint32 GetBSXMemoryPackStatusReadCount() const { return m_BSXMemory.StatusReadCount(); }
    Uint32 GetBSXMemoryPackVendorReadCount() const { return m_BSXMemory.VendorReadCount(); }

    void 	SetRom(class Emu::Rom *pRom);
    void	SetSnesRom(SnesRom *pRom);
    /* AURORA_SWC_FLOPPY_V1_20260831 -- isolated copier mode. */
    Bool    LoadSuperWildCard(const Char *pFirmwarePath, const Char *pDiskPath);
    Bool    LoadSuperMagicom(const Char *pFirmwarePath, const Char *pDiskPath); /* AURORA_V6_MAGICOM_FRONT_FAREAST_20260831 */
    Bool    SwapSuperWildCardDisk(const Char *pDiskPath);
    void    ShutdownSuperWildCard();
    /* AURORA_FRONT_COPIER_POWER_CYCLE_V2_20260902
     * Host-requested copier power cycle: keep inserted media/topology but
     * return the Front copier and the SNES to their boot/BIOS state. */
    void    PowerCycleFrontCopier();
    /* Legacy predicate intentionally means active classic Front copier. */
    /* AURORA_SGB_RUNTIME_V0_4_20260904 */
    Bool AttachSuperGameBoyGame(const Uint8 *pData, Uint32 nBytes, Bool bSgb2);
    void DetachSuperGameBoyGame();
    Bool IsSuperGameBoy() const { return m_SGB.IsActive(); }
    Uint32 GetSuperGameBoyGameBytes() const { return m_SGB.GetGameBytes(); }
    Uint32 GetSuperGameBoyGameCRC() const { return m_SGB.GetGameCRC(); }
    Uint32 GetSuperGameBoySavedataBytes() { return m_SGB.GetSavedataBytes(); }
    Bool LoadSuperGameBoySavedata(const Uint8 *pData, Uint32 nBytes) { return m_SGB.AttachSavedata(pData, nBytes); }
    Bool ExportSuperGameBoySavedata(Uint8 *pData, Uint32 nCap, Uint32 *pActual) { return m_SGB.ExportSavedata(pData, nCap, pActual); }
    Bool IsSuperGameBoySavedataDirty() const { return m_SGB.SavedataDirty(); }
    void ClearSuperGameBoySavedataDirty() { m_SGB.ClearSavedataDirty(); }

    Bool    IsSuperWildCard() const { return m_bSuperWildCard; }
    Bool    IsSuperWildCardFirmwareMode() const
        { return m_bSuperWildCard && m_SWC.IsFirmwareMode(); }
    Bool    IsSuperMagicom() const { return m_bSuperWildCard && m_SWC.IsSuperMagicom(); }
    const Char *GetFrontCopierName() const { return m_SWC.GetModelName(); }
    const Char *GetSuperWildCardDiskPath() const { return m_SWC.GetDiskPath(); }
    const Char *GetSuperWildCardError() const { return m_SWC.GetLastError(); }
    /* AURORA_FRONT_TRACE_V10_8_20260831 */
    Bool ConsumeFrontCopierDebugTransition(
        SNSuperWildCard::DebugTransitionT *pOut)
    {
        return m_bSuperWildCard ? m_SWC.ConsumeDebugTransition(pOut) : FALSE;
    }
    /* AURORA_SWC_FLOPPY_V5_20260831 */
    Bool    InsertSuperWildCardCartridge(SnesRom *pRom);
    void    EjectSuperWildCardCartridge();
    Bool    HasSuperWildCardCartridge() const { return m_SWC.HasExternalCartridge(); }

    /* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901 */
    Int32   GetSuperWildCardCartridgeSRAMBytes() const
        { return (Int32)m_SWC.GetExternalCartridgeSRAMBytes(); }
    Uint8  *GetSuperWildCardCartridgeSRAMData()
        { return m_SWC.GetExternalCartridgeSRAMData(); }
    Bool    HasSuperWildCardCartridgeBatterySRAM() const
        { return m_SWC.HasExternalCartridgeBatterySRAM(); }
    Bool    IsSuperWildCardCartridgeSRAMDirty() const
        { return m_SWC.IsExternalCartridgeSRAMDirty(); }
    void    ClearSuperWildCardCartridgeSRAMDirty()
        { m_SWC.ClearExternalCartridgeSRAMDirty(); }

    Bool    HasSuperWildCardDisk() const { return m_SWC.HasDisk(); }
    /* AURORA_SWC_MEDIA_PROBE_V10_2_20260831 */
    Bool    IsSuperWildCardDiskWritable() const { return m_SWC.IsDiskWritable(); }
    void	Reset();
	void	SoftReset();
	void	ExecuteFrame(Emu::SysInputT *pInput, class CRenderSurface *pTarget, class CMixBuffer *pSound, ModeE eMode);

	/* Host-only peripheral injection; SysInputT/save-state layout stays fixed. */
	void	SetMouseInput(Bool bConnected, Int32 nDeltaX, Int32 nDeltaY, Uint32 uButtons)
	{
		m_IO.SetMouseInput(bConnected, nDeltaX, nDeltaY, uButtons);
	}

    void	SaveState(struct SnesStateT *pState);
    Bool	RestoreState(struct SnesStateT *pState);

    void    SaveState(void *pState, Int32 nStateBytes);
    void    RestoreState(void *pState, Int32 nStateBytes);
    Int32   GetStateSize();
    /* AURORA_SWC_FLOPPY_V4_20260831 */
    Bool    SaveStateChecked(void *pState, Int32 nStateBytes);
    Bool    RestoreStateChecked(void *pState, Int32 nStateBytes);

    /* AURORA_CX4_STATE_V7
     * CX4 data is packed into unused tail bytes of the legacy 256 KiB SRAM
     * field, preserving sizeof(SnesStateT) and normal SNES state files. */
    Bool    CanSerializeCX4State();

    /* AURORA_SPECIAL_CHIP_STATE_V1 */
    Bool    CanSerializeSpecialChipState();

    Int32   GetSRAMBytes();
    Uint8   *GetSRAMData();

	virtual const char *GetString(StringE eString);
    virtual Uint32 GetSampleRate() {return SNSPCDSP_SAMPLERATE;}

    static const Char *GetRegName(Uint32 uAddr);


private:
	SNCpuT		m_Cpu;
	SnesPPU		m_PPU;
	SnesDMAC	m_DMAC;
	SnesIO		m_IO;
	SNSpcIO		m_SpcIO;
	SNSpcT		m_Spc;
	SNSpcDsp	m_SpcDsp;

	// extra hardware
	ISNDSP		*m_pDsp;

#if SNES_DSP1
	SNDSP1		m_DSP1;
	SNDSP2		m_DSP2;
	// DSP-4 (Top Gear 3000): HLE self-contained, sem firmware.
	SNDSP4		m_DSP4;
#endif

	SNOBC1		m_OBC1;

	SNCX4		m_CX4;

	// SuperFX / GSU (Star Fox, Yoshi's Island, etc.) -- core experimental
	SNGSU		m_GSU;

	SNSDD1		m_SDD1;
	Bool		m_bSDD1;

	SNSRTC		m_SRTC;
	Bool		m_bSRTC;
	Bool		m_bSuperFX;   // cartucho usa SuperFX/GSU -> rotear $3000-34FF

	SNSA1       m_SA1;      /* AURORA_SA1_V1_REFERENCE_LOGIC_20260902 */
	Bool        m_bSA1IRQ;
	SNBSXMemoryPack m_BSXMemory; /* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNES_H */

	/* AURORA_SWC_FLOPPY_V1_20260831 */
	SNSuperWildCard m_SWC;
    SNSuperGameBoy m_SGB;
    Uint32 m_uSGBSyncClock;
	Bool            m_bSuperWildCard;

	SnesRom		*m_pRom;
Bool            m_bRegionLocked;
	SnesPPURender	m_PPURender;
	SNSpcDspMixFull		m_SpcDspMixer;		    // non-deterministic mixer
	SNSpcDspMixSilent	m_SpcDspSilentMixer;    // deterministic mixer

	Uint32		m_uSramSize;
#if SNES_HVIRQ_RESCHEDULE
	/* AURORA_HVIRQ_RESCHEDULE_V4 -- transient per-scanline scheduler state. */
	Bool		m_bLineIRQActive;
	Bool		m_bLineIRQReschedule;
	Bool		m_bLineIRQFired;
	Bool		m_bLineIRQInstant;
	Int32		m_nLineIRQCycle;
	Int32		m_nLineIRQClock;
#endif

	/* AURORA_RASTER_MMIO_CATCHUP_V1_20260907
	 * Transient scheduler guards only; they are reconstructed every scanline
	 * and intentionally are not part of the serialized SNES state. */
	Bool		m_bRasterLineActive;
	Bool		m_bRasterCatchupActive;
	Bool		m_bRasterHBlankDone;
	Bool		m_bRasterHDMADone;

	Uint8		m_Ram[SNES_RAMSIZE] _ALIGN(16);
	Uint8		m_SRam[SNES_SRAMSIZE] _ALIGN(16);


private:
	static Uint8 SNCPU_TRAPFUNC ReadMem(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  WriteMem(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 SNCPU_TRAPFUNC Read2000(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  Write2000(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 SNCPU_TRAPFUNC Read4000(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  Write4000(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 SNCPU_TRAPFUNC ReadSRAM(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  WriteSRAM(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 SNCPU_TRAPFUNC ReadDSP1(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  WriteDSP1(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 SNCPU_TRAPFUNC ReadOBC1(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  WriteOBC1(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 SNCPU_TRAPFUNC ReadCX4(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  WriteCX4(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 SNCPU_TRAPFUNC ReadGSU(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  WriteGSU(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 SNCPU_TRAPFUNC ReadSWC(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  WriteSWC(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
    static Uint8 SNCPU_TRAPFUNC ReadSGB(SNCpuT *pCpu, Uint32 uAddr);
    static void SNCPU_TRAPFUNC WriteSGB(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
    void MapSuperGameBoy();
    void SyncSuperGameBoy();
    /* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNES_H */
    static Uint8 SNCPU_TRAPFUNC ReadBSXSlot(SNCpuT *pCpu, Uint32 uAddr);
    static void SNCPU_TRAPFUNC WriteBSXSlot(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 SNCPU_TRAPFUNC ReadSA1BWRAM(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  WriteSA1BWRAM(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 SNCPU_TRAPFUNC ReadSA1ROM(SNCpuT *pCpu, Uint32 uAddr);
	static void SNCPU_TRAPFUNC  WriteSA1ROM(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
	static Uint8 CX4ReadMem(void *pCtx, Uint32 uAddr);

    static Uint8 SNCPU_TRAPFUNC Read2000Debug(SNCpuT *pCpu, Uint32 uAddr);
    static Uint8 SNCPU_TRAPFUNC Read4000Debug(SNCpuT *pCpu, Uint32 uAddr);
    static void SNCPU_TRAPFUNC  Write2000Debug(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);
    static void SNCPU_TRAPFUNC  Write4000Debug(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);



	void	MapLoRom();
	void	MapHiRom();
	void	MapMem(struct SnesMemMapT *pMemMap);
	void	MapMem(SNRomMappingE eRomMapping, Uint32 uFlags);
	void	MapMemExLoRom(void);

	void	MapBSCLoRom(void); /* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNES_H */
	void	MapBSCHiRom(void);
	Bool    ResolveBSXSlotAddress(Uint32 uAddr, Uint32 *pOffset) const;

	void	MapSuperWildCard(void); /* AURORA_SWC_FLOPPY_V1_20260831 */
	void	MapSuperWildCardDevice(struct SnesMemMapT *pMemMap);
	void	MapSuperWildCardCoprocessor(void);
	void    SetSA1IRQ(Bool bPending);
	void    UpdateMainIRQLine(void);
	void    MarkSA1BWRAMDirty(void);
	friend class SNSA1;
	void	RemapSuperWildCardMode0Dram(void); /* AURORA_SWC_V11_MENU_FASTPATH_20260831 */
	void	RemapSDD1(void);   // (re)mapeia $C0-$FF conforme $4804-$4807
	void	DumpMemMap();

	void	SetFastRom();
	void	SetSlowRom();

	void	SyncSPC(Int32 uExtra = 0);
	void	SyncPPU();
	void	CatchUpRasterEventsForCpuMMIO(SNCpuT *pCpu);
#if SNES_HVIRQ_RESCHEDULE
	Int32	CalculateLineIRQCycle();
	void	RescheduleLineIRQ(Bool bAllowImmediate);
#endif
	void	ExecuteLine();
    void    ExecuteWithIRQ(Int32 nCycles, Int32 &nIRQCycles);
    void    ExecuteCPU(Int32 nExecCycles);
};

void SnesDebugBegin(SnesSystem *pSnes, const char *pFileName);
void SnesDebugEnd();

#endif

