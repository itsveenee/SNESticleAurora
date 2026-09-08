#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>

#include "types.h"
#include "console.h"
#include "file.h"
#include "prof.h"
#include "memcard.h"
#include "miniz.h"
#include "mainloop_debug.h"
#include "mainloop_bgm.h"
#include "embedded_irx.h"
#include "mainloop_iop.h"

extern "C" {
#include "netplay_ee.h"
}

#include "mainloop_shared.h"
#include "mainloop_state.h"
#include "nes/quicknes/quicknes_bridge.h" /* AURORA_QN_TURBOFILE_SAVE_V2_20260828 */
#include "sega/picodrive/picodrive_bridge.h" /* AURORA_CD_STATE_V1_SAFE_20260903 */
#include "pce/beetle/pce_bridge.h" /* AURORA_CD_STATE_V1_SAFE_20260903 */

/* AURORA_PCE_EXPERIMENTAL_V1 */

/* AURORA_RUNTIME_LEAN_V1_MCSAVE_20260824
 * The old MCSAVE.IRX path is unreachable in Aurora: it is never loaded or
 * initialized. mainloop_state therefore contains only the supported
 * synchronous newlib/iomanX storage path. */

#if MAINLOOP_HISTORY
extern Uint32 _nHistory;
#endif


static Uint32 _PathCalcHash(const char *pStr)
{
    Uint32 hash = 0;

    while (*pStr)
    {
        hash *= 33;
        hash += *pStr;
        pStr++;
    }

    return hash;
}

void PathTruncFileName(Char *pOut, Char *pStr, Int32 nMaxChars)
{
    Uint32 hash;
    Int32 nOriginalMax = nMaxChars;

    /* AURORA_RUNTIME_SAFE_PATH_TRUNC_V1_4_2 */
    if (!pOut)
        return;
    if (!pStr || nMaxChars <= 0)
    {
        *pOut = '\0';
        return;
    }

    hash = _PathCalcHash(pStr);

    // copy string up to maxchars length
    while (*pStr && nMaxChars > 0)
    {
        *pOut++ = *pStr++;
        nMaxChars--;
    }

    // terminate
    *pOut = 0;

    /* pOut-3 is valid only after at least three characters were copied. */
    if (nMaxChars <= 0 && nOriginalMax >= 3)
    {
        sprintf(pOut - 3, "%03u", (unsigned int)(hash % 1000));
    }
}

int PathGetMaxFileNameLength(const char *pPath)
{
    if ((pPath[0] == 'm' && pPath[1] == 'c') ||
        (pPath[0] == 'm' && pPath[1] == 'm' &&
         pPath[2] == 'c' && pPath[3] == 'e'))
    {
        return 32;
    }

    return 256;
}

/* AURORA_FINAL_V1_7_D88_DUAL_SRAM_PERSIST_20260901 */
/* AURORA_SRAM_STORAGE_V1
 * _SramPath stays the settings/legacy root. SRAM data has its own policy. */
static MainLoopSramDeviceE _MainLoop_SramDevice = MAINLOOP_SRAMDEVICE_AUTO;

static const Char *_MainLoopSramGetSystemDirectoryName()
{
    if (_pSystem == _pNes)  return "NES";
    if (_pSystem == _pSega) return "SEGA";
    if (_pSystem == _pPce)  return "PCE";
    return "SNES";
}

static const Char *_MainLoopSramRoot(MainLoopSramDeviceE eDevice)
{
    return eDevice == MAINLOOP_SRAMDEVICE_USB ? "mass0:/SNESticle" : _SramPath;
}

static Bool _MainLoopSramUsbReady()
{
    struct stat Status;
    if (!MassStorageIsEnabled())
        return FALSE;
    return stat("mass0:/", &Status) == 0 ? TRUE : FALSE;
}

void MainLoopSramSetDevice(MainLoopSramDeviceE eDevice)
{
    if (eDevice < MAINLOOP_SRAMDEVICE_AUTO || eDevice >= MAINLOOP_SRAMDEVICE_NUM)
        eDevice = MAINLOOP_SRAMDEVICE_AUTO;
    _MainLoop_SramDevice = eDevice;
}

void MainLoopSramCycleDevice()
{
    _MainLoop_SramDevice = (MainLoopSramDeviceE)(_MainLoop_SramDevice + 1);
    if (_MainLoop_SramDevice >= MAINLOOP_SRAMDEVICE_NUM)
        _MainLoop_SramDevice = MAINLOOP_SRAMDEVICE_AUTO;
}

MainLoopSramDeviceE MainLoopSramGetDevice()
{
    return _MainLoop_SramDevice;
}

const Char *MainLoopSramGetDeviceName()
{
    switch (_MainLoop_SramDevice)
    {
        case MAINLOOP_SRAMDEVICE_USB:     return "USB only";
        case MAINLOOP_SRAMDEVICE_MEMCARD: return "Memory Card";
        default:                          return "USB -> MC fallback";
    }
}

const Char *MainLoopSramGetBrowseRoot()
{
    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_USB)
        return _MainLoopSramRoot(MAINLOOP_SRAMDEVICE_USB);
    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
        return _MainLoopSramRoot(MAINLOOP_SRAMDEVICE_MEMCARD);
    return _MainLoopSramUsbReady()
        ? _MainLoopSramRoot(MAINLOOP_SRAMDEVICE_USB)
        : _MainLoopSramRoot(MAINLOOP_SRAMDEVICE_MEMCARD);
}

Bool MainLoopSramNeedsMemoryCardPreflight()
{
    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_USB)
        return FALSE;
    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
        return TRUE;
    return _MainLoopSramUsbReady() ? FALSE : TRUE;
}

static Bool _MainLoopSramEnsureOneDir(const Char *pPath)
{
    struct stat Status;

    /* AURORA_RUNTIME_SAFE_SRAM_DIR_V1_4_1
     * EEXIST alone is insufficient: the path could be a regular file. */
    if (mkdir(pPath, 0777) == 0)
        return TRUE;
    return stat(pPath, &Status) == 0 && S_ISDIR(Status.st_mode)
        ? TRUE : FALSE;
}

/* AURORA_SYSTEM_BIOS_PATH_FIX_V1_20260826
 * SYSTEM stores user-supplied firmware, not SRAM. Do not let the SRAM target
 * decide where BIOS files live.
 *
 * Prefer writable mass roots independently of the SRAM setting. This also
 * covers a normal USB device exposed as mass0: and alternative mass aliases.
 * If no mass root is writable, preserve the previous SRAM-root behaviour as
 * a compatibility fallback (including the memory-card save-directory helper).
 */
static Bool _MainLoopSystemCopyDirectory(Char *pOut, Int32 nOutBytes,
                                         const Char *pDirectory)
{
    int nChars;

    if (!pOut || nOutBytes <= 0 || !pDirectory || !*pDirectory)
        return FALSE;

    nChars = snprintf(pOut, (size_t)nOutBytes, "%s", pDirectory);
    if (nChars < 0 || nChars >= nOutBytes)
    {
        pOut[0] = '\0';
        return FALSE;
    }
    return TRUE;
}

static Bool _MainLoopSystemTryWritableRoot(Char *pOut, Int32 nOutBytes,
                                           const Char *pRoot)
{
    Char Directory[512];
    int nChars;

    if (!pRoot || !*pRoot)
        return FALSE;

    if (!_MainLoopSramEnsureOneDir(pRoot))
        return FALSE;

    nChars = snprintf(Directory, sizeof(Directory), "%s/SYSTEM", pRoot);
    if (nChars < 0 || nChars >= (int)sizeof(Directory))
        return FALSE;

    if (!_MainLoopSramEnsureOneDir(Directory))
        return FALSE;

    return _MainLoopSystemCopyDirectory(
        pOut, nOutBytes, Directory);
}

static Bool _MainLoopSystemFileAtRoot(Char *pOut, Int32 nOutBytes,
                                      const Char *pRoot,
                                      const Char *pFileName)
{
    struct stat Status;
    Char Directory[512];
    Char FilePath[1024];
    int nChars;

    if (!pOut || nOutBytes <= 0 ||
        !pRoot || !*pRoot || !pFileName || !*pFileName)
        return FALSE;

    nChars = snprintf(Directory, sizeof(Directory), "%s/SYSTEM", pRoot);
    if (nChars < 0 || nChars >= (int)sizeof(Directory))
        return FALSE;

    nChars = snprintf(FilePath, sizeof(FilePath),
                      "%s/%s", Directory, pFileName);
    if (nChars < 0 || nChars >= (int)sizeof(FilePath))
        return FALSE;

    if (stat(FilePath, &Status) != 0 || S_ISDIR(Status.st_mode))
        return FALSE;

    return _MainLoopSystemCopyDirectory(
        pOut, nOutBytes, Directory);
}

Bool MainLoopFindSystemFileDirectory(Char *pOut, Int32 nOutBytes,
                                     const Char *pFileName)
{
    static const Char *pPreferredRoots[] =
    {
        "mass0:/SNESticle",
        "mass1:/SNESticle",
        "mass:/SNESticle",
        NULL
    };
    static const Char *pFallbackRoots[] =
    {
        "mc0:/SNESticle",
        "mc1:/SNESticle",
        "mmce0:/SNESticle",
        "mmce1:/SNESticle",
        NULL
    };
    const Char *pActiveRoot;
    Int32 i;

    if (!pOut || nOutBytes <= 0 || !pFileName || !*pFileName)
        return FALSE;
    pOut[0] = '\0';

    /* Canonical USB firmware tree wins first. */
    for (i = 0; pPreferredRoots[i]; ++i)
    {
        if (_MainLoopSystemFileAtRoot(
                pOut, nOutBytes, pPreferredRoots[i], pFileName))
        {
            printf("[SYSTEM] found %s in %s\n", pFileName, pOut);
            return TRUE;
        }
    }

    /* Preserve any configured/legacy root as a secondary lookup. */
    pActiveRoot = MainLoopSramGetBrowseRoot();
    if (pActiveRoot && *pActiveRoot &&
        _MainLoopSystemFileAtRoot(
            pOut, nOutBytes, pActiveRoot, pFileName))
    {
        printf("[SYSTEM] found %s in %s\n", pFileName, pOut);
        return TRUE;
    }

    for (i = 0; pFallbackRoots[i]; ++i)
    {
        if (_MainLoopSystemFileAtRoot(
                pOut, nOutBytes, pFallbackRoots[i], pFileName))
        {
            printf("[SYSTEM] found %s in %s\n", pFileName, pOut);
            return TRUE;
        }
    }

    pOut[0] = '\0';
    return FALSE;
}

Bool MainLoopEnsureSystemDirectory(Char *pOut, Int32 nOutBytes)
{
    static const Char *pPreferredRoots[] =
    {
        "mass0:/SNESticle",
        "mass1:/SNESticle",
        "mass:/SNESticle",
        NULL
    };
    const Char *pRoot;
    Bool bMemCard;
    Char Directory[512];
    int nChars;
    Int32 i;

    if (!pOut || nOutBytes <= 0)
        return FALSE;
    pOut[0] = '\0';

    /* Firmware storage is independent of the SRAM destination. */
    for (i = 0; pPreferredRoots[i]; ++i)
    {
        if (_MainLoopSystemTryWritableRoot(
                pOut, nOutBytes, pPreferredRoots[i]))
        {
            printf("[SYSTEM] directory: %s\n", pOut);
            return TRUE;
        }
    }

    /* Compatibility fallback: retain the old selected SRAM root behaviour. */
    pRoot = MainLoopSramGetBrowseRoot();
    bMemCard = MainLoopSramNeedsMemoryCardPreflight();
    if (!pRoot || !*pRoot)
        return FALSE;

    if (!bMemCard && !_MainLoopSramEnsureOneDir(pRoot))
        return FALSE;

    nChars = snprintf(Directory, sizeof(Directory), "%s/SYSTEM", pRoot);
    if (nChars < 0 || nChars >= (int)sizeof(Directory))
        return FALSE;

    if (!_MainLoopSramEnsureOneDir(Directory))
    {
        if (!bMemCard)
            return FALSE;

        {
            int Result = MemCardCreateSave(
                (char *)pRoot, _MainLoop_SaveTitle, TRUE);
            if (Result < 0)
            {
                printf("[SYSTEM] MemCardCreateSave('%s') failed: %d\n",
                       pRoot, Result);
                return FALSE;
            }
        }

        if (!_MainLoopSramEnsureOneDir(Directory))
            return FALSE;
    }

    if (!_MainLoopSystemCopyDirectory(
            pOut, nOutBytes, Directory))
        return FALSE;

    printf("[SYSTEM] fallback directory: %s\n", pOut);
    return TRUE;
}

/* AURORA_V5_COPIER_LOADER_MEDIA_FLOW_20260831
 * Keep copier media out of ROM directories and out of SYSTEM firmware.
 * DSK is copier-neutral so other hardware loaders can share the same media
 * root later without pretending their disks belong to the SWC.
 */
Bool MainLoopEnsureSwcDirectory(Char *pOut, Int32 nOutBytes)
{
    Char SystemDirectory[512];
    Char Root[512];
    size_t n;
    int nChars;

    if (!pOut || nOutBytes <= 0)
        return FALSE;
    pOut[0] = 0;

    if (!MainLoopEnsureSystemDirectory(
            SystemDirectory, (Int32)sizeof(SystemDirectory)))
        return FALSE;

    n = strlen(SystemDirectory);
    if (n < 7 || strcmp(SystemDirectory + n - 7, "/SYSTEM") != 0 ||
        n - 7 >= sizeof(Root))
        return FALSE;

    memcpy(Root, SystemDirectory, n - 7);
    Root[n - 7] = 0;

    nChars = snprintf(pOut, (size_t)nOutBytes, "%s/DSK", Root);
    if (nChars < 0 || nChars >= nOutBytes)
    {
        pOut[0] = 0;
        return FALSE;
    }

    if (!_MainLoopSramEnsureOneDir(pOut))
    {
        pOut[0] = 0;
        return FALSE;
    }

    printf("[DSK] directory: %s\n", pOut);
    return TRUE;
}

static void _MainLoopSramBuildPath(Char *pPath, Int32 nPathBytes,
                                   const Char *pRoot, Bool bLegacyRoot)
{
    Char Directory[512];
    Char SaveName[256];
    const Char *pExtension =
        _pSystem->GetString(Emu::System::StringE::STRING_SRAMEXT);
    Int32 nSuffixBytes = (Int32)strlen(pExtension) + 1;

    if (bLegacyRoot)
        snprintf(Directory, sizeof(Directory), "%s", pRoot);
    else
        snprintf(Directory, sizeof(Directory), "%s/%s", pRoot,
                 _MainLoopSramGetSystemDirectoryName());

    PathTruncFileName(SaveName, _RomName,
        PathGetMaxFileNameLength(Directory) - nSuffixBytes);
    snprintf(pPath, nPathBytes, "%s/%s.%s", Directory, SaveName, pExtension);
}

/* AURORA_SRAM_MC_COPY_ALIAS_V1
 * A .srm created on a PS2 Memory Card has a filename ceiling of 32 chars.
 * PathTruncFileName therefore shortens long ROM names and replaces the last
 * three base-name chars with its stable decimal hash. A normal file copy to
 * USB preserves that short filename, while the canonical USB path uses the
 * full long filename. Build the exact MC spelling so copied saves remain
 * discoverable without relying on MC attributes or timestamps. */
static void _MainLoopSramBuildCopiedMcPath(Char *pPath, Int32 nPathBytes,
                                           const Char *pRoot,
                                           Bool bLegacyRoot)
{
    Char Directory[512];
    Char SaveName[256];
    const Char *pExtension =
        _pSystem->GetString(Emu::System::StringE::STRING_SRAMEXT);
    const Int32 nMcMaxFileName = 32;
    Int32 nSuffixBytes = (Int32)strlen(pExtension) + 1;
    Int32 nBaseMax = nMcMaxFileName - nSuffixBytes;

    if (bLegacyRoot)
        snprintf(Directory, sizeof(Directory), "%s", pRoot);
    else
        snprintf(Directory, sizeof(Directory), "%s/%s", pRoot,
                 _MainLoopSramGetSystemDirectoryName());

    PathTruncFileName(SaveName, _RomName, nBaseMax);
    snprintf(pPath, nPathBytes, "%s/%s.%s",
             Directory, SaveName, pExtension);
}

static Bool _MainLoopSramEnsureSystemDirectory(const Char *pRoot, Bool bMemCard)
{
    Char Directory[512];

    if (!bMemCard && !_MainLoopSramEnsureOneDir(pRoot))
        return FALSE;

    snprintf(Directory, sizeof(Directory), "%s/%s", pRoot,
             _MainLoopSramGetSystemDirectoryName());
    if (_MainLoopSramEnsureOneDir(Directory))
        return TRUE;

    if (bMemCard)
    {
        int Result = MemCardCreateSave((char *)pRoot, _MainLoop_SaveTitle, TRUE);
        if (Result < 0)
        {
            printf("[SRAM] MemCardCreateSave('%s') failed: %d\n", pRoot, Result);
            return FALSE;
        }
    }
    return _MainLoopSramEnsureOneDir(Directory);
}

/* Transactional read: a short/corrupt USB file cannot partially overwrite
 * live SRAM before AUTO falls back to MC. */
static Bool _MainLoopSramReadFile(const Char *pPath, Uint8 *pData, Uint32 nBytes)
{
    FILE *pFile;
    Uint8 *pTemp;
    size_t nRead;
    struct stat Status;

    if (stat(pPath, &Status) != 0 || (Uint32)Status.st_size != nBytes)
        return FALSE;

    pTemp = (Uint8 *)malloc(nBytes);
    if (!pTemp)
        return FALSE;

    pFile = fopen(pPath, "rb");
    if (!pFile)
    {
        free(pTemp);
        return FALSE;
    }
    nRead = fread(pTemp, 1, nBytes, pFile);
    fclose(pFile);
    if (nRead == nBytes)
        memcpy(pData, pTemp, nBytes);
    free(pTemp);
    return nRead == nBytes ? TRUE : FALSE;
}

static Bool _MainLoopSramWriteFile(const Char *pPath, Uint8 *pData, Uint32 nBytes)
{
    FILE *pFile = fopen(pPath, "wb");
    size_t nWritten;
    Bool bOK;
    if (!pFile)
        return FALSE;
    nWritten = fwrite(pData, 1, nBytes, pFile);
    bOK = fflush(pFile) == 0 ? TRUE : FALSE;
    if (fclose(pFile) != 0)
        bOK = FALSE;
    return nWritten == nBytes && bOK ? TRUE : FALSE;
}

/* AURORA_SGB_GB_SAVEDATA_V0_3_20260904
 * Variable-size Game Boy savedata storage. Separate from the exact-size
 * SRAM reader used by existing systems. The byte stream is the Game Boy backend's complete
 * VFile (SRAM plus RTC/mapper footer), never a raw SRAM slice.
 */
#define MAINLOOP_GB_SAVEDATA_MAX (1024U * 1024U)

static Bool _MainLoopGBEnsureDirectory(const Char *pRoot, Bool bMemCard)
{
    Char Directory[512];
    if (!pRoot || !*pRoot) return FALSE;
    if (!bMemCard && !_MainLoopSramEnsureOneDir(pRoot)) return FALSE;
    if (snprintf(Directory, sizeof(Directory), "%s/GB", pRoot) >= (int)sizeof(Directory)) return FALSE;
    if (_MainLoopSramEnsureOneDir(Directory)) return TRUE;
    if (bMemCard) {
        int r = MemCardCreateSave((char *)pRoot, _MainLoop_SaveTitle, TRUE);
        if (r < 0) return FALSE;
    }
    return _MainLoopSramEnsureOneDir(Directory);
}

static Bool _MainLoopGBBuildSavePath(Char *pPath, Int32 nPathBytes,
                                     const Char *pRoot, Bool bCopiedMcName)
{
    Char Directory[512], SaveName[256];
    Int32 nBaseMax;
    int n;
    if (!pPath || nPathBytes <= 0 || !pRoot || !*pRoot) return FALSE;
    n = snprintf(Directory, sizeof(Directory), "%s/GB", pRoot);
    if (n < 0 || n >= (int)sizeof(Directory)) return FALSE;
    nBaseMax = bCopiedMcName ? (32 - 4) : (PathGetMaxFileNameLength(Directory) - 4);
    if (nBaseMax <= 0) return FALSE;
    PathTruncFileName(SaveName, _RomName, nBaseMax);
    n = snprintf(pPath, (size_t)nPathBytes, "%s/%s.sav", Directory, SaveName);
    return n >= 0 && n < nPathBytes ? TRUE : FALSE;
}

static Bool _MainLoopGBReadVariable(const Char *pPath, Uint8 **ppData, Uint32 *pBytes)
{
    struct stat st;
    FILE *f;
    Uint8 *p;
    size_t got;
    if (!ppData || !pBytes) return FALSE;
    *ppData = NULL; *pBytes = 0;
    if (stat(pPath, &st) != 0 || st.st_size <= 0 ||
        (Uint64)st.st_size > MAINLOOP_GB_SAVEDATA_MAX) return FALSE;
    p = (Uint8 *)malloc((size_t)st.st_size);
    if (!p) return FALSE;
    f = fopen(pPath, "rb");
    if (!f) { free(p); return FALSE; }
    got = fread(p, 1, (size_t)st.st_size, f);
    fclose(f);
    if (got != (size_t)st.st_size) { free(p); return FALSE; }
    *ppData = p; *pBytes = (Uint32)st.st_size;
    return TRUE;
}

static Bool _MainLoopLoadGBSavedataFrom(MainLoopSramDeviceE eDevice,
                                        Uint8 **ppData, Uint32 *pBytes)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    Char Path[1024], Alias[1024];
    if (_MainLoopGBBuildSavePath(Path, sizeof(Path), pRoot, FALSE) &&
        _MainLoopGBReadVariable(Path, ppData, pBytes)) {
        ConPrint("GB savedata loaded: %s (%u bytes)\n", Path, (unsigned)*pBytes);
        return TRUE;
    }
    if (eDevice == MAINLOOP_SRAMDEVICE_USB &&
        _MainLoopGBBuildSavePath(Alias, sizeof(Alias), pRoot, TRUE) &&
        strcmp(Alias, Path) != 0 && _MainLoopGBReadVariable(Alias, ppData, pBytes)) {
        ConPrint("GB savedata loaded (MC-copy alias): %s (%u bytes)\n", Alias, (unsigned)*pBytes);
        return TRUE;
    }
    return FALSE;
}

