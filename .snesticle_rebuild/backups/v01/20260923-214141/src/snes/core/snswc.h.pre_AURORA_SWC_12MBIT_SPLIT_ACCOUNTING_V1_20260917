#ifndef _SNSWC_H
#define _SNSWC_H

#include <stdio.h>
#include "types.h"

/*
 * AURORA_SWC_FLOPPY_V1_20260831
 *
 * Classic Super Wild Card hardware only:
 *   - 4 MiB allocated DRAM (covers 32-Mbit upgraded classic units)
 *   - 32 KiB battery SRAM supplied by SnesSystem
 *   - 16 KiB firmware
 *   - MCS3201 / NEC765-compatible polling floppy controller
 *
 * No external cartridge passthrough in V1.
 */
class SNSuperWildCard
{
public:
    /* AURORA_V6_1_MAGICOM_QUICK_CURE_20260831
     * AURORA_V6_1C_MAGICOM_PREREQ_CURE_20260831
     * AURORA_V6_1D_MAGICOM_V11_MASK_CURE_20260831
     * AURORA_V6_1E_MAGICOM_SKIP_REDUNDANT_V5_20260831
     * AURORA_V6_MAGICOM_FRONT_FAREAST_20260831:
     * classic Front Fareast copier family. */
    enum ModelE
    {
        MODEL_SWC = 0,
        MODEL_MAGICOM = 1
    };

    /* AURORA_FRONT_TRACE_V10_8_20260831
     * Host-only diagnostic snapshot. Not serialized and not part of the
     * emulated hardware state. */
    struct DebugTransitionT
    {
        Uint8 oldMode, newMode, parallel;
        Uint8 mappedD5, loD5, hiD5;
        Uint16 mappedReset, loReset, hiReset;
        Uint32 fdcReadBytes;
        Uint32 dramWriteBytes;
        Uint32 dramMaxOffset;
    };

    SNSuperWildCard();
    ~SNSuperWildCard();

    /* AURORA_SWC_32MBIT_SRAM_FIDELITY_V1_1_20260916
     * The frontend may lend the classic SWC its pre-core 4 MiB DRAM block.
     * Ownership stays with the frontend; Magicom/default callers retain the
     * original internal-allocation path through the default arguments. */
    Bool Load(const Char *pFirmwarePath, const Char *pDiskPath,
              ModelE eModel = MODEL_SWC,
              Uint8 *pExternalDRAM = NULL,
              Uint32 nExternalDRAMBytes = 0);
    void Shutdown();
    void Reset();

    Bool MountDisk(const Char *pDiskPath);
    Bool SwapDisk(const Char *pDiskPath);

    /* AURORA_SWC_FLOPPY_V5_20260831 */
    /* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901 */
    Bool SetExternalCartridge(const Uint8 *pRom, Uint32 nRomBytes,
                              Int32 iMapping, Uint32 nSramBytes,
                              Bool bBatterySRAM, Uint32 uFlags);
    void ClearExternalCartridge();
    Bool HasExternalCartridge() const { return m_pCartRom != NULL; }
    Uint32 GetExternalCartridgeSRAMBytes() const { return m_nCartSRAMBytes; }
    Uint8 *GetExternalCartridgeSRAMData() { return m_pCartSRAM; }
    Bool HasExternalCartridgeBatterySRAM() const
        { return m_pCartSRAM && m_nCartSRAMBytes && m_bCartSRAMBattery; }
    Bool IsExternalCartridgeSRAMDirty() const
        { return m_bCartSRAMDirty; }
    void ClearExternalCartridgeSRAMDirty()
        { m_bCartSRAMDirty = FALSE; }
    Bool HasDisk() const { return m_pDisk != NULL; }
    /* AURORA_SWC_MEDIA_PROBE_V10_2_20260831: host-side media diagnostics. */
    Bool IsDiskWritable() const { return m_pDisk != NULL && m_bDiskWritable; }

