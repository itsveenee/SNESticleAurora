
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "snrom.h"
#include "snio.h"
#include "dataio.h"
#include "sndbglog.h"
Uint32 g_FakeSRAMSize = 0;
SnesForceRegionE g_SnesForceRegion = SNES_FORCE_REGION_OFF;

/* AURORA_SONIC_BLAST_MAN_COLOR_V7
 * Runtime-only renderer compatibility flag. It is reset on every ROM load.
 * No Sonic Blast Man ROM byte is modified. */
Bool g_SnesCompatSonicBlastManColorMath = FALSE;

/* AURORA_CRC_ZERO_INIT_DB_V8
 * Set from the normalized/headerless ROM CRC before any in-memory ROM patch.
 * SnesSystem consumes it only while attaching a new cartridge. */
Bool g_SnesCompatZeroInit = FALSE;

/* AURORA_HK97_SPC_BOOT_DB_V9
 * Exact-CRC runtime flag for Hong Kong '97. This does not patch ROM bytes.
 * Both known 512 KiB images are accepted:
 *   11A6E64B = standard PD image
 *   C6A95816 = CM variant
 */
Bool g_SnesCompatHongKong97SPCBoot = FALSE;

/* AURORA_TOP_GEAR_FASTROM_V1
 * Exact clean normalized/headerless ROMs only. No ROM bytes are modified. */
Bool g_SnesCompatTopGearFastRom = FALSE;


/* AURORA_SUNSET_RIDERS_CRC_OBJ128_V2_ROM_20260825
 * Diagnostic-only exact-CRC flag. CRC is from normalized/headerless ROM data.
 * No ROM bytes are modified.
 *   52ADA404 = Sunset Riders (USA)
 *   64EDFC5D = Sunset Riders (Europe) */
Bool g_SnesCompatSunsetRidersObj128 = FALSE;

/* AURORA_SWC_FLOPPY_V1_20260831
 * Sunset Riders OBJ128 experiment did not fix the reported graphics.
 * Preserve it for A/B work but keep release/default builds off. */
#ifndef SNES_SUNSET_RIDERS_OBJ128_HACK
#define SNES_SUNSET_RIDERS_OBJ128_HACK 0
#endif

/* AURORA_SWC_MEGA_V9_20260831
 * SnesRom::LoadRom() selects exact-CRC compatibility/accessory globals for a
 * normal cartridge boot. A ROM merely exposed behind the SWC is not the
 * running SNES cartridge, so clear those parser-global selections again.
 */
void SnesRomResetRuntimeCompatForExternalDevice(void)
{
    SnesTurboFileSelectForCRC(0);
    g_SnesCompatSonicBlastManColorMath = FALSE;
    g_SnesCompatZeroInit = FALSE;
    g_SnesCompatHongKong97SPCBoot = FALSE;
    g_SnesCompatTopGearFastRom = FALSE;
    g_SnesCompatSunsetRidersObj128 = FALSE;
}