Bool MainLoopLoadGBSavedata(Uint8 **ppData, Uint32 *pBytes)
{
    if (!ppData || !pBytes) return FALSE;
    *ppData = NULL; *pBytes = 0;
    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_USB)
        return _MainLoopSramUsbReady() ? _MainLoopLoadGBSavedataFrom(MAINLOOP_SRAMDEVICE_USB, ppData, pBytes) : FALSE;
    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
        return _MainLoopLoadGBSavedataFrom(MAINLOOP_SRAMDEVICE_MEMCARD, ppData, pBytes);
    if (_MainLoopSramUsbReady() && _MainLoopLoadGBSavedataFrom(MAINLOOP_SRAMDEVICE_USB, ppData, pBytes)) return TRUE;
    return _MainLoopLoadGBSavedataFrom(MAINLOOP_SRAMDEVICE_MEMCARD, ppData, pBytes);
}

Bool MainLoopSaveGBSavedata(const Uint8 *pData, Uint32 nBytes)
{
    MainLoopSramDeviceE eDevice = _MainLoop_SramDevice;
    const Char *pRoot;
    Bool bMemCard;
    Char Path[1024];
    if ((!pData && nBytes) || nBytes > MAINLOOP_GB_SAVEDATA_MAX) return FALSE;
    if (!nBytes) return TRUE;
    if (eDevice == MAINLOOP_SRAMDEVICE_AUTO)
        eDevice = _MainLoopSramUsbReady() ? MAINLOOP_SRAMDEVICE_USB : MAINLOOP_SRAMDEVICE_MEMCARD;
    if (eDevice == MAINLOOP_SRAMDEVICE_USB && !_MainLoopSramUsbReady()) return FALSE;
    pRoot = _MainLoopSramRoot(eDevice);
    bMemCard = eDevice == MAINLOOP_SRAMDEVICE_MEMCARD ? TRUE : FALSE;
    if (!_MainLoopGBEnsureDirectory(pRoot, bMemCard) ||
        !_MainLoopGBBuildSavePath(Path, sizeof(Path), pRoot, FALSE) ||
        !_MainLoopSramWriteFile(Path, (Uint8 *)pData, nBytes)) return FALSE;
    ConPrint("GB savedata saved: %s (%u bytes)\n", Path, (unsigned)nBytes);
    return TRUE;
}

void MainLoopFreeGBSavedata(Uint8 *pData) { free(pData); }

static Bool _MainLoopSaveGBSavedataToDevice(MainLoopSramDeviceE eDevice,
                                                const Uint8 *pData, Uint32 nBytes)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    Bool bMemCard = eDevice == MAINLOOP_SRAMDEVICE_MEMCARD ? TRUE : FALSE;
    Char Path[1024];
    if ((!pData && nBytes) || !nBytes || nBytes > MAINLOOP_GB_SAVEDATA_MAX) return FALSE;
    if (eDevice == MAINLOOP_SRAMDEVICE_USB && !_MainLoopSramUsbReady()) return FALSE;
    if (!_MainLoopGBEnsureDirectory(pRoot, bMemCard) ||
        !_MainLoopGBBuildSavePath(Path, sizeof(Path), pRoot, FALSE) ||
        !_MainLoopSramWriteFile(Path, (Uint8 *)pData, nBytes)) return FALSE;
    ConPrint("GB savedata saved: %s (%u bytes)\n", Path, (unsigned)nBytes);
    return TRUE;
}





/* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901
 *
 * Physical cartridge SRAM persistence is intentionally separate from the
 * classic copier's own 32 KiB battery B-RAM. The filename is derived from
 * the inserted cartridge exactly like an ordinary SNES .srm.
 */
static Char s_SwcCartSRAMName[512] = {0};
static Bool s_SwcCartSRAMMigrationPending = FALSE;

static Bool _MainLoopSwcCartSramBuildPath(
    Char *pPath, Int32 nPathBytes,
    const Char *pRoot, Bool bLegacyRoot,
    Bool bCopiedMcName)
{
    Char Directory[512];
    Char SaveName[512];
    const Char *pExtension;
    Int32 nSuffixBytes;
    Int32 nBaseMax;
    int n;

    if (!pPath || nPathBytes <= 0 || !pRoot ||
        !s_SwcCartSRAMName[0] || !_pSnes)
        return FALSE;

    pExtension =
        _pSnes->GetString(Emu::System::StringE::STRING_SRAMEXT);
    if (!pExtension || !*pExtension)
        return FALSE;

    if (bLegacyRoot)
        snprintf(Directory, sizeof(Directory), "%s", pRoot);
    else
        snprintf(Directory, sizeof(Directory), "%s/%s", pRoot,
                 _MainLoopSramGetSystemDirectoryName());

    nSuffixBytes = (Int32)strlen(pExtension) + 1;
    nBaseMax = bCopiedMcName
        ? (32 - nSuffixBytes)
        : (PathGetMaxFileNameLength(Directory) - nSuffixBytes);

    if (nBaseMax <= 0)
        return FALSE;

    PathTruncFileName(SaveName, s_SwcCartSRAMName, nBaseMax);
    n = snprintf(
        pPath, (size_t)nPathBytes,
        "%s/%s.%s", Directory, SaveName, pExtension);
    return n >= 0 && n < nPathBytes ? TRUE : FALSE;
}

static Bool _MainLoopLoadSwcCartSRAMFrom(
    MainLoopSramDeviceE eDevice, Bool *pbLegacy)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    Uint8 *pData;
    Int32 nBytes;
    Char Path[1024];
    Char Alias[1024];

    Path[0] = 0;
    Alias[0] = 0;
    if (pbLegacy)
        *pbLegacy = FALSE;

    if (!_pSnes || _pSystem != _pSnes ||
        !_pSnes->IsSuperWildCard() ||
        !_pSnes->HasSuperWildCardCartridgeBatterySRAM())
        return FALSE;

    nBytes = _pSnes->GetSuperWildCardCartridgeSRAMBytes();
    pData = _pSnes->GetSuperWildCardCartridgeSRAMData();
    if (!pData || nBytes <= 0)
        return FALSE;

    if (_MainLoopSwcCartSramBuildPath(
            Path, sizeof(Path), pRoot, FALSE, FALSE) &&
        _MainLoopSramReadFile(Path, pData, (Uint32)nBytes))
    {
        ConPrint("SWC cartridge SRAM loaded: %s\n", Path);
        return TRUE;
    }

    if (eDevice == MAINLOOP_SRAMDEVICE_USB &&
        _MainLoopSwcCartSramBuildPath(
            Alias, sizeof(Alias), pRoot, FALSE, TRUE) &&
        (!Path[0] || strcmp(Alias, Path) != 0) &&
        _MainLoopSramReadFile(Alias, pData, (Uint32)nBytes))
    {
        if (pbLegacy) *pbLegacy = TRUE;
        ConPrint("SWC cartridge SRAM loaded (MC-copy alias): %s\n", Alias);
        return TRUE;
    }

    Path[0] = 0;
    if (_MainLoopSwcCartSramBuildPath(
            Path, sizeof(Path), pRoot, TRUE, FALSE) &&
        _MainLoopSramReadFile(Path, pData, (Uint32)nBytes))
    {
        if (pbLegacy) *pbLegacy = TRUE;
        ConPrint("SWC cartridge SRAM loaded (legacy): %s\n", Path);
        return TRUE;
    }

    Alias[0] = 0;
    if (eDevice == MAINLOOP_SRAMDEVICE_USB &&
        _MainLoopSwcCartSramBuildPath(
            Alias, sizeof(Alias), pRoot, TRUE, TRUE) &&
        (!Path[0] || strcmp(Alias, Path) != 0) &&
        _MainLoopSramReadFile(Alias, pData, (Uint32)nBytes))
    {
        if (pbLegacy) *pbLegacy = TRUE;
        ConPrint(
            "SWC cartridge SRAM loaded (legacy MC-copy alias): %s\n",
            Alias);
        return TRUE;
    }

    return FALSE;
}

void _MainLoopSwcCartSRAMAttach(const Char *pCartPath)
{
    const Char *pName;
    Bool bLoaded = FALSE;
    Bool bLegacy = FALSE;
    Bool bMcFallback = FALSE;
    int n;

    s_SwcCartSRAMName[0] = 0;
    s_SwcCartSRAMMigrationPending = FALSE;

    if (!pCartPath || !*pCartPath || !_pSnes ||
        _pSystem != _pSnes || !_pSnes->IsSuperWildCard() ||
        !_pSnes->HasSuperWildCardCartridge())
        return;

    pName = pCartPath;
    for (const Char *p = pCartPath; *p; ++p)
        if (*p == '/' || *p == '\\')
            pName = p + 1;

    if (!*pName)
        return;

    n = snprintf(
        s_SwcCartSRAMName, sizeof(s_SwcCartSRAMName), "%s", pName);
    if (n < 0 || n >= (int)sizeof(s_SwcCartSRAMName))
    {
        s_SwcCartSRAMName[0] = 0;
        return;
    }

    /* AURORA_FINAL_V1_7_D88_DUAL_SRAM_PERSIST_20260901
     * Ordinary Aurora saves use the ROM stem. Keep the physical cart SRAM
     * namespace identical: "Game.sfc" -> "Game.srm". */
    {
        Char *pExt = strrchr(s_SwcCartSRAMName, '.');
        if (pExt && pExt != s_SwcCartSRAMName)
            *pExt = 0;
    }

    /* Volatile cartridge RAM is emulated by the core but never persisted. */
    if (!_pSnes->HasSuperWildCardCartridgeBatterySRAM())
        return;

    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_USB)
    {
        if (_MainLoopSramUsbReady())
            bLoaded = _MainLoopLoadSwcCartSRAMFrom(
                MAINLOOP_SRAMDEVICE_USB, &bLegacy);
    }
    else if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
    {
        bLoaded = _MainLoopLoadSwcCartSRAMFrom(
            MAINLOOP_SRAMDEVICE_MEMCARD, &bLegacy);
    }
    else
    {
        if (_MainLoopSramUsbReady())
            bLoaded = _MainLoopLoadSwcCartSRAMFrom(
                MAINLOOP_SRAMDEVICE_USB, &bLegacy);

        if (!bLoaded)
        {
            Bool bMcLegacy = FALSE;
            bLoaded = _MainLoopLoadSwcCartSRAMFrom(
                MAINLOOP_SRAMDEVICE_MEMCARD, &bMcLegacy);
            if (bLoaded)
            {
                bLegacy = bMcLegacy;
                bMcFallback = TRUE;
            }
        }
    }

    _pSnes->ClearSuperWildCardCartridgeSRAMDirty();

    /* Same copy/migration policy as ordinary Aurora SRAM: never delete source. */
    s_SwcCartSRAMMigrationPending =
        bLoaded &&
        (bLegacy || (bMcFallback && _MainLoopSramUsbReady()));

    if (s_SwcCartSRAMMigrationPending)
        _MainLoop_SRAMUpdated = TRUE;
}

void _MainLoopSwcCartSRAMDetach()
{
    s_SwcCartSRAMName[0] = 0;
    s_SwcCartSRAMMigrationPending = FALSE;
}

static Bool _MainLoopSaveSwcCartSRAMTo(
    MainLoopSramDeviceE eDevice)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    Bool bMemCard =
        eDevice == MAINLOOP_SRAMDEVICE_MEMCARD ? TRUE : FALSE;
    Uint8 *pData;
    Int32 nBytes;
    Char Path[1024];

    if (!_pSnes || _pSystem != _pSnes ||
        !_pSnes->IsSuperWildCard() ||
        !_pSnes->HasSuperWildCardCartridgeBatterySRAM())
        return TRUE;

    /* AURORA_FINAL_V1_7_D88_DUAL_SRAM_PERSIST_20260901
     * Menu/unload are explicit persistence boundaries. Do not use the dirty
     * bit to skip a battery-cart write once the boundary has been entered. */
    nBytes = _pSnes->GetSuperWildCardCartridgeSRAMBytes();
    pData = _pSnes->GetSuperWildCardCartridgeSRAMData();

    if (!pData || nBytes <= 0 || !s_SwcCartSRAMName[0] ||
        !_MainLoopSramEnsureSystemDirectory(pRoot, bMemCard) ||
        !_MainLoopSwcCartSramBuildPath(
            Path, sizeof(Path), pRoot, FALSE, FALSE))
        return FALSE;

    if (!_MainLoopSramWriteFile(Path, pData, (Uint32)nBytes))
        return FALSE;

    _pSnes->ClearSuperWildCardCartridgeSRAMDirty();
    s_SwcCartSRAMMigrationPending = FALSE;
    ConPrint("SWC cartridge SRAM saved: %s\n", Path);
    return TRUE;
}


/* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_STATE_CPP
 * One independent 1 MiB Type-1 Memory Pack image per slotted SNES ROM.
 * Example: Derby Stallion 96.srm (if battery-backed RAM exists) plus
 *          Derby Stallion 96.mpk (Satellaview Memory Pack flash). */
static void _MainLoopBSXMemoryPackBuildPath(Char *pPath, Int32 nPathBytes,
                                            const Char *pRoot)
{
    Char Directory[512];
    Char SaveName[256];
    Int32 nBaseMax;

    snprintf(Directory, sizeof(Directory), "%s/SNES", pRoot);
    nBaseMax = PathGetMaxFileNameLength(Directory) - 4; /* .mpk */
    if (nBaseMax < 1) nBaseMax = 1;
    PathTruncFileName(SaveName, _RomName, nBaseMax);
    snprintf(pPath, nPathBytes, "%s/%s.mpk", Directory, SaveName);
}

/* AURORA_BSXSLOT_MEMORY_PACK_V1_1_PERSIST_FIX_20260906
 * A Memory Pack is already backed by a live 1 MiB allocation. Avoid the
 * generic transactional SRAM reader's second 1 MiB temporary allocation.
 * Exact-size validation happens before the read; a short/error read restores
 * FF so AUTO can safely fall back to the other save device. */
static Bool _MainLoopReadBSXMemoryPackFile(const Char *pPath,
                                           Uint8 *pData, Uint32 nBytes)
{
    struct stat Status;
    FILE *pFile;
    Uint32 done = 0;

    if (!pPath || !*pPath || !pData ||
        nBytes != (Uint32)SNES_BSX_MEMORY_PACK_BYTES)
        return FALSE;

    if (stat(pPath, &Status) != 0 || S_ISDIR(Status.st_mode) ||
        (Uint32)Status.st_size != nBytes)
        return FALSE;

    pFile = fopen(pPath, "rb");
    if (!pFile)
        return FALSE;

    while (done < nBytes)
    {
        size_t got = fread(pData + done, 1, (size_t)(nBytes - done), pFile);
        if (!got)
            break;
        done += (Uint32)got;
    }

    if (fclose(pFile) != 0 || done != nBytes)
    {
        memset(pData, 0xFF, nBytes);
        return FALSE;
    }
    return TRUE;
}

static Bool _MainLoopLoadBSXMemoryPackFrom(MainLoopSramDeviceE eDevice)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    Uint8 *pData;
    Int32 nBytes;
    Char Path[1024];

    if (_pSystem != _pSnes || !_pSnes || !_pSnes->HasBSXMemoryPack())
        return FALSE;

    pData = _pSnes->GetBSXMemoryPackData();
    nBytes = _pSnes->GetBSXMemoryPackBytes();
    if (!pData || nBytes != SNES_BSX_MEMORY_PACK_BYTES)
        return FALSE;

    _MainLoopBSXMemoryPackBuildPath(Path, sizeof(Path), pRoot);
    if (!_MainLoopReadBSXMemoryPackFile(Path, pData, (Uint32)nBytes))
        return FALSE;

    if (!_pSnes->LoadBSXMemoryPack(pData, (Uint32)nBytes))
        return FALSE;

    ConPrint("BS-X Memory Pack loaded: %s\n", Path);
    return TRUE;
}

/* AURORA_BSX_MPK_LIFECYCLE_V1_20260907
 *
 * A slotted cartridge always has a physical 8M Memory Pack attached while
 * the game is running. Persistence therefore uses load-or-create semantics:
 *
 *   existing exact 1 MiB .mpk -> load it
 *   no .mpk                  -> immediately create erased 1 MiB backing
 *
 * Never overwrite an existing-but-unreadable/wrong-size file here. That is
 * treated as a storage problem so a damaged user image is not destroyed.
 *
 * When ordinary .srm selected a concrete device, the .mpk stays on that same
 * device. AUTO without ordinary SRAM may create on USB, then fall back to MC.
 */
/* AURORA_V6_RUNTIME_EFFECT_ALL5_20260908
 * Wrong-size .mpk files are a known legacy/failure state. Preserve the old
 * bytes under the first free .bad/.badN name, then allow creation of the exact
 * 1 MiB physical image. We do NOT quarantine an exact-size file merely because
 * an fopen/read failed: that may be a transient storage problem. */
static Bool _MainLoopQuarantineBadBSXMemoryPack(const Char *pPath)
{
    Char Backup[1024];
    struct stat Status;
    struct stat SourceStatus;
    unsigned i;

    if (!pPath || !*pPath || stat(pPath, &SourceStatus) != 0 ||
        S_ISDIR(SourceStatus.st_mode))
        return FALSE;

    for (i = 0; i < 100U; ++i)
    {
        int n = i == 0
            ? snprintf(Backup, sizeof(Backup), "%s.bad", pPath)
            : snprintf(Backup, sizeof(Backup), "%s.bad%u", pPath, i);
        if (n <= 0 || n >= (int)sizeof(Backup))
            return FALSE;

        if (stat(Backup, &Status) != 0)
        {
            /* Same-filesystem rename is the cheap path. Some PS2 storage
             * backends are stricter, so fall back to a small streaming copy
             * and remove the original only after an exact-size backup exists. */
            if (rename(pPath, Backup) != 0)
            {
                FILE *src = fopen(pPath, "rb");
                FILE *dst = src ? fopen(Backup, "wb") : NULL;
                Uint8 Buffer[4096];
                Bool ok = (src && dst) ? TRUE : FALSE;

                while (ok)
                {
                    size_t got = fread(Buffer, 1, sizeof(Buffer), src);
                    if (got && fwrite(Buffer, 1, got, dst) != got)
                    {
                        ok = FALSE;
                        break;
                    }
                    if (got < sizeof(Buffer))
                    {
                        if (ferror(src)) ok = FALSE;
                        break;
                    }
                }

                if (dst)
                {
                    if (fflush(dst) != 0 || fclose(dst) != 0) ok = FALSE;
                    dst = NULL;
                }
                if (src && fclose(src) != 0) ok = FALSE;

                if (!ok || stat(Backup, &Status) != 0 ||
                    S_ISDIR(Status.st_mode) ||
                    Status.st_size != SourceStatus.st_size ||
                    remove(pPath) != 0)
                {
                    remove(Backup);
                    return FALSE;
                }
            }

            ConPrint("BS-X invalid Memory Pack preserved as: %s\n", Backup);
            return TRUE;
        }
    }
    return FALSE;
}

static Bool _MainLoopCreateBSXMemoryPackOn(MainLoopSramDeviceE eDevice)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    const Bool bMemCard =
        eDevice == MAINLOOP_SRAMDEVICE_MEMCARD ? TRUE : FALSE;
    Uint8 *pData;
    Int32 nBytes;
    Char Path[1024];
    struct stat Status;

    if (_pSystem != _pSnes || !_pSnes || !_pSnes->HasBSXMemoryPack())
        return FALSE;

    if (eDevice == MAINLOOP_SRAMDEVICE_USB && !_MainLoopSramUsbReady())
        return FALSE;

    pData = _pSnes->GetBSXMemoryPackData();
    nBytes = _pSnes->GetBSXMemoryPackBytes();
    if (!pData || nBytes != SNES_BSX_MEMORY_PACK_BYTES)
        return FALSE;

    if (!_MainLoopSramEnsureSystemDirectory(pRoot, bMemCard))
        return FALSE;

    _MainLoopBSXMemoryPackBuildPath(Path, sizeof(Path), pRoot);

    /* AURORA_V6_RUNTIME_EFFECT_ALL5_20260908
     * Exact-size-but-unreadable targets remain untouched (possible transient
     * I/O). A wrong-size legacy image is unambiguously not a physical 8M pack:
     * preserve it under .bad/.badN and replace the active name. */
    if (stat(Path, &Status) == 0)
    {
        if (S_ISDIR(Status.st_mode) ||
            (Uint32)Status.st_size == (Uint32)nBytes)
        {
            ConPrint("WARNING: BS-X Memory Pack exists but is not loadable: %s\n",
                     Path);
            return FALSE;
        }

        ConPrint("WARNING: BS-X Memory Pack has wrong size (%ld, expected %d): %s\n",
                 (long)Status.st_size, (int)nBytes, Path);
        if (!_MainLoopQuarantineBadBSXMemoryPack(Path))
        {
            ConPrint("BS-X Memory Pack quarantine FAILED: %s\n", Path);
            return FALSE;
        }
    }

    if (!_MainLoopSramWriteFile(Path, pData, (Uint32)nBytes))
    {
        ConPrint("BS-X Memory Pack initial create FAILED: %s\n", Path);
        return FALSE;
    }

    /* Verify at least the exact physical file size without allocating
     * another 1 MiB temporary buffer. */
    if (stat(Path, &Status) != 0 || S_ISDIR(Status.st_mode) ||
        (Uint32)Status.st_size != (Uint32)nBytes)
    {
        ConPrint("BS-X Memory Pack initial create size verify FAILED: %s\n",
                 Path);
        return FALSE;
    }

    ConPrint("BS-X Memory Pack created: %s (%u bytes)\n",
             Path, (unsigned)nBytes);
    return TRUE;
}