    Bool IsActive() const { return m_bActive; }
    /* AURORA_SWC_MEDIA_FLOW_V2_20260902 */
    Bool IsFirmwareMode() const
        { return m_bActive && m_uSystemMode == 0; }
    Uint8 GetSystemMode() const { return m_uSystemMode; }
    Uint8 GetParallelMode() const { return m_uParallel; }
    Uint32 GetExternalCartridgeFlags() const { return m_uCartFlags; }
    Int32 GetExternalCartridgeMapping() const { return m_iCartMapping; }
    const Uint8 *GetExternalCartridgeData() const { return m_pCartRom; }
    Uint32 GetExternalCartridgeBytes() const { return m_nCartBytes; }
    void MarkExternalCartridgeSRAMDirty()
        { if (m_pCartSRAM && m_bCartSRAMBattery) m_bCartSRAMDirty = TRUE; }
    const Uint8 *GetDRAMData() const { return m_pDRAM; }
    Uint32 GetDRAMBytes() const { return m_nDRAMBytes; }
    Bool IsSuperMagicom() const
        { return m_bActive && m_eModel == MODEL_MAGICOM; }
    ModelE GetModel() const { return m_eModel; }
    /* AURORA_FRONT_GAMEBUS_V10_7_20260831
     * Front DRAM is writable only through the BIOS page window (Mode 0).
     * In cartridge emulation Modes 2/3 it is read-only on the SNES bus. */
    /* AURORA_FRONT_TRACE_V10_8_20260831
     * Diagnostic build: keep direct DRAM reads but trap ALL copier-DRAM
     * writes so the loader byte counter is exact. This is semantically
     * identical but slower only while the copier BIOS writes DRAM. */
    Bool IsDirectDramWritable() const { return FALSE; }
    Bool ConsumeDebugTransition(DebugTransitionT *pOut);
    const Char *GetModelName() const
        { return m_eModel == MODEL_MAGICOM ? "Super Magicom" : "Super Wild Card"; }
    const Char *GetDiskPath() const { return m_DiskPath; }
    const Char *GetLastError() const { return m_LastError; }

    /* AURORA_SWC_FLOPPY_V4_20260831
     * Private copier-state extension. Normal SnesStateT is untouched. */
    Uint32 GetStateBytes() const;
    Bool SaveState(void *pState, Uint32 nStateBytes) const;
    Bool RestoreState(const void *pState, Uint32 nStateBytes);

    Bool Read(Uint32 uAddr, Uint8 *pData, Uint8 *pSRAM, Uint32 nSRAMBytes);
    Bool Write(Uint32 uAddr, Uint8 uData, Uint8 *pSRAM, Uint32 nSRAMBytes);
    Bool ResolveDirectDram(Uint8 bank, Uint16 addr, Uint8 **ppMem); /* AURORA_SWC_MEGA_V9_20260831; AURORA_SWC_V11_MENU_FASTPATH_20260831 */
    Bool ResolveDirectCartridge(Uint8 bank, Uint16 addr,
                                const Uint8 **ppMem) const;
    Bool ResolveDirectFirmware(Uint8 bank, Uint16 addr,
                               const Uint8 **ppMem) const; /* AURORA_SWC_V10_MENU_INDEX_CARTRESET_20260831 */

private:
    enum
    {
        SWC_DRAM_BYTES = 4 * 1024 * 1024,
        MAGICOM_DRAM_BYTES = 2 * 1024 * 1024,
        SWC_FIRMWARE_BYTES = 16 * 1024,
        MAGICOM_FIRMWARE_BYTES = 8 * 1024,
        SWC_SECTOR_BYTES = 512,
        SWC_MAX_FORMAT_IDS = 36 * 4,
        /* AURORA_SWC_D88_ONLY_V5_20260901 */
        D88_TRACK_TABLE = 164,
        D88_TRACK_ACTIVE = 160,
        D88_HEADER_MIN_BYTES = 0x2A0,
        D88_HEADER_MAX_BYTES = 0x2B0,
        D88_SECTOR_HEADER_BYTES = 16,
        D88_SLOT_SECTORS = 20,
        D88_MAX_SECTORS = 20,

        FDC_PHASE_COMMAND = 0,
        FDC_PHASE_READ,
        FDC_PHASE_WRITE,
        FDC_PHASE_RESULT,
        FDC_PHASE_FORMAT
    };

    Bool m_bActive;
    ModelE m_eModel;
    Uint8 *m_pDRAM;
    Uint32 m_nDRAMBytes;
    Bool m_bOwnDRAM; /* AURORA_SWC_32MBIT_SRAM_FIDELITY_V1_1_20260916: borrowed frontend DRAM is never freed here. */
    Uint8 *m_pFirmware;
    Uint32 m_nFirmwareBytes;

    const Uint8 *m_pCartRom; /* AURORA_SWC_FLOPPY_V5_20260831 */
    Uint32 m_nCartBytes;
    Int32 m_iCartMapping;
    Uint32 m_uCartFlags; /* AURORA_SWC_DONOR_CART_V1_20260902 */

