#ifndef _AURORA_GBHOST_H
#define _AURORA_GBHOST_H

#include "types.h"
#include <stdint.h> /* AURORA_SGB_GAMBATTE_LINKFIX_V1_1_1_20260907 */

class SNSGBICD2;

/* AURORA_SGB_GAMBATTE_BACKEND_V1_1_20260907
 * Neutral Game Boy host used by SNSuperGameBoy.
 *
 * Backend: staged libgambatte, forced to DMG mode and clocked by Aurora's
 * existing ICD2 grant. Aurora remains responsible for the SNES + ICD2 side.
 */
class GBHost
{
public:
    enum ModelE { MODEL_SGB1 = 1, MODEL_SGB2 = 2 };
    enum { SERIALIZED_BYTES = 0x20000 };

    typedef Uint8 (*JoypHookT)(void *pContext, Bool bP14, Bool bP15, Bool bWrite);
    typedef void (*PixelHookT)(void *pContext, Uint8 uColor);
    typedef void (*ResetHookT)(void *pContext);

    /* Public declaration only so translation-unit helpers can name it.
       The actual instance pointer remains private. */
    struct Impl;

    struct StateT
    {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Model;
        Uint32 Reserved;       /* actual Gambatte state bytes */
        Int64 ClockCredit;     /* instruction-boundary overshoot */
        Uint32 PendingClocks;  /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907: deferred, not yet executed */
        Int32 Reserved2;
        Uint8 Serialized[SERIALIZED_BYTES];
    };

    GBHost();
    ~GBHost();

    Bool Init();
    void Shutdown();
    Bool IsInitialized() const;
    Bool IsLoaded() const;

    Bool LoadROM(const Uint8 *pData, Uint32 nBytes, ModelE eModel);
    void UnloadROM();
    void Reset(ModelE eModel);

    Bool AttachSavedata(const Uint8 *pData, Uint32 nBytes);
    Uint32 GetSavedataBytes();
    Bool ExportSavedata(Uint8 *pData, Uint32 nCapacity, Uint32 *pActualBytes);
    Bool SavedataDirty() const;
    void ClearSavedataDirty();

    Uint32 RunClocks(Uint32 nTargetClocks);
    void FlushClocks(); /* AURORA_SGB_CLASSIC_PLUS_LINK_V2_20260907 */

    Uint32 DebugPreTickState() const;
    const char *DebugPreEventName() const;
    Int32 DebugPreEventDelta() const;
    Bool DebugSkipDueAudioSample();

    Uint32 GetClockHz() const;
    Int64 GetClockCredit() const;
    Uint32 GetROMBytes() const;
    Uint32 GetROMCRC() const;

    Uint32 ReadAudioFrames(Int16 *pStereoInterleaved, Uint32 nFrames);
    void ClearAudio();

    void SetHooks(JoypHookT pJoyp, PixelHookT pPixel,
                  ResetHookT pHReset, ResetHookT pVReset,
                  void *pContext);

    void SetICD2FastPath(SNSGBICD2 *pICD2);

    Bool SaveState(StateT *pState) const;
    Bool RestoreState(const StateT *pState);

    static unsigned char GambatteJoypCallback(
        void *pOpaque, unsigned char p14p15, bool bWrite);

private:
    Impl *m_p;
};

#endif
