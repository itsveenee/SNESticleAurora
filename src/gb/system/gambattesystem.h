#ifndef _AURORA_GAMBATTE_SYSTEM_H
#define _AURORA_GAMBATTE_SYSTEM_H

/* AURORA_GAMBATTE_STANDALONE_V2_20260908
 * Native Aurora client of the pinned Gambatte core. This is deliberately an
 * Emu::System, not a second libretro frontend: Aurora owns menu/reset/audio/
 * state/SRAM/video policy exactly like the other native clients. */

#include "types.h"
#include "emusys.h"

class GambatteSystem : public Emu::System
{
public:
    enum StandaloneModeE {
        STANDALONE_CGB = 0,
        STANDALONE_SGB_PALETTE = 1
    };

    GambatteSystem();
    virtual ~GambatteSystem();

    /* AURORA_SGB_GAMBATTE_R8_COMPLETE_20260909
     * CGB mode requires an authentic 0x900-byte CGB boot ROM. SGB-palette
     * mode is standalone DMG execution with SGB presentation, never SNES. */
    Bool LoadGame(const Uint8 *pData, Uint32 nBytes, Uint32 uCRC,
                  StandaloneModeE eMode,
                  const Uint8 *pCgbBootRom, Uint32 nCgbBootRomBytes);
    void UnloadGame();
    Bool IsGameLoaded() const;
    Uint32 GetGameCRC() const;
    Uint32 GetGameBytes() const;

    Uint32 GetSavedataBytes() const;
    Bool AttachSavedata(const Uint8 *pData, Uint32 nBytes);
    Bool ExportSavedata(Uint8 *pData, Uint32 nCapacity, Uint32 *pActual) const;
    Bool SavedataDirty() const;
    void ClearSavedataDirty();

    /* AURORA_GB_ASCII_TURBO_FILE_R8_20260909
     * Physical shared accessory backing, allocated only for the two
     * No-Intro RPG Tsukuru GB CRCs. */
    Bool HasTurboFile() const;
    Uint32 GetTurboFileBytes() const;
    Uint8 *GetTurboFileData();
    const Uint8 *GetTurboFileData() const;
    Bool AttachTurboFile(const Uint8 *pData, Uint32 nBytes);
    Bool TurboFileDirty() const;
    void ClearTurboFileDirty();

    Bool SaveStateChecked(void *pState, Int32 nStateBytes);
    Bool RestoreStateChecked(const void *pState, Int32 nStateBytes);

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

    /* Battery/RTC persistence is intentionally NOT exposed as generic SRAM:
     * Aurora stores the same concatenated GB/<rom>.sav used by SGB. */
    virtual Int32 GetSRAMBytes() { return 0; }
    virtual Uint8 *GetSRAMData() { return NULL; }
    virtual const char *GetString(Emu::System::StringE eString);
    virtual Uint32 GetSampleRate();

public:
    struct Impl;
private:
    Impl *m_p;

    GambatteSystem(const GambatteSystem &);
    GambatteSystem &operator=(const GambatteSystem &);
};

#endif