    /* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901
     * Separate physical Game Pak RAM from the copier's own 32 KiB B-RAM. */
    Uint8 *m_pCartSRAM;
    Uint32 m_nCartSRAMBytes;
    Bool m_bCartSRAMBattery;
    Bool m_bCartSRAMDirty;

    Uint32 m_uSelectedDRAMPage;
    Uint32 m_uSelectedSRAMPage;
    Uint8 m_uSystemMode;
    Uint8 m_uParallel;
    Bool m_bPageSRAM;
    Bool m_bCartridgeMap; /* AURORA_SWC_FLOPPY_V3_20260831: external cart window in Modes 2/3 */

    FILE *m_pDisk;
    /* AURORA_SWC_D88_ONLY_V5_20260901 */
    Uint32 m_D88TrackOffset[D88_TRACK_TABLE];
    Uint32 m_uD88DiskBytes;

    /* AURORA_D88_RAM_IO_PERF_V1_1_20260901
     * Host-only acceleration. The D88 remains authoritative on storage:
     * reads use this RAM mirror; writes mark tracks dirty and are persisted
     * by FdcFlushDisk() at the existing command boundary. */
    Uint8 *m_pD88Image;
    Uint32 m_D88SectorDataOffset[D88_TRACK_ACTIVE][D88_MAX_SECTORS];
    Uint8 m_D88TrackSpt[D88_TRACK_ACTIVE];
    Uint8 m_D88FirstSectorR[D88_TRACK_ACTIVE];
    Bool m_D88TrackDirty[D88_TRACK_ACTIVE];

    /* AURORA_D88_RAM_IO_PERF_V1_5_MULTIDISK_20260901
     * WRITE DATA dirties sector payloads; FORMAT dirties a whole track.
     * This removes 10-KiB write amplification for one-sector commands. */
    Bool m_D88SectorDirty[D88_TRACK_ACTIVE][D88_MAX_SECTORS];

    /* Transient host-side split-media handshake. Intentionally not
     * serialized into save states. */
    Bool m_bSplitNextMediaRequired;
    Bool m_bSplitAwaitingMediaSwap;

    /* AURORA_D88_V1_6_MULTIDISK_1600_20260901
     * Save-side split accounting is transient host state. The SWC header
     * stores an 8-KiB block count for each part; accumulate those counts and
     * stop honoring "more split files" once the real cartridge is exhausted. */
    Uint32 m_uSplitSavedBlocks;
    Uint32 m_uSplitBlocksOnMedia;

    Bool m_bD88HeaderDirty;

    Bool m_bDiskWritable;
    Bool m_bDiskDirty; /* AURORA_SWC_MEGA_V9_20260831: flush once per FDC command */
    Bool m_bDiskChanged;
    Uint32 m_uIndexPollCounter; /* AURORA_SWC_V10_MENU_INDEX_CARTRESET_20260831: synthetic rotating INDEX pulse */
    Char m_DiskPath[1024];
    Char m_LastError[192];

    Int32 m_nTracks;
    Int32 m_nHeads;
    Int32 m_nSectorsPerTrack;

    Uint8 m_uDOR;
    Uint8 m_uDCR;
    Uint8 m_uCylinder;
    Uint8 m_uHead;
    Uint8 m_uDrive;
    Bool m_bIRQ;
    Uint8 m_uLastST0;
    Uint8 m_uResetSensePending; /* AURORA_SWC_FLOPPY_V2_20260831 */

    Uint8 m_FdcPhase;
    Uint8 m_Command[16];
    Uint8 m_nCommand;
    Uint8 m_nCommandExpected;

    Uint8 m_Result[16];
    Uint8 m_nResult;
    Uint8 m_iResult;

    Uint8 m_Sector[SWC_SECTOR_BYTES];
    Uint16 m_iSectorByte;
    Uint8 m_uDataC;
    Uint8 m_uDataH;
    Uint8 m_uDataR;
    Uint8 m_uDataN;
    Uint8 m_uDataEOT;
    Bool m_bDataMT;

    Uint8 m_FormatIDs[SWC_MAX_FORMAT_IDS];
    Uint16 m_nFormatIDBytes;
    Uint16 m_iFormatIDByte;
    Uint8 m_uFormatSC;
    Uint8 m_uFormatFill;

    /* AURORA_FRONT_TRACE_V10_8_20260831: transient diagnostics only. */
    Uint32 m_uDebugFdcReadBytes;
    Uint32 m_uDebugDramWriteBytes;
    Uint32 m_uDebugDramMaxOffset;
    Bool m_bDebugTransitionPending;
    DebugTransitionT m_DebugTransition;