/* AURORA_BSXSLOT_MEMORY_PACK_V1_3_COHERENT_BUNDLE_VERIFY_20260906
 * Keep ordinary cartridge SRAM and the slotted Memory Pack on the same save
 * device whenever an SRAM backing was actually selected.  This prevents AUTO
 * from pairing an SRAM file from MC with a stale .mpk from USB (or vice versa).
 * If there is no ordinary SRAM backing, AUTO retains the normal USB -> MC
 * search so Memory-Pack-only carts still work as before. */
static void _MainLoopLoadBSXMemoryPack(MainLoopSramDeviceE ePreferredDevice)
{
    Bool bLoaded = FALSE;
    Bool bCreated = FALSE;
    MainLoopSramDeviceE eBackingDevice = MAINLOOP_SRAMDEVICE_AUTO;

    if (_pSystem != _pSnes || !_pSnes)
        return;

    /* AURORA_V6_RUNTIME_EFFECT_ALL5_20260908
     * Re-assert the physical pack/base hardware at the persistence boundary.
     * This turns a failed first allocation into a retry instead of silently
     * skipping the entire .mpk lifecycle. */
    if (!_pSnes->EnsureBSXMemoryPack())
    {
        ConPrint("WARNING: BS-X Memory Pack is expected but could not be attached\n");
        return;
    }

    /* First choice: keep .mpk coherent with an ordinary .srm backing if
     * _MainLoopLoadSRAM() already resolved one concrete device. */
    if (ePreferredDevice == MAINLOOP_SRAMDEVICE_USB)
    {
        if (_MainLoopSramUsbReady())
        {
            bLoaded = _MainLoopLoadBSXMemoryPackFrom(
                MAINLOOP_SRAMDEVICE_USB);
            if (bLoaded)
                eBackingDevice = MAINLOOP_SRAMDEVICE_USB;
            else
            {
                bCreated = _MainLoopCreateBSXMemoryPackOn(
                    MAINLOOP_SRAMDEVICE_USB);
                if (bCreated)
                    eBackingDevice = MAINLOOP_SRAMDEVICE_USB;
            }
        }
    }
    else if (ePreferredDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
    {
        bLoaded = _MainLoopLoadBSXMemoryPackFrom(
            MAINLOOP_SRAMDEVICE_MEMCARD);
        if (bLoaded)
            eBackingDevice = MAINLOOP_SRAMDEVICE_MEMCARD;
        else
        {
            bCreated = _MainLoopCreateBSXMemoryPackOn(
                MAINLOOP_SRAMDEVICE_MEMCARD);
            if (bCreated)
                eBackingDevice = MAINLOOP_SRAMDEVICE_MEMCARD;
        }
    }
    else if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_USB)
    {
        if (_MainLoopSramUsbReady())
        {
            bLoaded = _MainLoopLoadBSXMemoryPackFrom(
                MAINLOOP_SRAMDEVICE_USB);
            if (bLoaded)
                eBackingDevice = MAINLOOP_SRAMDEVICE_USB;
            else
            {
                bCreated = _MainLoopCreateBSXMemoryPackOn(
                    MAINLOOP_SRAMDEVICE_USB);
                if (bCreated)
                    eBackingDevice = MAINLOOP_SRAMDEVICE_USB;
            }
        }
    }
    else if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
    {
        bLoaded = _MainLoopLoadBSXMemoryPackFrom(
            MAINLOOP_SRAMDEVICE_MEMCARD);
        if (bLoaded)
            eBackingDevice = MAINLOOP_SRAMDEVICE_MEMCARD;
        else
        {
            bCreated = _MainLoopCreateBSXMemoryPackOn(
                MAINLOOP_SRAMDEVICE_MEMCARD);
            if (bCreated)
                eBackingDevice = MAINLOOP_SRAMDEVICE_MEMCARD;
        }
    }
    else
    {
        /* AUTO with no ordinary SRAM backing:
         * load USB -> load MC -> create USB -> create MC.
         *
         * Loading both locations before creating matters: an existing MC
         * pack must beat creation of a new blank USB pack. */
        if (_MainLoopSramUsbReady())
        {
            bLoaded = _MainLoopLoadBSXMemoryPackFrom(
                MAINLOOP_SRAMDEVICE_USB);
            if (bLoaded)
                eBackingDevice = MAINLOOP_SRAMDEVICE_USB;
        }

        if (!bLoaded)
        {
            bLoaded = _MainLoopLoadBSXMemoryPackFrom(
                MAINLOOP_SRAMDEVICE_MEMCARD);
            if (bLoaded)
                eBackingDevice = MAINLOOP_SRAMDEVICE_MEMCARD;
        }

        if (!bLoaded && _MainLoopSramUsbReady())
        {
            bCreated = _MainLoopCreateBSXMemoryPackOn(
                MAINLOOP_SRAMDEVICE_USB);
            if (bCreated)
                eBackingDevice = MAINLOOP_SRAMDEVICE_USB;
        }

        if (!bLoaded && !bCreated)
        {
            bCreated = _MainLoopCreateBSXMemoryPackOn(
                MAINLOOP_SRAMDEVICE_MEMCARD);
            if (bCreated)
                eBackingDevice = MAINLOOP_SRAMDEVICE_MEMCARD;
        }
    }

    /* Loading/initial creation establishes the persistent baseline. Runtime
     * ProgramFlashByte()/erase operations will mark dirty again naturally. */
    _pSnes->ClearBSXMemoryPackDirty();

    ConPrint("BS-X Memory Pack backing: %s (device=%s)\n",
             bLoaded ? "loaded" :
             bCreated ? "created" : "blank in RAM / backing FAILED",
             eBackingDevice == MAINLOOP_SRAMDEVICE_USB ? "USB" :
             eBackingDevice == MAINLOOP_SRAMDEVICE_MEMCARD ? "MC" : "NONE");
}

static Bool _MainLoopVerifyBSXMemoryPackFile(const Char *pPath,
                                               const Uint8 *pData,
                                               Uint32 nBytes)
{
    struct stat Status;
    FILE *pFile;
    Uint8 Verify[4096];
    Uint32 done = 0;
    Bool bOK = TRUE;

    if (!pPath || !*pPath || !pData ||
        nBytes != (Uint32)SNES_BSX_MEMORY_PACK_BYTES)
        return FALSE;

    if (stat(pPath, &Status) != 0 || S_ISDIR(Status.st_mode) ||
        (Uint32)Status.st_size != nBytes)
        return FALSE;

    pFile = fopen(pPath, "rb");
    if (!pFile)
        return FALSE;

    while (done < nBytes)
    {
        Uint32 chunk = nBytes - done;
        size_t got;
        if (chunk > (Uint32)sizeof(Verify))
            chunk = (Uint32)sizeof(Verify);
        got = fread(Verify, 1, (size_t)chunk, pFile);
        if (got != (size_t)chunk ||
            memcmp(Verify, pData + done, (size_t)chunk) != 0)
        {
            bOK = FALSE;
            break;
        }
        done += chunk;
    }

    if (fclose(pFile) != 0)
        bOK = FALSE;

    return bOK && done == nBytes;
}

static Bool _MainLoopSaveBSXMemoryPackTo(MainLoopSramDeviceE eDevice)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    const Bool bMemCard =
        eDevice == MAINLOOP_SRAMDEVICE_MEMCARD ? TRUE : FALSE;
    Uint8 *pData;
    Int32 nBytes;
    Char Path[1024];

    if (_pSystem != _pSnes || !_pSnes ||
        !_pSnes->HasBSXMemoryPack() || !_pSnes->IsBSXMemoryPackDirty())
        return TRUE;

    pData = _pSnes->GetBSXMemoryPackData();
    nBytes = _pSnes->GetBSXMemoryPackBytes();
    if (!pData || nBytes != SNES_BSX_MEMORY_PACK_BYTES ||
        !_MainLoopSramEnsureSystemDirectory(pRoot, bMemCard))
        return FALSE;

    _MainLoopBSXMemoryPackBuildPath(Path, sizeof(Path), pRoot);
    if (!_MainLoopSramWriteFile(Path, pData, (Uint32)nBytes))
    {
        ConPrint("BS-X Memory Pack write FAILED: %s\n", Path);
        return FALSE;
    }

    /* V1.3: verify storage contents without allocating another 1 MiB buffer.
     * A 4 KiB streaming compare runs only at an explicit save boundary. */
    if (!_MainLoopVerifyBSXMemoryPackFile(Path, pData, (Uint32)nBytes))
    {
        ConPrint("BS-X Memory Pack verify FAILED: %s\n", Path);
        return FALSE;
    }

    /* Do NOT clear dirty here. AUTO may still fail on another member of the
     * save bundle and fall back USB -> MC. The outer transaction clears the
     * pack only after the entire device write succeeds. */
    ConPrint("BS-X Memory Pack saved+verified: %s\n", Path);
    ConPrint("[BSX/MPK] io save: R=%u W=%u prog=%u erase=%u chip=%u status=%u vendor=%u\n",
             (unsigned)_pSnes->GetBSXMemoryPackReadCount(),
             (unsigned)_pSnes->GetBSXMemoryPackWriteCount(),
             (unsigned)_pSnes->GetBSXMemoryPackProgramCount(),
             (unsigned)_pSnes->GetBSXMemoryPackBlockEraseCount(),
             (unsigned)_pSnes->GetBSXMemoryPackChipEraseCount(),
             (unsigned)_pSnes->GetBSXMemoryPackStatusReadCount(),
             (unsigned)_pSnes->GetBSXMemoryPackVendorReadCount());
    return TRUE;
}

/* AURORA_QN_TURBOFILE_SAVE_V2_20260828
 * The original ASCII Turbo File is one 8 KiB expansion-port memory unit,
 * shared by compatible Famicom software. Keep one physical-style file in
 * the NES save directory rather than pretending it is cartridge SRAM.
 */
static void _MainLoopTurboFileBuildPath(Char *pPath, Int32 nPathBytes,
                                        const Char *pRoot)
{
    snprintf(pPath, nPathBytes, "%s/NES/TurboFile.sav", pRoot);
}

static Bool _MainLoopLoadTurboFileFrom(MainLoopSramDeviceE eDevice)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    Uint8 *pData = QuicknesBridge_GetTurboFileData();
    const Int32 nBytes = QuicknesBridge_GetTurboFileBytes();
    Char Path[1024];

    if (!pData || nBytes != 0x2000)
        return FALSE;

    _MainLoopTurboFileBuildPath(Path, sizeof(Path), pRoot);
    if (!_MainLoopSramReadFile(Path, pData, (Uint32)nBytes))
        return FALSE;

    QuicknesBridge_ClearTurboFileDirty();
    ConPrint("Turbo File loaded: %s\n", Path);
    return TRUE;
}

static void _MainLoopLoadTurboFile(void)
{
    /* AURORA_CD_AUDIO_STREAM_V3_NES_SAVE_GATE_20260829 */
    if (_pSystem != _pNes ||
        !QuicknesBridge_TurboFileEnabled() ||
        QuicknesBridge_TurboFileDirty())
        return;

    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_USB)
    {
        if (_MainLoopSramUsbReady())
            (void)_MainLoopLoadTurboFileFrom(MAINLOOP_SRAMDEVICE_USB);
        return;
    }

    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
    {
        (void)_MainLoopLoadTurboFileFrom(MAINLOOP_SRAMDEVICE_MEMCARD);
        return;
    }

    if (_MainLoopSramUsbReady() &&
        _MainLoopLoadTurboFileFrom(MAINLOOP_SRAMDEVICE_USB))
        return;

    (void)_MainLoopLoadTurboFileFrom(MAINLOOP_SRAMDEVICE_MEMCARD);
}

static Bool _MainLoopSaveTurboFileTo(MainLoopSramDeviceE eDevice)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    const Bool bMemCard =
        eDevice == MAINLOOP_SRAMDEVICE_MEMCARD ? TRUE : FALSE;
    Uint8 *pData;
    Int32 nBytes;
    Char Path[1024];

    if (_pSystem != _pNes ||
        !QuicknesBridge_TurboFileEnabled() ||
        !QuicknesBridge_TurboFileDirty())
        return TRUE;

    nBytes = QuicknesBridge_GetTurboFileBytes();
    pData = QuicknesBridge_GetTurboFileData();
    if (!pData || nBytes != 0x2000 ||
        !_MainLoopSramEnsureSystemDirectory(pRoot, bMemCard))
        return FALSE;

    _MainLoopTurboFileBuildPath(Path, sizeof(Path), pRoot);
    if (!_MainLoopSramWriteFile(Path, pData, (Uint32)nBytes))
        return FALSE;

    QuicknesBridge_ClearTurboFileDirty();
    ConPrint("Turbo File saved: %s\n", Path);
    return TRUE;
}

/* AURORA_SNES_TURBOFILE_V4_20260829
 * One physical Twin image, separate from cartridge SRAM:
 *   SNES/TurboFileTwin.sav = 160 KiB
 * Layout matches the emulated hardware backing:
 *   first 32 KiB = four TFII banks, next 128 KiB = STF.
 */
static void _MainLoopSnesTurboFileBuildPath(
    Char *pPath, Int32 nPathBytes, const Char *pRoot)
{
    snprintf(pPath, nPathBytes, "%s/SNES/TurboFileTwin.sav", pRoot);
}

static Bool _MainLoopLoadSnesTurboFileFrom(MainLoopSramDeviceE eDevice)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    Uint8 *pData = SnesTurboFileGetData();
    const Int32 nBytes = SnesTurboFileGetBytes();
    Char Path[1024];

    if (!pData || nBytes != 160 * 1024)
        return FALSE;

    _MainLoopSnesTurboFileBuildPath(Path, sizeof(Path), pRoot);
    if (!_MainLoopSramReadFile(Path, pData, (Uint32)nBytes))
        return FALSE;

    SnesTurboFileClearDirty();
    ConPrint("SNES Turbo File Twin loaded: %s\n", Path);
    return TRUE;
}

static void _MainLoopLoadSnesTurboFile(void)
{
    if (_pSystem != _pSnes ||
        !SnesTurboFileEnabled() ||
        SnesTurboFileDirty())
        return;

    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_USB)
    {
        if (_MainLoopSramUsbReady())
            (void)_MainLoopLoadSnesTurboFileFrom(MAINLOOP_SRAMDEVICE_USB);
        return;
    }

    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
    {
        (void)_MainLoopLoadSnesTurboFileFrom(MAINLOOP_SRAMDEVICE_MEMCARD);
        return;
    }

    if (_MainLoopSramUsbReady() &&
        _MainLoopLoadSnesTurboFileFrom(MAINLOOP_SRAMDEVICE_USB))
        return;

    (void)_MainLoopLoadSnesTurboFileFrom(MAINLOOP_SRAMDEVICE_MEMCARD);
}

static Bool _MainLoopSaveSnesTurboFileTo(MainLoopSramDeviceE eDevice)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    const Bool bMemCard =
        eDevice == MAINLOOP_SRAMDEVICE_MEMCARD ? TRUE : FALSE;
    Uint8 *pData;
    Int32 nBytes;
    Char Path[1024];

    if (_pSystem != _pSnes ||
        !SnesTurboFileEnabled() ||
        !SnesTurboFileDirty())
        return TRUE;

    nBytes = SnesTurboFileGetBytes();
    pData = SnesTurboFileGetData();
    if (!pData || nBytes != 160 * 1024 ||
        !_MainLoopSramEnsureSystemDirectory(pRoot, bMemCard))
        return FALSE;

    _MainLoopSnesTurboFileBuildPath(Path, sizeof(Path), pRoot);
    if (!_MainLoopSramWriteFile(Path, pData, (Uint32)nBytes))
        return FALSE;

    SnesTurboFileClearDirty();
    ConPrint("SNES Turbo File Twin saved: %s\n", Path);
    return TRUE;
}

/* AURORA_QN_BATTLEBOX_V5_20260829
 * One physical 512-byte Battle Box shared by compatible Famicom games.
 */
static void _MainLoopBattleBoxBuildPath(
    Char *pPath, Int32 nPathBytes, const Char *pRoot)
{
    snprintf(pPath, nPathBytes, "%s/NES/BattleBox.sav", pRoot);
}

static Bool _MainLoopLoadBattleBoxFrom(MainLoopSramDeviceE eDevice)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    Uint8 *pData = QuicknesBridge_GetBattleBoxData();
    const Int32 nBytes = QuicknesBridge_GetBattleBoxBytes();
    Char Path[1024];

    if (!pData || nBytes != 0x0200)
        return FALSE;

    _MainLoopBattleBoxBuildPath(Path, sizeof(Path), pRoot);
    if (!_MainLoopSramReadFile(Path, pData, (Uint32)nBytes))
        return FALSE;

    QuicknesBridge_ClearBattleBoxDirty();
    ConPrint("Battle Box loaded: %s\n", Path);
    return TRUE;
}

static void _MainLoopLoadBattleBox(void)
{
    if (_pSystem != _pNes ||
        !QuicknesBridge_BattleBoxEnabled() ||
        QuicknesBridge_BattleBoxDirty())
        return;

    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_USB)
    {
        if (_MainLoopSramUsbReady())
            (void)_MainLoopLoadBattleBoxFrom(MAINLOOP_SRAMDEVICE_USB);
        return;
    }

    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
    {
        (void)_MainLoopLoadBattleBoxFrom(MAINLOOP_SRAMDEVICE_MEMCARD);
        return;
    }

    if (_MainLoopSramUsbReady() &&
        _MainLoopLoadBattleBoxFrom(MAINLOOP_SRAMDEVICE_USB))
        return;

    (void)_MainLoopLoadBattleBoxFrom(MAINLOOP_SRAMDEVICE_MEMCARD);
}

static Bool _MainLoopSaveBattleBoxTo(MainLoopSramDeviceE eDevice)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    const Bool bMemCard =
        eDevice == MAINLOOP_SRAMDEVICE_MEMCARD ? TRUE : FALSE;
    Uint8 *pData;
    Int32 nBytes;
    Char Path[1024];

    if (_pSystem != _pNes ||
        !QuicknesBridge_BattleBoxEnabled() ||
        !QuicknesBridge_BattleBoxDirty())
        return TRUE;

    nBytes = QuicknesBridge_GetBattleBoxBytes();
    pData = QuicknesBridge_GetBattleBoxData();
    if (!pData || nBytes != 0x0200 ||
        !_MainLoopSramEnsureSystemDirectory(pRoot, bMemCard))
        return FALSE;

    _MainLoopBattleBoxBuildPath(Path, sizeof(Path), pRoot);
    if (!_MainLoopSramWriteFile(Path, pData, (Uint32)nBytes))
        return FALSE;

    QuicknesBridge_ClearBattleBoxDirty();
    ConPrint("Battle Box saved: %s\n", Path);
    return TRUE;
}

static Uint32 _CalcChecksum(Uint32 *pData, Uint32 nWords)
{
    Uint32 uSum = 0;

    while (nWords > 0)
    {
        uSum += pData[0];
        pData++;
        nWords--;
    }

    return uSum;
}

Bool _MainLoopHasSRAM()
{
    if (!_pSystem)
        return FALSE;
    if (_pSystem == _pSnes && _pSnes && _pSnes->IsSuperGameBoy())
        return _pSnes->GetSuperGameBoySavedataBytes() > 0 ? TRUE : FALSE;
    if (_pSystem->GetSRAMBytes() > 0)
        return TRUE;
    if (_pSystem == _pSnes && _pSnes && _pSnes->HasBSXMemoryPack())
        return TRUE; /* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_STATE_CPP */
    /* AURORA_QN_TURBOFILE_SAVE_V2_20260828: only advertise external
     * persistence after the Turbo File has actually been written. */
    if (_pSystem == _pNes &&
        QuicknesBridge_TurboFileEnabled() &&
        QuicknesBridge_TurboFileDirty())
        return TRUE;
    if (_pSystem == _pNes &&
        QuicknesBridge_BattleBoxEnabled() &&
        QuicknesBridge_BattleBoxDirty())
        return TRUE;
    if (_pSystem == _pSnes &&
        SnesTurboFileEnabled() &&
        SnesTurboFileDirty())
        return TRUE;
    return FALSE;
}

