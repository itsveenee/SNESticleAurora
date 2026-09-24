#ifndef _SNSA1_H
#define _SNSA1_H

#include "types.h"
extern "C" {
#include "sncpu.h"
}

class SnesSystem;

/* AURORA_SA1_PERF_STATE_V8_3_20260903
 *
 * Pointer-free SA-1 snapshot. Host pointers, Bank[] descriptors and trap
 * function pointers are deliberately NOT serialized; RestoreState rebuilds
 * those mappings against the cartridge that is currently loaded.
 */
struct SNSA1StateT
{
    Uint8       Tag[8];
    Uint32      Version;

    SNCpuRegsT  CpuRegs;
    Int32       CpuCycles;
    Int32       CpuCounter[SNCPU_COUNTER_NUM];
    Uint8       CpuSignal;
    Uint8       CpuNmiDmaDelay;
    Uint8       CpuMDR;
    Uint8       Reserved0;

    Uint8       Reg[0x200];
    Uint8       IRAM[0x800];
    Uint8       CharData[0x80];

    unsigned long long Sum;
    Uint32      HCounter;
    Uint32      VCounter;
    Uint32      PrevHCounter;
    Uint16      LatchedHCounter;
    Uint16      LatchedVCounter;
    Uint16      Op1;
    Uint16      Op2;

    Uint8       ArithmeticOp;
    Uint8       ArithmeticOverflow;
    Uint8       VariableBitPos;
    Uint8       CharIndex;
    Uint8       CharDMA;
    Uint8       BitmapFormat;
    Uint8       TimerLastState;
    Uint8       Reserved1;
};

/* AURORA_SA1_V1_REFERENCE_LOGIC_20260902
 *
 * SA-1 is modelled as an Aurora peripheral around a second native SNCpuT.
 * The register/DMA/MMC/arithmetic/bitstream behaviour follows the reference emulator
 * implementation, translated to Aurora's memory/trap and scheduling model.
 */
class SNSA1
{
public:
    SNSA1();
    ~SNSA1();

    Bool Attach(SnesSystem *pOwner,
                const Uint8 *pRom, Uint32 nRomBytes,
                Uint8 *pBWRAM, Uint32 nBWRAMBytes,
                Bool bMapMainRom, Bool bDonorBWRAM,
                Bool bTrackBWRAMDirty);
    void Detach();
    void Reset();
    void Run(Int32 nMainMasterCycles);

    /* AURORA_SA1_PERF_V8_3_2_20260903
     * 192 is the second conservative step: 33% fewer scheduler/native
     * re-entries than V8.3's 128, while remaining far below a scanline.
     * The SA-1 bound remains exactly 3x the main interleave budget.
     */
    enum
    {
        MAIN_INTERLEAVE_QUANTUM = 192,
        CPU_EXEC_QUANTUM = MAIN_INTERLEAVE_QUANTUM * 3
    };

    Bool SaveState(SNSA1StateT *pState) const;
    Bool RestoreState(const SNSA1StateT *pState);

    Bool IsActive() const { return m_bActive; }
    SNCpuT *GetCPU() { return &m_Cpu; }

    /* S-CPU side of the SA-1 cartridge. */
    Uint8 ReadMainRegister(Uint16 uAddr, Uint8 uOpenBus);
    void  WriteMainRegister(Uint16 uAddr, Uint8 uData);
    Uint8 ReadMainIRAM(Uint16 uAddr, Uint8 uOpenBus) const;
    void  WriteMainIRAM(Uint16 uAddr, Uint8 uData);
    Uint8 ReadMainBWRAM(Uint32 uAddr, Uint8 uOpenBus);
    void  WriteMainBWRAM(Uint32 uAddr, Uint8 uData);
    Uint8 ReadMainROM(Uint32 uAddr, Uint8 uOpenBus) const;
    void  WriteMainROM(Uint32 uAddr, Uint8 uData); /* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNSA1_H */

    void MapMainCPU(SNCpuT *pMainCpu);

    /* Current reference emulator S-CPU vector override semantics. */
    Bool EnterMainIRQOverride(SNCpuT *pMainCpu);
    Bool EnterMainNMIOverride(SNCpuT *pMainCpu);

private:
    SnesSystem *m_pOwner;
    SNCpuT m_Cpu;
    Bool m_bActive;
    Bool m_bMapMainRom;
    Bool m_bDonorBWRAM;
    Bool m_bTrackBWRAMDirty;