    void CaptureDebugTransition(Uint8 uNewMode);
    void ResetDebugTrace();

    void SetError(const Char *pText);
    Bool LoadFirmware(const Char *pPath);
    /* AURORA_SWC_D88_ONLY_V5_20260901 */
    Bool D88Probe(const Uint8 *pImage, Uint32 nBytes,
                  Int32 *pTracks, Int32 *pHeads, Int32 *pMaxSpt,
                  Uint32 *pOffsets, Uint32 *pSectorOffsets,
                  Uint8 *pTrackSpt, Uint8 *pFirstR,
                  Uint32 *pDiskBytes, Bool *pProtected);
    /* AURORA_D88_DIRECT_FILE_V3_20260913
     * Low-memory D88 parser: keeps only CHRN/offset metadata in EE RAM. */
    Bool D88ProbeFile(FILE *pFile, Uint32 nBytes,
                      Int32 *pTracks, Int32 *pHeads, Int32 *pMaxSpt,
                      Uint32 *pOffsets, Uint32 *pSectorOffsets,
                      Uint8 *pTrackSpt, Uint8 *pFirstR,
                      Uint32 *pDiskBytes, Bool *pProtected);
    Bool D88FindSector(Uint8 c, Uint8 h, Uint8 r, Uint8 n,
                       long *pDataOffset);
    Uint8 D88TrackSectorCount(Uint8 c, Uint8 h);
    Uint8 D88TrackSlotCapacity(Uint8 c, Uint8 h);
    Bool D88FirstSectorID(Uint8 c, Uint8 h,
                          Uint8 *pC, Uint8 *pH, Uint8 *pR, Uint8 *pN);
    Bool D88RefreshGeometry();
    Bool D88FormatSectorCountAllowed(Uint8 count) const;
    Bool D88FormatCurrentTrack();

    void FdcReset(Bool bRaiseIRQ);
    Bool FdcFlushDisk();
    /* AURORA_FRONT_FDC_DRIVE_MODEL_V10_3_20260831
     * Front/MCS3201-compatible DOR: bits0-1 DSEL, bit2 /RES,
     * bits4-7 MOTOR1..4. */
    Uint8 FdcSelectedDrive() const;
    Bool FdcDriveReady(Uint8 uDrive) const;
    Uint8 FdcMainStatus() const;
    Uint8 FdcReadData();
    void FdcWriteData(Uint8 uData);
    Uint8 FdcCommandLength(Uint8 uCommand) const;
    void FdcExecuteCommand();
    void FdcSetResult(const Uint8 *pData, Uint8 nBytes, Bool bIRQ);
    void FdcFinishRW(Bool bOK, Uint8 uST1, Uint8 uST2);
    Bool FdcLoadCurrentSector();
    Bool FdcStoreCurrentSector();
    Bool FdcAdvanceSector();
    Bool FdcValidCHS(Uint8 c, Uint8 h, Uint8 r);

    Bool ReadExternalCartridge(Uint8 bank, Uint16 addr, Uint8 *pData) const;

    /* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901 */
    Bool ExternalCartridgeSRAMOffset(
        Uint8 bank, Uint16 addr, Uint32 *pOffset) const;
    Bool ReadExternalCartridgeSRAM(
        Uint8 bank, Uint16 addr, Uint8 *pData) const;
    Bool WriteExternalCartridgeSRAM(
        Uint8 bank, Uint16 addr, Uint8 uData);
    Bool ExternalCartridgeIsDonor() const;

    /* AURORA_FRONT_MODE0_PAGEBUS_V10_9_20260831 */
    Bool ReadExternalPage(Uint8 bank, Uint16 addr, Uint8 *pData) const;
    Bool WriteExternalPage(Uint8 bank, Uint16 addr, Uint8 uData);

    Uint32 DramOffsetMode2(Uint8 bank, Uint16 addr) const;
    Bool ReadMode0(Uint8 bank, Uint16 addr, Uint8 *pData,
                   Uint8 *pSRAM, Uint32 nSRAMBytes);
    Bool WriteMode0(Uint8 bank, Uint16 addr, Uint8 uData,
                    Uint8 *pSRAM, Uint32 nSRAMBytes);
    Bool ReadEmulation(Uint8 bank, Uint16 addr, Uint8 *pData,
                       Uint8 *pSRAM, Uint32 nSRAMBytes);
    Bool WriteEmulation(Uint8 bank, Uint16 addr, Uint8 uData,
                        Uint8 *pSRAM, Uint32 nSRAMBytes);
};

#endif