static Bool _MainLoopSaveSRAMTo(MainLoopSramDeviceE eDevice, Bool bSync)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    Bool bMemCard = eDevice == MAINLOOP_SRAMDEVICE_MEMCARD ? TRUE : FALSE;
    Int32 nSramBytes = _pSystem ? _pSystem->GetSRAMBytes() : 0;
    Char Path[1024];
    Uint8 *pSRAM = NULL;
    Bool bAny = FALSE;
    Bool bOK = TRUE;

    /* AURORA_RUNTIME_LEAN_V1_MCSAVE_20260824: bSync only selected behavior in the retired async path. */
    (void)bSync;

    if (_pSystem == _pSnes && _pSnes && _pSnes->IsSuperGameBoy())
    {
        Uint32 nBytes = _pSnes->GetSuperGameBoySavedataBytes();
        Uint32 actual = 0;
        Uint8 *pData;
        if (!nBytes) return FALSE;
        pData = (Uint8 *)malloc(nBytes);
        if (!pData) return FALSE;
        bOK = _pSnes->ExportSuperGameBoySavedata(pData, nBytes, &actual) &&
              actual == nBytes &&
              _MainLoopSaveGBSavedataToDevice(eDevice, pData, nBytes);
        free(pData);
        if (bOK)
        {
            _pSnes->ClearSuperGameBoySavedataDirty();
            _MainLoop_SRAMUpdated = FALSE;
        }
        return bOK;
    }

    if (nSramBytes > 0)
    {
        bAny = TRUE;
        pSRAM = _pSystem->GetSRAMData();
        if (!pSRAM || !_MainLoopSramEnsureSystemDirectory(pRoot, bMemCard))
            bOK = FALSE;
        else
        {
            _MainLoopSramBuildPath(Path, sizeof(Path), pRoot, FALSE);

            if (_pSystem == _pSnes && g_FakeSRAMSize &&
                !_pSnes->IsSuperWildCard()) /* AURORA_SWC_FLOPPY_V1_20260831 */
            {
                struct stat Status;
                if (stat(Path, &Status) == 0 &&
                    (Uint32)Status.st_size != (Uint32)nSramBytes)
                {
                    printf("[SRAM] Force SRAM size mismatch: file=%ld expected=%d\n",
                           (long)Status.st_size, (int)nSramBytes);
                    memset(pSRAM, 0, nSramBytes);
                }
            }

            ML_TRACE("SRAM save path: %s", Path);
            if (!_MainLoopSramWriteFile(Path, pSRAM, (Uint32)nSramBytes))
                bOK = FALSE;
        }
    }

    /* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901: save physical cartridge SRAM independently. */
    if (_pSystem == _pSnes && _pSnes &&
        _pSnes->IsSuperWildCard() &&
        _pSnes->HasSuperWildCardCartridgeBatterySRAM())
    {
        bAny = TRUE;
        if (!_MainLoopSaveSwcCartSRAMTo(eDevice))
            bOK = FALSE;
    }

    /* AURORA_QN_TURBOFILE_SAVE_V2_20260828 */
    if (_pSystem == _pNes &&
        QuicknesBridge_TurboFileEnabled() &&
        QuicknesBridge_TurboFileDirty())
    {
        bAny = TRUE;
        if (!_MainLoopSaveTurboFileTo(eDevice))
            bOK = FALSE;
    }

    if (_pSystem == _pNes &&
        QuicknesBridge_BattleBoxEnabled() &&
        QuicknesBridge_BattleBoxDirty())
    {
        bAny = TRUE;
        if (!_MainLoopSaveBattleBoxTo(eDevice))
            bOK = FALSE;
    }

    if (_pSystem == _pSnes &&
        SnesTurboFileEnabled() &&
        SnesTurboFileDirty())
    {
        bAny = TRUE;
        if (!_MainLoopSaveSnesTurboFileTo(eDevice))
            bOK = FALSE;
    }

    if (_pSystem == _pSnes && _pSnes &&
        _pSnes->HasBSXMemoryPack() && _pSnes->IsBSXMemoryPackDirty())
    {
        bAny = TRUE;
        if (!_MainLoopSaveBSXMemoryPackTo(eDevice))
            bOK = FALSE;
    }

    if (bAny && bOK)
    {
        /* MPK V1.3: commit dirty state only after the whole save bundle for
         * this device succeeded. This keeps AUTO fallback transactional. */
        if (_pSystem == _pSnes && _pSnes &&
            _pSnes->HasBSXMemoryPack() && _pSnes->IsBSXMemoryPackDirty())
            _pSnes->ClearBSXMemoryPackDirty();

        _MainLoop_SRAMUpdated = FALSE;
        return TRUE;
    }
    return FALSE;
}

Bool _MainLoopSaveSRAM(Bool bSync)
{
    if (!_MainLoopHasSRAM())
        return FALSE;

    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_USB)
        return _MainLoopSramUsbReady()
            ? _MainLoopSaveSRAMTo(MAINLOOP_SRAMDEVICE_USB, bSync) : FALSE;

    if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
        return _MainLoopSaveSRAMTo(MAINLOOP_SRAMDEVICE_MEMCARD, bSync);

    if (_MainLoopSramUsbReady() &&
        _MainLoopSaveSRAMTo(MAINLOOP_SRAMDEVICE_USB, bSync))
        return TRUE;

    return _MainLoopSaveSRAMTo(MAINLOOP_SRAMDEVICE_MEMCARD, bSync);
}

static Bool _MainLoopLoadSRAMFrom(MainLoopSramDeviceE eDevice,
                                  Uint8 *pSRAM, Int32 nSramBytes,
                                  Bool *pbLegacy)
{
    const Char *pRoot = _MainLoopSramRoot(eDevice);
    Char Path[1024];
    Char McCopyPath[1024];
    *pbLegacy = FALSE;

    _MainLoopSramBuildPath(Path, sizeof(Path), pRoot, FALSE);
    if (_MainLoopSramReadFile(Path, pSRAM, (Uint32)nSramBytes))
    {
        ConPrint("SRAM loaded: %s\n", Path);
        return TRUE;
    }

    /* AURORA_SRAM_MC_COPY_ALIAS_V1
     * A literal copy from mc0:/mc1: to USB preserves the Memory Card's
     * shortened filename. Search that exact spelling as a USB alias. */
    if (eDevice == MAINLOOP_SRAMDEVICE_USB)
    {
        _MainLoopSramBuildCopiedMcPath(
            McCopyPath, sizeof(McCopyPath), pRoot, FALSE);

        if (strcmp(McCopyPath, Path) != 0 &&
            _MainLoopSramReadFile(
                McCopyPath, pSRAM, (Uint32)nSramBytes))
        {
            *pbLegacy = TRUE;
            ConPrint("SRAM loaded (MC-copy alias): %s\n", McCopyPath);
            return TRUE;
        }
    }

    /* AURORA_CD_SRAM_NOTICES_20260824: both SNES cores also import the original root-level save. */
    if (_pSystem == _pSnes)
    {
        _MainLoopSramBuildPath(Path, sizeof(Path), pRoot, TRUE);
        if (_MainLoopSramReadFile(Path, pSRAM, (Uint32)nSramBytes))
        {
            *pbLegacy = TRUE;
            ConPrint("SRAM loaded (legacy): %s\n", Path);
            return TRUE;
        }

        if (eDevice == MAINLOOP_SRAMDEVICE_USB)
        {
            _MainLoopSramBuildCopiedMcPath(
                McCopyPath, sizeof(McCopyPath), pRoot, TRUE);

            if (strcmp(McCopyPath, Path) != 0 &&
                _MainLoopSramReadFile(
                    McCopyPath, pSRAM, (Uint32)nSramBytes))
            {
                *pbLegacy = TRUE;
                ConPrint(
                    "SRAM loaded (legacy MC-copy alias): %s\n",
                    McCopyPath
                );
                return TRUE;
            }
        }
    }

    return FALSE;
}

void _MainLoopLoadSRAM()
{
    if (_pSystem == _pSnes && _pSnes && _pSnes->IsSuperGameBoy())
    {
        Uint8 *pData = NULL;
        Uint32 nBytes = 0;
        Bool loaded = MainLoopLoadGBSavedata(&pData, &nBytes);
        Bool ok = _pSnes->LoadSuperGameBoySavedata(loaded ? pData : NULL,
                                                    loaded ? nBytes : 0);
        if (pData) MainLoopFreeGBSavedata(pData);
        if (!ok) ConPrint("WARNING: could not attach SGB savedata backing\n");
        _pSnes->ClearSuperGameBoySavedataDirty();
        _MainLoop_SRAMUpdated = FALSE;
        _MainLoop_SaveCounter = 0;
        _bStateSaved = FALSE;
        return;
    }
    Int32 nSramBytes = _pSystem ? _pSystem->GetSRAMBytes() : 0;
    Uint8 *pSRAM = nSramBytes > 0 ? _pSystem->GetSRAMData() : NULL;
    Bool bLoaded = FALSE;
    Bool bLegacy = FALSE;
    Bool bMcFallback = FALSE;
    MainLoopSramDeviceE eLoadedDevice = MAINLOOP_SRAMDEVICE_AUTO;

    if (pSRAM && nSramBytes > 0)
    {
        if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_USB)
        {
            if (_MainLoopSramUsbReady())
            {
                bLoaded = _MainLoopLoadSRAMFrom(
                    MAINLOOP_SRAMDEVICE_USB, pSRAM, nSramBytes, &bLegacy);
                if (bLoaded) eLoadedDevice = MAINLOOP_SRAMDEVICE_USB;
            }
        }
        else if (_MainLoop_SramDevice == MAINLOOP_SRAMDEVICE_MEMCARD)
        {
            bLoaded = _MainLoopLoadSRAMFrom(
                MAINLOOP_SRAMDEVICE_MEMCARD, pSRAM, nSramBytes, &bLegacy);
            if (bLoaded) eLoadedDevice = MAINLOOP_SRAMDEVICE_MEMCARD;
        }
        else
        {
            if (_MainLoopSramUsbReady())
            {
                bLoaded = _MainLoopLoadSRAMFrom(
                    MAINLOOP_SRAMDEVICE_USB, pSRAM, nSramBytes, &bLegacy);
                if (bLoaded) eLoadedDevice = MAINLOOP_SRAMDEVICE_USB;
            }

            if (!bLoaded)
            {
                Bool bMcLegacy = FALSE;
                bLoaded = _MainLoopLoadSRAMFrom(
                    MAINLOOP_SRAMDEVICE_MEMCARD, pSRAM, nSramBytes, &bMcLegacy);
                if (bLoaded)
                {
                    bLegacy = bMcLegacy;
                    bMcFallback = TRUE;
                    eLoadedDevice = MAINLOOP_SRAMDEVICE_MEMCARD;
                }
            }
        }

        _MainLoop_SRAMChecksum =
            _CalcChecksum((Uint32 *)pSRAM, nSramBytes / 4);

        /* Never delete the source. Mark only for copy/migration. */
        _MainLoop_SRAMUpdated = bLoaded &&
            (bLegacy || (bMcFallback && _MainLoopSramUsbReady()));
    }

    /* AURORA_QN_TURBOFILE_SAVE_V2_20260828: loading an existing
     * TurboFile.sav never creates one and never marks it dirty. */
    if (_pSystem == _pNes)
    {
        _MainLoopLoadTurboFile();
        _MainLoopLoadBattleBox();
    }

    if (_pSystem == _pSnes)
    {
        _MainLoopLoadSnesTurboFile();
        _MainLoopLoadBSXMemoryPack(
            bLoaded ? eLoadedDevice : MAINLOOP_SRAMDEVICE_AUTO);
    }

    _MainLoop_SaveCounter = 0;
    _bStateSaved = FALSE;
}

/* Force-update the SRAM dirty flag (_MainLoop_SRAMUpdated) right now,
   ignoring the throttle in _MainLoopCheckSRAM. Used by _MenuEnable
   before it decides whether to fire the synchronous save: if the
   user wrote to SRAM in the last <CHECK_INTERVAL frames and pressed
   L2+R2 before _MainLoopCheckSRAM ran its next sampled checksum,
   the dirty flag would still be FALSE and the menu-open save would
   be skipped without this. The cost is one full-SRAM checksum at
   menu-open time, which is already a moment we accept a hitch for
   (the modal "Saving SRAM..." is already shown there). */
Bool _MainLoopForceCheckSRAM()
{
    Int32 nSramBytes = _pSystem ? _pSystem->GetSRAMBytes() : 0;

    if (nSramBytes > 0)
    {
        Uint8 *pSRAM = _pSystem->GetSRAMData();
        Uint32 uChecksum;

        /* AURORA_RUNTIME_SAFE_SRAM_PTR_V1_4_1 */
        if (!pSRAM)
            return FALSE;
        uChecksum = _CalcChecksum((Uint32 *)pSRAM, nSramBytes / 4);

        if (_MainLoop_SRAMChecksum != uChecksum)
        {
            ML_TRACE(
                "SRAM force-check: dirty (old=%08X new=%08X)",
                (unsigned int)_MainLoop_SRAMChecksum,
                (unsigned int)uChecksum
            );
            _MainLoop_SRAMUpdated = TRUE;
            _MainLoop_SRAMChecksum = uChecksum;
        }
    }

    if (_pSystem == _pSnes && _pSnes && _pSnes->IsSuperGameBoy() &&
        _pSnes->IsSuperGameBoySavedataDirty())
        _MainLoop_SRAMUpdated = TRUE;

    /* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901
     * Game Pak SRAM writes mark themselves dirty immediately; no full second
     * SRAM checksum is added to menu entry or gameplay. */
    if (_pSystem == _pSnes && _pSnes &&
        _pSnes->IsSuperWildCard() &&
        (_pSnes->IsSuperWildCardCartridgeSRAMDirty() ||
         s_SwcCartSRAMMigrationPending))
        _MainLoop_SRAMUpdated = TRUE;

    /* AURORA_QN_TURBOFILE_SAVE_V2_20260828: protocol writes set their
     * dirty bit immediately, so no 8 KiB checksum polling is required. */
    if (_pSystem == _pNes &&
        QuicknesBridge_TurboFileEnabled() &&
        QuicknesBridge_TurboFileDirty())
        _MainLoop_SRAMUpdated = TRUE;

    if (_pSystem == _pNes &&
        QuicknesBridge_BattleBoxEnabled() &&
        QuicknesBridge_BattleBoxDirty())
        _MainLoop_SRAMUpdated = TRUE;

    if (_pSystem == _pSnes &&
        SnesTurboFileEnabled() &&
        SnesTurboFileDirty())
        _MainLoop_SRAMUpdated = TRUE;

    if (_pSystem == _pSnes && _pSnes &&
        _pSnes->HasBSXMemoryPack() && _pSnes->IsBSXMemoryPackDirty())
        _MainLoop_SRAMUpdated = TRUE; /* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_STATE_CPP */

    return TRUE;
}

Bool _MainLoopCheckSRAM()
{
    Int32 nSramBytes = _pSystem ? _pSystem->GetSRAMBytes() : 0;

    /* AURORA_MEGA_V2_SNES_SRAM_NO_POLL
       SNES SRAM is force-checked immediately when the in-game menu
       opens, before the save decision. Therefore a 30-frame full
       memory sweep during gameplay is redundant and can create a
       small periodic EE workload spike on large SRAM carts. */
    if (_pSystem == _pSnes)
    {
        if (_pSnes && _pSnes->IsSuperGameBoy() && _pSnes->IsSuperGameBoySavedataDirty())
            _MainLoop_SRAMUpdated = TRUE;

        /* AURORA_SNES_TURBOFILE_V4_20260829
         * External protocol writes already maintain a dirty boolean.
         * Keep the normal no-checksum SNES path, but expose that O(1)
         * dirty state to the existing deterministic menu-save flow. */
        if (SnesTurboFileEnabled() && SnesTurboFileDirty())
            _MainLoop_SRAMUpdated = TRUE;

        if (_pSnes && _pSnes->HasBSXMemoryPack() &&
            _pSnes->IsBSXMemoryPackDirty())
            _MainLoop_SRAMUpdated = TRUE; /* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_STATE_CPP */

        /* AURORA_SWC_CART_SRAM_MEMORY_FINAL_V5_3_20260901: O(1) physical-cart dirty state. */
        if (_pSnes && _pSnes->IsSuperWildCard() &&
            (_pSnes->IsSuperWildCardCartridgeSRAMDirty() ||
             s_SwcCartSRAMMigrationPending))
            _MainLoop_SRAMUpdated = TRUE;

        return TRUE;
    }

    if (nSramBytes > 0)
    {
        /* The inline auto-save trigger (decrement SaveCounter -> call
           _MainLoopSaveSRAM(FALSE) when it hits zero) was removed
           deliberately. On the !_MainLoop_bMCSaveReady fallback path
           (NetherSX2 / any setup without MCSAVE.IRX next to the ELF)
           _MainLoopSaveSRAM ends up in MemCardWriteFile, which
           drives fopen/fwrite/fclose on the EE main thread and
           blocks the per-frame loop for the full duration of the
           memcard write. Games that keep the SRAM continuously
           dirty (RPG stats counters, HUD timers, etc.) caused this
           to fire at unpredictable moments and showed up as a
           gameplay hitch, while games that don't keep it dirty just
           saved at a different unpredictable point.

           The user-visible save path is now exclusively the
           synchronous one in _MenuEnable(TRUE) (mainloop_menu_runtime.cpp):
           opening the in-game menu with L2+R2 still calls
           _MainLoopSaveSRAM(TRUE) and shows the "Saving SRAM..." modal,
           so the save still happens at a deterministic, user-driven
           moment. _MainLoop_SRAMUpdated and _MainLoop_SRAMChecksum
           below are still maintained because _MenuEnable reads
           _MainLoop_SRAMUpdated to decide whether to actually run
           the save block at all. */

        /* The full-SRAM checksum used to run every frame (60Hz).
           For larger carts (up to MAINLOOP_MAXSRAMSIZE = 64 KB,
           i.e. 16k u32 adds) that's pure busywork: the only
           consumer is the _MainLoop_SRAMUpdated dirty bit that
           _MenuEnable polls when the user opens the menu, which
           never needs frame-accurate freshness. Run the check
           once every CHECK_INTERVAL frames (~0.5s @ 60Hz) so the
           dirty flag is still set well before any plausible
           L2+R2 press, without paying the cost on every frame. */
        static Uint32 sCheckFrame = 0;
        const Uint32 CHECK_INTERVAL = 30;
        if ((sCheckFrame++ % CHECK_INTERVAL) != 0)
        {
            return TRUE;
        }

        Uint8 *pSRAM = _pSystem->GetSRAMData();
        Uint32 uChecksum;

        if (!pSRAM)
            return FALSE;

        PROF_ENTER("_MainLoopCheckSRAM");

        uChecksum = _CalcChecksum((Uint32 *)pSRAM, nSramBytes / 4);

        if (_MainLoop_SRAMChecksum != uChecksum)
        {
#if CODE_DEBUG
            printf("SRAM changed!\n");
#endif
            ML_TRACE(
                "SRAM checksum changed: old=%08X new=%08X",
                (unsigned int)_MainLoop_SRAMChecksum,
                (unsigned int)uChecksum
            );

            _MainLoop_SRAMUpdated = TRUE;
            _MainLoop_SRAMChecksum = uChecksum;
        }

        PROF_LEAVE("_MainLoopCheckSRAM");
    }

    return TRUE;
}

/* ---- Versioned SNES/NES save states --------------------------------
 *
 * The recovered iaddis code wrote SnesStateT directly to host0:.  Besides
 * being a development-only path, that format had no version, ROM identity
 * or integrity check, and its load path could restore stale RAM after a
 * failed read.  The PS2 front-end now wraps the core payload in a small
 * header and keeps two banks per slot.  A bank only becomes visible after
 * its full payload has been flushed and the committed header is written.
 * The other bank remains untouched, so a reset/power loss during saving
 * cannot destroy the last known-good state.
 */

#define MAINLOOP_STATE_SLOT_NUM       5
#define MAINLOOP_STATE_BANK_NUM       2
/* The outer container remains version 1 for compatibility with existing SNES
   banks. Reserved[2] identifies the core; NesStateT has its own version. */
#define MAINLOOP_STATE_FORMAT_VERSION 1
#define MAINLOOP_STATE_HEADER_BYTES   64
#define MAINLOOP_STATE_MAX_ROOTS      8
#define MAINLOOP_STATE_MAX_CANDIDATES (MAINLOOP_STATE_MAX_ROOTS * MAINLOOP_STATE_BANK_NUM)
#define MAINLOOP_STATE_PAYLOAD_RAW     0
#define MAINLOOP_STATE_PAYLOAD_DEFLATE 1
#define MAINLOOP_STATE_SYSTEM_SNES      0
#define MAINLOOP_STATE_SYSTEM_NES       1
#define MAINLOOP_STATE_SYSTEM_SEGA      2
#define MAINLOOP_STATE_SYSTEM_PCE       3
#define MAINLOOP_STATE_SYSTEM_FDS       5 /* AURORA_FCEUMM_FDS_V0_6_STATE */
#define MAINLOOP_STATE_SYSTEM_SWC       6 /* AURORA_SWC_FLOPPY_V4_20260831 */
#define MAINLOOP_STATE_SYSTEM_SEGACD    7 /* AURORA_CD_STATE_V1_SAFE_20260903 */
#define MAINLOOP_STATE_SYSTEM_PCECD     8 /* AURORA_CD_STATE_V1_SAFE_20260903 */
#define MAINLOOP_STATE_SYSTEM_SGB       9 /* AURORA_SGB_RUNTIME_V0_4_20260904 */
#define MAINLOOP_STATE_RAW_BYTES \
    (sizeof(SnesStateT) > sizeof(NesStateT) \
        ? sizeof(SnesStateT) \
        : sizeof(NesStateT))
/* mz_compressBound() currently uses a conservative 110% + 128 bound.
   Keeping the buffer static avoids heap fragmentation on the 32 MB PS2. */
#define MAINLOOP_STATE_COMPRESS_BYTES \
    ((MAINLOOP_STATE_RAW_BYTES * 110) / 100 + 128)

struct MainLoopStateFileHeaderT
{
    Uint8  Magic[8];
    Uint32 uVersion;
    Uint32 nHeaderBytes;
    Uint32 nPayloadBytes;
    Uint32 uPayloadCRC;
    Uint32 uRomCRC;
    Uint32 nRomBytes;
    Uint32 uRomFlags;
    Uint32 iSlot;
    Uint32 uGeneration;
    /* Reserved[0] = payload encoding (raw/deflate).
       Reserved[1] = CRC32 of the stored compressed bytes. The public
       uPayloadCRC remains the CRC32 of the uncompressed core state.
       Reserved[2] = core ID (0 SNES, 1 NES). Old SNES banks were
       zero-initialised, so they remain valid. */
    Uint32 Reserved[5];
};

struct MainLoopStateConfigT
{
    Uint8  Magic[8];
    Uint32 uVersion;
    Uint32 nConfigBytes;
    Uint32 eDevice;
    Uint32 iSlot;
    Uint32 Reserved[2];
};

struct MainLoopStateRootT
{
    Char Root[16];
    Char DeviceName[24];
    Bool bMemCard;
};

struct MainLoopStateCandidateT
{
    Char Path[1024];
    Char DeviceName[16];
    MainLoopStateFileHeaderT Header;
};

typedef char MainLoopStateHeaderSizeCheck[
    sizeof(MainLoopStateFileHeaderT) == MAINLOOP_STATE_HEADER_BYTES ? 1 : -1
];
typedef char MainLoopStateConfigSizeCheck[
    sizeof(MainLoopStateConfigT) == 32 ? 1 : -1
];

static const Uint8 _MainLoop_StateMagic[8] =
{
    'S', 'N', 'R', 'S', 'T', 'A', 'T', 'E'
};
static const Uint8 _MainLoop_StateConfigMagic[8] =
{
    'S', 'N', 'R', 'S', 'C', 'F', 'G', '1'
};