    const Uint8 *m_pRom;
    Uint32 m_nRomBytes;
    Uint8 *m_pBWRAM;
    Uint32 m_nBWRAMBytes;
    Uint32 m_uBWRAMMask; /* V8.3.2: size-1 when power-of-two, else 0 */

    Uint8 m_Reg[0x200];       /* $2200-$23FF backing/status */
    Uint8 m_IRAM[0x800];      /* real 2 KiB SA-1 I-RAM */
    Uint8 m_CharData[0x80];   /* CC2 staging, not hidden in ROM storage */

    Uint16 m_uOp1;
    Uint16 m_uOp2;
    Uint8 m_uArithmeticOp;
    unsigned long long m_uSum;
    Bool m_bArithmeticOverflow;

    Uint8 m_uVariableBitPos;
    Uint8 m_uCharIndex;
    Bool m_bCharDMA;
    Uint8 m_uBitmapFormat;

    Uint32 m_uHCounter;
    Uint32 m_uVCounter;
    Uint32 m_uPrevHCounter;
    Uint16 m_uLatchedHCounter; /* AURORA_SA1_ACCURACY_REVIEW_V2_20260902 */
    Uint16 m_uLatchedVCounter;
    Bool m_bTimerLastState;

    static Uint8 SNCPU_TRAPFUNC ReadCPU(SNCpuT *pCpu, Uint32 uAddr);
    static void  SNCPU_TRAPFUNC WriteCPU(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData);

    Uint8 ReadBus(Uint32 uAddr, Uint8 uOpenBus);
    void  WriteBus(Uint32 uAddr, Uint8 uData);

    Uint8 ReadRegister(Uint16 uAddr, Uint8 uOpenBus);
    void  WriteRegister(Uint16 uAddr, Uint8 uData);
    static Bool MainCanWriteRegister(Uint16 uAddr);
    static Bool SA1CanWriteRegister(Uint16 uAddr);

    Uint32 MirrorRomOffset(Uint32 uPos) const;
    Uint32 RomOffset(Uint8 uBank, Uint16 uAddr) const;
    Bool BSXMemoryOffset(Uint8 uBank, Uint16 uAddr, Uint32 *pOffset) const;
    void MapRomWindows(SNCpuT *pCpu, Bool bMainCpu);
    void MapRomGroup(SNCpuT *pCpu, Uint32 uGroup, Bool bMainCpu);
    void MapRomPage(SNCpuT *pCpu, Uint32 uBus, Bool bMainCpu);
    void MapSA1BWRAMWindow();
    void MapMainBWRAMWindow(SNCpuT *pMainCpu);
    void MapSA1CPU();

    Uint32 MainBWRAMOffset(Uint32 uAddr, Bool *pOK) const;
    Uint32 SA1BWRAMOffset(Uint32 uAddr, Bool *pOK, Bool *pBitmap) const;
    _INLINE Uint32 WrapBWRAMOffset(Uint32 uOffset) const
    {
        if (!m_nBWRAMBytes) return 0;
        return m_uBWRAMMask ? (uOffset & m_uBWRAMMask)
                            : (uOffset % m_nBWRAMBytes);
    }
    Uint8 ReadBWRAMLinear(Uint32 uOffset, Uint8 uOpenBus) const;
    void WriteBWRAMLinear(Uint32 uOffset, Uint8 uData);
    Uint8 ReadBitmap(Uint32 uPixelAddr, Uint8 uOpenBus) const;
    void WriteBitmap(Uint32 uPixelAddr, Uint8 uData);
    Bool BWRAMWriteProtected(Uint32 uOffset) const;
    Bool IRAMWriteAllowed(Bool bSA1, Uint32 uOffset) const;

    void DoDMA();
    Uint8 ReadCC1(Uint32 uBWRAMOffset);
    void DoCC2();
    void ReadVariableLength(Bool bInc, Bool bNoShift);
    void DoArithmetic();

    void UpdateMainIRQ();
    void UpdateTimer(Uint32 nSA1Cycles);
    void ServiceInterrupts();
    void ResetCPUToVector(Uint16 uVector);
    static Bool EnterInterrupt(SNCpuT *pCpu, Uint16 uVector, Bool bNMI);

    void MarkBWRAMDirty();
};

#endif
