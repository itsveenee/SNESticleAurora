#pragma once

#include "types.h"
#include "emusys.h"

class CRenderSurface;
class CMixBuffer;

/* AURORA_GPSP_GBA_V1_20260911
 * Aurora-owned adapter around the pinned gpSP PS2/libretro core.
 * The frontend contract remains Emu::System, just like Gambatte: gpSP does
 * not own the Aurora browser, GS, pads, SRAM UI or state UI. */
class GpSPSystem : public Emu::System
{
public:
    struct Impl;

    GpSPSystem();
    virtual ~GpSPSystem();

    Bool LoadGame(const Char *pPath, const Char *pSystemDirectory);
    void UnloadGame();
    Bool IsGameLoaded() const;

    virtual void SetRom(Emu::Rom *pRom);
    virtual void Reset();
    virtual void SoftReset();
    virtual void ExecuteFrame(Emu::SysInputT *pInput,
                              CRenderSurface *pTarget,
                              CMixBuffer *pMixBuf,
                              Emu::System::ModeE eMode);

    virtual Int32 GetStateSize();
    virtual void SaveState(void *pState, Int32 nStateBytes);
    virtual void RestoreState(void *pState, Int32 nStateBytes);

    /* AURORA_GPSP_GBA_V13_SAFE_PERF_20260911
     * Checked variants are used by Aurora's state-file pipeline so an
     * allocation/size/core serialization failure is never reported as a
     * successful save or restore. */
    Bool SaveStateChecked(void *pState, Int32 nStateBytes);
    Bool RestoreStateChecked(const void *pState, Int32 nStateBytes);

    virtual Int32 GetSRAMBytes();
    virtual Uint8 *GetSRAMData();
    virtual const char *GetString(Emu::System::StringE eString);
    virtual Uint32 GetSampleRate();

    /* AURORA_GPSP_GBA_V16_DIRECT_GS_CT16_20260912
     * Normal gpSP PS2 video is already 0BGR1555, the GS' native CT16
     * channel ordering. Let the frontend upload it directly instead of
     * expanding 38,400 pixels to Aurora RGBA32 first. */
    Bool CanDirectGsVideo() const;
    Bool DrawDirectGs(Uint32 auroraOutBaseTBP, Float32 intensity);

    /* AURORA_GPSP_GBA_V2_TFA_BLEND_20260911 */
    Bool HasTurboFileAdvance() const;
    Uint8 *GetTurboFileAdvanceData();
    const Uint8 *GetTurboFileAdvanceData() const;
    Uint32 GetTurboFileAdvanceBytes() const;
    Bool AttachTurboFileAdvance(const Uint8 *pData, Uint32 nBytes);
    Bool TurboFileAdvanceDirty() const;
    void ClearTurboFileAdvanceDirty();
    Uint32 GetGameCRC() const;

private:
    Impl *m_p;
};