static MainLoopStateDeviceE _MainLoop_StateDevice = MAINLOOP_STATEDEVICE_AUTO;
static Int32 _MainLoop_StateSlot = 0;
static Bool _MainLoop_StateDeviceChosen = FALSE;
static Char _MainLoop_StateLastMessage[192] = "No save-state operation yet.";
static Char _MainLoop_StateAvailability[192];
static Bool _MainLoop_StateRomCRCValid = FALSE;
static Uint32 _MainLoop_StateRomCRC = 0;
static MainLoopStateCandidateT _MainLoop_StateCandidates[MAINLOOP_STATE_MAX_CANDIDATES];
static Uint8 _MainLoop_StateCompressed[MAINLOOP_STATE_COMPRESS_BYTES]
    __attribute__((aligned(64)));

/* AURORA_PICODRIVE_STAGE2_DYNAMIC_STATE
 * Never tax SNES/NES BSS for PicoDrive's variable-size state. These buffers
 * appear only after a Sega state operation and are released on ROM change. */
static Uint8 *_MainLoop_SegaStateData = NULL;
static Uint32 _MainLoop_SegaStateCapacity = 0;
static Uint8 *_MainLoop_SegaCompressed = NULL;
static Uint32 _MainLoop_SegaCompressedCapacity = 0;

static Bool _MainLoopStateIsSwc()
{
    return (_pSystem == _pSnes && _pSnes &&
            _pSnes->IsSuperWildCard()) ? TRUE : FALSE;
}
/* AURORA_SGB_RUNTIME_V0_4_20260904 */
static Bool _MainLoopStateIsSgb()
{
    return (_pSystem == _pSnes && _pSnes &&
            _pSnes->IsSuperGameBoy()) ? TRUE : FALSE;
}
/* AURORA_SWC_FLOPPY_V4_20260831 */

static void _MainLoopStateReleaseSegaScratch()
{
    if (_MainLoop_SegaStateData) free(_MainLoop_SegaStateData);
    if (_MainLoop_SegaCompressed) free(_MainLoop_SegaCompressed);
    _MainLoop_SegaStateData = NULL;
    _MainLoop_SegaStateCapacity = 0;
    _MainLoop_SegaCompressed = NULL;
    _MainLoop_SegaCompressedCapacity = 0;
}

/* AURORA_PD_STATE_SCRATCH_RELEASE_V3
 *
 * PicoDrive state data is temporary working memory. Keep the raw state and
 * optional compression buffer alive for the whole Save/Load operation, then
 * release them automatically on every exit path. This avoids leaving both
 * buffers pinned on the 32 MiB EE heap after a Sega state operation.
 */
class MainLoopSegaStateScratchGuard
{
public:
    MainLoopSegaStateScratchGuard()
        : m_bActive((_pSystem == _pSega || _pSystem == _pPce ||
                     _pSystem == _pFds || /* AURORA_FCEUMM_FDS_V0_6_STATE */
                     _MainLoopStateIsSwc() || _MainLoopStateIsSgb()) ? TRUE : FALSE)
    {
    }

    ~MainLoopSegaStateScratchGuard()
    {
        if (m_bActive)
            _MainLoopStateReleaseSegaScratch();
    }

private:
    Bool m_bActive;

    MainLoopSegaStateScratchGuard(
        const MainLoopSegaStateScratchGuard &);
    MainLoopSegaStateScratchGuard &operator=(
        const MainLoopSegaStateScratchGuard &);
};

static Uint8 *_MainLoopStateEnsureSegaStateData(Uint32 nBytes)
{
    if (!nBytes)
        return NULL;
    if (_MainLoop_SegaStateCapacity < nBytes)
    {
        void *p = realloc(_MainLoop_SegaStateData, nBytes);
        if (!p)
            return NULL;
        _MainLoop_SegaStateData = (Uint8 *)p;
        _MainLoop_SegaStateCapacity = nBytes;
    }
    return _MainLoop_SegaStateData;
}

static Uint32 _MainLoopStateCompressedLimit(Uint32 nRawBytes)
{
    if (_pSystem != _pSega && _pSystem != _pPce &&
        _pSystem != _pFds && /* AURORA_FCEUMM_FDS_V0_6_STATE */
        !_MainLoopStateIsSwc() && !_MainLoopStateIsSgb())
        return (Uint32)sizeof(_MainLoop_StateCompressed);

    unsigned long long n =
        ((unsigned long long)nRawBytes * 110ULL) / 100ULL + 128ULL;
    return n <= 0xffffffffULL ? (Uint32)n : 0;
}

static Uint8 *_MainLoopStateGetCompressedBuffer(Uint32 nNeed, Uint32 *pCapacity)
{
    if (_pSystem != _pSega && _pSystem != _pPce &&
        _pSystem != _pFds && /* AURORA_FCEUMM_FDS_V0_6_STATE */
        !_MainLoopStateIsSwc() && !_MainLoopStateIsSgb())
    {
        if (pCapacity) *pCapacity = (Uint32)sizeof(_MainLoop_StateCompressed);
        return _MainLoop_StateCompressed;
    }

    if (!nNeed)
        return NULL;
    if (_MainLoop_SegaCompressedCapacity < nNeed)
    {
        void *p = realloc(_MainLoop_SegaCompressed, nNeed);
        if (!p)
            return NULL;
        _MainLoop_SegaCompressed = (Uint8 *)p;
        _MainLoop_SegaCompressedCapacity = nNeed;
    }
    if (pCapacity) *pCapacity = _MainLoop_SegaCompressedCapacity;
    return _MainLoop_SegaCompressed;
}
static Int32 _MainLoop_StateUnformattedCard = -1;
static Char _MainLoop_StateConfigPath[1024] = "";

static Bool _MainLoopStateEnsureOneDir(const Char *pPath);
static void _MainLoopStateDeleteSettings();
static void _MainLoopStateLoadSettingsFromRomDevice();

static Uint32 _MainLoopStateGetSystemId()
{
    if (_pSystem == _pNes)  return MAINLOOP_STATE_SYSTEM_NES;
    if (_pSystem == _pSega && PicoDriveBridge_IsSegaCD())
        return MAINLOOP_STATE_SYSTEM_SEGACD;
    if (_pSystem == _pPce && PceBridge_IsDiscLoaded())
        return MAINLOOP_STATE_SYSTEM_PCECD;
    if (_pSystem == _pSega) return MAINLOOP_STATE_SYSTEM_SEGA;
    if (_pSystem == _pPce)  return MAINLOOP_STATE_SYSTEM_PCE;
    if (_pSystem == _pFds)  return MAINLOOP_STATE_SYSTEM_FDS; /* AURORA_FCEUMM_FDS_V0_6_STATE */
    if (_MainLoopStateIsSwc()) return MAINLOOP_STATE_SYSTEM_SWC;
    if (_MainLoopStateIsSgb()) return MAINLOOP_STATE_SYSTEM_SGB;
    return MAINLOOP_STATE_SYSTEM_SNES;
}

static Uint32 _MainLoopStateGetPayloadBytes()
{
    if (_MainLoopStateIsSgb())
    {
        Int32 nBytes = _pSnes->GetStateSize();
        return nBytes > 0 ? (Uint32)nBytes : 0;
    }
    if (_MainLoopStateIsSwc())
    {
        Int32 nBytes = _pSnes->GetStateSize();
        return nBytes > 0 ? (Uint32)nBytes : 0;
    }
    /* AURORA_PICODRIVE_STAGE2_STATE_SIZE */
    if (_pSystem == _pSega)
    {
        Int32 nBytes = _pSega ? _pSega->GetStateSize() : 0;
        return nBytes > 0 ? (Uint32)nBytes : 0;
    }
    if (_pSystem == _pFds)
    {
        /* AURORA_FCEUMM_FDS_V0_6_STATE: dynamic FCEUmm FDS snapshot. */
        Int32 nBytes = _pFds ? _pFds->GetStateSize() : 0;
        return nBytes > 0 ? (Uint32)nBytes : 0;
    }
    if (_pSystem == _pPce)
    {
        Int32 nBytes = _pPce ? _pPce->GetStateSize() : 0;
        return nBytes > 0 ? (Uint32)nBytes : 0;
    }
    if (_pSystem == _pNes)
    {
        /*
         * Let the active NES implementation describe its state envelope.
         *
         * InfoNES deliberately returns sizeof(NesStateT), preserving its
         * existing file format. QuickNES returns its compact native envelope.
         */
        Int32 nBytes = _pNes ? _pNes->GetStateSize() : 0;

        if (nBytes > 0 && nBytes <= (Int32)sizeof(_NesState))
        {
            return (Uint32)nBytes;
        }

        /* Defensive compatibility fallback. */
        return (Uint32)sizeof(_NesState);
    }

    return (Uint32)sizeof(_SnesState);
}

static Uint8 *_MainLoopStateGetPayloadData()
{
    if (_MainLoopStateIsSgb())
        return _MainLoopStateEnsureSegaStateData(_MainLoopStateGetPayloadBytes());
    if (_MainLoopStateIsSwc())
        return _MainLoopStateEnsureSegaStateData(
            _MainLoopStateGetPayloadBytes());
    if (_pSystem == _pNes)
        return (Uint8 *)&_NesState;
    if (_pSystem == _pSega || _pSystem == _pPce ||
        _pSystem == _pFds) /* AURORA_FCEUMM_FDS_V0_6_STATE */
        return _MainLoopStateEnsureSegaStateData(
            _MainLoopStateGetPayloadBytes());
    return (Uint8 *)&_SnesState;
}

static void _MainLoopStateSetMessage(const Char *pFormat, ...)
{
    va_list Args;

    va_start(Args, pFormat);
    vsnprintf(
        _MainLoop_StateLastMessage,
        sizeof(_MainLoop_StateLastMessage),
        pFormat,
        Args
    );
    va_end(Args);
}

void MainLoopStateOnRomChanged()
{
    /* AURORA_PICODRIVE_STAGE2_RELEASE_STATE */
    _MainLoopStateReleaseSegaScratch();
    _MainLoop_StateRomCRCValid = FALSE;
    _MainLoop_StateRomCRC = 0;
    _MainLoop_StateUnformattedCard = -1;
    _bStateSaved = FALSE;

    /* A disc/ISO boot may have no writable config device until the user opens
       a ROM from mass2+, MMCE or HDD. Once that device is known, recover its
       saved default without doing a broad probe or directory scan. */
    if (_pSystem && _RomPath[0] && !_MainLoop_StateDeviceChosen)
    {
        _MainLoopStateLoadSettingsFromRomDevice();
    }
}

/* AURORA_PD_MEGA_FIX_20260820
 * Called immediately after MainLoopStateOnRomChanged(), with the CRC taken
 * before the active core can transform the shared ROM buffer. */
void MainLoopStatePrimeRomIdentityCRC(Uint32 uCRC)
{
    _MainLoop_StateRomCRC = uCRC;
    _MainLoop_StateRomCRCValid = TRUE;
}

Int32 MainLoopStateGetSlot()
{
    return _MainLoop_StateSlot;
}

MainLoopStateDeviceE MainLoopStateGetDevice()
{
    return _MainLoop_StateDevice;
}

const Char *MainLoopStateGetDeviceName()
{
    switch (_MainLoop_StateDevice)
    {
        case MAINLOOP_STATEDEVICE_USB:     return "USB";
        case MAINLOOP_STATEDEVICE_MEMCARD: return "Memory Card";
        case MAINLOOP_STATEDEVICE_MMCE:    return "MMCE";
        case MAINLOOP_STATEDEVICE_HDD:     return "Internal HDD";
        default:                           return "Auto";
    }
}

const Char *MainLoopStateGetLastMessage()
{
    return _MainLoop_StateLastMessage;
}

Int32 MainLoopStateGetUnformattedCard()
{
    return _MainLoop_StateUnformattedCard;
}

Bool MainLoopStateHasDeviceChoice()
{
    return _MainLoop_StateDeviceChosen;
}

void MainLoopStateForgetDeviceChoice()
{
    _MainLoop_StateDevice = MAINLOOP_STATEDEVICE_AUTO;
    _MainLoop_StateSlot = 0;
    _MainLoop_StateDeviceChosen = FALSE;
    _MainLoopStateDeleteSettings();
}

void MainLoopStateSetDevice(MainLoopStateDeviceE eDevice)
{
    if (eDevice >= MAINLOOP_STATEDEVICE_AUTO &&
        eDevice < MAINLOOP_STATEDEVICE_NUM)
    {
        _MainLoop_StateDevice = eDevice;
        if (eDevice == MAINLOOP_STATEDEVICE_AUTO)
        {
            _MainLoop_StateSlot = 0;
        }
    }
}

Bool MainLoopStateDeviceAvailable(MainLoopStateDeviceE eDevice)
{
    switch (eDevice)
    {
        case MAINLOOP_STATEDEVICE_AUTO:
            return TRUE;

        case MAINLOOP_STATEDEVICE_USB:
            return MassStorageIsEnabled() ? TRUE : FALSE;

        case MAINLOOP_STATEDEVICE_MEMCARD:
            return TRUE;

        case MAINLOOP_STATEDEVICE_MMCE:
            return MmceProbeAvailableSlots() ? TRUE : FALSE;

        case MAINLOOP_STATEDEVICE_HDD:
            return HddSupportIsEnabled() &&
                   (!strncmp(_RomPath, "hdd0:", 5) ||
                    !strncmp(_RomPath, "pfs0:", 5));

        default:
            return FALSE;
    }
}

void MainLoopStateCycleSlot()
{
    /* Auto is intentionally a zero-configuration quick-save mode. Its
       quick slot is always the first slot, including configurations
       written by older builds. Explicit devices retain all five slots. */
    if (_MainLoop_StateDevice == MAINLOOP_STATEDEVICE_AUTO)
    {
        _MainLoop_StateSlot = 0;
        return;
    }

    _MainLoop_StateSlot++;
    if (_MainLoop_StateSlot >= MAINLOOP_STATE_SLOT_NUM)
    {
        _MainLoop_StateSlot = 0;
    }
}

void MainLoopStateCycleDevice()
{
    _MainLoop_StateDevice = (MainLoopStateDeviceE)(_MainLoop_StateDevice + 1);
    if (_MainLoop_StateDevice >= MAINLOOP_STATEDEVICE_NUM)
    {
        _MainLoop_StateDevice = MAINLOOP_STATEDEVICE_AUTO;
    }
    if (_MainLoop_StateDevice == MAINLOOP_STATEDEVICE_AUTO)
    {
        _MainLoop_StateSlot = 0;
    }
}

static Bool _MainLoopStateConfigValid(const MainLoopStateConfigT *pConfig)
{
    return !memcmp(
                pConfig->Magic,
                _MainLoop_StateConfigMagic,
                sizeof(pConfig->Magic)) &&
           pConfig->uVersion == 2 &&
           pConfig->nConfigBytes == sizeof(*pConfig) &&
           pConfig->eDevice < MAINLOOP_STATEDEVICE_NUM &&
           pConfig->iSlot < MAINLOOP_STATE_SLOT_NUM;
}

static Bool _MainLoopStateConfigPathIsWritable(const Char *pPath)
{
    if (!pPath || !pPath[0])
    {
        return FALSE;
    }

    /* host: is a development filesystem and smb:/cdfs: are intentionally
       read-only in this frontend. Never pick one as the sole persistence
       location for the user's quick-save default. */
    return strncmp(pPath, "host:", 5) &&
           strncmp(pPath, "smb:", 4) &&
           strncmp(pPath, "cdfs:", 6) &&
           strncmp(pPath, "cdrom", 5) &&
           strncmp(pPath, "rom", 3);
}

static Bool _MainLoopStateConfigMapPath(
    const Char *pPath,
    Char *pMapped,
    Int32 nMappedBytes)
{
    if (!pPath || !pMapped || nMappedBytes <= 0)
    {
        return FALSE;
    }

    if (!strncmp(pPath, "hdd0:", 5))
    {
        return HddMapPath(pPath, pMapped, nMappedBytes) == 1;
    }

    return snprintf(pMapped, nMappedBytes, "%s", pPath) < nMappedBytes;
}

static void _MainLoopStateConfigEnsureParent(const Char *pPath)
{
    Char Directory[1024];
    Char *pSlash;
    size_t nLength;

    if (!pPath || strlen(pPath) >= sizeof(Directory))
    {
        return;
    }

    strcpy(Directory, pPath);
    pSlash = strrchr(Directory, '/');
    if (!pSlash)
    {
        return;
    }
    *pSlash = 0;
    nLength = strlen(Directory);
    if (nLength > 0 && Directory[nLength - 1] != ':')
    {
        _MainLoopStateEnsureOneDir(Directory);
    }
}

static Bool _MainLoopStateConfigRead(
    const Char *pPath,
    MainLoopStateConfigT *pConfig)
{
    Char MappedPath[1024];
    FILE *pFile;
    size_t nRead;

    if (!_MainLoopStateConfigMapPath(
            pPath,
            MappedPath,
            sizeof(MappedPath)))
    {
        return FALSE;
    }

    pFile = fopen(MappedPath, "rb");
    if (!pFile)
    {
        return FALSE;
    }
    nRead = fread(pConfig, 1, sizeof(*pConfig), pFile);
    fclose(pFile);

    if (nRead != sizeof(*pConfig) || !_MainLoopStateConfigValid(pConfig))
    {
        return FALSE;
    }

    snprintf(
        _MainLoop_StateConfigPath,
        sizeof(_MainLoop_StateConfigPath),
        "%s",
        pPath
    );
    return TRUE;
}

static Bool _MainLoopStateConfigWrite(
    const Char *pPath,
    const MainLoopStateConfigT *pConfig)
{
    Char MappedPath[1024];
    FILE *pFile;
    size_t nWritten;
    Bool bOK;

    if (!_MainLoopStateConfigPathIsWritable(pPath) ||
        !_MainLoopStateConfigMapPath(
            pPath,
            MappedPath,
            sizeof(MappedPath)))
    {
        return FALSE;
    }

    _MainLoopStateConfigEnsureParent(MappedPath);
    pFile = fopen(MappedPath, "wb");
    if (!pFile)
    {
        return FALSE;
    }
    nWritten = fwrite(pConfig, 1, sizeof(*pConfig), pFile);
    bOK = fflush(pFile) == 0;
    if (fclose(pFile) != 0)
    {
        bOK = FALSE;
    }
    if (nWritten != sizeof(*pConfig) || !bOK)
    {
        return FALSE;
    }

    snprintf(
        _MainLoop_StateConfigPath,
        sizeof(_MainLoop_StateConfigPath),
        "%s",
        pPath
    );
    return TRUE;
}

static Bool _MainLoopStateConfigApply(const MainLoopStateConfigT *pConfig)
{
    if (!_MainLoopStateConfigValid(pConfig))
    {
        return FALSE;
    }

    _MainLoop_StateDevice = (MainLoopStateDeviceE)pConfig->eDevice;
    _MainLoop_StateSlot = (Int32)pConfig->iSlot;
    if (_MainLoop_StateDevice == MAINLOOP_STATEDEVICE_AUTO)
    {
        _MainLoop_StateSlot = 0;
    }
    _MainLoop_StateDeviceChosen = TRUE;
    return TRUE;
}

static Bool _MainLoopStateConfigBuildRomPath(Char *pPath, Int32 nPathBytes)
{
    const Char *pColon;
    const Char *pPartitionEnd;
    Int32 nRootBytes;

    if (!_RomPath[0] || !pPath || nPathBytes <= 0)
    {
        return FALSE;
    }

    if (!strncmp(_RomPath, "hdd0:/", 6))
    {
        pPartitionEnd = strchr(_RomPath + 6, '/');
        if (!pPartitionEnd)
        {
            pPartitionEnd = _RomPath + strlen(_RomPath);
        }
        nRootBytes = (Int32)(pPartitionEnd - _RomPath);
    }
    else if (!strncmp(_RomPath, "pfs0:", 5))
    {
        nRootBytes = 5;
    }
    else if (!strncmp(_RomPath, "mass", 4) ||
             !strncmp(_RomPath, "mc", 2) ||
             !strncmp(_RomPath, "mmce", 4))
    {
        pColon = strchr(_RomPath, ':');
        if (!pColon)
        {
            return FALSE;
        }
        nRootBytes = (Int32)(pColon - _RomPath) + 1;
    }
    else
    {
        /* cdfs, smb and host are deliberately read-only/non-persistent. */
        return FALSE;
    }

    return snprintf(
               pPath,
               nPathBytes,
               "%.*s/SNESticle/state.cfg",
               nRootBytes,
               _RomPath) < nPathBytes;
}

static void _MainLoopStateLoadSettingsFromRomDevice()
{
    MainLoopStateConfigT Config;
    Char RomConfigPath[1024];

    if (_MainLoopStateConfigBuildRomPath(
            RomConfigPath,
            sizeof(RomConfigPath)) &&
        _MainLoopStateConfigRead(RomConfigPath, &Config))
    {
        _MainLoopStateConfigApply(&Config);
    }
}

