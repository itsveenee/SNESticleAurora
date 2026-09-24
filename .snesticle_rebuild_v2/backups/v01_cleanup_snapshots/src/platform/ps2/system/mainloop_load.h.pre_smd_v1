#pragma once

#include "types.h"

namespace Emu { class Rom; }

void _MainLoopGetName(Char *pName, const Char *pPath);
int _MainLoopReadBinaryData(Uint8 *pBuffer, Int32 nBufferBytes, const char *pRomFile);
int _MainLoopReadGZData(Uint8 *pBuffer, Int32 nBufferBytes, const char *pRomFile);
int _MainLoopReadZipData(Uint8 *pBuffer, Int32 nBufferBytes, const char *pZipFile, char *pFileName);
Bool _MainLoopLoadRomData(Emu::Rom *pRom, Uint8 *pRomData, Int32 nRomBytes);
Bool _MainLoopLoadBios(Emu::Rom *pRom, const Char *pFilePath);
Bool _MainLoopLoadNesPalette(const char *pFileName); /* AURORA_FCEUMM_FDS_V4_TURBO_PAL_PERF_20260827 */
Bool _MainLoopLoadSnesPalette(const char *pFileName);
void _MainLoopUnloadRom();
Bool _MainLoopExecuteFile(const char *pFileName, Bool bLoadSRAM);
Bool MainLoopSwcSwapNextDisk(void); /* AURORA_SWC_FLOPPY_V1_20260831 */
Bool MainLoopSwcCreateNextDisk(void); /* AURORA_SWC_FLOPPY_V5_20260831 */
void _MainLoopSetSampleRate(Uint32 uSampleRate);
