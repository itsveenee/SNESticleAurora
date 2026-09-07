#ifndef _AURORA_GBHOST_H
#define _AURORA_GBHOST_H

#include "types.h"

class SNSGBICD2;

/* AURORA_SGB_SAMEBOY_BACKEND_V1_20260906
 * Neutral Game Boy host used by SNSuperGameBoy.
 *
 * Backend: SameBoy in GB_MODEL_SGB_NO_SFC / GB_MODEL_SGB2_NO_SFC mode.
 * SameBoy emulates GB CPU/PPU/APU/MBC and executes the open SGB bootstrap;
 * Aurora remains responsible for the SNES and ICD2 side.
 */
class GBHost
{
public:
    enum ModelE { MODEL_SGB1 = 1, MODEL_SGB2 = 2 };
    enum { SERIALIZED_BYTES = 0x20000 };

    typedef Uint8 (*JoypHookT)(void *pContext, Bool bP14, Bool bP15, Bool bWrite);
    typedef void (*PixelHookT)(void *pContext, Uint8 uColor);
    typedef void (*ResetHookT)(void *pContext);

    struct StateT
    {
        Uint32 Magic;
        Uint32 Version;
        Uint32 Model;
        Uint32 Reserved; /* actual SameBoy state bytes */
        Int64 ClockCredit; /* SameBoy 8 MHz tick credit */
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

    /* Source compatibility with retired mGBA scheduler probes. */
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

    /* AURORA_SGB_HOTPATH_V2_20260907
     * Optional SGB-only raster fast path. Generic callbacks remain the
     * fallback so GBHost stays reusable outside SNSuperGameBoy. */
    void SetICD2FastPath(SNSGBICD2 *pICD2);

    Bool SaveState(StateT *pState) const;
    Bool RestoreState(const StateT *pState);

    /* C callback shims. Public only because SameBoy callbacks are C ABI. */
    static void BootRomThunk(void *pOpaque, Int32 eBootType);
    static void JoypThunk(void *pOpaque, Uint8 value);
    static void PixelThunk(void *pOpaque, Uint8 pixel);
    /* AURORA_SGB_SCANLINE_BATCH_V4_20260907 */
    static void LineThunk(void *pOpaque, const Uint32 *pPixels, Int32 nLine);
    static void HResetThunk(void *pOpaque);
    static void VResetThunk(void *pOpaque);
    static void SampleThunk(void *pOpaque, Int16 left, Int16 right);

private:
    struct Impl;
    Impl *m_p;
};

#endif