void MainLoopStateSettingsLoad()
{
    MainLoopStateConfigT Config;
    static const Char *pMemoryCardPaths[] =
    {
        "mc0:/SNESticle/state.cfg",
        "mc1:/SNESticle/state.cfg",
        NULL
    };
    static const Char *pMassPaths[] =
    {
        "mass0:/SNESticle/state.cfg",
        "mass1:/SNESticle/state.cfg",
        "mass:/SNESticle/state.cfg",
        NULL
    };
    static const Char *pMMCEPaths[] =
    {
        "mmce0:/SNESticle/state.cfg",
        "mmce1:/SNESticle/state.cfg",
        NULL
    };
    Char BootPath[1024];
    Int32 i;

    _MainLoop_StateDevice = MAINLOOP_STATEDEVICE_AUTO;
    _MainLoop_StateSlot = 0;
    _MainLoop_StateDeviceChosen = FALSE;
    _MainLoop_StateConfigPath[0] = 0;

    /* Preserve the existing mc0/mc1 priority so upgrades keep their chosen
       target. A standalone ELF directory and every enabled writable local
       backend are fallbacks for consoles without a usable memory card. */
    for (i = 0; pMemoryCardPaths[i]; i++)
    {
        if (_MainLoopStateConfigRead(pMemoryCardPaths[i], &Config) &&
            _MainLoopStateConfigApply(&Config))
        {
            return;
        }
    }

    if (_MainLoop_BootDir[0] &&
        _MainLoopStateConfigPathIsWritable(_MainLoop_BootDir) &&
        snprintf(
            BootPath,
            sizeof(BootPath),
            "%sstate.cfg",
            _MainLoop_BootDir) < (Int32)sizeof(BootPath) &&
        _MainLoopStateConfigRead(BootPath, &Config) &&
        _MainLoopStateConfigApply(&Config))
    {
        return;
    }

    if (MassStorageIsEnabled() || Mx4sioIsEnabled())
    {
        for (i = 0; pMassPaths[i]; i++)
        {
            if (_MainLoopStateConfigRead(pMassPaths[i], &Config) &&
                _MainLoopStateConfigApply(&Config))
            {
                return;
            }
        }
    }

    if (MmceSupportIsEnabled() && !MmceNeedsRestart())
    {
        Int32 iSlots = MmceGetAvailableSlots();
        if (!iSlots)
        {
            iSlots = MmceProbeAvailableSlots();
        }
        for (i = 0; pMMCEPaths[i]; i++)
        {
            if ((iSlots & (1 << i)) &&
                _MainLoopStateConfigRead(pMMCEPaths[i], &Config) &&
                _MainLoopStateConfigApply(&Config))
            {
                return;
            }
        }
    }
}

Bool MainLoopStateSettingsSave()
{
    MainLoopStateConfigT Config;
    static const Char *pMemoryCardPaths[] =
    {
        "mc0:/SNESticle/state.cfg",
        "mc1:/SNESticle/state.cfg",
        NULL
    };
    static const Char *pMassPaths[] =
    {
        "mass0:/SNESticle/state.cfg",
        "mass1:/SNESticle/state.cfg",
        "mass:/SNESticle/state.cfg",
        NULL
    };
    static const Char *pMMCEPaths[] =
    {
        "mmce0:/SNESticle/state.cfg",
        "mmce1:/SNESticle/state.cfg",
        NULL
    };
    Char BootPath[1024];
    Char RomConfigPath[1024];
    Int32 i;
    Bool bSaved = FALSE;

    memset(&Config, 0, sizeof(Config));
    memcpy(Config.Magic, _MainLoop_StateConfigMagic, sizeof(Config.Magic));
    /* Version 2 deliberately invalidates the earlier menu-based target
       choice so the redesigned one-time chooser is shown once after update. */
    Config.uVersion = 2;
    Config.nConfigBytes = sizeof(Config);
    Config.eDevice = (Uint32)_MainLoop_StateDevice;
    Config.iSlot = (Uint32)_MainLoop_StateSlot;
    _MainLoop_StateDeviceChosen = TRUE;

    /* Update the location that supplied the config first. If none exists,
       prefer the writable ELF directory, then mc, mass/MX4SIO and MMCE.
       This makes the one-time choice persistent even without a memory card. */
    BgmIOBegin();
    if (_MainLoop_StateConfigPath[0])
    {
        bSaved = _MainLoopStateConfigWrite(
            _MainLoop_StateConfigPath,
            &Config
        );
    }

    if (!bSaved && _MainLoop_BootDir[0] &&
        _MainLoopStateConfigPathIsWritable(_MainLoop_BootDir) &&
        snprintf(
            BootPath,
            sizeof(BootPath),
            "%sstate.cfg",
            _MainLoop_BootDir) < (Int32)sizeof(BootPath))
    {
        bSaved = _MainLoopStateConfigWrite(BootPath, &Config);
    }

    if (!bSaved && _MainLoopStateConfigBuildRomPath(
            RomConfigPath,
            sizeof(RomConfigPath)))
    {
        bSaved = _MainLoopStateConfigWrite(RomConfigPath, &Config);
    }

    for (i = 0; !bSaved && pMemoryCardPaths[i]; i++)
    {
        bSaved = _MainLoopStateConfigWrite(pMemoryCardPaths[i], &Config);
    }

    if (!bSaved && (MassStorageIsEnabled() || Mx4sioIsEnabled()))
    {
        for (i = 0; !bSaved && pMassPaths[i]; i++)
        {
            bSaved = _MainLoopStateConfigWrite(pMassPaths[i], &Config);
        }
    }

    if (!bSaved && MmceSupportIsEnabled() && !MmceNeedsRestart())
    {
        Int32 iSlots = MmceGetAvailableSlots();
        if (!iSlots)
        {
            iSlots = MmceProbeAvailableSlots();
        }
        for (i = 0; !bSaved && pMMCEPaths[i]; i++)
        {
            if (iSlots & (1 << i))
            {
                bSaved = _MainLoopStateConfigWrite(pMMCEPaths[i], &Config);
            }
        }
    }
    BgmIOEnd();

    return bSaved;
}

static void _MainLoopStateDeleteSettings()
{
    static const Char *pConfigPaths[] =
    {
        "mc0:/SNESticle/state.cfg",
        "mc1:/SNESticle/state.cfg",
        "mass0:/SNESticle/state.cfg",
        "mass1:/SNESticle/state.cfg",
        "mass:/SNESticle/state.cfg",
        "mmce0:/SNESticle/state.cfg",
        "mmce1:/SNESticle/state.cfg",
        NULL
    };
    Char MappedPath[1024];
    Int32 i;

    if (_MainLoop_StateConfigPath[0] &&
        _MainLoopStateConfigMapPath(
            _MainLoop_StateConfigPath,
            MappedPath,
            sizeof(MappedPath)))
    {
        remove(MappedPath);
    }
    for (i = 0; pConfigPaths[i]; i++)
    {
        remove(pConfigPaths[i]);
    }
    _MainLoop_StateConfigPath[0] = 0;
}

static const Char *_MainLoopStateGetUnsupportedChip(Uint32 uFlags)
{
    /* AURORA_SPECIAL_CHIP_STATE_V1
     * AURORA_SA1_FRONTEND_STATE_V8_3_20260903
     * DSP-1/2/4, OBC1, SuperFX, S-DD1, S-RTC and SA-1 have a tagged snapshot
     * in the unused SRAM-state tail. Keep refusing them only if the current
     * cartridge cannot fit that envelope safely. */
    if (uFlags & SNROM_FLAG_GAMEBOY) return "Super Game Boy";
    if (uFlags & SNROM_FLAG_DSP3)    return "DSP-3";
    if ((uFlags & SNROM_FLAG_SA1) &&
        (!_pSnes || !_pSnes->CanSerializeSpecialChipState()))
        return "SA-1";

    if ((uFlags & SNROM_FLAG_SUPERFX) &&
        (!_pSnes || !_pSnes->CanSerializeSpecialChipState()))
        return "SuperFX";
    if ((uFlags & SNROM_FLAG_DSP1) &&
        (!_pSnes || !_pSnes->CanSerializeSpecialChipState()))
        return "DSP-1";
    if ((uFlags & SNROM_FLAG_DSP2) &&
        (!_pSnes || !_pSnes->CanSerializeSpecialChipState()))
        return "DSP-2";
    if ((uFlags & SNROM_FLAG_DSP4) &&
        (!_pSnes || !_pSnes->CanSerializeSpecialChipState()))
        return "DSP-4";
    if ((uFlags & SNROM_FLAG_OBC1) &&
        (!_pSnes || !_pSnes->CanSerializeSpecialChipState()))
        return "OBC1";
    if ((uFlags & SNROM_FLAG_SDD1) &&
        (!_pSnes || !_pSnes->CanSerializeSpecialChipState()))
        return "S-DD1";
    if ((uFlags & SNROM_FLAG_SRTC) &&
        (!_pSnes || !_pSnes->CanSerializeSpecialChipState()))
        return "S-RTC";

    /* Existing CX4 format/offset is intentionally unchanged. */
    if ((uFlags & SNROM_FLAG_CX4) &&
        (!_pSnes || !_pSnes->CanSerializeCX4State()))
        return "CX4";

    return NULL;
}

static Bool _MainLoopStateCheckAvailability(Char *pReason, Int32 nReasonBytes)
{
    const Char *pChip;
    NetPlayRPCStatusT NetStatus;

    if (!_pSystem)
    {
        snprintf(pReason, nReasonBytes, "No game loaded.");
        return FALSE;
    }

    if (_pSystem != _pSnes && _pSystem != _pNes &&
        _pSystem != _pSega && _pSystem != _pPce &&
        _pSystem != _pFds) /* AURORA_FCEUMM_FDS_V0_6_STATE */
    {
        snprintf(pReason, nReasonBytes, "This system cannot save states.");
        return FALSE;
    }

    if (_pSystem == _pNes)
    {
        if (!_pNesRom || !_pNesRom->IsLoaded() ||
            !_pNes || !_pNes->IsRomReady())
        {
            snprintf(pReason, nReasonBytes,
                     "NES state unavailable for this cartridge/mapper.");
            return FALSE;
        }
    }
    else if (_pSystem == _pFds)
    {
        /* AURORA_FCEUMM_FDS_V0_6_STATE */
        if (!_pFds || !_pFds->IsRomReady() || _pFds->GetStateSize() <= 0)
        {
            snprintf(pReason, nReasonBytes, "FCEUmm FDS state unavailable.");
            return FALSE;
        }
    }
    else if (_pSystem == _pSega)
    {
        /* AURORA_PICODRIVE_STAGE2_STATE_AVAILABLE */
        if (PicoDriveBridge_IsSegaCD())
        {
            if (!_pSega || !_pSega->IsRomReady() ||
                _pSega->GetStateSize() <= 0 ||
                !PicoDriveBridge_GetDiscPath())
            {
                snprintf(pReason, nReasonBytes,
                         "Sega CD state unavailable.");
                return FALSE;
            }
        }
        else if (!_pSegaRom || !_pSegaRom->IsLoaded() ||
                 !_pSega || !_pSega->IsRomReady())
        {
            snprintf(pReason, nReasonBytes, "PicoDrive state unavailable.");
            return FALSE;
        }
    }
    else if (_pSystem == _pPce)
    {
        if (PceBridge_IsDiscLoaded())
        {
            if (!_pPce || !_pPce->IsRomReady() ||
                _pPce->GetStateSize() <= 0 ||
                !PceBridge_GetDiscPath())
            {
                snprintf(pReason, nReasonBytes,
                         "PC Engine CD state unavailable.");
                return FALSE;
            }
        }
        else if (!_pPceRom || !_pPceRom->IsLoaded() ||
                 !_pPce || !_pPce->IsRomReady())
        {
            snprintf(pReason, nReasonBytes,
                     "Beetle PCE Fast state unavailable.");
            return FALSE;
        }
    }
    else if (_MainLoopStateIsSwc())
    {
        /* AURORA_SWC_FLOPPY_V5_20260831
         * External cartridge ROM is not embedded in V5 states. */
        if (_pSnes->HasSuperWildCardCartridge())
        {
            snprintf(
                pReason, nReasonBytes,
                "SWC state with external cartridge is not serialized.");
            return FALSE;
        }

        /* AURORA_SWC_MEGA_V9_20260831: V5 allowed BIOS-only boot, but a SWC state identifies
         * and remounts a concrete floppy image. Do not advertise a state that
         * cannot be restored consistently. */
        if (!_pSnes->HasSuperWildCardDisk())
        {
            snprintf(
                pReason, nReasonBytes,
                "Insert a Super Wild Card disk before using save states.");
            return FALSE;
        }

        if (_pSnes->GetStateSize() <= (Int32)sizeof(SnesStateT))
        {
            snprintf(pReason, nReasonBytes,
                     "Super Wild Card state extension unavailable.");
            return FALSE;
        }
    }
    else if (!_pSnesRom || !_pSnesRom->IsLoaded())
    {
        snprintf(pReason, nReasonBytes, "No SNES ROM loaded.");
        return FALSE;
    }

    pChip = (_pSystem == _pSnes && !_MainLoopStateIsSwc())
        ? _MainLoopStateGetUnsupportedChip(_pSnesRom->m_Flags)
        : NULL;
    if (pChip)
    {
        snprintf(pReason, nReasonBytes, "%s state is not serialized yet.", pChip);
        return FALSE;
    }

    if (s_pMovieClip &&
        (s_pMovieClip->IsRecording() || s_pMovieClip->IsPlaying()))
    {
        snprintf(pReason, nReasonBytes, "Stop movie recording/playback first.");
        return FALSE;
    }

    memset(&NetStatus, 0, sizeof(NetStatus));
    NetPlayGetStatus(&NetStatus);
    if (NetStatus.eServerStatus != NETPLAY_STATUS_IDLE ||
        NetStatus.eClientStatus != NETPLAY_STATUS_IDLE)
    {
        snprintf(pReason, nReasonBytes, "Save states are disabled during netplay.");
        return FALSE;
    }

    if (_MainLoopStateIsSwc())
        snprintf(pReason, nReasonBytes, "Ready: Super Wild Card + DRAM/FDC state.");
    else if (_pSystem == _pSega)
        snprintf(
            pReason, nReasonBytes,
            PicoDriveBridge_IsSegaCD()
                ? "Ready: Sega CD full-core state."
                : "Ready: PicoDrive cartridge state.");
    else if (_pSystem == _pPce)
        snprintf(
            pReason, nReasonBytes,
            PceBridge_IsDiscLoaded()
                ? "Ready: PC Engine CD full-core state."
                : "Ready: PC Engine HuCard state.");
    else if (_pSystem == _pFds)
        snprintf(pReason, nReasonBytes, "Ready: Famicom Disk System state."); /* AURORA_FCEUMM_FDS_V0_6_STATE */
    else
        snprintf(
            pReason,
            nReasonBytes,
            _pSystem == _pNes
                ? "Ready: NES cartridge and mapper state."
                : "Ready: base SNES hardware."
        );
    return TRUE;
}

const Char *MainLoopStateGetAvailability()
{
    _MainLoopStateCheckAvailability(
        _MainLoop_StateAvailability,
        sizeof(_MainLoop_StateAvailability)
    );
    return _MainLoop_StateAvailability;
}

/* AURORA_SWC_FLOPPY_V4_20260831
 * NAME_1.img, NAME_2.img, ... share one state namespace: NAME. */
static Bool _MainLoopStateGetSwcBaseName(Char *pOut, Int32 nOutBytes)
{
    const Char *pPath;
    const Char *pName;
    const Char *pExt;
    size_t n;
    size_t i;

    if (!pOut || nOutBytes <= 1 || !_MainLoopStateIsSwc())
        return FALSE;

    pPath = _pSnes->GetSuperWildCardDiskPath();
    if (!pPath || !*pPath)
        return FALSE;

    pName = pPath;
    for (const Char *p = pPath; *p; ++p)
        if (*p == '/' || *p == '\\')
            pName = p + 1;

    pExt = strrchr(pName, '.');
    n = pExt ? (size_t)(pExt - pName) : strlen(pName);
    if (!n || n >= (size_t)nOutBytes)
        return FALSE;

    memcpy(pOut, pName, n);
    pOut[n] = 0;

    i = n;
    while (i > 0 && pOut[i - 1] >= '0' && pOut[i - 1] <= '9')
        --i;
    if (i > 0 && i < n && pOut[i - 1] == '_')
        pOut[i - 1] = 0;

    return pOut[0] ? TRUE : FALSE;
}


/* AURORA_CD_STATE_V1_SAFE_20260903
 *
 * Save-state ROM identity for path-backed optical media.
 *
 * Do not use the CUE/TOC filename as identity: it is too weak and breaks
 * when a game directory is renamed.  Instead hash descriptor semantics and
 * bounded samples from every referenced physical track file.  FILE-path
 * text itself is intentionally excluded, so moving a complete game folder
 * does not invalidate a state.
 *
 * The helper is deliberately fail-closed.  If the descriptor or any
 * referenced file cannot be resolved/read, the frontend refuses the state
 * operation rather than creating a weakly identified bank.
 */
static Bool _MainLoopStateDiscPrefixNoCase(
    const Char *pText, const Char *pWord)
{
    if (!pText || !pWord)
        return FALSE;

    while (*pWord)
    {
        Char a = *pText++;
        Char b = *pWord++;
        if (a >= 'a' && a <= 'z') a = (Char)(a - ('a' - 'A'));
        if (b >= 'a' && b <= 'z') b = (Char)(b - ('a' - 'A'));
        if (a != b)
            return FALSE;
    }
    return TRUE;
}

static Bool _MainLoopStateDiscExtractFileToken(
    const Char *pLine, Char *pOut, Int32 nOutBytes)
{
    const Char *p;
    Int32 nKeyword = 0;
    Int32 n = 0;
    Bool bQuoted = FALSE;

    if (!pLine || !pOut || nOutBytes <= 1)
        return FALSE;

    p = pLine;
    while (*p == ' ' || *p == '\t')
        ++p;

    if (_MainLoopStateDiscPrefixNoCase(p, "FILE") &&
        (p[4] == ' ' || p[4] == '\t'))
        nKeyword = 4;
    else if (_MainLoopStateDiscPrefixNoCase(p, "DATAFILE") &&
             (p[8] == ' ' || p[8] == '\t'))
        nKeyword = 8;
    else if (_MainLoopStateDiscPrefixNoCase(p, "AUDIOFILE") &&
             (p[9] == ' ' || p[9] == '\t'))
        nKeyword = 9;
    else
        return FALSE;

    p += nKeyword;
    while (*p == ' ' || *p == '\t')
        ++p;

    if (*p == '"')
    {
        bQuoted = TRUE;
        ++p;
    }

    while (*p)
    {
        if (bQuoted)
        {
            if (*p == '"')
                break;
        }
        else if (*p == ' ' || *p == '\t' ||
                 *p == '\r' || *p == '\n')
            break;

        if (n + 1 >= nOutBytes)
            return FALSE;
        pOut[n++] = *p++;
    }

    if (bQuoted && *p != '"')
        return FALSE;
    if (!n)
        return FALSE;

    pOut[n] = 0;
    return TRUE;
}

static Bool _MainLoopStateDiscResolveFile(
    const Char *pDescriptorPath,
    const Char *pToken,
    Char *pOut,
    Int32 nOutBytes)
{
    const Char *pSlash;
    const Char *pBackslash;
    const Char *pColon;
    Int32 nDir;
    Int32 nWritten;
    Char *p;

    if (!pDescriptorPath || !*pDescriptorPath ||
        !pToken || !*pToken || !pOut || nOutBytes <= 1)
        return FALSE;

    if (strchr(pToken, ':') || pToken[0] == '/' || pToken[0] == '\\')
    {
        nWritten = snprintf(pOut, nOutBytes, "%s", pToken);
    }
    else
    {
        pSlash = strrchr(pDescriptorPath, '/');
        pBackslash = strrchr(pDescriptorPath, '\\');
        if (!pSlash || (pBackslash && pBackslash > pSlash))
            pSlash = pBackslash;

        if (pSlash)
        {
            nDir = (Int32)(pSlash - pDescriptorPath) + 1;
            nWritten = snprintf(
                pOut, nOutBytes, "%.*s%s",
                nDir, pDescriptorPath, pToken);
        }
        else
        {
            pColon = strrchr(pDescriptorPath, ':');
            if (pColon)
            {
                nDir = (Int32)(pColon - pDescriptorPath) + 1;
                nWritten = snprintf(
                    pOut, nOutBytes, "%.*s/%s",
                    nDir, pDescriptorPath, pToken);
            }
            else
                nWritten = snprintf(pOut, nOutBytes, "%s", pToken);
        }
    }

    if (nWritten < 0 || nWritten >= nOutBytes)
        return FALSE;

    for (p = pOut; *p; ++p)
        if (*p == '\\')
            *p = '/';

    return TRUE;
}

static Uint32 _MainLoopStateDiscCrcU32(Uint32 uCRC, Uint32 uValue)
{
    Uint8 Bytes[4];
    Bytes[0] = (Uint8)uValue;
    Bytes[1] = (Uint8)(uValue >> 8);
    Bytes[2] = (Uint8)(uValue >> 16);
    Bytes[3] = (Uint8)(uValue >> 24);
    return (Uint32)mz_crc32(
        uCRC, (const unsigned char *)Bytes, sizeof(Bytes));
}

static Bool _MainLoopStateDiscHashPhysicalFile(
    const Char *pPath,
    Uint32 *puCRC,
    Uint32 *puSizeSignature)
{
    enum { SAMPLE_BYTES = 1024 };
    struct stat Status;
    FILE *pFile = NULL;
    Uint8 Sample[SAMPLE_BYTES];
    Uint32 nFileBytes;
    long Offset[3];
    Int32 i;
    Int32 j;

    if (!pPath || !*pPath || !puCRC || !puSizeSignature)
        return FALSE;

    if (stat(pPath, &Status) != 0 || S_ISDIR(Status.st_mode) ||
        Status.st_size <= 0 || Status.st_size > 0x7fffffffL)
        return FALSE;

    nFileBytes = (Uint32)Status.st_size;
    pFile = fopen(pPath, "rb");
    if (!pFile)
        return FALSE;

    *puCRC = _MainLoopStateDiscCrcU32(*puCRC, nFileBytes);
    *puSizeSignature ^= nFileBytes;
    *puSizeSignature *= 16777619u;

    Offset[0] = 0;
    Offset[1] = Status.st_size > SAMPLE_BYTES
        ? (long)((Status.st_size - SAMPLE_BYTES) / 2) : 0;
    Offset[2] = Status.st_size > SAMPLE_BYTES
        ? (long)(Status.st_size - SAMPLE_BYTES) : 0;

    for (i = 0; i < 3; ++i)
    {
        size_t nWant;
        size_t nRead;
        Bool bDuplicate = FALSE;

        for (j = 0; j < i; ++j)
            if (Offset[j] == Offset[i])
                bDuplicate = TRUE;
        if (bDuplicate)
            continue;

        if (fseek(pFile, Offset[i], SEEK_SET) != 0)
        {
            fclose(pFile);
            return FALSE;
        }

        nWant = (size_t)(Status.st_size - Offset[i]);
        if (nWant > SAMPLE_BYTES)
            nWant = SAMPLE_BYTES;

        nRead = fread(Sample, 1, nWant, pFile);
        if (nRead != nWant)
        {
            fclose(pFile);
            return FALSE;
        }

        *puCRC = _MainLoopStateDiscCrcU32(
            *puCRC, (Uint32)Offset[i]);
        *puCRC = (Uint32)mz_crc32(
            *puCRC, (const unsigned char *)Sample, nRead);
    }

    fclose(pFile);
    return TRUE;
}

