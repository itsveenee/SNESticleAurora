/* AURORA_CORE_FLAGS_FINAL_20261104: no dynamic core, no source mutation. */
#include "platform/ps2/system/aurora_core_build_config.h"
#include <stddef.h>
#if AURORA_NO_GAMBATTE
#include "gb/system/gambattesystem.h"
GambatteSystem::GambatteSystem() : m_p(NULL) {}
GambatteSystem::~GambatteSystem() {}
Bool GambatteSystem::LoadGame(const Uint8 *, Uint32, Uint32, StandaloneModeE, const Uint8 *, Uint32) { return FALSE; }
void GambatteSystem::UnloadGame() {}
Bool GambatteSystem::IsGameLoaded() const { return FALSE; }
Bool GambatteSystem::UsesSquarePixelPresentation() const { return FALSE; }
Uint32 GambatteSystem::GetGameCRC() const { return 0; }
Uint32 GambatteSystem::GetGameBytes() const { return 0; }
Uint32 GambatteSystem::GetSavedataBytes() const { return 0; }
Bool GambatteSystem::AttachSavedata(const Uint8 *, Uint32) { return FALSE; }
Bool GambatteSystem::ExportSavedata(Uint8 *, Uint32, Uint32 *) const { return FALSE; }
Bool GambatteSystem::SavedataDirty() const { return FALSE; }
void GambatteSystem::ClearSavedataDirty() {}
Bool GambatteSystem::HasTurboFile() const { return FALSE; }
Uint32 GambatteSystem::GetTurboFileBytes() const { return 0; }
Uint8 *GambatteSystem::GetTurboFileData() { return NULL; }
const Uint8 *GambatteSystem::GetTurboFileData() const { return NULL; }
Bool GambatteSystem::AttachTurboFile(const Uint8 *, Uint32) { return FALSE; }
Bool GambatteSystem::TurboFileDirty() const { return FALSE; }
void GambatteSystem::ClearTurboFileDirty() {}
Bool GambatteSystem::SaveStateChecked(void *, Int32) { return FALSE; }
Bool GambatteSystem::RestoreStateChecked(const void *, Int32) { return FALSE; }
void GambatteSystem::SetRom(Emu::Rom *) {}
void GambatteSystem::Reset() {}
void GambatteSystem::SoftReset() {}
void GambatteSystem::ExecuteFrame(Emu::SysInputT *, CRenderSurface *, CMixBuffer *, Emu::System::ModeE) {}
Int32 GambatteSystem::GetStateSize() { return 0; }
void GambatteSystem::SaveState(void *, Int32) {}
void GambatteSystem::RestoreState(void *, Int32) {}
const char *GambatteSystem::GetString(Emu::System::StringE) { return ""; }
Uint32 GambatteSystem::GetSampleRate() { return 0; }
#endif
#if AURORA_NO_GPSP
#include "gba/system/gpspsystem.h"
static Bool s_AuroraDisabledGpSPBlending = FALSE;
Bool GpSPGetFrameBlending(void) { return s_AuroraDisabledGpSPBlending; }
void GpSPSetFrameBlending(Bool enabled) { s_AuroraDisabledGpSPBlending = enabled; }
GpSPSystem::GpSPSystem() : m_p(NULL) {}
GpSPSystem::~GpSPSystem() {}
Bool GpSPSystem::LoadGame(const Char *, const Char *, Uint32, Uint32) { return FALSE; }
Bool GpSPSystem::LoadGameMemory(const void *, Uint32, Uint32, const Char *, const Char *) { return FALSE; }
void GpSPSystem::UnloadGame() {}
Bool GpSPSystem::IsGameLoaded() const { return FALSE; }
void GpSPSystem::SetRom(Emu::Rom *) {}
void GpSPSystem::Reset() {}
void GpSPSystem::SoftReset() {}
void GpSPSystem::ExecuteFrame(Emu::SysInputT *, CRenderSurface *, CMixBuffer *, Emu::System::ModeE) {}
Int32 GpSPSystem::GetStateSize() { return 0; }
void GpSPSystem::SaveState(void *, Int32) {}
void GpSPSystem::RestoreState(void *, Int32) {}
Bool GpSPSystem::SaveStateChecked(void *, Int32) { return FALSE; }
Bool GpSPSystem::RestoreStateChecked(const void *, Int32) { return FALSE; }
Int32 GpSPSystem::GetSRAMBytes() { return 0; }
Uint8 *GpSPSystem::GetSRAMData() { return NULL; }
const char *GpSPSystem::GetString(Emu::System::StringE) { return ""; }
Uint32 GpSPSystem::GetSampleRate() { return 0; }
Bool GpSPSystem::CanDirectGsVideo() const { return FALSE; }
Bool GpSPSystem::DrawDirectGs(Uint32, Float32) { return FALSE; }
Bool GpSPSystem::HasTurboFileAdvance() const { return FALSE; }
Uint8 *GpSPSystem::GetTurboFileAdvanceData() { return NULL; }
const Uint8 *GpSPSystem::GetTurboFileAdvanceData() const { return NULL; }
Uint32 GpSPSystem::GetTurboFileAdvanceBytes() const { return 0; }
Bool GpSPSystem::AttachTurboFileAdvance(const Uint8 *, Uint32) { return FALSE; }
Bool GpSPSystem::TurboFileAdvanceDirty() const { return FALSE; }
void GpSPSystem::ClearTurboFileAdvanceDirty() {}
Uint32 GpSPSystem::GetGameCRC() const { return 0; }
Uint32 GpSPSystem::GetGameBytes() const { return 0; }
#endif
