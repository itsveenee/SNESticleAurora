#pragma once

#include "types.h"

enum MainLoopSramDeviceE
{
    MAINLOOP_SRAMDEVICE_AUTO,
    MAINLOOP_SRAMDEVICE_USB,
    MAINLOOP_SRAMDEVICE_MEMCARD,
    MAINLOOP_SRAMDEVICE_NUM
};

enum MainLoopStateDeviceE
{
	MAINLOOP_STATEDEVICE_AUTO,
	MAINLOOP_STATEDEVICE_USB,
	MAINLOOP_STATEDEVICE_MEMCARD,
	MAINLOOP_STATEDEVICE_MMCE,
	MAINLOOP_STATEDEVICE_HDD,

	MAINLOOP_STATEDEVICE_NUM
};

void PathTruncFileName(Char *pOut, Char *pStr, Int32 nMaxChars);
int PathGetMaxFileNameLength(const char *pPath);

Bool _MainLoopHasSRAM();
Bool _MainLoopSaveSRAM(Bool bSync);
void _MainLoopLoadSRAM();
Bool _MainLoopCheckSRAM();
Bool _MainLoopForceCheckSRAM();

/* AURORA_SGB_GB_SAVEDATA_V0_3_20260904
 * Full variable-size Game Boy savedata buffer. Returned load buffer is malloc'd. */
Bool MainLoopLoadGBSavedata(Uint8 **ppData, Uint32 *pBytes);
Bool MainLoopSaveGBSavedata(const Uint8 *pData, Uint32 nBytes);
void MainLoopFreeGBSavedata(Uint8 *pData);

/* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901 */
void _MainLoopSwcCartSRAMAttach(const Char *pCartPath);
void _MainLoopSwcCartSRAMDetach();

void MainLoopSramSetDevice(MainLoopSramDeviceE eDevice);
void MainLoopSramCycleDevice();
MainLoopSramDeviceE MainLoopSramGetDevice();
const Char *MainLoopSramGetDeviceName();
const Char *MainLoopSramGetBrowseRoot();
/* AURORA_SYSTEM_BIOS_PATH_FIX_V1_20260826: SYSTEM/firmware storage is independent from SRAM selection. */
Bool MainLoopEnsureSystemDirectory(Char *pOut, Int32 nOutBytes);
Bool MainLoopEnsureSwcDirectory(Char *pOut, Int32 nOutBytes); /* AURORA_SWC_MEGA_V9_20260831 */
Bool MainLoopFindSystemFileDirectory(Char *pOut, Int32 nOutBytes,
                                     const Char *pFileName);
Bool MainLoopSramNeedsMemoryCardPreflight();
Bool _MainLoopLoadState();
Bool _MainLoopSaveState();

void MainLoopStateSettingsLoad();
Bool MainLoopStateSettingsSave();
void MainLoopStateOnRomChanged();
void MainLoopStatePrimeRomIdentityCRC(Uint32 uCRC);
/* AURORA_PD_MEGA_FIX_20260820 */
Bool MainLoopStateHasDeviceChoice();
void MainLoopStateForgetDeviceChoice();
void MainLoopStateSetDevice(MainLoopStateDeviceE eDevice);
Bool MainLoopStateDeviceAvailable(MainLoopStateDeviceE eDevice);
void MainLoopStateCycleSlot();
void MainLoopStateCycleDevice();
Int32 MainLoopStateGetSlot();
MainLoopStateDeviceE MainLoopStateGetDevice();
const Char *MainLoopStateGetDeviceName();
const Char *MainLoopStateGetAvailability();
const Char *MainLoopStateGetLastMessage();
Int32 MainLoopStateGetUnformattedCard();

#if MAINLOOP_HISTORY
void _MainLoopResetHistory();
#endif
void _MainLoopResetInputChecksums();
#if MAINLOOP_HISTORY
void _MainLoopSaveHistory();
#endif