static Bool _MainLoopStateGetDiscIdentity(
    const Char *pDescriptorPath,
    Uint32 uFamilyFlags,
    Uint32 *puCRC,
    Uint32 *pnBytes,
    Uint32 *puFlags)
{
    enum { MAX_DISC_FILES = 99 };
    FILE *pDescriptor;
    Char Line[2048];
    Char Token[1024];
    Char Resolved[1024];
    Char LastToken[1024];
    Uint32 uCRC = MZ_CRC32_INIT;
    Uint32 uSizeSignature = 2166136261u;
    Int32 nFiles = 0;

    if (!pDescriptorPath || !*pDescriptorPath ||
        !puCRC || !pnBytes || !puFlags)
        return FALSE;

    pDescriptor = fopen(pDescriptorPath, "rb");
    if (!pDescriptor)
        return FALSE;

    LastToken[0] = 0;

    while (fgets(Line, sizeof(Line), pDescriptor))
    {
        size_t nLine = strlen(Line);

        /* A truncated descriptor line could hide part of a track path. */
        if (nLine == sizeof(Line) - 1 &&
            Line[nLine - 1] != '\n' && !feof(pDescriptor))
        {
            fclose(pDescriptor);
            return FALSE;
        }

        Token[0] = 0;
        if (_MainLoopStateDiscExtractFileToken(
                Line, Token, sizeof(Token)))
        {
            /* Canonical FILE semantic marker; do NOT hash its path text. */
            uCRC = _MainLoopStateDiscCrcU32(uCRC, 0x46494c45u);

            /* CUEs may repeat one FILE for several contiguous TRACK entries. */
            if (strcmp(Token, LastToken))
            {
                if (nFiles >= MAX_DISC_FILES ||
                    !_MainLoopStateDiscResolveFile(
                        pDescriptorPath, Token,
                        Resolved, sizeof(Resolved)) ||
                    !_MainLoopStateDiscHashPhysicalFile(
                        Resolved, &uCRC, &uSizeSignature))
                {
                    fclose(pDescriptor);
                    return FALSE;
                }

                snprintf(LastToken, sizeof(LastToken), "%s", Token);
                ++nFiles;
            }
        }
        else
        {
            static const Uint8 Newline = '\n';

            /* Normalize only line endings. Structural CUE/TOC text remains
               part of identity, including TRACK/INDEX/PREGAP metadata. */
            while (nLine &&
                   (Line[nLine - 1] == '\r' || Line[nLine - 1] == '\n'))
                --nLine;

            uCRC = (Uint32)mz_crc32(
                uCRC, (const unsigned char *)Line, nLine);
            uCRC = (Uint32)mz_crc32(
                uCRC, (const unsigned char *)&Newline, 1);
        }
    }

    if (ferror(pDescriptor) || nFiles <= 0)
    {
        fclose(pDescriptor);
        return FALSE;
    }
    fclose(pDescriptor);

    uSizeSignature ^= (Uint32)nFiles;
    uSizeSignature *= 16777619u;
    if (!uSizeSignature)
        uSizeSignature = 1;

    *puCRC = uCRC;
    *pnBytes = uSizeSignature;
    *puFlags = uFamilyFlags;
    return TRUE;
}


static Bool _MainLoopStateGetRomIdentity(
    Uint32 *puCRC,
    Uint32 *pnBytes,
    Uint32 *puFlags)
{
    Uint8 *pRomData = NULL; /* AURORA_FCEUMM_FDS_V0_6_STATE */
    Uint32 nRomBytes;
    if (_MainLoopStateIsSgb())
    {
        Uint32 crc = _pSnes->GetSuperGameBoyGameCRC();
        Uint32 bytes = _pSnes->GetSuperGameBoyGameBytes();
        if (!bytes) return FALSE;
        _MainLoop_StateRomCRC = crc;
        _MainLoop_StateRomCRCValid = TRUE;
        *puCRC = crc; *pnBytes = bytes; *puFlags = 0x53474204u;
        return TRUE;
    }

    if (_MainLoopStateIsSwc())
    {
        Char BaseName[256];
        Uint32 uIdentity;

        if (!_MainLoopStateGetSwcBaseName(BaseName, sizeof(BaseName)))
            return FALSE;

        uIdentity = (Uint32)mz_crc32(
            MZ_CRC32_INIT,
            (const unsigned char *)BaseName,
            strlen(BaseName));

        _MainLoop_StateRomCRC = uIdentity;
        _MainLoop_StateRomCRCValid = TRUE;
        *puCRC = uIdentity;
        *pnBytes = 0x4000u;
        *puFlags = 0x53574304u;
        return TRUE;
    }


    if (_pSystem == _pSega && PicoDriveBridge_IsSegaCD())
    {
        const Char *pDiscPath = PicoDriveBridge_GetDiscPath();
        return pDiscPath &&
            _MainLoopStateGetDiscIdentity(
                pDiscPath, 0x53434401u,
                puCRC, pnBytes, puFlags);
    }

    if (_pSystem == _pPce && PceBridge_IsDiscLoaded())
    {
        const Char *pDiscPath = PceBridge_GetDiscPath();
        return pDiscPath &&
            _MainLoopStateGetDiscIdentity(
                pDiscPath, 0x50434401u,
                puCRC, pnBytes, puFlags);
    }


    if (_pSystem == _pNes)
    {
        if (!_pNesRom || !_pNesRom->IsLoaded())
        {
            return FALSE;
        }
        pRomData = _pNesRom->GetData();
        nRomBytes = _pNesRom->GetBytes();
    }
    else if (_pSystem == _pFds)
    {
        /* AURORA_FCEUMM_FDS_V0_6_STATE: full-path FDS has no frontend ROM buffer. */
        if (!_pFds || !_pFds->IsRomReady() || !_pFds->GetContentBytes())
            return FALSE;
        nRomBytes = _pFds->GetContentBytes();
        if (!_MainLoop_StateRomCRCValid)
        {
            _MainLoop_StateRomCRC = _pFds->GetContentCRC();
            _MainLoop_StateRomCRCValid = TRUE;
        }
    }
    else if (_pSystem == _pSega)
    {
        /* AURORA_PICODRIVE_STAGE2_ROM_ID */
        if (!_pSegaRom || !_pSegaRom->IsLoaded())
            return FALSE;
        pRomData = _pSegaRom->GetData();
        nRomBytes = _pSegaRom->GetBytes();
    }
    else if (_pSystem == _pPce)
    {
        if (!_pPceRom || !_pPceRom->IsLoaded()) return FALSE;
        pRomData = _pPceRom->GetData(); nRomBytes = _pPceRom->GetBytes();
    }
    else if (_pSnesRom && _pSnesRom->IsLoaded())
    {
        pRomData = _pSnesRom->GetData();
        nRomBytes = _pSnesRom->GetBytes();
    }
    else
    {
        return FALSE;
    }
    if (!nRomBytes)
    {
        return FALSE;
    }

    if (!_MainLoop_StateRomCRCValid)
    {
        /* AURORA_PS2LEAN_V2_20260824: only a non-primed fallback CRC needs raw bytes. */
        if (!pRomData)
            return FALSE;
        _MainLoop_StateRomCRC = (Uint32)mz_crc32(
            MZ_CRC32_INIT,
            pRomData,
            nRomBytes
        );
        _MainLoop_StateRomCRCValid = TRUE;
    }

    *puCRC = _MainLoop_StateRomCRC;
    *pnBytes = nRomBytes;
    if (_pSystem == _pNes)
        *puFlags = _pNesRom->GetMapperNumber();
    else if (_pSystem == _pSega || _pSystem == _pPce ||
             _pSystem == _pFds) /* AURORA_FCEUMM_FDS_V0_6_STATE */
        *puFlags = 0;
    else
        *puFlags = _pSnesRom->m_Flags;
    return TRUE;
}

static Bool _MainLoopStateIsMassRoot(const Char *pPath, Char *pRoot, Int32 nRootBytes)
{
    const Char *pColon;
    Int32 nLength;
    Int32 i;

    if (!pPath || strncmp(pPath, "mass", 4))
    {
        return FALSE;
    }

    pColon = strchr(pPath, ':');
    if (!pColon)
    {
        return FALSE;
    }

    nLength = (Int32)(pColon - pPath) + 1;
    if (nLength < 5 || nLength >= nRootBytes)
    {
        return FALSE;
    }

    for (i = 4; i < nLength - 1; i++)
    {
        if (pPath[i] < '0' || pPath[i] > '9')
        {
            return FALSE;
        }
    }

    memcpy(pRoot, pPath, nLength);
    pRoot[nLength] = 0;
    return TRUE;
}

static Bool _MainLoopStateIsNumberedRoot(
    const Char *pPath,
    const Char *pPrefix,
    Int32 nPrefixBytes,
    Char *pRoot,
    Int32 nRootBytes)
{
    const Char *pColon;
    Int32 nLength;
    Int32 i;

    if (!pPath || strncmp(pPath, pPrefix, nPrefixBytes))
    {
        return FALSE;
    }

    pColon = strchr(pPath, ':');
    if (!pColon)
    {
        return FALSE;
    }

    nLength = (Int32)(pColon - pPath) + 1;
    if (nLength <= nPrefixBytes + 1 || nLength >= nRootBytes)
    {
        return FALSE;
    }

    for (i = nPrefixBytes; i < nLength - 1; i++)
    {
        if (pPath[i] < '0' || pPath[i] > '9')
        {
            return FALSE;
        }
    }

    memcpy(pRoot, pPath, nLength);
    pRoot[nLength] = 0;
    return TRUE;
}

static Bool _MainLoopStateIsMemCardRoot(
    const Char *pPath,
    Char *pRoot,
    Int32 nRootBytes)
{
    return _MainLoopStateIsNumberedRoot(
        pPath,
        "mc",
        2,
        pRoot,
        nRootBytes
    );
}

static Bool _MainLoopStateIsMMCERoot(
    const Char *pPath,
    Char *pRoot,
    Int32 nRootBytes)
{
    return _MainLoopStateIsNumberedRoot(
        pPath,
        "mmce",
        4,
        pRoot,
        nRootBytes
    );
}

static Bool _MainLoopStateGetHddRoot(Char *pRoot, Int32 nRootBytes)
{
    Char MappedPath[1024];

    if (!_RomPath[0])
    {
        return FALSE;
    }

    if (!strncmp(_RomPath, "pfs0:", 5))
    {
        snprintf(pRoot, nRootBytes, "pfs0:");
        return TRUE;
    }

    if (strncmp(_RomPath, "hdd0:", 5) ||
        !HddSupportIsEnabled() ||
        HddLoadEmbeddedIrx() < 0)
    {
        return FALSE;
    }

    if (HddMapPath(_RomPath, MappedPath, sizeof(MappedPath)) != 1)
    {
        return FALSE;
    }

    snprintf(pRoot, nRootBytes, "pfs0:");
    return TRUE;
}

static void _MainLoopStateAddRoot(
    MainLoopStateRootT *pRoots,
    Int32 *pnRoots,
    const Char *pRoot,
    const Char *pDeviceName,
    Bool bMemCard)
{
    Int32 i;

    for (i = 0; i < *pnRoots; i++)
    {
        if (!strcmp(pRoots[i].Root, pRoot))
        {
            return;
        }
    }

    if (*pnRoots >= MAINLOOP_STATE_MAX_ROOTS)
    {
        return;
    }

    snprintf(pRoots[*pnRoots].Root, sizeof(pRoots[*pnRoots].Root), "%s", pRoot);
    snprintf(
        pRoots[*pnRoots].DeviceName,
        sizeof(pRoots[*pnRoots].DeviceName),
        "%s",
        pDeviceName
    );
    pRoots[*pnRoots].bMemCard = bMemCard;
    (*pnRoots)++;
}

static Int32 _MainLoopStateBuildRoots(
    MainLoopStateDeviceE eDevice,
    MainLoopStateRootT *pRoots)
{
    Int32 nRoots = 0;
    Char Root[16];
    Bool bAuto = eDevice == MAINLOOP_STATEDEVICE_AUTO;
    Int32 iMMCESlots = 0;

    if ((bAuto || eDevice == MAINLOOP_STATEDEVICE_MMCE) &&
        MmceSupportIsEnabled())
    {
        iMMCESlots = MmceProbeAvailableSlots();
    }

    /* Auto starts with the ROM's own device.  This also covers mass2+,
       mc2+ and future MMCE unit numbers without hard-coding them. */
    if (bAuto)
    {
        if (_MainLoopStateIsMassRoot(_RomPath, Root, sizeof(Root)))
        {
            _MainLoopStateAddRoot(pRoots, &nRoots, Root, Root, FALSE);
        }
        else if (_MainLoopStateIsMemCardRoot(_RomPath, Root, sizeof(Root)))
        {
            _MainLoopStateAddRoot(pRoots, &nRoots, Root, Root, TRUE);
        }
        else if (_MainLoopStateIsMMCERoot(_RomPath, Root, sizeof(Root)) &&
                 Root[4] >= '0' && Root[4] <= '1' &&
                 (iMMCESlots & (1 << (Root[4] - '0'))))
        {
            _MainLoopStateAddRoot(pRoots, &nRoots, Root, Root, TRUE);
        }
        else if (_MainLoopStateGetHddRoot(Root, sizeof(Root)))
        {
            _MainLoopStateAddRoot(
                pRoots,
                &nRoots,
                Root,
                "Internal HDD",
                FALSE
            );
        }
    }
    else if (eDevice == MAINLOOP_STATEDEVICE_USB &&
             _MainLoopStateIsMassRoot(_RomPath, Root, sizeof(Root)))
    {
        _MainLoopStateAddRoot(pRoots, &nRoots, Root, Root, FALSE);
    }
    else if (eDevice == MAINLOOP_STATEDEVICE_MEMCARD &&
             _MainLoopStateIsMemCardRoot(_RomPath, Root, sizeof(Root)))
    {
        _MainLoopStateAddRoot(pRoots, &nRoots, Root, Root, TRUE);
    }
    else if (eDevice == MAINLOOP_STATEDEVICE_MMCE &&
             _MainLoopStateIsMMCERoot(_RomPath, Root, sizeof(Root)) &&
             Root[4] >= '0' && Root[4] <= '1' &&
             (iMMCESlots & (1 << (Root[4] - '0'))))
    {
        _MainLoopStateAddRoot(pRoots, &nRoots, Root, Root, TRUE);
    }
    else if (eDevice == MAINLOOP_STATEDEVICE_HDD &&
             _MainLoopStateGetHddRoot(Root, sizeof(Root)))
    {
        _MainLoopStateAddRoot(
            pRoots,
            &nRoots,
            Root,
            "Internal HDD",
            FALSE
        );
    }

    if (bAuto || eDevice == MAINLOOP_STATEDEVICE_USB)
    {
        _MainLoopStateAddRoot(pRoots, &nRoots, "mass0:", "mass0:", FALSE);
        _MainLoopStateAddRoot(pRoots, &nRoots, "mass1:", "mass1:", FALSE);
        _MainLoopStateAddRoot(pRoots, &nRoots, "mass:", "mass:", FALSE);
    }

    if (bAuto || eDevice == MAINLOOP_STATEDEVICE_MEMCARD)
    {
        _MainLoopStateAddRoot(pRoots, &nRoots, "mc0:", "mc0:", TRUE);
        _MainLoopStateAddRoot(pRoots, &nRoots, "mc1:", "mc1:", TRUE);
    }

    if (bAuto || eDevice == MAINLOOP_STATEDEVICE_MMCE)
    {
        if (iMMCESlots & 1)
            _MainLoopStateAddRoot(pRoots, &nRoots, "mmce0:", "mmce0:", TRUE);
        if (iMMCESlots & 2)
            _MainLoopStateAddRoot(pRoots, &nRoots, "mmce1:", "mmce1:", TRUE);
    }

    return nRoots;
}

static Bool _MainLoopStateEnsureOneDir(const Char *pPath)
{
    struct stat Status;

    if (mkdir(pPath, 0777) == 0 || errno == EEXIST)
    {
        return TRUE;
    }

    return stat(pPath, &Status) == 0 && S_ISDIR(Status.st_mode);
}

static Bool _MainLoopStateEnsureRoot(const MainLoopStateRootT *pRoot)
{
    Char Path[1024];

    /* MMCE also uses the short memory-card filename rules, hence
       bMemCard, but only real mcN: roots have PS2-card format state. */
    if (pRoot->Root[0] == 'm' &&
        pRoot->Root[1] == 'c' &&
        pRoot->Root[2] >= '0' &&
        pRoot->Root[2] <= '1' &&
        pRoot->Root[3] == ':')
    {
        Int32 iPort = pRoot->Root[2] - '0';
        MemCardStatusE eStatus = MemCardGetStatus(iPort);

        if (eStatus == MEMCARD_STATUS_UNFORMATTED)
        {
            if (_MainLoop_StateUnformattedCard < 0)
            {
                _MainLoop_StateUnformattedCard = iPort;
            }
            return FALSE;
        }
        /* For READY, absent and unknown results, retain the established
           mkdir/stat write probe below. It is the compatibility fallback
           for unusual drivers that support stdio but not GetStat on "/". */
    }

    snprintf(Path, sizeof(Path), "%s/SNESticle", pRoot->Root);
    if (!_MainLoopStateEnsureOneDir(Path))
    {
        return FALSE;
    }

    if (!pRoot->bMemCard)
    {
        snprintf(Path, sizeof(Path), "%s/SNESticle/states", pRoot->Root);
        if (!_MainLoopStateEnsureOneDir(Path))
        {
            return FALSE;
        }
    }

    return TRUE;
}

/* AURORA_CD_STATE_V1_SAFE_20260903
 * One-character bank class remains compatible with the existing filename
 * layout while preventing CD banks from colliding with cartridge banks. */
static Char _MainLoopStateGetBankClass()
{
    if (_pSystem == _pNes) return 'n';
    if (_pSystem == _pFds) return 'f';
    if (_pSystem == _pSega)
        return PicoDriveBridge_IsSegaCD() ? 'c' : 'g';
    if (_pSystem == _pPce && PceBridge_IsDiscLoaded())
        return 'p';
    if (_MainLoopStateIsSwc()) return 'w';
    return 's';
}

static void _MainLoopStateBuildBankPath(
    const MainLoopStateRootT *pRoot,
    Int32 iSlot,
    Int32 iBank,
    Char *pPath,
    Int32 nPathBytes)
{
    Char SaveName[256];
    Char Directory[256];
    Int32 nMaxName;

    if (pRoot->bMemCard)
    {
        snprintf(Directory, sizeof(Directory), "%s/SNESticle", pRoot->Root);
    }
    else
    {
        snprintf(Directory, sizeof(Directory), "%s/SNESticle/states", pRoot->Root);
    }

    nMaxName = PathGetMaxFileNameLength(Directory) - 4;
    if (_MainLoopStateIsSwc())
    {
        Char SwcName[256];
        if (!_MainLoopStateGetSwcBaseName(SwcName, sizeof(SwcName)))
            snprintf(SwcName, sizeof(SwcName), "%s", "Super Wild Card");
        PathTruncFileName(SaveName, SwcName, nMaxName);
    }
    else
    {
        PathTruncFileName(SaveName, _RomName, nMaxName);
    }
    snprintf(
        pPath,
        nPathBytes,
        "%s/%s.%c%d%c",
        Directory,
        SaveName,
        _MainLoopStateGetBankClass(),
        iSlot + 1,
        iBank ? 'b' : 'a'
    );
}

/* Header result: 1 = valid/current ROM, 0 = missing,
   -1 = incomplete/corrupt, -2 = valid format but another ROM. */
static Int32 _MainLoopStateReadHeader(
    const Char *pPath,
    Int32 iSlot,
    Uint32 uRomCRC,
    Uint32 nRomBytes,
    Uint32 uRomFlags,
    MainLoopStateFileHeaderT *pHeader)
{
    FILE *pFile;
    size_t nRead;
    Bool bPayloadLayoutValid;
    Uint32 nExpectedPayloadBytes = _MainLoopStateGetPayloadBytes();
    Uint32 uExpectedSystem = _MainLoopStateGetSystemId();

    pFile = fopen(pPath, "rb");
    if (!pFile)
    {
        return 0;
    }

    nRead = fread(pHeader, 1, sizeof(*pHeader), pFile);
    fclose(pFile);
    if (nRead != sizeof(*pHeader))
    {
        return -1;
    }

    bPayloadLayoutValid =
        (pHeader->Reserved[0] == MAINLOOP_STATE_PAYLOAD_RAW &&
         pHeader->nPayloadBytes == nExpectedPayloadBytes) ||
        (pHeader->Reserved[0] == MAINLOOP_STATE_PAYLOAD_DEFLATE &&
         pHeader->nPayloadBytes > 0 &&
         pHeader->nPayloadBytes <= _MainLoopStateCompressedLimit(nExpectedPayloadBytes));

    if (memcmp(pHeader->Magic, _MainLoop_StateMagic, sizeof(pHeader->Magic)) ||
        pHeader->uVersion != MAINLOOP_STATE_FORMAT_VERSION ||
        pHeader->nHeaderBytes != sizeof(*pHeader) ||
        !bPayloadLayoutValid ||
        pHeader->Reserved[2] != uExpectedSystem ||
        pHeader->iSlot != (Uint32)iSlot)
    {
        return -1;
    }

    if (pHeader->uRomCRC != uRomCRC ||
        pHeader->nRomBytes != nRomBytes ||
        pHeader->uRomFlags != uRomFlags)
    {
        return -2;
    }

    return 1;
}