static Uint32 _SNRomRuntimeCRC32(const Uint8 *pData, Uint32 nBytes)
{
    Uint32 crc = 0xFFFFFFFFu;
    while (nBytes--)
    {
        crc ^= *pData++;
        for (Uint32 bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

#ifndef SNES_HK97_SPC_BOOT
#define SNES_HK97_SPC_BOOT 0
#endif

#if SNES_HK97_SPC_BOOT
static Uint32 _SNRomHK97CRC32(const Uint8 *pData, Uint32 nBytes)
{
    Uint32 crc = 0xFFFFFFFFu;
    while (nBytes--)
    {
        crc ^= *pData++;
        for (Uint32 bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}
#endif

/* AURORA_CRC_COMPAT_DB_V6
 * Tiny CRC-gated ROM compatibility database.
 *
 * Sources represented here:
 *   - SNESAdvance SuperDAT ordinary-byte patches for Lost Vikings 1 (USA)
 *     and The Addams Family (USA).
 *   - GameHacking.org-generated Lost Vikings 2 cheat data; reference emulator's standard
 *     SNES Game Genie decoder maps E1E7-CFD4 -> $80FA34=F6 and
 *     DDE7-CF04 -> $80FA35=00. For normal LoROM $80:FA34 maps to file $7A34.
 *   - No-Intro 2026-08-01 CRC32 values identify clean regional layouts.
 *
 * Cross-region entries intentionally reuse the same payload/offsets. They are
 * exact-CRC isolated and can all be disabled with SNES_ROM_COMPAT_REGIONAL=0.
 */
#ifndef SNES_ROM_COMPAT_PATCHES
#define SNES_ROM_COMPAT_PATCHES 0
#endif
#ifndef SNES_ROM_COMPAT_REGIONAL
#define SNES_ROM_COMPAT_REGIONAL 1
#endif
#ifndef SNES_CRC_ZERO_INIT
#define SNES_CRC_ZERO_INIT 0
#endif
#ifndef SNES_TOP_GEAR_FASTROM_HACK
#define SNES_TOP_GEAR_FASTROM_HACK 0
#endif

#if SNES_CRC_ZERO_INIT || SNES_TOP_GEAR_FASTROM_HACK
/* AURORA_CRC_ZERO_INIT_DB_V8
 * Exact CRC32 allow-list. CRC is computed after copier-header removal /
 * deinterleaving and before V6 mutates any ROM bytes.
 *
 * Included:
 *   Hong Kong '97 (standard + CM)
 *   Lost Vikings 1 (U/E/F/G/S/J)
 *   Lost Vikings 2 (U/E)
 *   The Addams Family (U/E/J)
 *   Sonic Blast Man (U/E/J)
 *   Speedy Gonzales - Los Gatos Bandidos (USA + Rev 1)
 *   Top Gear / Top Racer (U/E/J)
 *   Super Mario World 2 - Yoshi's Island / Yossy Island (U/E/J revisions)
 *
 * Pilotwings is intentionally NOT included:
 *   USA 266C44ED, Japan 77871727, Europe DEF45776.
 */
static Uint32 _SNRomZeroInitCRC32(const Uint8 *pData, Uint32 nBytes)
{
    Uint32 crc = 0xFFFFFFFFu;
    while (nBytes--)
    {
        crc ^= *pData++;
        for (Uint32 bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

static Bool _SNRomNeedsZeroInit(Uint32 crc)
{
    switch (crc)
    {
        /* Hong Kong '97 */
        case 0x11A6E64Bu:
        case 0xC6A95816u:

        /* The Lost Vikings 1 */
        case 0x6838BE08u:
        case 0x66989491u:
        case 0x94093298u:
        case 0x4EE705ABu:
        case 0x6E8A1081u:
        case 0x50FEF979u:

        /* The Lost Vikings 2 / Norse by Norsewest */
        case 0x3AA01DBDu:
        case 0x3C49A285u:

        /* The Addams Family */
        case 0x2E8034ABu:
        case 0x82497F19u:
        case 0xCD80351Cu:

        /* Sonic Blast Man */
        case 0x8886396Eu:
        case 0x5441F25Bu:
        case 0xBE523800u:

        /* Speedy Gonzales - Los Gatos Bandidos */
        case 0xE2DBAD76u:
        case 0xCB0653D0u:

        /* Top Gear / Top Racer */
        case 0xD34C49B7u:
        case 0xB0150052u:
        case 0xE5A57B12u:

        /* Super Mario World 2 - Yoshi's Island */
        case 0xD138F224u:
        case 0xCF98DDAAu:
        case 0x857980B2u:
        case 0x07E01EF9u:
        case 0x343E0DFBu:
        case 0x0F66698Bu:
        case 0xF1063FADu:
            return TRUE;

        default:
            return FALSE;
    }
}
#endif

#if SNES_ROM_COMPAT_PATCHES
struct SNRomCompatWriteT
{
    Uint32 Offset;
    Uint8 Data0;
    Uint8 Data1;
};

struct SNRomCompatEntryT
{
    Uint32 CRC32;
    Uint32 RomBytes;
    const SNRomCompatWriteT *pWrites;
    Uint8 nWrites;
};

static Uint32 _SNRomCompatCRC32(const Uint8 *pData, Uint32 nBytes)
{
    Uint32 crc = 0xFFFFFFFFu;
    while (nBytes--)
    {
        crc ^= *pData++;
        for (Uint32 bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xEDB88320u : 0u);
    }
    return crc ^ 0xFFFFFFFFu;
}

static const SNRomCompatWriteT _SNRomCompat_LostVikings1[] =
{
    { 0x28D83u, 0xEA, 0xEA },
    { 0x28DF6u, 0xEA, 0xEA },
    { 0x28E09u, 0xEA, 0xEA },
};

static const SNRomCompatWriteT _SNRomCompat_AddamsFamily[] =
{
    { 0x014E9u, 0xEA, 0xEA },
    { 0x07870u, 0xEA, 0xEA },
    { 0x0789Du, 0xEA, 0xEA },
};

static const SNRomCompatWriteT _SNRomCompat_LostVikings2SkipIntro[] =
{
    /* $80FA34=F6, $80FA35=00 -> LoROM file offset $7A34. */
    { 0x07A34u, 0xF6, 0x00 },
};

static const SNRomCompatEntryT _SNRomCompatEntries[] =
{
    /* Payload documented directly for these USA layouts. */
    { 0x6838BE08u, 0x100000u, _SNRomCompat_LostVikings1,
      (Uint8)(sizeof(_SNRomCompat_LostVikings1) / sizeof(_SNRomCompat_LostVikings1[0])) },
    { 0x2E8034ABu, 0x100000u, _SNRomCompat_AddamsFamily,
      (Uint8)(sizeof(_SNRomCompat_AddamsFamily) / sizeof(_SNRomCompat_AddamsFamily[0])) },
    { 0x3AA01DBDu, 0x100000u, _SNRomCompat_LostVikings2SkipIntro,
      (Uint8)(sizeof(_SNRomCompat_LostVikings2SkipIntro) / sizeof(_SNRomCompat_LostVikings2SkipIntro[0])) },

#if SNES_ROM_COMPAT_REGIONAL
    /* Lost Vikings 1: Europe, France, Germany, Spain, Japan. */
    { 0x66989491u, 0x100000u, _SNRomCompat_LostVikings1, 3 },
    { 0x94093298u, 0x100000u, _SNRomCompat_LostVikings1, 3 },
    { 0x4EE705ABu, 0x100000u, _SNRomCompat_LostVikings1, 3 },
    { 0x6E8A1081u, 0x100000u, _SNRomCompat_LostVikings1, 3 },
    { 0x50FEF979u, 0x100000u, _SNRomCompat_LostVikings1, 3 },

    /* The Addams Family: Europe, Japan. */
    { 0x82497F19u, 0x100000u, _SNRomCompat_AddamsFamily, 3 },
    { 0xCD80351Cu, 0x100000u, _SNRomCompat_AddamsFamily, 3 },

    /* Lost Vikings 2 / Norse by Norsewest: Europe. */
    { 0x3C49A285u, 0x100000u, _SNRomCompat_LostVikings2SkipIntro, 1 },
#endif
};
#endif

/* Pontua um header LoROM candidato em 'base' (deslocamento do $FFC0 da
   metade). Usado para descobrir QUAL metade de uma ROM ExLoROM contem o
   header/vetores reais, para normalizar a ordem (igual ao scoring do
   reference emulator, porem simplificado). */
static int _ExLoRomHeaderScore(const Uint8 *pRom, Uint32 base, Uint32 romBytes)
{
	if ((base + 0x40) > romBytes) return -1000;

	int score = 0;

	// reset vector ($FFFC) deve apontar para a area de ROM (>= $8000)
	Uint16 reset = (Uint16)(pRom[base + 0x3C] | (pRom[base + 0x3D] << 8));
	if (reset >= 0x8000) score += 8; else score -= 8;

	// checksum + complemento == 0xFFFF
	Uint16 cmp = (Uint16)(pRom[base + 0x1C] | (pRom[base + 0x1D] << 8));
	Uint16 chk = (Uint16)(pRom[base + 0x1E] | (pRom[base + 0x1F] << 8));
	if (chk != 0 && (Uint16)(chk + cmp) == 0xFFFF) score += 8;

	// titulo em ASCII imprimivel
	int printable = 0;
	for (int i = 0; i < 21; i++) {
		Uint8 c = pRom[base + i];
		if (c >= 0x20 && c < 0x7F) printable++;
	}
	if (printable >= 16) score += 4;

	return score;
}


//
//
//


struct SNRomCountryT
{
	const char *pName;
	SNRomVideoE eVideoType;
};

struct SNRomLicenseT
{
	Uint8	uCode;
	const char *pName;
};


//
//
//

static SNRomCountryT _SNRom_Country[]=
{
    { "Japan"                                   ,   SNROM_VIDEO_NTSC    },
    { "USA"                                     ,   SNROM_VIDEO_NTSC    },
    { "Australia, Europe, Oceania and Asia"     ,   SNROM_VIDEO_PAL     },
    { "Sweden"                                  ,   SNROM_VIDEO_PAL     },
    { "Finland"                                 ,   SNROM_VIDEO_PAL     },
    { "Denmark"                                 ,   SNROM_VIDEO_PAL     },
    { "France"                                  ,   SNROM_VIDEO_PAL     },
    { "Holland"                                 ,   SNROM_VIDEO_PAL     },
    { "Spain"                                   ,   SNROM_VIDEO_PAL     },
    { "Germany, Austria and Switzerland"        ,   SNROM_VIDEO_PAL     },
    { "Italy"                                   ,   SNROM_VIDEO_PAL     },
    { "Hong Kong and China"                     ,   SNROM_VIDEO_PAL     },
    { "Indonesia"                               ,   SNROM_VIDEO_PAL     },
    { "Korea"                                   ,   SNROM_VIDEO_PAL     },
};

static SNRomLicenseT _SNRom_License[]=
{
    { 1   , "Nintendo"                                  },
    { 3   , "Imagineer-Zoom"                            },
    { 5   , "Zamuse"                                    },
    { 6   , "Falcom"                                    },
    { 8   , "Capcom"                                    },
    { 9   , "HOT-B"                                     },
    { 10  , "Jaleco"                                    },
    { 11  , "Coconuts"                                  },
    { 12  , "Rage Software"                             },
    { 14  , "Technos"                                   },
    { 15  , "Mebio Software"                            },
    { 18  , "Gremlin Graphics"                          },
    { 19  , "Electronic Arts"                           },
    { 21  , "COBRA Team"                                },
    { 22  , "Human/Field"                               },
    { 23  , "KOEI"                                      },
    { 24  , "Hudson Soft"                               },
    { 26  , "Yanoman"                                   },
    { 28  , "Tecmo"                                     },
    { 30  , "Open System"                               },
    { 31  , "Virgin Games"                              },
    { 32  , "KSS"                                       },
    { 33  , "Sunsoft"                                   },
    { 34  , "POW"                                       },
    { 35  , "Micro World"                               },
    { 38  , "Enix"                                      },
    { 39  , "Loriciel/Electro Brain"                    },
    { 40  , "Kemco"                                     },
    { 41  , "Seta Co.,Ltd."                             },
    { 45  , "Visit Co.,Ltd."                            },
    { 49  , "Carrozzeria"                               },
    { 50  , "Dynamic"                                   },
    { 51  , "Nintendo"                                  },
    { 52  , "Magifact"                                  },
    { 53  , "Hect"                                      },
    { 60  , "Empire Software"                           },
    { 61  , "Loriciel"                                  },
    { 64  , "Seika Corp."                               },
    { 65  , "UBI Soft"                                  },
    { 70  , "System 3"                                  },
    { 71  , "Spectrum Holobyte"                         },
    { 73  , "Irem"                                      },
    { 75  , "Raya Systems/Sculptured Software"          },
    { 76  , "Renovation Products"                       },
    { 77  , "Malibu Games/Black Pearl"                  },
    { 79  , "U.S. Gold"                                 },
    { 80  , "Absolute Entertainment"                    },
    { 81  , "Acclaim"                                   },
    { 82  , "Activision"                                },
    { 83  , "American Sammy"                            },
    { 84  , "GameTek"                                   },
    { 85  , "Hi Tech Expressions"                       },
    { 86  , "LJN Toys"                                  },
    { 90  , "Mindscape"                                 },
    { 93  , "Tradewest"                                 },
    { 95  , "American Softworks Corp."                  },
    { 96  , "Titus"                                     },
    { 97  , "Virgin Interactive Entertainment"          },
    { 98  , "Maxis"                                     },
    {103  , "Ocean"                                     },
    {105  , "Electronic Arts"                           },
    {107  , "Laser Beam"                                },
    {110  , "Elite"                                     },
    {111  , "Electro Brain"                             },
    {112  , "Infogrames"                                },
    {113  , "Interplay"                                 },
    {114  , "LucasArts"                                 },
    {115  , "Parker Brothers"                           },
    {117  , "STORM"                                     },
    {120  , "THQ Software"                              },
    {121  , "Accolade Inc."                             },
    {122  , "Triffix Entertainment"                     },
    {124  , "Microprose"                                },
    {127  , "Kemco"                                     },
    {128  , "Misawa"                                    },
    {129  , "Teichio"                                   },
    {130  , "Namco Ltd."                                },
    {131  , "Lozc"                                      },
    {132  , "Koei"                                      },
    {134  , "Tokuma Shoten Intermedia"                  },
    {136  , "DATAM-Polystar"                            },
    {139  , "Bullet-Proof Software"                     },
    {140  , "Vic Tokai"                                 },
    {142  , "Character Soft"                            },
    {143  , "I''Max"                                    },
    {144  , "Takara"                                    },
    {145  , "CHUN Soft"                                 },
    {146  , "Video System Co., Ltd."                    },
    {147  , "BEC"                                       },
    {149  , "Varie"                                     },
    {151  , "Kaneco"                                    },
    {153  , "Pack in Video"                             },
    {154  , "Nichibutsu"                                },
    {155  , "TECMO"                                     },
    {156  , "Imagineer Co."                             },
    {160  , "Telenet"                                   },
    {164  , "Konami"                                    },
    {165  , "K.Amusement Leasing Co."                   },
    {167  , "Takara"                                    },
    {169  , "Technos Jap."                              },
    {170  , "JVC"                                       },
    {172  , "Toei Animation"                            },
    {173  , "Toho"                                      },
    {175  , "Namco Ltd."                                },
    {177  , "ASCII Co. Activison"                       },
    {178  , "BanDai America"                            },
    {180  , "Enix"                                      },
    {182  , "Halken"                                    },
    {186  , "Culture Brain"                             },
    {187  , "Sunsoft"                                   },
    {188  , "Toshiba EMI"                               },
    {189  , "Sony Imagesoft"                            },
    {191  , "Sammy"                                     },
    {192  , "Taito"                                     },
    {194  , "Kemco"                                     },
    {195  , "Square"                                    },
    {196  , "Tokuma Soft"                               },
    {197  , "Data East"                                 },
    {198  , "Tonkin House"                              },
    {200  , "KOEI"                                      },
    {202  , "Konami USA"                                },
    {203  , "NTVIC"                                     },
    {205  , "Meldac"                                    },
    {206  , "Pony Canyon"                               },
    {207  , "Sotsu Agency/Sunrise"                      },
    {208  , "Disco/Taito"                               },
    {209  , "Sofel"                                     },
    {210  , "Quest Corp."                               },
    {211  , "Sigma"                                     },
    {214  , "Naxat"                                     },
    {216  , "Capcom Co., Ltd."                          },
    {217  , "Banpresto"                                 },
    {218  , "Tomy"                                      },
    {219  , "Acclaim"                                   },
    {221  , "NCS"                                       },
    {222  , "Human Entertainment"                       },
    {223  , "Altron"                                    },
    {224  , "Jaleco"                                    },
    {226  , "Yutaka"                                    },
    {228  , "T&ESoft"                                   },
    {229  , "EPOCH Co.,Ltd."                            },
    {231  , "Athena"                                    },
    {232  , "Asmik"                                     },
    {233  , "Natsume"                                   },
    {234  , "King Records"                              },
    {235  , "Atlus"                                     },
    {236  , "Sony Music Entertainment"                  },
    {238  , "IGS"                                       },
    {241  , "Motown Software"                           },
    {242  , "Left Field Entertainment"                  },
    {243  , "Beam Software"                             },
    {244  , "Tec Magik"                                 },
    {249  , "Cybersoft"                                 },
    {255  , "Hudson Soft"                               },
	{0  , NULL                               }
};



//
//
//

                                                        

static SNRomCountryT *_SNRomGetCountry(Uint8 uCode)
{
    if (uCode < sizeof(_SNRom_Country) / sizeof(_SNRom_Country[0]))
    {
        return &_SNRom_Country[uCode];
    }   
    else
    {
        // invalid country code
        return NULL;
    }
}

static SNRomLicenseT *_SNRomGetLicense(Uint8 uCode)
{
    SNRomLicenseT *pLicense = _SNRom_License;

    while (pLicense->pName)
    {
        if (pLicense->uCode == uCode) 
        {
            // found license
            return pLicense;
        }

        pLicense++;
    }

    // invalid license
    return NULL;
}


SNRomHdrTypeE SnesRom::SNRomGetHdrType(SNRomHdrU *pRomHdr)
{
	// check SWC tag
	if (pRomHdr->SWC.Tag[0] == 0xAA && pRomHdr->SWC.Tag[1] == 0xBB && pRomHdr->SWC.Tag[2] == 0x04)
	{
		return SNROM_HDRTYPE_SWC;
	}

	// ???

	return SNROM_HDRTYPE_UNKNOWN;
}

//
//
//

static Bool _SNRomIsValidCartInfo(SNRomInfoT *pCartInfo)
{
	return pCartInfo && ((pCartInfo->InverseChecksum ^ pCartInfo->Checksum) == 0xFFFF);
}

/* AURORA_REVIVE_7C3A5DE_TYPE1_HEADER_SCORING_20260829
 * Port of ReyFxck/SNESticleRevive commit 7c3a5de:
 * score checksum-valid LoROM/HiROM candidates before Type-1 deinterleave.
 *
 * Checking the map byte alone is unsafe: an ordinary game can contain a
 * coincidental checksum/complement pair at the other header address.
 * Pinocchio (USA), a clean 3 MiB HiROM, is one such case; the old code could
 * treat the false LoROM candidate as Type-1 and scramble the real header.
 *
 * The reset-vector/opcode tests mirror the conservative parts of header
 * scoring used by current reference emulators. A bad reset vector rejects
 * the candidate for normal selection, while the legacy Type-1 fallback below
 * remains available when neither physical location can be scored yet. */
static Int32 _SNRomHeaderScore(const Uint8 *pRom, Uint32 uRomBytes,
	Uint32 uHeaderOffset, Bool bLoHeader)
{
	const SNRomInfoT *pCartInfo;
	Uint16 uReset;
	Uint32 uResetBase;
	Uint32 uOpcodeOffset;
	Uint8 uOpcode;
	Uint8 uMapLow;
	Int32 nPrintable = 0;
	Int32 nScore = 0;
	Int32 i;

	if (!pRom || uHeaderOffset + 0x40 > uRomBytes)
		return -1000;

	pCartInfo = (const SNRomInfoT *)(pRom + uHeaderOffset);
	if (!_SNRomIsValidCartInfo((SNRomInfoT *)pCartInfo))
		return -1000;

	/* Zero/FFFF is technically complementary but is much weaker evidence
	   than the non-zero checksum pair found in commercial dumps. */
	if (pCartInfo->Checksum && pCartInfo->InverseChecksum)
		nScore += 8;
	else
		nScore += 1;

	uMapLow = pCartInfo->RomMakeup & 0x0F;
	if ((bLoHeader && (uMapLow == 0 || uMapLow == 2 || uMapLow == 3)) ||
	    (!bLoHeader && (uMapLow == 1 || uMapLow == 5)))
		nScore += 4;

	if (pCartInfo->RomType < 0x08) nScore++;
	if (pCartInfo->RomSize < 0x10) nScore++;
	if (pCartInfo->SRAMSize < 0x08) nScore++;
	if (pCartInfo->Country < 14) nScore++;

	for (i = 0; i < 21; i++)
	{
		Uint8 c = pCartInfo->Title[i];
		if (c >= 0x20 && c < 0x7F) nPrintable++;
	}
	if (nPrintable >= 16) nScore += 4;
	else if (nPrintable >= 8) nScore += 1;

	uReset = (Uint16)(pRom[uHeaderOffset + 0x3C] |
	                  (pRom[uHeaderOffset + 0x3D] << 8));
	if (uReset < 0x8000)
		return -1000;

	uResetBase = bLoHeader ? 0 : 0x8000;
	uOpcodeOffset = uResetBase + (uReset & 0x7FFF);
	if (uOpcodeOffset >= uRomBytes)
		return -1000;

	uOpcode = pRom[uOpcodeOffset];
	switch (uOpcode)
	{
	/* CLC, SEI, JMP, JML, JSR, JSL, STZ */
	case 0x18: case 0x78: case 0x4C: case 0x5C:
	case 0x20: case 0x22: case 0x9C:
		nScore += 8;
		break;
	/* REP, SEP, LDA, LDX, LDY */
	case 0xC2: case 0xE2: case 0xA9: case 0xA2: case 0xA0:
		nScore += 4;
		break;
	/* BRK, erased ROM, unlikely CPY immediate */
	case 0x00: case 0xFF: case 0xCC:
		nScore -= 8;
		break;
	default:
		break;
	}

	return nScore;
}

/* A type-1 interleaved image places a header which claims the opposite
   mapper at the otherwise valid header location.  The checksum guard keeps
   random ROM data from being mistaken for a header. */
static Bool _SNRomHeaderSaysType1(SNRomInfoT *pCartInfo, Bool bLoHeader)
{
	Uint8 uMapMode;
	Uint8 uLow;

	if (!_SNRomIsValidCartInfo(pCartInfo))
		return FALSE;

	uMapMode = pCartInfo->RomMakeup;
	if ((uMapMode & 0xF0) != 0x20 && (uMapMode & 0xF0) != 0x30)
		return FALSE;

	uLow = uMapMode & 0x0F;
	return bLoHeader ? (uLow == 1 || uLow == 5)
	                 : (uLow == 0 || uLow == 3);
}

/* Convert the common copier type-1 layout to linear 32 KiB blocks. */
static Bool _SNRomDeinterleaveType1(Uint8 *pRom, Uint32 uRomBytes)
{
	Uint8 Blocks[256];
	Uint8 *pTmp;
	Uint32 nBlocks;
	Uint32 i;

	if (!pRom || uRomBytes < 0x10000 || (uRomBytes & 0xFFFF))
		return FALSE;

	nBlocks = uRomBytes >> 16;
	if (nBlocks == 0 || nBlocks > 128)
		return FALSE;

	for (i = 0; i < nBlocks; i++)
	{
		Blocks[i * 2]     = (Uint8)(i + nBlocks);
		Blocks[i * 2 + 1] = (Uint8)i;
	}

	pTmp = (Uint8 *)malloc(0x8000);
	if (!pTmp)
		return FALSE;

	for (i = 0; i < nBlocks * 2; i++)
	{
		Uint32 j;
		for (j = i; j < nBlocks * 2; j++)
		{
			if (Blocks[j] == i)
			{
				Uint8 uBlock;
				memcpy(pTmp, pRom + Blocks[j] * 0x8000, 0x8000);
				memmove(pRom + Blocks[j] * 0x8000,
				        pRom + Blocks[i] * 0x8000, 0x8000);
				memcpy(pRom + Blocks[i] * 0x8000, pTmp, 0x8000);
				uBlock = Blocks[j];
				Blocks[j] = Blocks[i];
				Blocks[i] = uBlock;
				break;
			}
		}
	}

	free(pTmp);
	return TRUE;
}

//
//
//


SnesRom::SnesRom()
{
	m_bLoaded	= false;
	m_pRomMem	= NULL;
	m_pRomData	= NULL;
	m_pCartInfo = NULL;
	m_uRomBytes	= 0;
	m_Flags      = SNROM_FLAG_ROM;
	m_eMapping   = SNROM_MAPPING_LOROM;
	m_eVideoType = SNROM_VIDEO_NTSC;
	m_uROMSize   = 0;
	m_uSRAMSize  = 0;
	memset(m_Name, 0, sizeof(m_Name));
}

SnesRom::~SnesRom()
{
	Unload();
}

SNRomInfoT *SnesRom::GetCartInfo(Uint32 uOffset)
{
	if (m_pRomData)
	{
		// make sure offset doest go past end of rom data
		if ((uOffset + sizeof(SNRomInfoT)) <= m_uRomBytes)
		{
			// return cartinfo at offset
			return (SNRomInfoT *)(m_pRomData + uOffset);
		}
	} 
	return NULL;
}


/* AURORA_V4_4_CUMULATIVE_20260908
 * Exact internal-title identity for Nintendo's BS-X interface/base ROM.
 * This is cartridge hardware identity, not a filename heuristic. */
static Bool _SNRomIsBSXBase(const SNRomInfoT *pInfo)
{
    return (pInfo &&
            !memcmp(pInfo->Title, "Satellaview BS-X     ", 21))
        ? TRUE : FALSE;
}

/* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNROM_CPP
 * Detect the standalone Satellaview slot from the extended-header signature
 * used by reference emulators.  This identifies board hardware rather than
 * filenames or title allow-lists. */
static Bool _SNRomHasBSXSlot(const Uint8 *pRom, Uint32 nRomBytes,
                             const SNRomInfoT *pInfo)
{
    const Uint8 *p;
    Uint32 uOffset;
    Uint8 uCode;

    if (!pRom || !pInfo || !nRomBytes)
        return FALSE;

    p = (const Uint8 *)pInfo;
    if (p < pRom)
        return FALSE;
    uOffset = (Uint32)(p - pRom);
    if (uOffset < 14u || uOffset + sizeof(SNRomInfoT) > nRomBytes)
        return FALSE;

    if (p[-14] != 'Z' || p[-11] != 'J')
        return FALSE;

    uCode = p[-13];
    if (!((uCode >= 'A' && uCode <= 'Z') ||
          (uCode >= '0' && uCode <= '9')))
        return FALSE;

    return (pInfo->License == 0x33 ||
            (p[-10] == 0x00 && p[-4] == 0x00)) ? TRUE : FALSE;
}

void SnesRom::SetCartInfo(SNRomInfoT *pCartInfo)
{

	m_pCartInfo = pCartInfo;
	memset(m_Name, 0, sizeof(m_Name));
	/* Keep cartridge classification deterministic even for an unknown or
	   newly-added type.  Previously m_Flags retained uninitialised heap data
	   whenever RomType was not listed in the switch below.  SuperFX games
	   commonly use 14h/15h/1Ah, so the same ROM could accidentally enable the
	   GSU on a PC build and leave it disabled on a real PS2. */
	m_Flags = SNROM_FLAG_ROM;
	if (pCartInfo)
	{
		Int32 iTitle;
		Int32 nTitle = 21;
		SNRomLicenseT* pLicense __attribute__((unused));
		SNRomCountryT* pCountry;

		/* The cartridge title is a fixed 21-byte field, not a C string.
		   Copy it into the owned, terminated buffer used by the UI and logs;
		   replace control/high bytes which the PS2 font cannot render and
		   remove padding spaces from the right. */
		for (iTitle = 0; iTitle < 21; iTitle++)
		{
			Uint8 c = pCartInfo->Title[iTitle];
			m_Name[iTitle] = (c >= 0x20 && c <= 0x7E) ? c : '?';
		}
		while (nTitle > 0 && (m_Name[nTitle - 1] == ' ' ||
		                      m_Name[nTitle - 1] == '?'))
			m_Name[--nTitle] = 0;

		pCountry = _SNRomGetCountry(pCartInfo->Country);
		pLicense = _SNRomGetLicense(pCartInfo->License);

		m_eVideoType = pCountry ? pCountry->eVideoType : SNROM_VIDEO_NTSC;
		m_uROMSize	  = 1 << (pCartInfo->RomSize - 7);
		/* AURORA_ACCURACY_SRAM_SIZE_EXPONENT_V1
		 * The SNES internal header stores ordinary Game Pak RAM size as an
		 * exponent.  In bytes the conventional formula is 2^(code + 10);
		 * m_uSRAMSize is expressed in kilobits, therefore 8 << code.
		 * Codes through 8 fit Aurora's existing 256 KiB backing store
		 * (code 8 = 256 KiB). Do not clamp larger declarations
		 * into that buffer: treating them as absent is safer than aliasing or
		 * writing past the allocation.  SuperFX may override this value later
		 * from its extended header, as before. */
		if (g_FakeSRAMSize)
		{
			m_uSRAMSize = g_FakeSRAMSize;
		}
		else if (pCartInfo->SRAMSize == 0)
		{
			m_uSRAMSize = 0;
		}
		else if (pCartInfo->SRAMSize <= 8)
		{
			m_uSRAMSize = (Uint32)8 << pCartInfo->SRAMSize;
		}
		else
		{
			m_uSRAMSize = 0;
		}
		/* AURORA_SA1_V1_REFERENCE_LOGIC_20260902
	 * reference emulator's cartridge classifier uses IDs $3423/$3523.  Do this before
	 * the legacy RomType-only switch so $34/$35 are not mistaken for a
	 * generic RAM layout. */
	if (pCartInfo->RomMakeup == 0x23 &&
	    (pCartInfo->RomType == 0x34 || pCartInfo->RomType == 0x35))
	{
		m_Flags = SNROM_FLAG_ROM | SNROM_FLAG_SA1 |
		    ((pCartInfo->RomType == 0x35) ? SNROM_FLAG_SAVERAM : SNROM_FLAG_RAM);
		return;
	}

	switch (pCartInfo->RomType)
	{

		case 0:
		case 53:
			m_Flags		 = SNROM_FLAG_ROM;
			break;
		case 1:
			m_Flags		 = SNROM_FLAG_ROM | SNROM_FLAG_RAM;
			break;
		case 2:
			m_Flags		 = SNROM_FLAG_ROM | SNROM_FLAG_SAVERAM;
			break;
		case 3:
			m_Flags		 = SNROM_FLAG_ROM | SNROM_FLAG_DSP1;
			break;
		case 4:
			m_Flags		 = SNROM_FLAG_ROM | SNROM_FLAG_RAM | SNROM_FLAG_DSP1;
			break;
		case 5:
			m_Flags		 = SNROM_FLAG_ROM | SNROM_FLAG_SAVERAM | SNROM_FLAG_DSP1;
			break;
		/* SuperFX/GSU cartridge types.  The low nibble follows the normal
		   ROM/chip/RAM/battery convention; 1Ah is the additional type used by
		   a small group of commercial SuperFX cartridges. */
		case 0x13:
			m_Flags = SNROM_FLAG_ROM | SNROM_FLAG_SUPERFX;
			break;
		case 0x14:
		case 0x1A:
			m_Flags = SNROM_FLAG_ROM | SNROM_FLAG_RAM | SNROM_FLAG_SUPERFX;
			break;
		case 0x15:
			m_Flags = SNROM_FLAG_ROM | SNROM_FLAG_SAVERAM | SNROM_FLAG_SUPERFX;
			break;
		case 227:
			m_Flags		 = SNROM_FLAG_ROM | SNROM_FLAG_RAM | SNROM_FLAG_GAMEBOY;
			break;
		case 246:
			m_Flags		 = SNROM_FLAG_ROM | SNROM_FLAG_DSP2;
			break;
		}

		// O byte RomType nao distingue a variante do DSP (1/2/3/4): todos
		// os jogos de DSP reportam 0x03/0x04/0x05. Como cada variante so'
		// e' usada por pouquissimos jogos, detecta-se pelo titulo do
		// cabecalho e troca o flag de DSP-1 para a variante correta.
		//   - DSP-2: Dungeon Master (unico).
		//   - DSP-3: SD Gundam GX (unico, Japan).
		//   - DSP-4: Top Gear 3000 / The Planet's Champ TG3000 (unicos).
		if (m_Flags & SNROM_FLAG_DSP1)
		{
			char t[8];
			int k;
			for (k = 0; k < 7; k++)
			{
				char c = (char)pCartInfo->Title[k];
				if (c >= 'a' && c <= 'z') c -= 32;   // upper
				t[k] = c;
			}
			t[7] = 0;
			if (!strncmp(t, "DUNGEON", 7))
			{
				m_Flags &= ~SNROM_FLAG_DSP1;
				m_Flags |=  SNROM_FLAG_DSP2;
			}

			// Titulo completo (21 chars) em maiusculas para os demais
			// testes.  So' o ASCII a-z e' convertido; bytes altos (katakana
			// de meia-largura do cabecalho japones) ficam intactos.
			char ut[22];
			for (k = 0; k < 21; k++)
			{
				char c = (char)pCartInfo->Title[k];
				if (c >= 'a' && c <= 'z') c -= 32;
				ut[k] = c;
			}
			ut[21] = 0;

			// DSP-4: Top Gear 3000 (USA, "TOP GEAR 3000") e a versao
			// japonesa "The Planet's Champ TG3000" -- ambos carregam
			// "TG3000" no titulo.
			if (strstr(ut, "TOP GEAR 3000") || strstr(ut, "TG3000"))
			{
				m_Flags &= ~SNROM_FLAG_DSP1;
				m_Flags |=  SNROM_FLAG_DSP4;
			}
			// DSP-3: SD Gundam GX (Japan).  O titulo do cabecalho usa
			// katakana de meia-largura ("SD" + bytes >=0x80 + "GX").
			// Aceita tambem variantes/redumps em ASCII com "GUNDAM".
			// OBS: confirmar os bytes exatos do cabecalho com um dump real;
			// como estamos dentro do bloco de jogos-DSP, "SD"+katakana e'
			// um sinal seguro (nenhum outro jogo DSP comeca assim).
			else if (strstr(ut, "GUNDAM") ||
			         (ut[0] == 'S' && ut[1] == 'D' &&
			          (Uint8)pCartInfo->Title[2] >= 0x80))
			{
				m_Flags &= ~SNROM_FLAG_DSP1;
				m_Flags |=  SNROM_FLAG_DSP3;
			}
		}

		// OBC1 (Metal Combat: Falcon's Revenge): o cartucho reporta
		// RomType 0x13, que cairia no case de SuperFX. E' o unico jogo
		// OBC1 -> detecta pelo titulo e corrige o flag.
		{
			char t[13];
			int k;
			for (k = 0; k < 12; k++)
			{
				char c = (char)pCartInfo->Title[k];
				if (c >= 'a' && c <= 'z') c -= 32;
				t[k] = c;
			}
			t[12] = 0;
			if (!strncmp(t, "METAL COMBAT", 12))
			{
				m_Flags &= ~SNROM_FLAG_SUPERFX;
				m_Flags |=  SNROM_FLAG_OBC1;
			}
		}

		// CX4 (Mega Man X2/X3, Rockman X2/X3): o cartucho reporta RomType
		// 0xF3, que nao cai em nenhum case do switch acima (m_Flags ficaria
		// indefinido). E' detectado pelo titulo. Mega Man X1 e' "MEGAMAN X "
		// (com espaco), entao casar 10 chars de "MEGAMAN X2"/"X3" nao pega o X1.
		{
			char t[11];
			int k;
			for (k = 0; k < 10; k++)
			{
				char c = (char)pCartInfo->Title[k];
				if (c >= 'a' && c <= 'z') c -= 32;
				t[k] = c;
			}
			t[10] = 0;
			if (!strncmp(t, "MEGAMAN X2", 10) || !strncmp(t, "MEGAMAN X3", 10) ||
			    !strncmp(t, "ROCKMAN X2", 10) || !strncmp(t, "ROCKMAN X3", 10))
			{
				m_Flags = SNROM_FLAG_ROM | SNROM_FLAG_SAVERAM | SNROM_FLAG_CX4;
			}
		}

		/* SuperFX stores the size of its Game Pak RAM in the extended
		   header byte at $7FBD when the new-license marker is present.
		   The ordinary SRAMSize field is commonly zero even though this RAM
		   is the framebuffer (Yoshi's Island is 05h = 32 KiB).  Older
		   headers have no extended value; those cartridges conventionally
		   contain 32 KiB, matching the hardware/reference-emulator fallback. */
		if (m_Flags & SNROM_FLAG_SUPERFX)
		{
			const Uint8 *pHeader = (const Uint8 *)pCartInfo;
			Uint8 uRamCode = (pCartInfo->License == 0x33) ? pHeader[-3] : 5;
			if (uRamCode == 0 || uRamCode > 7) uRamCode = 5;
			m_uSRAMSize = (Uint32)8 << uRamCode;  // kilobits
		}

		// S-DD1 (Star Ocean, Street Fighter Alpha 2 / Zero 2): detectado pelo
		// nibble alto do byte de tipo do cartucho (0x4x = coprocessador S-DD1;
		// 0x43 = sem save, 0x45 = com bateria). Isso e' confiavel mesmo quando
		// o titulo esta adulterado (ex.: watermark "Vimm's Lair: ..."). O
		// titulo serve so de reserva. makeup 0x32 = LoROM; o >4MB (Star Ocean,
		// 48Mbit) e' acessado pela troca de segmento do S-DD1, nao por ExLoROM.
		{
			Bool bSDD1 = ((pCartInfo->RomType & 0xF0) == 0x40);
			if (!bSDD1)
			{
				char t[22];
				int k;
				for (k = 0; k < 21; k++)
				{
					char c = (char)pCartInfo->Title[k];
					if (c >= 'a' && c <= 'z') c -= 32;
					t[k] = c;
				}
				t[21] = 0;
				if (!strncmp(t, "STAR OCEAN", 10) ||
				    !strncmp(t, "STREET FIGHTER ALPHA", 20) ||
				    !strncmp(t, "STREET FIGHTER ZERO", 19))
					bSDD1 = TRUE;
			}
			if (bSDD1)
			{
				m_eMapping = SNROM_MAPPING_LOROM;
				m_Flags    = SNROM_FLAG_ROM | SNROM_FLAG_SAVERAM | SNROM_FLAG_SDD1;
			}
		}

		// S-RTC (Daikaijuu Monogatari II): relogio de tempo real. Detectado
		// pelo nibble alto do tipo de cartucho (0x5x = S-RTC). E' um jogo
		// HiROM com bateria.
		if ((pCartInfo->RomType & 0xF0) == 0x50)
		{
			m_eMapping = SNROM_MAPPING_HIROM;
			m_Flags    = SNROM_FLAG_ROM | SNROM_FLAG_SAVERAM | SNROM_FLAG_SRTC;
		}

	} else
	{
		m_eVideoType = SNROM_VIDEO_NTSC;
		m_Flags		 = SNROM_FLAG_ROM;
		m_uROMSize   = 0;
		m_uSRAMSize  = 0;
	}
}

Uint32	SnesRom::GetNumRomRegions()
{
	return 1;
}

char *SnesRom::GetRomRegionName(Uint32 eRegion)
{
	switch (eRegion)
	{
		case 0:
			return (char *)"ROM";
		default:
			return NULL;
	}

}
Uint32 	SnesRom::GetRomRegionSize(Uint32 eRegion)
{
	switch (eRegion)
	{
		case 0:
			return m_uRomBytes;
		default:
			return 0;
	}
}



char   *SnesRom::GetRomTitle()
{
	return m_pCartInfo ? (char *)m_Name : NULL;
}


Emu::Rom::LoadErrorE SnesRom::LoadRom(CDataIO *pFileIO, Uint8 *pBuffer, Uint32 nBufferBytes)
{
	SNRomHdrU		RomHdr;
	SNRomHdrTypeE	eRomHdrType;
	size_t nBytesRead;
	Uint32 nFileBytes;
	Uint32 nHeaderBytes;
	Uint32 nRomBytes;

	// determine file size
	pFileIO->Seek(0, SEEK_END);
	nFileBytes= (Uint32)pFileIO->GetPos();
	pFileIO->Seek(0, SEEK_SET);
	if (nFileBytes <= 0)
	{
		return LOADERROR_BADHEADERSIZE;
	}

//	printf("%d file size\n", nFileBytes);

	// determine header size
	nHeaderBytes = (nFileBytes & 0x1FFF);
	nRomBytes    = nFileBytes - nHeaderBytes;

	// is header size valid?
	if (nHeaderBytes!=0 && nHeaderBytes!=sizeof(SNRomHdrU))
	{
		nHeaderBytes = 0;
//		return LOADERROR_BADHEADERSIZE;
	}

	m_eMapping = SNROM_MAPPING_LOROM;

	// does header exist?
	if (nHeaderBytes== sizeof(SNRomHdrU))
	{
		// header exists!

		// read rom header from file
		pFileIO->Read(&RomHdr, sizeof(RomHdr));

		// determine type of file (if possible)
		eRomHdrType = SNRomGetHdrType(&RomHdr);

		switch (eRomHdrType)
		{
		case SNROM_HDRTYPE_SWC:
		{
			/* AURORA_SWC_PHYSICAL_PAYLOAD_V10_10_20260831
			 *
			 * SWC header bytes 0-1 are an 8-KiB page count. Historically
			 * this loader trusted that count and overwrote nRomBytes.
			 * A stale/bad copier header can therefore truncate a perfectly
			 * valid physical payload before a Front copier ever sees it.
			 *
			 * nRomBytes was already derived above from the real file length
			 * after removing the 512-byte copier header. Keep that physical
			 * payload as the source of truth. The SWC header still selects
			 * LoROM/HiROM exactly as before.
			 */
			const Uint32 uDeclaredBytes =
				(Uint32)RomHdr.SWC.uSize * 0x2000u;
			(void)uDeclaredBytes;
			m_eMapping = (RomHdr.SWC.uImageInfo & 0x10)
				? SNROM_MAPPING_HIROM : SNROM_MAPPING_LOROM;
			break;
		}
		default:
		case SNROM_HDRTYPE_UNKNOWN:
			m_eMapping = SNROM_MAPPING_LOROM;
			break;
		}
	} 

	// set size of ROM
	m_uRomBytes = nRomBytes;
	if (m_uRomBytes == 0)
	{
		return LOADERROR_BADROMSIZE;
	}

	// try to get data pointer from file
	m_pRomMem = NULL;
	m_pRomData = pFileIO->ReadPtr(m_uRomBytes);

	if (m_pRomData == NULL)
	{
		if (pBuffer)
		{
			if (m_uRomBytes < nBufferBytes)
			{	// use provided buffer space
				m_pRomMem = NULL;
				m_pRomData = pBuffer;
			} else
			{
				// not enough buffer space provided
				return LOADERROR_OUTOFSPACE;
			}
		} else
		{

			// allocate memory for rom
			m_pRomMem = 
			m_pRomData = (Uint8 *)malloc(m_uRomBytes);
			if (!m_pRomData)
			{
				return LOADERROR_OUTOFSPACE;
			}
		}

		// read rom data
		nBytesRead = pFileIO->Read(m_pRomData, m_uRomBytes);
		if (nBytesRead != m_uRomBytes)
		{
			Unload();
			return LOADERROR_READFILE;
		}
	}

	SNRomInfoT *pCartInfo;
	SNRomInfoT *pLoCartInfo;
	SNRomInfoT *pHiCartInfo;
	Bool bOriginalDSP1Op28 = FALSE;

	/* AURORA_REVIVE_7C3A5DE_TYPE1_HEADER_SCORING_20260829
	 * Score both physical header positions first. Only the best plausible
	 * candidate may request Type-1 conversion; accepting either candidate
	 * unconditionally can corrupt clean HiROM images such as Pinocchio. */
	pLoCartInfo = GetCartInfo(32704);
	pHiCartInfo = GetCartInfo(65472);
	Int32 nLoScore = _SNRomHeaderScore(m_pRomData, m_uRomBytes, 32704, TRUE);
	Int32 nHiScore = _SNRomHeaderScore(m_pRomData, m_uRomBytes, 65472, FALSE);
	Bool bBestIsLo = (nLoScore >= nHiScore);
	SNRomInfoT *pBestCartInfo = bBestIsLo ? pLoCartInfo : pHiCartInfo;

	/* Some genuine Type-1 images do not expose a usable reset opcode until
	   after conversion. Preserve the old checksum/map fallback, but only
	   when neither normal candidate could be scored. */
	if (nLoScore <= -1000 && nHiScore <= -1000)
	{
		if (_SNRomHeaderSaysType1(pLoCartInfo, TRUE))
		{
			bBestIsLo = TRUE;
			pBestCartInfo = pLoCartInfo;
		}
		else if (_SNRomHeaderSaysType1(pHiCartInfo, FALSE))
		{
			bBestIsLo = FALSE;
			pBestCartInfo = pHiCartInfo;
		}
		else
		{
			pBestCartInfo = NULL;
		}
	}

#if SNDBG_LOG
	DLog("[rom-map] raw lo=%d hi=%d selected=%s type1=%d",
	     (int)nLoScore, (int)nHiScore,
	     pBestCartInfo ? (bBestIsLo ? "Lo" : "Hi") : "none",
	     pBestCartInfo && _SNRomHeaderSaysType1(pBestCartInfo, bBestIsLo));
#endif

	if (pBestCartInfo &&
	    _SNRomHeaderSaysType1(pBestCartInfo, bBestIsLo))
	{
		if (_SNRomDeinterleaveType1(m_pRomData, m_uRomBytes))
		{
			pLoCartInfo = GetCartInfo(32704);
			pHiCartInfo = GetCartInfo(65472);
		}
	}

	/* AURORA_SNES_TURBOFILE_V4_20260829
	 * Exact normalized/headerless CRC, before compatibility patching.
	 * This normalized runtime-CRC pass remains for CRC-selected accessories;
	 * open-bus accuracy no longer depends on a title identity. */
	{
		const Uint32 uRuntimeCRC =
			(m_pRomData && m_uRomBytes)
				? _SNRomRuntimeCRC32(m_pRomData, m_uRomBytes) : 0;

		SnesTurboFileSelectForCRC(uRuntimeCRC);

	}

	/* AURORA_UPSTREAM_20260827_DSP1_OP28_REVISION_V1
	 * Identify the untouched normalized/headerless image before any Aurora
	 * compatibility byte patch can run.  Pilotwings shipped on boards with multiple DSP revisions. Prefer the
	 * original DSP-1/1A op28 behavior for USA/Japan/Europe; a later PAL
	 * board revision with DSP-1B cannot be distinguished from ROM data alone.
	 *   266C44ED = Pilotwings (USA)
	 *   77871727 = Pilotwings (Japan)
	 *   DEF45776 = Pilotwings (Europe; DSP-1 and later DSP-1B boards exist) */
	if (m_pRomData && m_uRomBytes == 0x80000u)
	{
		const Uint32 uPilotwingsCRC =
			_SNRomRuntimeCRC32(m_pRomData, m_uRomBytes);
		bOriginalDSP1Op28 =
			(uPilotwingsCRC == 0x266C44EDu) || /* USA */
			(uPilotwingsCRC == 0x77871727u) || /* Japan */
			(uPilotwingsCRC == 0xDEF45776u);   /* Europe: prefer original DSP-1 behavior */
	}

/* AURORA_SONIC_BLAST_MAN_COLOR_V7
 * Never leak the previous game's renderer workaround into the next load. */
g_SnesCompatSonicBlastManColorMath = FALSE;

/* AURORA_HK97_SPC_BOOT_DB_V9
 * Detect the normalized/headerless ROM before any V6 in-memory patching.
 * The flag is reset on every load so it can never leak to the next game. */
g_SnesCompatHongKong97SPCBoot = FALSE;
#if SNES_HK97_SPC_BOOT
if (m_pRomData && m_uRomBytes == 0x80000u)
{
    const Uint32 uHK97CRC = _SNRomHK97CRC32(m_pRomData, m_uRomBytes);
    g_SnesCompatHongKong97SPCBoot =
        (uHK97CRC == 0x11A6E64Bu) ||
        (uHK97CRC == 0xC6A95816u);
}
#endif

/* AURORA_SUNSET_RIDERS_CRC_OBJ128_V2_GATE_20260825
 * Exact clean 1 MiB images only. This runs before any compatibility
 * byte-patching so altered dumps cannot match by title/header alone. */
g_SnesCompatSunsetRidersObj128 = FALSE;
#if SNES_SUNSET_RIDERS_OBJ128_HACK
if (m_pRomData && m_uRomBytes == 0x100000u)
{
    const Uint32 uSunsetRidersCRC =
        _SNRomRuntimeCRC32(m_pRomData, m_uRomBytes);

    g_SnesCompatSunsetRidersObj128 =
        (uSunsetRidersCRC == 0x52ADA404u) || /* USA */
        (uSunsetRidersCRC == 0x64EDFC5Du);   /* Europe */
}
#endif

/* AURORA_CRC_ZERO_INIT_DB_V8 + AURORA_TOP_GEAR_FASTROM_V1
 * Compute the untouched normalized/headerless CRC once, before V6 can mutate
 * compatibility bytes. The Top Gear timing flag is reset every load. */
g_SnesCompatZeroInit = FALSE;
g_SnesCompatTopGearFastRom = FALSE;
#if SNES_CRC_ZERO_INIT || SNES_TOP_GEAR_FASTROM_HACK
if (m_pRomData && m_uRomBytes)
{
    const Uint32 uCompatIdentityCRC = _SNRomZeroInitCRC32(m_pRomData, m_uRomBytes);
#if SNES_CRC_ZERO_INIT
    g_SnesCompatZeroInit = _SNRomNeedsZeroInit(uCompatIdentityCRC);
#endif
#if SNES_TOP_GEAR_FASTROM_HACK
    if (m_uRomBytes == 0x80000u)
    {
        g_SnesCompatTopGearFastRom =
            (uCompatIdentityCRC == 0xD34C49B7u) || /* Top Gear USA */
            (uCompatIdentityCRC == 0xB0150052u) || /* Top Gear Europe */
            (uCompatIdentityCRC == 0xE5A57B12u);   /* Top Racer Japan */
    }
#endif
}
#endif

#if SNES_ROM_COMPAT_PATCHES
	/* AURORA_CRC_COMPAT_DB_V6
	 * Apply only after copier deinterleaving, so CRC32 and offsets refer to the
	 * normalized/headerless ROM image.  CRC is the identity gate requested by
	 * the compatibility policy; valid LoROM + exact size are cheap extra guards.
	 * No expected-original-byte signature is required.
	 */
	if (m_uRomBytes == 0x100000u && _SNRomIsValidCartInfo(pLoCartInfo))
	{
		const Uint32 uCompatCRC = _SNRomCompatCRC32(m_pRomData, m_uRomBytes);

#if SNES_SONIC_COLOR_WORKAROUND
		/* Sonic Blast Man clean 1 MiB dumps: USA / Europe / Japan.
		 * The renderer uses this only to suppress CGADSUB color math. */
		g_SnesCompatSonicBlastManColorMath =
			(uCompatCRC == 0x8886396Eu) ||
			(uCompatCRC == 0x5441F25Bu) ||
			(uCompatCRC == 0xBE523800u);
#endif

		const SNRomCompatEntryT *pCompat = NULL;

		for (Uint32 i = 0;
		     i < (Uint32)(sizeof(_SNRomCompatEntries) / sizeof(_SNRomCompatEntries[0]));
		     ++i)
		{
			if (_SNRomCompatEntries[i].CRC32 == uCompatCRC &&
			    _SNRomCompatEntries[i].RomBytes == m_uRomBytes)
			{
				pCompat = &_SNRomCompatEntries[i];
				break;
			}
		}

		if (pCompat)
		{
			Bool bBoundsOK = TRUE;
			for (Uint32 i = 0; i < pCompat->nWrites; ++i)
			{
				if (pCompat->pWrites[i].Offset + 1u >= m_uRomBytes)
				{
					bBoundsOK = FALSE;
					break;
				}
			}

			if (bBoundsOK)
			{
				Bool bWritable = (m_pRomMem != NULL) ? TRUE : FALSE;
				if (!bWritable)
				{
					Uint8 *pCompatCopy = (Uint8 *)malloc(m_uRomBytes);
					if (pCompatCopy)
					{
						memcpy(pCompatCopy, m_pRomData, m_uRomBytes);
						m_pRomMem = pCompatCopy;
						m_pRomData = pCompatCopy;
						bWritable = TRUE;
					}
				}

				if (bWritable)
				{
					for (Uint32 i = 0; i < pCompat->nWrites; ++i)
					{
						const SNRomCompatWriteT *pWrite = &pCompat->pWrites[i];
						m_pRomData[pWrite->Offset + 0] = pWrite->Data0;
						m_pRomData[pWrite->Offset + 1] = pWrite->Data1;
					}

					/* Copying changes the base pointer. Rebuild header pointers before
					 * mapper detection stores m_pCartInfo. */
					pLoCartInfo = GetCartInfo(32704);
					pHiCartInfo = GetCartInfo(65472);
				}
			}
		}
	}
#endif

	/* AURORA_REVIVE_7C3A5DE_TYPE1_HEADER_SCORING_20260829
	 * Rescore because Type-1 conversion moves both the header and reset
	 * opcode. This is deliberately done here, after Aurora compatibility
	 * handling too, so any rebuilt ROM/header pointers are current.
	 *
	 * Fall back to the historical checksum-only choice for unusual
	 * homebrew/copier headers which are valid but use an uncommon reset
	 * prologue. */
	nLoScore = _SNRomHeaderScore(m_pRomData, m_uRomBytes, 32704, TRUE);
	nHiScore = _SNRomHeaderScore(m_pRomData, m_uRomBytes, 65472, FALSE);
	if (nLoScore > -1000 || nHiScore > -1000)
	{
		if (nLoScore >= nHiScore)
		{
			pCartInfo = pLoCartInfo;
			m_eMapping = SNROM_MAPPING_LOROM;
		}
		else
		{
			pCartInfo = pHiCartInfo;
			m_eMapping = SNROM_MAPPING_HIROM;
		}
	}
	else if (_SNRomIsValidCartInfo(pLoCartInfo))
	{
		pCartInfo = pLoCartInfo;
		m_eMapping = SNROM_MAPPING_LOROM;
	}
	else if (_SNRomIsValidCartInfo(pHiCartInfo))
	{
		pCartInfo = pHiCartInfo;
		m_eMapping = SNROM_MAPPING_HIROM;
	}
	else
	{
		pCartInfo = NULL;
	}

#if SNDBG_LOG
	DLog("[rom-map] final lo=%d hi=%d mapper=%s title='%.21s'",
	     (int)nLoScore, (int)nHiScore,
	     pCartInfo ? (m_eMapping == SNROM_MAPPING_HIROM ? "HiROM" : "LoROM") : "none",
	     pCartInfo ? (const char *)pCartInfo->Title : "");
#endif

	SetCartInfo(pCartInfo);

	/* AURORA_V4_4_CUMULATIVE_20260908
	 * The interface/base cartridge owns a slot but keeps its ordinary ROM
	 * mapping beneath the dynamic MCC. Other slotted carts retain BSC mapping. */
	if (_SNRomIsBSXBase(pCartInfo))
	{
		m_Flags |= SNROM_FLAG_BSXSLOT | SNROM_FLAG_BSXBASE;
	}
	else if (_SNRomHasBSXSlot(m_pRomData, m_uRomBytes, pCartInfo))
	{
		m_Flags |= SNROM_FLAG_BSXSLOT;
		if (!(m_Flags & SNROM_FLAG_SA1))
		{
			if (m_eMapping == SNROM_MAPPING_LOROM)
				m_eMapping = SNROM_MAPPING_BSCLOROM;
			else if (m_eMapping == SNROM_MAPPING_HIROM)
				m_eMapping = SNROM_MAPPING_BSCHIROM;
		}
	}

	// ---- ExLoROM (Jumbo LoROM): LoROM maior que 4MB ----
	// Hacks grandes (ex.: SMW expandida pelo Lunar Magic) usam ExLoROM:
	// a metade de cima dos bancos ($80-$FF) deixa de ser espelho e passa a
	// conter dados extras, chegando a 8MB. Seguimos o reference emulator
	// (Map_JumboLoROMMap): a ROM e' normalizada para que a metade que tem o
	// header/vetores fique em offset 0x400000 (mapeada em $00-$3F, de onde a
	// CPU le os vetores) e os outros 4MB em offset 0 ($80-$FF).
	// S-DD1 (Star Ocean tem 48Mbit): NAO e' ExLoROM. O >4MB e' acessado
	// pela troca de segmento de 1MB do S-DD1 na janela $C0-$FF, com a ROM
	// no formato linear original (sem rearranjo).
	if (m_eMapping == SNROM_MAPPING_LOROM && m_uRomBytes > 0x400000
	    && !(m_Flags & SNROM_FLAG_SDD1))
	{
		int score0  = _ExLoRomHeaderScore(m_pRomData, 0x007FC0, m_uRomBytes);
		int score4M = _ExLoRomHeaderScore(m_pRomData, 0x407FC0, m_uRomBytes);

		// header na frente do arquivo -> trocar as metades para coloca-lo
		// em 0x400000 (caso "SMALLFIRST" do reference emulator).
		if (score0 > score4M)
		{
			Uint32 smallBytes = m_uRomBytes - 0x400000;
			Uint8 *pTmp = (Uint8 *)malloc(smallBytes);
			if (pTmp)
			{
				memcpy (pTmp, m_pRomData, smallBytes);                     // metade da frente (com header)
				memmove(m_pRomData, m_pRomData + smallBytes, 0x400000);    // 4MB de tras -> frente
				memcpy (m_pRomData + 0x400000, pTmp, smallBytes);          // header -> 0x400000
				free(pTmp);
			}
		}

		m_eMapping = SNROM_MAPPING_EXLOROM;
	}

	/* The CRC alone is not enough: require the cartridge header to have
	 * classified the image as DSP-1 before enabling a DSP-1 revision quirk. */
	if ((m_Flags & SNROM_FLAG_DSP1) && bOriginalDSP1Op28)
		m_Flags |= SNROM_FLAG_DSP1_ORIGINAL_OP28;

	m_bLoaded   = true;
	return LOADERROR_NONE;
}

void SnesRom::Unload()
{
	if (m_pRomMem)
	{
		free(m_pRomMem);
		m_pRomMem = NULL;
	}

	m_pCartInfo = NULL;
	m_pRomData = NULL;
	m_uRomBytes = 0;
	m_bLoaded   = false;
	memset(m_Name, 0, sizeof(m_Name));
}



Uint32 SnesRom::GetNumExts()
{
	/* The original iaddis SNESticle only registered .smc and .fig (the
	   two SNES ROM extensions that were common when it was written).
	   Modern dumps almost always come as .sfc (Super Famicom) and the
	   older Super Wild Card dumps use .swc; without these the browser
	   silently classifies those files as BROWSER_ENTRYTYPE_OTHER and
	   refuses to launch them. List all four flavours so the launcher
	   recognises the ROMs people actually have. */
	return 4;
}

char *SnesRom::GetExtName(Uint32 uExt)
{
	switch (uExt)
	{
		case 0:
			return (char *)"smc";
		case 1:
			return (char *)"sfc";
		case 2:
			return (char *)"swc";
		case 3:
			return (char *)"fig";
		default:
			return NULL;
	}
}

/* virtual */
char   *SnesRom::GetMapperName()
{
	return NULL;
}