static Bool _MainLoopStateReadPayload(
    const Char *pPath,
    const MainLoopStateFileHeaderT *pExpectedHeader)
{
    MainLoopStateFileHeaderT Header;
    FILE *pFile;
    size_t nRead;
    Uint32 uCRC;
    Bool bDecoded = FALSE;
    Uint8 *pStateData = _MainLoopStateGetPayloadData();
    Uint32 nStateBytes = _MainLoopStateGetPayloadBytes();

    /* AURORA_PICODRIVE_STAGE2_STATE_DATA_GUARD */
    if (!pStateData || !nStateBytes)
        return FALSE;

    pFile = fopen(pPath, "rb");
    if (!pFile)
    {
        return FALSE;
    }

    nRead = fread(&Header, 1, sizeof(Header), pFile);
    if (nRead != sizeof(Header) ||
        memcmp(&Header, pExpectedHeader, sizeof(Header)))
    {
        fclose(pFile);
        return FALSE;
    }

    if (Header.Reserved[0] == MAINLOOP_STATE_PAYLOAD_RAW)
    {
        nRead = fread(pStateData, 1, nStateBytes, pFile);
        bDecoded = nRead == nStateBytes;
    }
    else if (Header.Reserved[0] == MAINLOOP_STATE_PAYLOAD_DEFLATE)
    {
        /* AURORA_PICODRIVE_STAGE2_READ_COMPRESSED */
        mz_ulong nDecodedBytes = nStateBytes;
        Uint32 nCompressedCapacity = 0;
        Uint8 *pCompressed = _MainLoopStateGetCompressedBuffer(
            Header.nPayloadBytes, &nCompressedCapacity);

        if (pCompressed && Header.nPayloadBytes <= nCompressedCapacity)
        {
            nRead = fread(
                pCompressed,
                1,
                Header.nPayloadBytes,
                pFile
            );
            if (nRead == Header.nPayloadBytes &&
                mz_uncompress(
                    pStateData,
                    &nDecodedBytes,
                    pCompressed,
                    Header.nPayloadBytes
                ) == MZ_OK &&
                nDecodedBytes == nStateBytes)
            {
                bDecoded = TRUE;
            }
        }
    }

    fclose(pFile);
    if (!bDecoded)
    {
        return FALSE;
    }

    uCRC = (Uint32)mz_crc32(
        MZ_CRC32_INIT,
        pStateData,
        nStateBytes
    );
    return uCRC == Header.uPayloadCRC;
}

static Bool _MainLoopStateWriteBank(
    const Char *pPath,
    MainLoopStateFileHeaderT *pHeader,
    const Uint8 *pPayload,
    Uint32 nPayloadBytes)
{
    MainLoopStateFileHeaderT PendingHeader;
    FILE *pFile;
    Bool bOK = TRUE;

    PendingHeader = *pHeader;
    memset(PendingHeader.Magic, 0, sizeof(PendingHeader.Magic));

    pFile = fopen(pPath, "wb");
    if (!pFile)
    {
        return FALSE;
    }

    if (fwrite(&PendingHeader, 1, sizeof(PendingHeader), pFile) != sizeof(PendingHeader) ||
        fwrite(pPayload, 1, nPayloadBytes, pFile) != nPayloadBytes ||
        fflush(pFile) != 0 ||
        fseek(pFile, 0, SEEK_SET) != 0 ||
        fwrite(pHeader, 1, sizeof(*pHeader), pFile) != sizeof(*pHeader) ||
        fflush(pFile) != 0)
    {
        bOK = FALSE;
    }

    if (fclose(pFile) != 0)
    {
        bOK = FALSE;
    }

    return bOK;
}

static Bool _MainLoopStateGenerationNewer(Uint32 uA, Uint32 uB)
{
    return (Int32)(uA - uB) > 0;
}

static Int32 _MainLoopStateScanCandidates(
    MainLoopStateDeviceE eDevice,
    Int32 iSlot,
    Uint32 uRomCRC,
    Uint32 nRomBytes,
    Uint32 uRomFlags,
    Bool *pbWrongRom,
    Bool *pbCorrupt)
{
    MainLoopStateRootT Roots[MAINLOOP_STATE_MAX_ROOTS];
    Int32 nRoots;
    Int32 nCandidates = 0;
    Int32 iRoot;
    Int32 iBank;

    nRoots = _MainLoopStateBuildRoots(eDevice, Roots);
    for (iRoot = 0; iRoot < nRoots; iRoot++)
    {
        for (iBank = 0; iBank < MAINLOOP_STATE_BANK_NUM; iBank++)
        {
            MainLoopStateFileHeaderT Header;
            Char Path[1024];
            Int32 Result;

            _MainLoopStateBuildBankPath(
                &Roots[iRoot],
                iSlot,
                iBank,
                Path,
                sizeof(Path)
            );
            Result = _MainLoopStateReadHeader(
                Path,
                iSlot,
                uRomCRC,
                nRomBytes,
                uRomFlags,
                &Header
            );

            if (Result == 1 && nCandidates < MAINLOOP_STATE_MAX_CANDIDATES)
            {
                MainLoopStateCandidateT *pCandidate =
                    &_MainLoop_StateCandidates[nCandidates++];
                snprintf(pCandidate->Path, sizeof(pCandidate->Path), "%s", Path);
                snprintf(
                    pCandidate->DeviceName,
                    sizeof(pCandidate->DeviceName),
                    "%s",
                    Roots[iRoot].DeviceName
                );
                pCandidate->Header = Header;
            }
            else if (Result == -2)
            {
                *pbWrongRom = TRUE;
            }
            else if (Result == -1)
            {
                *pbCorrupt = TRUE;
            }
        }
    }

    return nCandidates;
}

static void _MainLoopStateSortCandidates(Int32 nCandidates)
{
    Int32 i;
    Int32 j;

    for (i = 0; i < nCandidates; i++)
    {
        for (j = i + 1; j < nCandidates; j++)
        {
            if (_MainLoopStateGenerationNewer(
                    _MainLoop_StateCandidates[j].Header.uGeneration,
                    _MainLoop_StateCandidates[i].Header.uGeneration))
            {
                MainLoopStateCandidateT Temp = _MainLoop_StateCandidates[i];
                _MainLoop_StateCandidates[i] = _MainLoop_StateCandidates[j];
                _MainLoop_StateCandidates[j] = Temp;
            }
        }
    }
}

Bool _MainLoopLoadState()
{
    MainLoopSegaStateScratchGuard SegaScratchGuard;
    Char Reason[192];
    Uint32 uRomCRC;
    Uint32 nRomBytes;
    Uint32 uRomFlags;
    Bool bWrongRom = FALSE;
    Bool bCorrupt = FALSE;
    Int32 nCandidates;
    Int32 iCandidate;

    _bStateSaved = FALSE;

    if (!_MainLoopStateCheckAvailability(Reason, sizeof(Reason)))
    {
        _MainLoopStateSetMessage("%s", Reason);
        return FALSE;
    }

    if (!_MainLoopStateGetRomIdentity(&uRomCRC, &nRomBytes, &uRomFlags))
    {
        _MainLoopStateSetMessage("Cannot identify the loaded ROM.");
        return FALSE;
    }

    nCandidates = _MainLoopStateScanCandidates(
        _MainLoop_StateDevice,
        _MainLoop_StateSlot,
        uRomCRC,
        nRomBytes,
        uRomFlags,
        &bWrongRom,
        &bCorrupt
    );
    _MainLoopStateSortCandidates(nCandidates);

    for (iCandidate = 0; iCandidate < nCandidates; iCandidate++)
    {
        MainLoopStateCandidateT *pCandidate =
            &_MainLoop_StateCandidates[iCandidate];

        Bool bPayloadOK = _MainLoopStateReadPayload(
            pCandidate->Path,
            &pCandidate->Header
        );
        Bool bRestoreOK = FALSE;
        if (bPayloadOK)
        {
            /* AURORA_PICODRIVE_STAGE2_STATE_RESTORE */
            if (_pSystem == _pNes)
                bRestoreOK = _pNes->RestoreState(&_NesState);
            else if (_pSystem == _pFds)
            {
                /* AURORA_FCEUMM_FDS_V0_6_STATE */
                Uint8 *pFdsStateData = _MainLoopStateGetPayloadData();
                Uint32 nFdsStateBytes = _MainLoopStateGetPayloadBytes();
                bRestoreOK = pFdsStateData && nFdsStateBytes &&
                    _pFds->RestoreStateChecked(pFdsStateData, (Int32)nFdsStateBytes);
            }
            else if (_pSystem == _pSega)
            {
                Uint8 *pSegaStateData = _MainLoopStateGetPayloadData();
                Uint32 nSegaStateBytes = _MainLoopStateGetPayloadBytes();
                bRestoreOK = pSegaStateData && nSegaStateBytes &&
                    _pSega->RestoreStateChecked(pSegaStateData, (Int32)nSegaStateBytes);
            }
            else if (_pSystem == _pPce)
            {
                Uint8 *pPceStateData = _MainLoopStateGetPayloadData();
                Uint32 nPceStateBytes = _MainLoopStateGetPayloadBytes();
                bRestoreOK = pPceStateData && nPceStateBytes &&
                    _pPce->RestoreStateChecked(pPceStateData, (Int32)nPceStateBytes);
            }
            else if (_MainLoopStateIsSwc())
            {
                Uint8 *pSwcStateData = _MainLoopStateGetPayloadData();
                Uint32 nSwcStateBytes = _MainLoopStateGetPayloadBytes();
                bRestoreOK = pSwcStateData && nSwcStateBytes &&
                    _pSnes->RestoreStateChecked(
                        pSwcStateData, (Int32)nSwcStateBytes);
            }
            else
                bRestoreOK = _pSnes->RestoreState(&_SnesState);
        }

        if (bRestoreOK)
        {
            Int32 nSramBytes = _pSystem->GetSRAMBytes();

            _bStateSaved = TRUE;
            _MainLoop_SaveCounter = 0;
            if (nSramBytes > 0)
            {
                _MainLoop_SRAMChecksum = _CalcChecksum(
                    (Uint32 *)_pSystem->GetSRAMData(),
                    nSramBytes / 4
                );
                _MainLoop_SRAMUpdated = TRUE;
            }

            if (_AudMix)
            {
                _AudMix->Reset();
            }

            _MainLoopResetInputChecksums();
#if MAINLOOP_HISTORY
            _MainLoopResetHistory();
#endif

            _MainLoopStateSetMessage(
                "Loaded slot %d from %s.",
                _MainLoop_StateSlot + 1,
                pCandidate->DeviceName
            );
            ConPrint("State loaded: %s\n", pCandidate->Path);
            ML_TRACE("State load ok: %s", pCandidate->Path);
            return TRUE;
        }

        bCorrupt = TRUE;
    }

    if (bCorrupt)
    {
        _MainLoopStateSetMessage(
            "Slot %d is incomplete or corrupt.",
            _MainLoop_StateSlot + 1
        );
    }
    else if (bWrongRom)
    {
        _MainLoopStateSetMessage(
            "Slot %d belongs to another ROM.",
            _MainLoop_StateSlot + 1
        );
    }
    else
    {
        _MainLoopStateSetMessage(
            "No state found in slot %d.",
            _MainLoop_StateSlot + 1
        );
    }

    ML_TRACE("State load failed: %s", _MainLoop_StateLastMessage);
    return FALSE;
}

Bool _MainLoopSaveState()
{
    MainLoopSegaStateScratchGuard SegaScratchGuard;
    Char Reason[192];
    Uint32 uRomCRC;
    Uint32 nRomBytes;
    Uint32 uRomFlags;
    Uint32 uGeneration = 1;
    Uint32 uPayloadCRC;
    Uint32 uStoredCRC;
    Uint32 nPayloadBytes;
    Uint32 ePayloadEncoding;
    const Uint8 *pPayload;
    mz_ulong nCompressedBytes;
    Bool bWrongRom = FALSE;
    Bool bCorrupt = FALSE;
    Int32 nAllCandidates;
    Int32 i;
    MainLoopStateRootT Roots[MAINLOOP_STATE_MAX_ROOTS];
    Int32 nRoots;
    Int32 iRoot;
    Uint8 *pStateData;
    Uint32 nStateBytes;

    _bStateSaved = FALSE;
    _MainLoop_StateUnformattedCard = -1;

    if (!_MainLoopStateCheckAvailability(Reason, sizeof(Reason)))
    {
        _MainLoopStateSetMessage("%s", Reason);
        return FALSE;
    }

    if (!_MainLoopStateGetRomIdentity(&uRomCRC, &nRomBytes, &uRomFlags))
    {
        _MainLoopStateSetMessage("Cannot identify the loaded ROM.");
        return FALSE;
    }

    /* Scan the selected target set for its next generation.  Keeping this
       scoped avoids loading optional MMCE/HDD modules when the user chose
       an unrelated explicit target such as USB. */
    nAllCandidates = _MainLoopStateScanCandidates(
        _MainLoop_StateDevice,
        _MainLoop_StateSlot,
        uRomCRC,
        nRomBytes,
        uRomFlags,
        &bWrongRom,
        &bCorrupt
    );
    for (i = 0; i < nAllCandidates; i++)
    {
        Uint32 uCandidateGeneration =
            _MainLoop_StateCandidates[i].Header.uGeneration;
        if (uGeneration == 1 ||
            _MainLoopStateGenerationNewer(uCandidateGeneration + 1, uGeneration))
        {
            uGeneration = uCandidateGeneration + 1;
            if (!uGeneration)
            {
                uGeneration = 1;
            }
        }
    }

    nRoots = _MainLoopStateBuildRoots(_MainLoop_StateDevice, Roots);

    /* Snapshot and compress exactly once while the game is paused. Older
       code repeated SaveState for every fallback root, then wrote the full
       ~500 KB structure. Fast deflate substantially cuts slow memory-card
       I/O while keeping the on-disk format backward compatible: version-1
       raw banks still load, and Reserved[0] advertises compressed banks. */
    pStateData = _MainLoopStateGetPayloadData();
    nStateBytes = _MainLoopStateGetPayloadBytes();
    if (!pStateData || !nStateBytes)
    {
        _MainLoopStateSetMessage(
            "Could not allocate core state buffer (%u bytes).",
            (unsigned)nStateBytes);
        return FALSE;
    }
    if (_pSystem == _pNes)
    {
        _pNes->SaveState(&_NesState);
        /* SNESTICLE_NES_CORE_STATE_MAGIC
         * Do not hard-code InfoNES's NSST payload magic here. Every NesSystem
         * implementation owns and validates its inner state format. Both the
         * InfoNES and QuickNES wrappers memset the envelope to zero first and
         * write a non-zero magic only after a complete snapshot succeeds. */
        if (_NesState.uMagic == 0)
        {
            _MainLoopStateSetMessage("Could not snapshot the NES core state.");
            return FALSE;
        }
    }
    else if (_pSystem == _pFds)
    {
        /* AURORA_FCEUMM_FDS_V0_6_STATE */
        if (!_pFds->SaveStateChecked(pStateData, (Int32)nStateBytes))
        {
            _MainLoopStateSetMessage("Could not snapshot the FCEUmm FDS state.");
            return FALSE;
        }
    }
    else if (_pSystem == _pSega)
    {
        /* AURORA_PICODRIVE_STAGE2_STATE_SAVE */
        if (!_pSega->SaveStateChecked(pStateData, (Int32)nStateBytes))
        {
            _MainLoopStateSetMessage("Could not snapshot the PicoDrive state.");
            return FALSE;
        }
    }
    else if (_pSystem == _pPce)
    {
        if (!_pPce->SaveStateChecked(pStateData, (Int32)nStateBytes))
        {
            _MainLoopStateSetMessage("Could not snapshot the Beetle PCE Fast state.");
            return FALSE;
        }
    }
    else if (_MainLoopStateIsSwc() || _MainLoopStateIsSgb())
    {
        if (!_pSnes->SaveStateChecked(pStateData, (Int32)nStateBytes))
        {
            _MainLoopStateSetMessage(
                "Could not snapshot the Super Wild Card state.");
            return FALSE;
        }
    }
    else
    {
        _pSnes->SaveState(&_SnesState);
    }
    uPayloadCRC = (Uint32)mz_crc32(
        MZ_CRC32_INIT,
        pStateData,
        nStateBytes
    );

    pPayload = pStateData;
    nPayloadBytes = nStateBytes;
    ePayloadEncoding = MAINLOOP_STATE_PAYLOAD_RAW;
    {
        /* AURORA_PICODRIVE_STAGE2_SAVE_COMPRESSED */
        Uint32 nCompressionCapacity = _MainLoopStateCompressedLimit(nStateBytes);
        Uint32 nActualCapacity = 0;
        Uint8 *pCompression = _MainLoopStateGetCompressedBuffer(
            nCompressionCapacity, &nActualCapacity);
        nCompressedBytes = nCompressionCapacity;
        if (pCompression && nCompressionCapacity > 0 &&
            nActualCapacity >= nCompressionCapacity &&
            mz_compress2(
                pCompression,
                &nCompressedBytes,
                pStateData,
                nStateBytes,
                MZ_BEST_SPEED) == MZ_OK &&
            nCompressedBytes < nStateBytes)
        {
            pPayload = pCompression;
            nPayloadBytes = (Uint32)nCompressedBytes;
            ePayloadEncoding = MAINLOOP_STATE_PAYLOAD_DEFLATE;
        }
    }
    uStoredCRC = (Uint32)mz_crc32(
        MZ_CRC32_INIT,
        pPayload,
        nPayloadBytes
    );

    ML_TRACE(
        "State payload: raw=%u stored=%u encoding=%s",
        (unsigned int)nStateBytes,
        (unsigned int)nPayloadBytes,
        ePayloadEncoding == MAINLOOP_STATE_PAYLOAD_DEFLATE
            ? "deflate"
            : "raw"
    );

    for (iRoot = 0; iRoot < nRoots; iRoot++)
    {
        MainLoopStateFileHeaderT BankHeader[MAINLOOP_STATE_BANK_NUM];
        Int32 BankResult[MAINLOOP_STATE_BANK_NUM];
        Int32 iBank;
        Int32 iTargetBank;
        Char Path[1024];
        MainLoopStateFileHeaderT Header;

        if (!_MainLoopStateEnsureRoot(&Roots[iRoot]))
        {
            continue;
        }

        /* The generation scan above already opened every candidate header.
           Reuse those copies here instead of opening both target files a
           second time -- directory and file-open latency is noticeable on
           a PS2 memory card even when only 64 bytes are read. */
        for (iBank = 0; iBank < MAINLOOP_STATE_BANK_NUM; iBank++)
        {
            _MainLoopStateBuildBankPath(
                &Roots[iRoot],
                _MainLoop_StateSlot,
                iBank,
                Path,
                sizeof(Path)
            );
            BankResult[iBank] = 0;
            for (i = 0; i < nAllCandidates; i++)
            {
                if (!strcmp(Path, _MainLoop_StateCandidates[i].Path))
                {
                    BankHeader[iBank] =
                        _MainLoop_StateCandidates[i].Header;
                    BankResult[iBank] = 1;
                    break;
                }
            }
        }

        if (BankResult[0] == 1 && BankResult[1] == 1)
        {
            /* Overwrite the older bank and preserve the newest one. CRC is
               checked when loading, where a damaged newest bank naturally
               falls back to the other bank. Avoiding a full pre-save read
               is the main latency win on mc0:/mc1:. */
            iTargetBank = _MainLoopStateGenerationNewer(
                BankHeader[0].uGeneration,
                BankHeader[1].uGeneration
            ) ? 1 : 0;
        }
        else if (BankResult[0] == 1)
        {
            iTargetBank = 1;
        }
        else
        {
            iTargetBank = 0;
        }

        _MainLoopStateBuildBankPath(
            &Roots[iRoot],
            _MainLoop_StateSlot,
            iTargetBank,
            Path,
            sizeof(Path)
        );

        memset(&Header, 0, sizeof(Header));
        memcpy(Header.Magic, _MainLoop_StateMagic, sizeof(Header.Magic));
        Header.uVersion = MAINLOOP_STATE_FORMAT_VERSION;
        Header.nHeaderBytes = sizeof(Header);
        Header.nPayloadBytes = nPayloadBytes;
        Header.uPayloadCRC = uPayloadCRC;
        Header.uRomCRC = uRomCRC;
        Header.nRomBytes = nRomBytes;
        Header.uRomFlags = uRomFlags;
        Header.iSlot = (Uint32)_MainLoop_StateSlot;
        Header.uGeneration = uGeneration;
        Header.Reserved[0] = ePayloadEncoding;
        Header.Reserved[1] =
            ePayloadEncoding == MAINLOOP_STATE_PAYLOAD_DEFLATE
                ? uStoredCRC
                : 0;
        Header.Reserved[2] = _MainLoopStateGetSystemId();

        ML_TRACE("State save path: %s", Path);
        if (_MainLoopStateWriteBank(
                Path,
                &Header,
                pPayload,
                nPayloadBytes))
        {
            _bStateSaved = TRUE;
            _MainLoopStateSetMessage(
                "Saved slot %d to %s.",
                _MainLoop_StateSlot + 1,
                Roots[iRoot].DeviceName
            );
            ConPrint("State saved: %s\n", Path);
            ML_TRACE("State save ok: %s", Path);
            return TRUE;
        }

        ML_TRACE("State save failed: %s", Path);
    }

    if (_MainLoop_StateUnformattedCard >= 0)
    {
        _MainLoopStateSetMessage(
            "mc%d: is not formatted.",
            _MainLoop_StateUnformattedCard
        );
    }
    else
    {
        _MainLoopStateSetMessage(
            "Could not save slot %d to %s.",
            _MainLoop_StateSlot + 1,
            MainLoopStateGetDeviceName()
        );
    }
    return FALSE;
}


void _MainLoopResetHistory()
{
#if MAINLOOP_HISTORY
    _nHistory = 0;
#endif
}


void _MainLoopResetInputChecksums()
{
	_uInputFrame =0;
	memset(_uInputChecksum, 0, sizeof(_uInputChecksum));
}

#if MAINLOOP_HISTORY
Uint32 _History[16384 * 2];
Uint32 _nHistory = 0;
#endif

#if MAINLOOP_HISTORY

void _MainLoopSaveHistory()
{
    FileWriteMem("host:game.hst", _History, _nHistory * sizeof(Uint32));
    printf("History written\n");
}
#endif

