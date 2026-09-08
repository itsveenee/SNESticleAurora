
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "snes.h"
#include "console.h"
#include "snmemmap.h"
#include "sndebug.h"
#include "sndbglog.h"
#include "file.h"

static SnesMemMapT	_SnesMemMap_LoRom[]=
{
	// map slow rom (estendido $00-$7D para cobrir 4MB; $7E-$7F e' WRAM)
	{0x00, 0x7D, 0x8000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_ROM},

	// map fast rom (estendido $80-$FF: cobre os 4MB inteiros via FastROM;
	// o wrap usa o tamanho real da ROM, entao ROMs menores continuam
	// espelhando certo)
	{0x80, 0xFF, 0x8000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_ROM},

	// mirror rom in lower 32k: a metade baixa ($0000-7FFF) de cada banco
	// espelha a metade ALTA ($8000-FFFF) do MESMO banco (LoROM). O offset
	// inicial 0x200000 faz o banco $40/$C0 casar com o offset da metade
	// alta (0x40*0x8000 = 0x200000). Sem isso, ROMs grandes (>~2MB) liam
	// dados do offset 0 -> graficos/niveis embaralhados (ex.: SMW 4MB
	// "12 Magic Orbs"). Em ROMs pequenas o wrap escondia o bug.
	{0x40, 0x6F, 0x0000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_ROM, 0x200000},
	{0xC0, 0xFF, 0x0000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_ROM, 0x200000},

	/* AURORA_MEGA_V2_SRAM_MAP
	 * Standard LoROM SRAM windows. $f0-$ff is a real high-bank mirror
	 * used by commercial software; $70-$7d is the low-bank window. */
	{0x70, 0x7D, 0x0000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},
	{0xF0, 0xFF, 0x0000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},

	// map ram
	{0x7E, 0x7F, 0x0000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_RAM},

	// map lo-ram / ppu areas
	{0x00, 0x3F, 0x0000, 0x1FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_LORAM},
	{0x00, 0x3F, 0x2000, 0x3FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU0},
	{0x00, 0x3F, 0x4000, 0x5FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU1},

	{0x80, 0xBF, 0x0000, 0x1FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_LORAM},
	{0x80, 0xBF, 0x2000, 0x3FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU0},
	{0x80, 0xBF, 0x4000, 0x5FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU1},

	{0, 0, 0, 0, SNESMEM_TYPE_NONE}
};

/* AURORA_MEGA_V31_LOROM_SRAM_DECODE
 * Small standard LoROM boards (ROM <= 2 MiB, SRAM <= 32 KiB) decode SRAM
 * across the complete $0000-$ffff range of banks $70-$7d/$f0-$ff. Larger
 * LoROMs leave the upper half to ROM. Keep this as a supplemental map so
 * the common mega-v2 low-half windows remain correct for every LoROM. */
static SnesMemMapT _SnesMemMap_LoRom_SRAMFullHigh[]=
{
	{0x70, 0x7D, 0x8000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},
	{0xF0, 0xFF, 0x8000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},
	{0, 0, 0, 0, SNESMEM_TYPE_NONE}
};

static SnesMemMapT	_SnesMemMap_HiRom[]=
{
	// map slow rom
	{0x00, 0x3F, 0x0000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_ROM},
	{0x40, 0x6F, 0x0000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_ROM},

	// map fast rom
	{0x80, 0xBF, 0x0000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_ROM},
	{0xC0, 0xFF, 0x0000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_ROM},

	// standard HiROM SRAM windows
	{0x20, 0x3F, 0x6000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},
	{0xA0, 0xBF, 0x6000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},

	// map ram
	{0x7E, 0x7F, 0x0000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_RAM},

	// map lo-ram / ppu areas
	{0x00, 0x3F, 0x0000, 0x1FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_LORAM},
	{0x00, 0x3F, 0x2000, 0x3FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU0},
	{0x00, 0x3F, 0x4000, 0x5FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU1},

	{0x80, 0xBF, 0x0000, 0x1FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_LORAM},
	{0x80, 0xBF, 0x2000, 0x3FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU0},
	{0x80, 0xBF, 0x4000, 0x5FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU1},

	{0, 0, 0, 0, SNESMEM_TYPE_NONE}
};



/* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNMEMMAP_CPP
 * System-only portions of the two BSC board maps.  ROM and the Memory Pack
 * are installed explicitly below because BSC-HiROM has shadow regions whose
 * 64 KiB stride cannot be represented by the legacy linear SnesMemMapT. */
static SnesMemMapT _SnesMemMap_BSCLoRom_Sys[]=
{
    {0x70, 0x7D, 0x0000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},
    {0xF0, 0xFF, 0x0000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},
    {0x7E, 0x7F, 0x0000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_RAM},
    {0x00, 0x3F, 0x0000, 0x1FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_LORAM},
    {0x00, 0x3F, 0x2000, 0x3FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU0},
    {0x00, 0x3F, 0x4000, 0x5FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU1},
    {0x80, 0xBF, 0x0000, 0x1FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_LORAM},
    {0x80, 0xBF, 0x2000, 0x3FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU0},
    {0x80, 0xBF, 0x4000, 0x5FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU1},
    {0, 0, 0, 0, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_NONE}
};

static SnesMemMapT _SnesMemMap_BSCHiRom_Sys[]=
{
    {0x20, 0x3F, 0x6000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},
    {0xA0, 0xBF, 0x6000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},
    {0x7E, 0x7F, 0x0000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_RAM},
    {0x00, 0x3F, 0x0000, 0x1FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_LORAM},
    {0x00, 0x3F, 0x2000, 0x3FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU0},
    {0x00, 0x3F, 0x4000, 0x5FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU1},
    {0x80, 0xBF, 0x0000, 0x1FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_LORAM},
    {0x80, 0xBF, 0x2000, 0x3FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU0},
    {0x80, 0xBF, 0x4000, 0x5FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU1},
    {0, 0, 0, 0, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_NONE}
};

#if SNES_DSP1
static SnesMemMapT	_SnesMemMap_HiRom_DSP1[]=
{
	{0x00, 0x1F, 0x6000, 0x7FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_DSP1},
	{0x80, 0x9F, 0x6000, 0x7FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_DSP1},

	{0, 0, 0, 0, SNESMEM_TYPE_NONE}
};
#endif


#if SNES_DSP1
static SnesMemMapT _SnesMemMap_LoRom_DSP1[]={
    {0x20,0x3F,0x8000,0xBFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0xA0,0xBF,0x8000,0xBFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    /* DR mirror $C0-$CF (regiao FastROM): jogos como Super Mario Kart
       apontam o HDMA do Mode-7 para o espelho do DSP em $C0-$CF.
       Sem isto o HDMA le ROM em vez do registrador de dados do DSP e a
       matriz Mode-7 vira lixo -> pista achatada (mas a CPU, que usa
       $30-$3F, funciona, por isso o jogo "roda" mesmo assim). Veja o
       mapa de memoria do DSP-1 LoROM ($30-$3F / $C0-$CF). */
    {0xC0,0xCF,0x8000,0xBFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    /* SR (Status Register): $20-$3F:C000-FFFF + mirror $A0-$BF + $C0-$CF
       Sem isto a CPU le ROM em vez do status e o DSP-1 trava */
    {0x20,0x3F,0xC000,0xFFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0xA0,0xBF,0xC000,0xFFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0xC0,0xCF,0xC000,0xFFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0,0,0,0,SNESMEM_TYPE_NONE}
};

/* AURORA_DSP4_REAL_LOROM_MAP_20260831
 * DSP-4 (Top Gear 3000 / Planet's Champ TG3000) uses the SHVC-1B0N-01
 * uPD77C25 decode, not Aurora's broad DSP-1 compatibility map:
 *
 *   $30-$3F/$B0-$BF:$8000-$BFFF = Data Register
 *   $30-$3F/$B0-$BF:$C000-$FFFF = Status Register
 *
 * Keep the ranges split into 8 KiB pages because this core's generic
 * MapMem() represents trapped devices at SNCPU_BANK_SIZE granularity.
 * This changes address decoding only; the existing DSP-4 HLE is untouched.
 */
static SnesMemMapT _SnesMemMap_LoRom_DSP4[]={
    {0x30,0x3F,0x8000,0x9FFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0x30,0x3F,0xA000,0xBFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0x30,0x3F,0xC000,0xDFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0x30,0x3F,0xE000,0xFFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0xB0,0xBF,0x8000,0x9FFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0xB0,0xBF,0xA000,0xBFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0xB0,0xBF,0xC000,0xDFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0xB0,0xBF,0xE000,0xFFFF,SNCPU_CYCLE_FAST,SNESMEM_TYPE_DSP1},
    {0,0,0,0,SNESMEM_TYPE_NONE}
};
#endif


// OBC1 (Metal Combat): 8KB de RAM + registradores em $6000-$7FFF,
// nos bancos LoROM $00-$3F e espelho FastROM $80-$BF.
static SnesMemMapT _SnesMemMap_OBC1[]={
    {0x00,0x3F,0x6000,0x7FFF,SNCPU_CYCLE_SLOW,SNESMEM_TYPE_OBC1},
    {0x80,0xBF,0x6000,0x7FFF,SNCPU_CYCLE_SLOW,SNESMEM_TYPE_OBC1},
    {0,0,0,0,SNESMEM_TYPE_NONE}
};

// CX4 (Mega Man X2/X3): C4RAM + registradores em $6000-$7FFF, nos bancos
// LoROM $00-$3F e espelho FastROM $80-$BF.
static SnesMemMapT _SnesMemMap_CX4[]={
    {0x00,0x3F,0x6000,0x7FFF,SNCPU_CYCLE_SLOW,SNESMEM_TYPE_CX4},
    {0x80,0xBF,0x6000,0x7FFF,SNCPU_CYCLE_SLOW,SNESMEM_TYPE_CX4},
    {0,0,0,0,SNESMEM_TYPE_NONE}
};

/* AURORA_SWC_DONOR_CART_V1_20260902
 * A floppy game has no SnesRom object: the real copier BIOS has already
 * assembled it in DRAM. Read only the normalized SNES internal header to
 * decide whether the game is wired for the chip exposed by the donor cart.
 * Do not require a valid checksum here; translations/hacks often update the
 * program without repairing the header pair, while RomType + reset vector +
 * the chip-specific title discriminator are still useful and deterministic. */
static Uint32 _SnesSwcDramCoprocessorFlags(
    const SNSuperWildCard &swc)
{
    const Uint8 *pDram = swc.GetDRAMData();
    Uint32 nBytes = swc.GetDRAMBytes();
    Uint32 uHeader = (swc.GetParallelMode() & 0x01) ? 0xFFC0u : 0x7FC0u;
    const SNRomInfoT *pInfo;
    Uint16 uReset;
    Uint32 uFlags = 0;
    Char uTitle[22];
    Int32 i;

    if (!pDram || uHeader + 0x40u > nBytes)
        return 0;

    pInfo = (const SNRomInfoT *)(pDram + uHeader);
    uReset = (Uint16)pDram[uHeader + 0x3Cu] |
             ((Uint16)pDram[uHeader + 0x3Du] << 8);
    if (uReset < 0x8000u || uReset == 0xFFFFu)
        return 0;

    for (i = 0; i < 21; ++i)
    {
        Char c = (Char)pInfo->Title[i];
        if (c >= 'a' && c <= 'z') c = (Char)(c - ('a' - 'A'));
        uTitle[i] = c;
    }
    uTitle[21] = 0;

    if (pInfo->RomMakeup == 0x23 &&
        (pInfo->RomType == 0x34 || pInfo->RomType == 0x35))
        return SNROM_FLAG_SA1;

    switch (pInfo->RomType)
    {
        case 3:
        case 4:
        case 5:
            uFlags = SNROM_FLAG_DSP1;
            break;
        case 0x13:
        case 0x14:
        case 0x15:
        case 0x1A:
            uFlags = SNROM_FLAG_SUPERFX;
            break;
        case 0xE3:
            uFlags = SNROM_FLAG_GAMEBOY;
            break;
        case 0xF6:
            uFlags = SNROM_FLAG_DSP2;
            break;
        default:
            break;
    }

    if (uFlags & SNROM_FLAG_DSP1)
    {
        if (!strncmp(uTitle, "DUNGEON", 7))
            uFlags = SNROM_FLAG_DSP2;
        else if (strstr(uTitle, "TOP GEAR 3000") ||
                 strstr(uTitle, "TG3000"))
            uFlags = SNROM_FLAG_DSP4;
        else if (strstr(uTitle, "GUNDAM") ||
                 (uTitle[0] == 'S' && uTitle[1] == 'D' &&
                  (Uint8)pInfo->Title[2] >= 0x80))
            uFlags = SNROM_FLAG_DSP3;
    }

    if (!strncmp(uTitle, "METAL COMBAT", 12))
        uFlags = SNROM_FLAG_OBC1;

    if (!strncmp(uTitle, "MEGAMAN X2", 10) ||
        !strncmp(uTitle, "MEGAMAN X3", 10) ||
        !strncmp(uTitle, "ROCKMAN X2", 10) ||
        !strncmp(uTitle, "ROCKMAN X3", 10))
        uFlags = SNROM_FLAG_CX4;

    if ((pInfo->RomType & 0xF0) == 0x40)
        uFlags = SNROM_FLAG_SDD1;
    if ((pInfo->RomType & 0xF0) == 0x50)
        uFlags = SNROM_FLAG_SRTC;

    return uFlags;
}

// SuperFX Game Pak RAM. Diferente da SRAM LoROM comum, os bancos $70-$71
// inteiros sao RAM; o tamanho fisico (normalmente 32/64 KiB) espelha dentro
// destes 128 KiB de espaco.
static SnesMemMapT _SnesMemMap_SuperFXRAM[]={
    {0x70,0x71,0x0000,0xFFFF,SNCPU_CYCLE_SLOW,SNESMEM_TYPE_SRAM},
    {0,0,0,0,SNESMEM_TYPE_NONE}
};

// GSU1 acrescenta espelhos altos de Game Pak RAM em $F0-$F1.
static SnesMemMapT _SnesMemMap_SuperFXGSU1RAM[]={
    {0x70,0x71,0x0000,0xFFFF,SNCPU_CYCLE_SLOW,SNESMEM_TYPE_SRAM},
    {0xF0,0xF1,0x0000,0xFFFF,SNCPU_CYCLE_SLOW,SNESMEM_TYPE_SRAM},
    {0,0,0,0,SNESMEM_TYPE_NONE}
};

// O Mario Chip 1 do Star Fox usa uma decodificacao anterior: os 32 KiB de
// Game Pak RAM aparecem, espelhados, em todos os bancos $60-$7D/$E0-$FF.
// Sem este mapa o 65816 le ROM/open-bus no lugar do framebuffer/work RAM e o
// jogo para logo no boot. Os GSU1/GSU2 posteriores usam $70-$71.
static SnesMemMapT _SnesMemMap_SuperFXMC1RAM[]={
    {0x60,0x7D,0x0000,0xFFFF,SNCPU_CYCLE_SLOW,SNESMEM_TYPE_SRAM},
    {0xE0,0xFF,0x0000,0xFFFF,SNCPU_CYCLE_SLOW,SNESMEM_TYPE_SRAM},
    {0,0,0,0,SNESMEM_TYPE_NONE}
};

enum
{
    SNES_SUPERFX_BOARD_MC1 = 1,
    SNES_SUPERFX_BOARD_GSU1,
    SNES_SUPERFX_BOARD_GSU2
};

/* Os headers nao identificam a revisao do GSU. A lista de cartuchos e' fixa,
   entao o titulo interno distingue MC1/GSU1/GSU2 sem depender do nome do ZIP.
   Desconhecidos/homebrews usam o mapa GSU2, que e' o mais novo. */
static Int32 _SnesSuperFXBoardType(const Char *pTitle)
{
    Char compact[22];
    Int32 n = 0;
    if (!pTitle) return SNES_SUPERFX_BOARD_GSU2;

    while (*pTitle && n < 21)
    {
        Char c = *pTitle++;
        if (c >= 'a' && c <= 'z') c = (Char)(c - ('a' - 'A'));
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
            compact[n++] = c;
    }
    compact[n] = 0;

    // Star Fox 2 e' a excecao de 1 MiB que ja usa GSU2.
    if (n >= 8 && !memcmp(compact, "STARFOX2", 8))
        return SNES_SUPERFX_BOARD_GSU2;

    if ((n >= 7 && !memcmp(compact, "STARFOX", 7)) ||
        (n >= 8 && !memcmp(compact, "STARWING", 8)))
        return SNES_SUPERFX_BOARD_MC1;

    if ((n >= 9  && !memcmp(compact, "DIRTRACER", 9)) ||
        (n >= 10 && !memcmp(compact, "DIRTTRAXFX", 10)) ||
        (n >= 10 && !memcmp(compact, "POWERSLIDE", 10)) ||
        (n >= 11 && !memcmp(compact, "STUNTRACEFX", 11)) ||
        (n >= 8  && !memcmp(compact, "WILDTRAX", 8)) ||
        (n >= 6  && !memcmp(compact, "VORTEX", 6)))
        return SNES_SUPERFX_BOARD_GSU1;

    return SNES_SUPERFX_BOARD_GSU2;
}

/* SNES ROM address lines do not mirror a non-power-of-two image with a
   simple modulo.  A 12-Mbit (1.5 MiB) cart, for example, mirrors its final
   4 Mbit into the next 4-Mbit window.  This is the recursive mirror used by
   real cartridge decoders (and by bsnes/reference emulator). */
static Uint32 _SnesMirrorRomOffset(Uint32 uSize, Uint32 uPos)
{
	Uint32 uMask;

	if (uSize == 0 || uPos < uSize)
		return (uSize == 0) ? 0 : uPos;

	uMask = 0x80000000u;
	while (!(uPos & uMask))
		uMask >>= 1;

	if (uSize <= (uPos & uMask))
		return _SnesMirrorRomOffset(uSize, uPos - uMask);

	return uMask + _SnesMirrorRomOffset(uSize - uMask, uPos - uMask);
}

/* Sobrepoe o mapa generico LoROM com as duas visoes do Program ROM usadas
   pelo GSU: 32 KiB/banco em $00-$3F e 64 KiB/banco em $40-$5F. Os espelhos
   altos sao mantidos porque os jogos comerciais tambem os enxergam. */
static void _MapSuperFXRom(SNCpuT *pCpu, Uint8 *pRom, Uint32 uRomBytes, Int32 nBoard)
{
	Uint32 uBank, uPage;
	if (!pRom || !uRomBytes) return;

	for (uBank = 0; uBank <= 0x3F; uBank++)
	{
		Uint32 uOffset = _SnesMirrorRomOffset(uRomBytes, uBank * 0x8000);
		Uint32 uAddr = uBank << 16;
		SNCPUSetMemSpeed(pCpu, uAddr | 0x8000, 0x8000, SNCPU_CYCLE_SLOW);
		SNCPUSetBank(pCpu, uAddr | 0x8000, 0x8000, pRom + uOffset, FALSE);
		SNCPUSetMemSpeed(pCpu, (uAddr | 0x800000) | 0x8000,
		                  0x8000, SNCPU_CYCLE_SLOW);
		SNCPUSetBank(pCpu, (uAddr | 0x800000) | 0x8000,
		             0x8000, pRom + uOffset, FALSE);
	}

	for (uBank = 0; uBank <= 0x1F; uBank++)
	{
		Uint32 uAddr = (0x40 + uBank) << 16;
		Uint32 uMirrorAddr = (0xC0 + uBank) << 16;
		for (uPage = 0; uPage < 0x10000; uPage += SNCPU_BANK_SIZE)
		{
			Uint32 uOffset = _SnesMirrorRomOffset(
				uRomBytes, uBank * 0x10000 + uPage);
			SNCPUSetMemSpeed(pCpu, uAddr + uPage,
			                  SNCPU_BANK_SIZE, SNCPU_CYCLE_SLOW);
			SNCPUSetBank(pCpu, uAddr + uPage,
			             SNCPU_BANK_SIZE, pRom + uOffset, FALSE);
			SNCPUSetMemSpeed(pCpu, uMirrorAddr + uPage,
			                  SNCPU_BANK_SIZE, SNCPU_CYCLE_SLOW);
			SNCPUSetBank(pCpu, uMirrorAddr + uPage,
			             SNCPU_BANK_SIZE, pRom + uOffset, FALSE);
		}
	}


    /* AURORA_GSU_REFERENCE_REVIEW_V1_20260902: GSU2 CPU ROM
     * GSU2 can expose CPU-only ROM beyond the 2 MiB shared GSU window.
     * Keep <=2 MiB carts byte-for-byte unchanged; only large GSU2/homebrew
     * images gain E0-FF -> ROM 2-4 MiB. The GSU's own CodeRead/ROMBR path
     * is deliberately unchanged and remains limited to the shared window. */
    if (nBoard == SNES_SUPERFX_BOARD_GSU2 && uRomBytes > 0x200000u)
    {
        for (uBank = 0x20; uBank <= 0x3F; ++uBank)
        {
            Uint32 uAddr = (0xC0 + uBank) << 16;
            for (uPage = 0; uPage < 0x10000; uPage += SNCPU_BANK_SIZE)
            {
                Uint32 uOffset = _SnesMirrorRomOffset(
                    uRomBytes, uBank * 0x10000u + uPage);
                SNCPUSetMemSpeed(pCpu, uAddr + uPage,
                                  SNCPU_BANK_SIZE, SNCPU_CYCLE_SLOW);
                SNCPUSetBank(pCpu, uAddr + uPage,
                             SNCPU_BANK_SIZE, pRom + uOffset, FALSE);
            }
        }
    }
}

void SnesSystem::MapMem(SnesMemMapT *pMemMap)
{
	SNCpuT *pCpu = &m_Cpu;
	Uint32 uSize[SNESMEM_TYPE_NUM];
	SnesRom *pRom = m_pRom;

	// determine size of SRAM in bytes 
	m_uSramSize = pRom->GetSRAMBytes();
	if (m_uSramSize > SNES_SRAMSIZE) m_uSramSize = SNES_SRAMSIZE; 
	//uSRAMBytes = (uSRAMBytes + SNCPU_BANK_SIZE - 1)  & ~(SNCPU_BANK_MASK);

	// calculate size of each map type
	Int32 i;
	for (i=0; i < SNESMEM_TYPE_NUM; i++)
	{
		uSize[i] = SNCPU_BANK_SIZE;
	}

	uSize[SNESMEM_TYPE_NONE]	 = 0;
	uSize[SNESMEM_TYPE_UNMAPPED] = 0;
	uSize[SNESMEM_TYPE_ROM]      = pRom->GetBytes();
	uSize[SNESMEM_TYPE_RAM]      = SNES_RAMSIZE;
	uSize[SNESMEM_TYPE_SRAM]     = m_uSramSize;

	while (pMemMap->eMemType!=SNESMEM_TYPE_NONE)
	{
		Uint32 uBank;
		Uint32 uOffset;

		uOffset = pMemMap->uOffset;

		// iterate through bank range
		for (uBank = pMemMap->uStartBank; uBank <= pMemMap->uEndBank; uBank++)
		{
			Uint32 uStartAddr, uEndAddr, nBytes;

			// determine range to map for this bank
			uStartAddr = (uBank << 16) | (Uint32)pMemMap->uStartAddr;
			uEndAddr   = (uBank << 16) | (Uint32)pMemMap->uEndAddr;
			uEndAddr++;

			assert(uStartAddr <= uEndAddr);
			nBytes     = uEndAddr - uStartAddr;

			//ConDebug("Mapping %06X -> %06X %06X %d\n", uStartAddr, uEndAddr, uOffset, pMemMap->eMemType);

			/* ROM keeps its logical offset here: each 8 KiB CPU page is
			   mirrored below with the cartridge address-line rule.  Other
			   memory types retain the legacy linear wrapping behavior. */
			if (pMemMap->eMemType != SNESMEM_TYPE_ROM)
			{
				// wrap address to size of memory
				if (uOffset >= uSize[pMemMap->eMemType])
					uOffset = 0;

				// wrap byte count to size of memory
				if (nBytes >= uSize[pMemMap->eMemType])
					nBytes = uSize[pMemMap->eMemType];
			}

			if (nBytes > 0)
			{
				Uint32 uAlignedBytes = (nBytes + SNCPU_BANK_SIZE - 1)  & ~(SNCPU_BANK_MASK);

				// set memory speed for region
				SNCPUSetMemSpeed(pCpu, uStartAddr, uAlignedBytes, pMemMap->uSpeed);

					switch (pMemMap->eMemType)
					{
					case SNESMEM_TYPE_ROM:
						{
							Uint32 uPage;
							Uint32 uRomBytes = pRom->GetBytes();
							for (uPage = 0; uPage < nBytes; uPage += SNCPU_BANK_SIZE)
							{
								Uint32 uRomOffset = _SnesMirrorRomOffset(
									uRomBytes, uOffset + uPage);
								SNCPUSetBank(pCpu, uStartAddr + uPage,
								              SNCPU_BANK_SIZE,
								              pRom->GetData() + uRomOffset, FALSE);
							}
						}
						break;
				case SNESMEM_TYPE_SRAM:
					if (nBytes & (SNCPU_BANK_SIZE - 1))
					{
						// size of sram wont map evenly to our bank size, so we must use a traphandler for reads/writes
						/* AURORA_MEGA_V31_SRAM_SMALL_MIRROR_TRAP
						 * Small SRAM is mirrored throughout the complete cartridge
						 * window; ReadSRAM/WriteSRAM perform the physical wrap. */
						SNCPUSetTrap(pCpu, uStartAddr, uEndAddr - uStartAddr,
						             ReadSRAM, WriteSRAM);
					}
					else
					{
						// map mirrored
						while (uStartAddr < uEndAddr)
						{
							SNCPUSetBank(pCpu, uStartAddr, nBytes, m_SRam + uOffset, TRUE);
							uStartAddr += nBytes;
						}
					}
					break;
				case SNESMEM_TYPE_RAM:
				case SNESMEM_TYPE_LORAM:
					SNCPUSetBank(pCpu, uStartAddr, nBytes, m_Ram + uOffset, TRUE);
					break;
				case SNESMEM_TYPE_PPU0:
#if SNES_DEBUG
					SNCPUSetTrap(pCpu, uStartAddr, nBytes, Read2000Debug, Write2000Debug);
#else
                    SNCPUSetTrap(pCpu, uStartAddr, nBytes, Read2000, Write2000);
#endif
					break;
				case SNESMEM_TYPE_PPU1:
#if SNES_DEBUG
					SNCPUSetTrap(pCpu, uStartAddr, nBytes, Read4000Debug, Write4000Debug);
#else
                    SNCPUSetTrap(pCpu, uStartAddr, nBytes, Read4000, Write4000);
#endif
					break;
				case SNESMEM_TYPE_DSP1:
#ifdef SNES_DSP1
					SNCPUSetTrap(&m_Cpu, uStartAddr, nBytes, ReadDSP1, WriteDSP1);
#endif 
					break;
				case SNESMEM_TYPE_OBC1:
					SNCPUSetTrap(&m_Cpu, uStartAddr, nBytes, ReadOBC1, WriteOBC1);
					break;
				case SNESMEM_TYPE_CX4:
					SNCPUSetTrap(&m_Cpu, uStartAddr, nBytes, ReadCX4, WriteCX4);
					break;
				case SNESMEM_TYPE_GSU:
					SNCPUSetTrap(&m_Cpu, uStartAddr, nBytes, ReadGSU, WriteGSU);
					break;
				default:
					break;
				}
			}
			uOffset += nBytes;
		}

		pMemMap++;
	}
}



/* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNMEMMAP_CPP */
static void _SnesMapBSCROMPage(SNCpuT *pCpu, Uint8 *pRom, Uint32 nRomBytes,
                               Uint32 uBus, Uint32 uLogicalOffset)
{
    Uint32 uOffset;
    if (!pCpu || !pRom || !nRomBytes)
        return;
    uOffset = _SnesMirrorRomOffset(nRomBytes, uLogicalOffset);
    SNCPUSetMemSpeed(pCpu, uBus, SNCPU_BANK_SIZE, SNCPU_CYCLE_SLOW);
    SNCPUSetBank(pCpu, uBus, SNCPU_BANK_SIZE, pRom + uOffset, FALSE);
}

/* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908
 * Derby Stallion 96 and Sound Novel Tsukuru use the newer-observed BSC-LoROM
 * slot decode: C0-DF with A15 ignored (32 KiB per bank). Keep every other
 * BSC-LoROM board on Aurora's existing C0-EF / 64 KiB decode. */
static Bool _SnesBSCUses32KPackDecode(const SnesRom *pRom)
{
    const char *pTitle = pRom ? pRom->GetRomTitle() : NULL;
    return pTitle &&
        (!strcmp(pTitle, "DERBY STALLION 96") ||
         !strcmp(pTitle, "SOUND NOVEL-TCOOL"))
        ? TRUE : FALSE;
}

void SnesSystem::MapBSCLoRom(void)
{
    SNCpuT *pCpu = &m_Cpu;
    Uint8 *pRom = m_pRom ? m_pRom->GetData() : NULL;
    Uint32 nRomBytes = m_pRom ? m_pRom->GetBytes() : 0;
    struct RegionT { Uint8 b0, b1; Uint32 base; };
    static const RegionT Regions[] =
    {
        {0x00, 0x1F, 0x000000u},
        {0x20, 0x3F, 0x100000u},
        {0x80, 0x9F, 0x200000u},
        {0xA0, 0xBF, 0x100000u},
    };
    Uint32 r, bank, a;
    Uint32 slotEnd = _SnesBSCUses32KPackDecode(m_pRom)
        ? 0xDFU : 0xEFU; /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908 */

    MapMem(_SnesMemMap_BSCLoRom_Sys);

    for (r = 0; r < sizeof(Regions) / sizeof(Regions[0]); ++r)
    {
        for (bank = Regions[r].b0; bank <= Regions[r].b1; ++bank)
        {
            for (a = 0x8000; a < 0x10000; a += SNCPU_BANK_SIZE)
            {
                Uint32 logical = Regions[r].base +
                    (bank - Regions[r].b0) * 0x8000u + (a - 0x8000u);
                _SnesMapBSCROMPage(pCpu, pRom, nRomBytes,
                                   (bank << 16) | a, logical);
            }
        }
    }

    /* AURORA_V4_7_FINAL_UNIFIED_SGB_BSX8M_20260908: ordinary BSC C0-EF; Derby/Sound Novel C0-DF. */
    for (bank = 0xC0; bank <= slotEnd; ++bank)
    {
        for (a = 0; a < 0x10000; a += SNCPU_BANK_SIZE)
        {
            Uint32 bus = (bank << 16) | a;
            SNCPUSetMemSpeed(pCpu, bus, SNCPU_BANK_SIZE, SNCPU_CYCLE_SLOW);
            SNCPUSetTrap(pCpu, bus, SNCPU_BANK_SIZE, ReadBSXSlot, WriteBSXSlot);
        }
    }
}

void SnesSystem::MapBSCHiRom(void)
{
    SNCpuT *pCpu = &m_Cpu;
    Uint8 *pRom = m_pRom ? m_pRom->GetData() : NULL;
    Uint32 nRomBytes = m_pRom ? m_pRom->GetBytes() : 0;
    Uint32 bank, a;

    MapMem(_SnesMemMap_BSCHiRom_Sys);

    /* ROM shadow 00-1F:8000-FFFF. */
    for (bank = 0x00; bank <= 0x1F; ++bank)
        for (a = 0x8000; a < 0x10000; a += SNCPU_BANK_SIZE)
            _SnesMapBSCROMPage(pCpu, pRom, nRomBytes, (bank << 16) | a,
                               bank * 0x10000u + a);

    /* ROM linear 40-5F:0000-FFFF. */
    for (bank = 0x40; bank <= 0x5F; ++bank)
        for (a = 0; a < 0x10000; a += SNCPU_BANK_SIZE)
            _SnesMapBSCROMPage(pCpu, pRom, nRomBytes, (bank << 16) | a,
                               (bank - 0x40u) * 0x10000u + a);

    /* ROM shadow 80-9F and linear C0-DF. */
    for (bank = 0x80; bank <= 0x9F; ++bank)
        for (a = 0x8000; a < 0x10000; a += SNCPU_BANK_SIZE)
            _SnesMapBSCROMPage(pCpu, pRom, nRomBytes, (bank << 16) | a,
                               (bank - 0x80u) * 0x10000u + a);
    for (bank = 0xC0; bank <= 0xDF; ++bank)
        for (a = 0; a < 0x10000; a += SNCPU_BANK_SIZE)
            _SnesMapBSCROMPage(pCpu, pRom, nRomBytes, (bank << 16) | a,
                               (bank - 0xC0u) * 0x10000u + a);

    /* BSC-HiROM Memory Pack windows. */
    for (bank = 0x20; bank <= 0x3F; ++bank)
        for (a = 0x8000; a < 0x10000; a += SNCPU_BANK_SIZE)
        {
            Uint32 bus = (bank << 16) | a;
            SNCPUSetMemSpeed(pCpu, bus, SNCPU_BANK_SIZE, SNCPU_CYCLE_SLOW);
            SNCPUSetTrap(pCpu, bus, SNCPU_BANK_SIZE, ReadBSXSlot, WriteBSXSlot);
        }
    for (bank = 0x60; bank <= 0x7D; ++bank)
        for (a = 0; a < 0x10000; a += SNCPU_BANK_SIZE)
        {
            Uint32 bus = (bank << 16) | a;
            SNCPUSetMemSpeed(pCpu, bus, SNCPU_BANK_SIZE, SNCPU_CYCLE_SLOW);
            SNCPUSetTrap(pCpu, bus, SNCPU_BANK_SIZE, ReadBSXSlot, WriteBSXSlot);
        }
    for (bank = 0xA0; bank <= 0xBF; ++bank)
        for (a = 0x8000; a < 0x10000; a += SNCPU_BANK_SIZE)
        {
            Uint32 bus = (bank << 16) | a;
            SNCPUSetMemSpeed(pCpu, bus, SNCPU_BANK_SIZE, SNCPU_CYCLE_SLOW);
            SNCPUSetTrap(pCpu, bus, SNCPU_BANK_SIZE, ReadBSXSlot, WriteBSXSlot);
        }
    for (bank = 0xE0; bank <= 0xFF; ++bank)
        for (a = 0; a < 0x10000; a += SNCPU_BANK_SIZE)
        {
            Uint32 bus = (bank << 16) | a;
            SNCPUSetMemSpeed(pCpu, bus, SNCPU_BANK_SIZE, SNCPU_CYCLE_SLOW);
            SNCPUSetTrap(pCpu, bus, SNCPU_BANK_SIZE, ReadBSXSlot, WriteBSXSlot);
        }
}

Bool SnesSystem::ResolveBSXSlotAddress(Uint32 uAddr, Uint32 *pOffset) const
{
    Uint8 bank;
    Uint16 addr;
    Uint32 off;
    if (!pOffset || !m_pRom || !m_BSXMemory.IsAttached())
        return FALSE;

    bank = (Uint8)(uAddr >> 16);
    addr = (Uint16)uAddr;

    if (m_pRom->m_eMapping == SNROM_MAPPING_BSCLOROM)
    {
        if (_SnesBSCUses32KPackDecode(m_pRom))
        {
            if (bank < 0xC0 || bank > 0xDF)
                return FALSE;
            off = ((Uint32)(bank - 0xC0) << 15) |
                  (Uint32)(addr & 0x7FFFu);
        }
        else
        {
            if (bank < 0xC0 || bank > 0xEF)
                return FALSE;
            off = ((Uint32)(bank - 0xC0) << 16) | addr;
        }
        *pOffset = off & (SNES_BSX_MEMORY_PACK_BYTES - 1);
        return TRUE;
    }

    if (m_pRom->m_eMapping == SNROM_MAPPING_BSCHIROM)
    {
        if (bank >= 0x20 && bank <= 0x3F && addr >= 0x8000)
            off = ((Uint32)(bank - 0x20) << 16) | addr;
        else if (bank >= 0x60 && bank <= 0x7D)
            off = ((Uint32)(bank - 0x60) << 16) | addr;
        else if (bank >= 0xA0 && bank <= 0xBF && addr >= 0x8000)
            off = ((Uint32)(bank - 0xA0) << 16) | addr;
        else if (bank >= 0xE0)
            off = ((Uint32)(bank - 0xE0) << 16) | addr;
        else
            return FALSE;

        *pOffset = off & (SNES_BSX_MEMORY_PACK_BYTES - 1);
        return TRUE;
    }

    return FALSE;
}

Uint8 SNCPU_TRAPFUNC SnesSystem::ReadBSXSlot(SNCpuT *pCpu, Uint32 uAddr)
{
    SnesSystem *pSnes = (SnesSystem *)pCpu->pUserData;
    Uint32 off;
    Uint8 v = pCpu->uMDR;
    if (pSnes && pSnes->ResolveBSXSlotAddress(uAddr, &off))
        v = pSnes->m_BSXMemory.Read(off);
    pCpu->uMDR = v;
    return v;
}

void SNCPU_TRAPFUNC SnesSystem::WriteBSXSlot(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData)
{
    SnesSystem *pSnes = (SnesSystem *)pCpu->pUserData;
    Uint32 off;
    pCpu->uMDR = uData;
    if (pSnes && pSnes->ResolveBSXSlotAddress(uAddr, &off))
        pSnes->m_BSXMemory.Write(off, uData);
}

/* AURORA_V4_4_CUMULATIVE_20260908
 * Potential cartridge-side pages owned by the MCC. System WRAM/PPU pages are
 * intentionally excluded.
 */
static Bool _SnesBSXBaseMCUPage(Uint8 bank, Uint16 addr)
{
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) &&
        addr >= 0x8000)
        return TRUE;

    if ((bank >= 0x40 && bank <= 0x7D) || bank >= 0xC0)
        return TRUE;

    if (((bank >= 0x20 && bank <= 0x3F) ||
         (bank >= 0xA0 && bank <= 0xBF)) &&
        addr >= 0x6000 && addr <= 0x7FFF)
        return TRUE;

    return FALSE;
}

void SnesSystem::MapBSXBase(void)
{
    SNCpuT *pCpu = &m_Cpu;
    Uint8 *pRom = m_pRom ? m_pRom->GetData() : NULL;
    Uint32 nRomBytes = m_pRom ? m_pRom->GetBytes() : 0;
    Uint8 *pPSRAM = m_BSXBase.GetPSRAM();
    Uint32 bank;
    Uint32 page;

    if (!m_BSXBase.IsActive() || !pRom || !nRomBytes || !pPSRAM)
        return;

    for (bank = 0; bank < 0x100u; ++bank)
    {
        for (page = 0; page < 0x10000u; page += SNCPU_BANK_SIZE)
        {
            Uint32 bus;
            Uint32 off = 0;
            SNBSXBase::AccessE kind;

            if (!_SnesBSXBaseMCUPage((Uint8)bank, (Uint16)page))
                continue;

            bus = (bank << 16) | page;

            /* SNCPUSetTrap clears pMem, so order is intentional:
             * trap baseline FIRST, then optional direct bank.
             * SNCPUSetBank preserves the trap callbacks.
             */
            SNCPUSetTrap(pCpu, bus, SNCPU_BANK_SIZE, ReadBSXMCC, WriteBSXMCC);
            SNCPUSetMemSpeed(pCpu, bus, SNCPU_BANK_SIZE, SNCPU_CYCLE_SLOW);

            kind = m_BSXBase.ResolveMCU(bus, &off);
            if (kind == SNBSXBase::ACCESS_ROM)
            {
                Uint32 romOff = _SnesMirrorRomOffset(nRomBytes, off);
                SNCPUSetBank(pCpu, bus, SNCPU_BANK_SIZE,
                             pRom + romOff, FALSE);
            }
            else if (kind == SNBSXBase::ACCESS_PSRAM)
            {
                SNCPUSetBank(
                    pCpu, bus, SNCPU_BANK_SIZE,
                    pPSRAM + (off & (SNES_BSX_PSRAM_BYTES - 1)), TRUE);
            }
            /* PACK stays trapped so the Type-1 command protocol is visible.
             * EX/NONE stay trapped and return S-CPU MDR/open bus.
             */
        }
    }

    SNCPUMirror24BitBus(pCpu);
}

Uint8 SNCPU_TRAPFUNC SnesSystem::ReadBSXMCC(SNCpuT *pCpu, Uint32 uAddr)
{
    SnesSystem *pSnes = (SnesSystem *)pCpu->pUserData;
    Uint8 value = pCpu->uMDR;

    if (pSnes && pSnes->m_BSXBase.IsActive())
        value = pSnes->m_BSXBase.ReadMCU(uAddr, value);

    pCpu->uMDR = value;
    return value;
}

void SNCPU_TRAPFUNC SnesSystem::WriteBSXMCC(
    SNCpuT *pCpu, Uint32 uAddr, Uint8 uData)
{
    SnesSystem *pSnes = (SnesSystem *)pCpu->pUserData;
    pCpu->uMDR = uData;

    if (pSnes && pSnes->m_BSXBase.IsActive())
        pSnes->m_BSXBase.WriteMCU(uAddr, uData);
}


/* AURORA_SA1_V1_REFERENCE_LOGIC_20260902 */
Uint8 SNCPU_TRAPFUNC SnesSystem::ReadSA1BWRAM(SNCpuT *pCpu, Uint32 uAddr)
{
    SnesSystem *pSnes = (SnesSystem *)pCpu->pUserData;
    Uint8 v = pSnes->m_SA1.ReadMainBWRAM(uAddr, pCpu->uMDR);
    pCpu->uMDR = v;
    return v;
}

void SNCPU_TRAPFUNC SnesSystem::WriteSA1BWRAM(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData)
{
    SnesSystem *pSnes = (SnesSystem *)pCpu->pUserData;
    pCpu->uMDR = uData;
    pSnes->m_SA1.WriteMainBWRAM(uAddr, uData);
}

Uint8 SNCPU_TRAPFUNC SnesSystem::ReadSA1ROM(SNCpuT *pCpu, Uint32 uAddr)
{
    SnesSystem *pSnes = (SnesSystem *)pCpu->pUserData;
    Uint8 v = pSnes->m_SA1.ReadMainROM(uAddr, pCpu->uMDR);
    pCpu->uMDR = v;
    return v;
}

void SNCPU_TRAPFUNC SnesSystem::WriteSA1ROM(SNCpuT *pCpu, Uint32 uAddr, Uint8 uData)
{
    SnesSystem *pSnes = (SnesSystem *)pCpu->pUserData;
    pCpu->uMDR = uData;
    if (pSnes)
        pSnes->m_SA1.WriteMainROM(uAddr, uData); /* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNMEMMAP_CPP */
}

/* AURORA_SWC_FLOPPY_V1_20260831 */
Uint8 SNCPU_TRAPFUNC SnesSystem::ReadSWC(SNCpuT *pCpu, Uint32 uAddr)
{
    SnesSystem *pSnes = (SnesSystem *)pCpu->pUserData;
    Uint8 uData = pCpu->uMDR;

    if (pSnes->m_bSuperWildCard &&
        pSnes->m_SWC.Read(uAddr, &uData, pSnes->m_SRam, 0x8000))
        return uData;

    return pCpu->uMDR;
}

void SNCPU_TRAPFUNC SnesSystem::WriteSWC(SNCpuT *pCpu,
                                         Uint32 uAddr, Uint8 uData)
{
    SnesSystem *pSnes = (SnesSystem *)pCpu->pUserData;
    if (pSnes->m_bSuperWildCard)
    {
        Bool handled =
            pSnes->m_SWC.Write(uAddr, uData, pSnes->m_SRam, 0x8000);
        Uint16 addr = (Uint16)(uAddr & 0xFFFF);

        /* AURORA_SWC_V11_MENU_FASTPATH_20260831
         * E000-E003 select the 8 KiB BIOS-mode DRAM page: update only that
         * direct window instead of rebuilding the complete 16 MiB map.
         *
         * E004-E007 really change System Mode and still require a full map.
         * C008 and E008-E00D do not change any direct Mode-0 window currently
         * installed by Aurora, so remapping the whole address space there was
         * pure host overhead.
         */
        if (handled && addr >= 0xE000 && addr <= 0xE003)
            pSnes->RemapSuperWildCardMode0Dram();
        else if (handled && addr >= 0xE004 && addr <= 0xE007)
        {
            /* AURORA_SWC_MODE_SWITCH_FETCH_ABORT_V1_20260902
             * E004-E007 change the physical S-CPU bus immediately.
             *
             * The PS2 MIPS 65816 executor caches the current direct-fetch
             * bank in R_PC/SP_PCBank. Rebuilding Bank[] alone is therefore
             * insufficient while the CPU is inside the WriteSWC trap: after
             * returning from the trap it could continue fetching opcodes
             * through the stale pre-switch firmware/ROM host pointer.
             *
             * Abort only the current execution slice after installing the new
             * map. SNCPUExecute() preserves the remaining cycle budget and the
             * next entry resolves PC against the new Bank[] mapping. No reset,
             * reset-vector jump or game-specific workaround is involved.
             */
            pSnes->MapSuperWildCard();
            SNCPUAbort(pCpu);
        }
    }
}

void SnesSystem::RemapSuperWildCardMode0Dram(void)
{
    SNCpuT *pCpu = &m_Cpu;
    Uint32 uBank;

    /* AURORA_FRONT_MODE0_PAGEBUS_V10_9_20260831
     * Each CPU bank contributes A16-A23 to the physical copier page
     * address. Never resolve bank 00 once and clone that pointer across the
     * whole bus. V10_8 intentionally keeps direct reads read-only so writes
     * remain observable by the diagnostic trap. */
    if (!m_bSuperWildCard)
        return;

    for (uBank = 0x00; uBank <= 0x7D; ++uBank)
    {
        Uint8 *pDram = NULL;
        if (m_SWC.ResolveDirectDram((Uint8)uBank, 0x8000, &pDram))
            SNCPUSetBank(pCpu, (uBank << 16) | 0x8000,
                         0x2000, pDram,
                         m_SWC.IsDirectDramWritable());
    }

    for (uBank = 0x80; uBank <= 0xFF; ++uBank)
    {
        Uint8 *pDram = NULL;
        if (m_SWC.ResolveDirectDram((Uint8)uBank, 0x8000, &pDram))
            SNCPUSetBank(pCpu, (uBank << 16) | 0x8000,
                         0x2000, pDram,
                         m_SWC.IsDirectDramWritable());
    }

    SNCPUMirror24BitBus(pCpu);
}

/* Install only a cartridge-side device overlay. MapMem() cannot be reused
 * here because a copier game deliberately has no m_pRom; its program image
 * belongs to SNSuperWildCard DRAM. */
void SnesSystem::MapSuperWildCardDevice(SnesMemMapT *pMemMap)
{
    SNCpuT *pCpu = &m_Cpu;

    while (pMemMap && pMemMap->eMemType != SNESMEM_TYPE_NONE)
    {
        Uint32 uBank;
        Uint32 nBytes =
            (Uint32)pMemMap->uEndAddr - (Uint32)pMemMap->uStartAddr + 1u;

        for (uBank = pMemMap->uStartBank;
             uBank <= pMemMap->uEndBank; ++uBank)
        {
            Uint32 uAddr = (uBank << 16) | pMemMap->uStartAddr;
            SNCPUSetMemSpeed(pCpu, uAddr, nBytes, pMemMap->uSpeed);

            switch (pMemMap->eMemType)
            {
                case SNESMEM_TYPE_DSP1:
#if SNES_DSP1
                    SNCPUSetTrap(pCpu, uAddr, nBytes,
                                 ReadDSP1, WriteDSP1);
#endif
                    break;
                case SNESMEM_TYPE_OBC1:
                    SNCPUSetTrap(pCpu, uAddr, nBytes,
                                 ReadOBC1, WriteOBC1);
                    break;
                case SNESMEM_TYPE_CX4:
                    SNCPUSetTrap(pCpu, uAddr, nBytes,
                                 ReadCX4, WriteCX4);
                    break;
                default:
                    break;
            }
        }
        ++pMemMap;
    }
}

void SnesSystem::MapSuperWildCardCoprocessor(void)
{
    const Uint32 uSupported =
        SNROM_FLAG_DSP1 | SNROM_FLAG_DSP2 | SNROM_FLAG_DSP3 |
        SNROM_FLAG_DSP4 | SNROM_FLAG_OBC1 | SNROM_FLAG_CX4 |
        SNROM_FLAG_SRTC | SNROM_FLAG_SA1;
    Uint8 uMode = m_SWC.GetSystemMode();
    Uint32 uDonor = m_SWC.GetExternalCartridgeFlags();
    Uint32 uActive = 0;
    Bool bOldSRTC = m_bSRTC;
#if SNES_DSP1
    ISNDSP *pOldDsp = m_pDsp;
#endif

    m_SA1.Detach(); /* AURORA_SA1_V1_REFERENCE_LOGIC_20260902 */
    m_bSA1IRQ = FALSE;
    m_bSDD1 = FALSE;
    m_bSRTC = FALSE;
    m_bSuperFX = FALSE;
#if SNES_DSP1
    m_pDsp = NULL;
#endif

    if (uMode == 1)
    {
        /* Run the inserted cart itself. */
        uActive = uDonor & uSupported;
    }
    else if (uMode == 2 || uMode == 3)
    {
        /* Run the DRAM dump and expose only a matching donor device. The
         * board's own address decoder is part of compatibility too: a HiROM
         * DSP cart cannot satisfy a LoROM game's $8000/$C000 register map. */
        Uint32 uLoaded = _SnesSwcDramCoprocessorFlags(m_SWC);
        Int32 iLoadedMapping =
            (m_SWC.GetParallelMode() & 0x01) ?
                SNROM_MAPPING_HIROM : SNROM_MAPPING_LOROM;

        if (m_SWC.GetExternalCartridgeMapping() == iLoadedMapping)
            uActive = uDonor & uLoaded & uSupported;
    }

#if SNES_DSP1
    m_DSP1.SetOriginalDistanceBug(
        (uDonor & SNROM_FLAG_DSP1_ORIGINAL_OP28) ? TRUE : FALSE);

    if (uActive & SNROM_FLAG_DSP1)
    {
        m_pDsp = &m_DSP1;
        MapSuperWildCardDevice(
            m_SWC.GetExternalCartridgeMapping() == SNROM_MAPPING_HIROM ?
                _SnesMemMap_HiRom_DSP1 : _SnesMemMap_LoRom_DSP1);
    }
    else if (uActive & SNROM_FLAG_DSP2)
    {
        m_pDsp = &m_DSP2;
        MapSuperWildCardDevice(
            m_SWC.GetExternalCartridgeMapping() == SNROM_MAPPING_HIROM ?
                _SnesMemMap_HiRom_DSP1 : _SnesMemMap_LoRom_DSP1);
    }
    else if (uActive & SNROM_FLAG_DSP4)
    {
        m_pDsp = &m_DSP4;
        MapSuperWildCardDevice(_SnesMemMap_LoRom_DSP4);
    }
    else if (uActive & SNROM_FLAG_DSP3)
    {
        /* The existing core intentionally exposes DSP-3 as an inert device
         * until its HLE exists; accepting a donor must remain crash-safe. */
        MapSuperWildCardDevice(_SnesMemMap_LoRom_DSP1);
    }

    if (m_pDsp && m_pDsp != pOldDsp)
        m_pDsp->Reset();
#endif

    if (uActive & SNROM_FLAG_OBC1)
        MapSuperWildCardDevice(_SnesMemMap_OBC1);

    if (uActive & SNROM_FLAG_CX4)
    {
        MapSuperWildCardDevice(_SnesMemMap_CX4);
        m_CX4.SetMemReader(CX4ReadMem, &m_Cpu);
    }

    if (uActive & SNROM_FLAG_SRTC)
    {
        m_bSRTC = TRUE;
        if (!bOldSRTC)
            m_SRTC.Reset();
    }

    if (uActive & SNROM_FLAG_SA1)
    {
        const Uint8 *pGameRom = NULL;
        Uint32 nGameBytes = 0;
        Uint8 *pBW = m_SWC.GetExternalCartridgeSRAMData();
        Uint32 nBW = m_SWC.GetExternalCartridgeSRAMBytes();
        Bool bMapMainRom = (uMode == 1) ? TRUE : FALSE;

        if (uMode == 1)
        {
            pGameRom = m_SWC.GetExternalCartridgeData();
            nGameBytes = m_SWC.GetExternalCartridgeBytes();
        }
        else
        {
            /* Donor mode: the floppy image remains the S-CPU program bus;
             * the same assembled DRAM image is the SA-1's ROM source. */
            pGameRom = m_SWC.GetDRAMData();
            nGameBytes = m_SWC.GetDRAMBytes();
        }

        /* AURORA_SA1_PERF_V8_3_2_20260903 */
        if (m_SA1.Attach(this, pGameRom, nGameBytes, pBW, nBW,
                         bMapMainRom, (uMode != 1) ? TRUE : FALSE, TRUE))
            m_SA1.MapMainCPU(&m_Cpu);
    }
}

void SnesSystem::MapSuperWildCard(void)
{
    SNCpuT *pCpu = &m_Cpu;
    Uint32 uBank;
    Uint32 uBlock;

    /* AURORA_SWC_MEGA_V9_20260831
     * Fail-closed trap baseline, then stable direct 8 KiB windows.
     * AURORA_SWC_V10_MENU_INDEX_CARTRESET_20260831: mode-0 BIOS ROM reads are direct; FDC/control stays trapped.
     * AURORA_SWC_V11_MENU_FASTPATH_20260831: mode-0 selected DRAM page is direct; page changes use a surgical remap.
     * Modes 2/3 retain the existing direct game/DRAM path.
     */
    SNCPUSetTrap(pCpu, 0, SNCPU_MEM_SIZE, ReadSWC, WriteSWC);
    SNCPUSetMemSpeed(pCpu, 0, SNCPU_MEM_SIZE, SNCPU_CYCLE_SLOW);

    for (uBank = 0; uBank < 0x100; ++uBank)
    {
        for (uBlock = 0; uBlock < 8; ++uBlock)
        {
            Uint16 addr = (Uint16)(uBlock << 13);
            Uint32 bus = (uBank << 16) | (Uint32)addr;
            Uint8 *pDram = NULL;
            const Uint8 *pCart = NULL;
            const Uint8 *pFirmware = NULL; /* AURORA_SWC_V10_MENU_INDEX_CARTRESET_20260831 */

            if (m_SWC.ResolveDirectFirmware(
                    (Uint8)uBank, addr, &pFirmware))
            {
                /* Reads direct; writes still use the installed SWC trap. */
                SNCPUSetBank(
                    pCpu, bus, 0x2000, (Uint8 *)pFirmware, FALSE);
            }
            else if (m_SWC.ResolveDirectDram((Uint8)uBank, addr, &pDram))
            {
                /* AURORA_FRONT_GAMEBUS_V10_7_20260831
                 * System Mode 0: loader page is real RW DRAM.
                 * System Modes 2/3: DRAM is cartridge ROM (R only).
                 * Keep direct reads, but trap writes in emulation modes. */
                SNCPUSetBank(
                    pCpu, bus, 0x2000, pDram,
                    m_SWC.IsDirectDramWritable());
            }
            else if (m_SWC.ResolveDirectCartridge(
                         (Uint8)uBank, addr, &pCart))
            {
                SNCPUSetBank(
                    pCpu, bus, 0x2000, (Uint8 *)pCart, FALSE);
            }
        }
    }

    SNCPUSetBank(pCpu, 0x7E0000, 0x20000, m_Ram, TRUE);

    for (uBank = 0; uBank <= 0x3F; ++uBank)
    {
        Uint32 uBase = uBank << 16;
        Uint32 uMirror = uBase | 0x800000;

        SNCPUSetBank(pCpu, uBase, 0x2000, m_Ram, TRUE);
        SNCPUSetBank(pCpu, uMirror, 0x2000, m_Ram, TRUE);

#if SNES_DEBUG
        SNCPUSetTrap(pCpu, uBase | 0x2000, 0x2000,
                     Read2000Debug, Write2000Debug);
        SNCPUSetTrap(pCpu, uMirror | 0x2000, 0x2000,
                     Read2000Debug, Write2000Debug);
        SNCPUSetTrap(pCpu, uBase | 0x4000, 0x2000,
                     Read4000Debug, Write4000Debug);
        SNCPUSetTrap(pCpu, uMirror | 0x4000, 0x2000,
                     Read4000Debug, Write4000Debug);
#else
        SNCPUSetTrap(pCpu, uBase | 0x2000, 0x2000, Read2000, Write2000);
        SNCPUSetTrap(pCpu, uMirror | 0x2000, 0x2000, Read2000, Write2000);
        SNCPUSetTrap(pCpu, uBase | 0x4000, 0x2000, Read4000, Write4000);
        SNCPUSetTrap(pCpu, uMirror | 0x4000, 0x2000, Read4000, Write4000);
#endif
        SNCPUSetMemSpeed(pCpu, uBase | 0x2000, 0x4000, SNCPU_CYCLE_FAST);
        SNCPUSetMemSpeed(pCpu, uMirror | 0x2000, 0x4000, SNCPU_CYCLE_FAST);
    }

    /* This must be last: the donor's decoded registers override the generic
     * copier DRAM/cart windows, exactly as a physical cartridge device does. */
    MapSuperWildCardCoprocessor();

    SNCPUMirror24BitBus(pCpu);
}


#if CODE_DEBUG
void SnesSystem::DumpMemMap()
{
	SNCpuT *pCpu = GetCpu();
	Uint32 uAddr;
	for (uAddr=0; uAddr < SNCPU_MEM_SIZE; uAddr+= SNCPU_BANK_SIZE)
	{
		SNCpuBankT *pBank =  &pCpu->Bank[uAddr >> SNCPU_BANK_SHIFT];
		ConDebug("%06X %p %p %p %d %d\n",
			uAddr,
			pBank->pMem,
			pBank->pReadTrapFunc,
			pBank->pWriteTrapFunc,
			pBank->uBankCycle,
			pBank->bRAM
			);
	}
}
#endif

/* Areas de sistema (SRAM/WRAM/LoRAM/PPU) do ExLoROM - iguais ao LoROM.
   Aplicadas DEPOIS do mapeamento de ROM para sobrepor (WRAM em $7E-$7F
   tem que vencer a ROM que a regiao $40-$7F mapeia ali). */
static SnesMemMapT _SnesMemMap_ExLoRom_Sys[]=
{
	/* v3.1: Jumbo/ExLoROM uses the standard LoROM SRAM mirrors. */
	{0x70, 0x7D, 0x0000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},
	{0xF0, 0xFF, 0x0000, 0x7FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_SRAM},
	{0x7E, 0x7F, 0x0000, 0xFFFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_RAM},
	{0x00, 0x3F, 0x0000, 0x1FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_LORAM},
	{0x00, 0x3F, 0x2000, 0x3FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU0},
	{0x00, 0x3F, 0x4000, 0x5FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU1},
	{0x80, 0xBF, 0x0000, 0x1FFF, SNCPU_CYCLE_SLOW, SNESMEM_TYPE_LORAM},
	{0x80, 0xBF, 0x2000, 0x3FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU0},
	{0x80, 0xBF, 0x4000, 0x5FFF, SNCPU_CYCLE_FAST, SNESMEM_TYPE_PPU1},
	{0, 0, 0, 0, SNESMEM_TYPE_NONE}
};

/* Mapeia uma faixa de bancos no estilo LoROM (32KB por banco). Se
   fullBank, a metade baixa ($0000-7FFF) espelha a alta ($8000-FFFF) do
   mesmo chunk. Replica o map_lorom_offset do reference emulator. */
static void _MapExLoRomRegion(SNCpuT *pCpu, Uint8 *pRom, Uint32 romBytes,
                              Uint32 bankS, Uint32 bankE, Bool fullBank,
                              Uint32 baseOffset)
{
	Uint32 c;
	if (romBytes == 0) return;
	for (c = bankS; c <= bankE; c++)
	{
		Uint32 chunk = baseOffset + ((c - bankS) * 0x8000);
		Uint8 *pMem;
		Uint32 bankAddr;
		chunk = _SnesMirrorRomOffset(romBytes, chunk);
		pMem     = pRom + chunk;
		bankAddr = c << 16;

		SNCPUSetMemSpeed(pCpu, bankAddr | 0x8000, 0x8000, SNCPU_CYCLE_SLOW);
		SNCPUSetBank    (pCpu, bankAddr | 0x8000, 0x8000, pMem, FALSE);
		if (fullBank)
		{
			SNCPUSetMemSpeed(pCpu, bankAddr | 0x0000, 0x8000, SNCPU_CYCLE_SLOW);
			SNCPUSetBank    (pCpu, bankAddr | 0x0000, 0x8000, pMem, FALSE);
		}
	}
}

// ----------------------------------------------------------------------
void SnesSystem::MapMemExLoRom(void)
{
	SNCpuT *pCpu     = &m_Cpu;
	Uint8  *pRom     = m_pRom->GetData();
	Uint32  romBytes = m_pRom->GetBytes();

	// Bancos de ROM (replica reference emulator Map_JumboLoROMMap, com as metades ja
	// normalizadas no loader: metade-com-header em 0x400000, 4MB em 0):
	//   $00-$3F:8000 -> 0x400000  (metade com header/vetores -> $00 le aqui)
	//   $40-$7F:0000 -> 0x600000  (full bank; em geral fora de range)
	//   $80-$BF:8000 -> 0x000000  (4MB principal, parte 1)
	//   $C0-$FF:0000 -> 0x200000  (4MB principal, parte 2; full bank)
	_MapExLoRomRegion(pCpu, pRom, romBytes, 0x00, 0x3F, FALSE, 0x400000);
	_MapExLoRomRegion(pCpu, pRom, romBytes, 0x40, 0x7F, TRUE,  0x600000);
	_MapExLoRomRegion(pCpu, pRom, romBytes, 0x80, 0xBF, FALSE, 0x000000);
	_MapExLoRomRegion(pCpu, pRom, romBytes, 0xC0, 0xFF, TRUE,  0x200000);

	// areas de sistema por cima
	MapMem(_SnesMemMap_ExLoRom_Sys);
}

void SnesSystem::MapSuperGameBoy()
{
    Uint32 bank;
    if (!m_pRom || !(m_pRom->m_Flags & SNROM_FLAG_GAMEBOY)) return;
    for (bank = 0; bank <= 0x3F; ++bank)
    {
        Uint32 a = (bank << 16) | 0x6000U;
        SNCPUSetMemSpeed(&m_Cpu, a, 0x2000, SNCPU_CYCLE_SLOW);
        SNCPUSetTrap(&m_Cpu, a, 0x2000, ReadSGB, WriteSGB);
        a |= 0x800000U;
        SNCPUSetMemSpeed(&m_Cpu, a, 0x2000, SNCPU_CYCLE_SLOW);
        SNCPUSetTrap(&m_Cpu, a, 0x2000, ReadSGB, WriteSGB);
    }
}

void SnesSystem::MapMem(SNRomMappingE eRomMapping, Uint32 uFlags)
{
	// set default traps
	SNCPUSetTrap(&m_Cpu,     0, SNCPU_MEM_SIZE, ReadMem, WriteMem);
	SNCPUSetMemSpeed(&m_Cpu, 0, SNCPU_MEM_SIZE, SNCPU_CYCLE_SLOW);

	m_bSDD1 = FALSE;
	m_bSRTC = (uFlags & SNROM_FLAG_SRTC) ? TRUE : FALSE;
	m_bSuperFX = (uFlags & SNROM_FLAG_SUPERFX) ? TRUE : FALSE;

	switch (eRomMapping)
	{
		default:

		// mode 20h
		case SNROM_MAPPING_LOROM:
			MapMem(_SnesMemMap_LoRom);

			/* v3.1: generic small-LoROM full-bank SRAM decode. The first
			 * MapMem call above has already resolved/capped m_uSramSize. */
			if (!(uFlags & (SNROM_FLAG_SUPERFX | SNROM_FLAG_SA1)) &&
			    m_pRom->GetBytes() <= 0x200000 &&
			    m_uSramSize > 0 && m_uSramSize <= 0x8000)
			{
				MapMem(_SnesMemMap_LoRom_SRAMFullHigh);
			}

#if SNES_DSP1
			if (uFlags & SNROM_FLAG_DSP1) { MapMem(_SnesMemMap_LoRom_DSP1); m_pDsp = &m_DSP1; }
			if (uFlags & SNROM_FLAG_DSP2) { MapMem(_SnesMemMap_LoRom_DSP1); m_pDsp = &m_DSP2; }
			// DSP-3 (SD Gundam GX): ainda SEM HLE proprio.  Mapeia a
			// regiao do DSP, mas m_pDsp fica NULL -> a guarda anti-crash
			// em ReadDSP1/WriteDSP1 mantem o emulador estavel (status
			// "pronto", dados 0).  O jogo nao renderiza certo, mas nao
			// trava.  (Sem dependencia de firmware externo.)
			if (uFlags & SNROM_FLAG_DSP3)
			{
				MapMem(_SnesMemMap_LoRom_DSP1);
				// m_pDsp permanece NULL (inerte)
			}
			// DSP-4 (Top Gear 3000): HLE self-contained -- NAO precisa de
			// firmware.  O chip esta sempre disponivel, entao a regiao do
			// DSP e' sempre mapeada e m_pDsp aponta para o HLE.
			if (uFlags & SNROM_FLAG_DSP4)
			{
				/* AURORA_DSP4_REAL_LOROM_MAP_20260831 */
				MapMem(_SnesMemMap_LoRom_DSP4);
				m_pDsp = &m_DSP4;
			}
#endif
			if (uFlags & SNROM_FLAG_OBC1) { MapMem(_SnesMemMap_OBC1); }
			if (uFlags & SNROM_FLAG_CX4)
			{
				MapMem(_SnesMemMap_CX4);
				m_CX4.SetMemReader(CX4ReadMem, &m_Cpu);
			}
			// SuperFX / GSU (Star Fox, Yoshi's Island, etc.).  So' ativa para
			// cartucho SuperFX (via m_bSuperFX), entao boot e jogos sem
			// SuperFX ficam 100% intactos.
			if (uFlags & SNROM_FLAG_SUPERFX)
			{
				Uint32 uBank;
				Int32 nBoard = _SnesSuperFXBoardType(m_pRom->GetRomTitle());
				Bool bMarioChip1 = (nBoard == SNES_SUPERFX_BOARD_MC1);

				_MapSuperFXRom(&m_Cpu, m_pRom->GetData(), m_pRom->GetBytes(), nBoard);
				if (bMarioChip1)
					MapMem(_SnesMemMap_SuperFXMC1RAM);
				else if (nBoard == SNES_SUPERFX_BOARD_GSU1)
					MapMem(_SnesMemMap_SuperFXGSU1RAM);
				else
					MapMem(_SnesMemMap_SuperFXRAM);

				// GSU1/2: $00-$3F/$80-$BF:6000-$7FFF espelha os primeiros
				// 8 KiB de $70:0000. O Mario Chip 1 nao possui esta janela.
				if (!bMarioChip1)
				{
					for (uBank = 0; uBank <= 0x3F; uBank++)
					{
						Uint32 uAddr = (uBank << 16) | 0x6000;
						SNCPUSetMemSpeed(&m_Cpu, uAddr, 0x2000, SNCPU_CYCLE_SLOW);
						SNCPUSetBank(&m_Cpu, uAddr, 0x2000, m_SRam, TRUE);
						uAddr |= 0x800000;
						SNCPUSetMemSpeed(&m_Cpu, uAddr, 0x2000, SNCPU_CYCLE_SLOW);
						SNCPUSetBank(&m_Cpu, uAddr, 0x2000, m_SRam, TRUE);
					}
				}

				// A MMIO do GSU ($3000-$34FF) compartilha a pagina do PPU e
				// continua roteada por Read2000/Write2000.
				/* AURORA_V81_SUPERFX_REVISION_BIND
				 * Keep the already board-specific S-CPU maps intact; only pass
				 * MC1/GSU1/GSU2 identity into the core for clock capabilities. */
				m_GSU.SetRevision((Uint8)nBoard);
				m_GSU.SetMemory(m_pRom->GetData(), m_pRom->GetBytes(),
				                m_SRam, m_uSramSize);
			}
			if (uFlags & SNROM_FLAG_SDD1)
			{
				m_bSDD1 = TRUE;
				RemapSDD1();   // sobrepoe $C0-$FF com os 4 segmentos
			}
			if (uFlags & SNROM_FLAG_SA1)
			{
				/* AURORA_SA1_V1_REFERENCE_LOGIC_20260902 */
				m_SA1.Detach();
				m_bSA1IRQ = FALSE;
				/* AURORA_SA1_PERF_V8_3_2_20260903 */
				if (m_SA1.Attach(this, m_pRom->GetData(), m_pRom->GetBytes(),
				                 m_SRam, m_uSramSize, TRUE, FALSE, FALSE))
					m_SA1.MapMainCPU(&m_Cpu);
			}
			break;

		// mode 21h
		case SNROM_MAPPING_HIROM:
			MapMem(_SnesMemMap_HiRom);

#if SNES_DSP1
			if (uFlags & SNROM_FLAG_DSP1)
			{
				MapMem(_SnesMemMap_HiRom_DSP1);
				m_pDsp = &m_DSP1;;
			}
			if (uFlags & SNROM_FLAG_DSP2)
			{
				MapMem(_SnesMemMap_HiRom_DSP1);
				m_pDsp = &m_DSP2;
			}
#endif
			if (uFlags & SNROM_FLAG_OBC1) { MapMem(_SnesMemMap_OBC1); }
			break;

		/* AURORA_BSXSLOT_MEMORY_PACK_V1_20260906_SNMEMMAP_CPP */
		case SNROM_MAPPING_BSCLOROM:
			MapBSCLoRom();
			break;

		case SNROM_MAPPING_BSCHIROM:
			MapBSCHiRom();
			break;

		// LoROM > 4MB (Jumbo / ExLoROM, ate 8MB)
		case SNROM_MAPPING_EXLOROM:
			MapMemExLoRom();
			break;
	}

	/* AURORA_V4_4_CUMULATIVE_20260908
	 * Generic mapping installs WRAM/PPU first; MCC then owns only the physical
	 * cartridge-side pages and may rebuild them on later bank-E commits. */
	if (uFlags & SNROM_FLAG_BSXBASE)
		MapBSXBase();

	/* AURORA_TOP_GEAR_FASTROM_V1
	 * The published FastROM patch for Top Gear gains speed by making ROM
	 * accesses fast. Aurora does the equivalent at the mapper timing layer
	 * for exact U/E/J CRCs, so no ROM bank-rewrite or ROM-byte mutation is
	 * needed. SNCPUSetRomSpeed changes direct ROM descriptors only. */
	if (g_SnesCompatTopGearFastRom)
		SNCPUSetRomSpeed(&m_Cpu, 0x000000, 0x1000000, SNCPU_CYCLE_FAST);

	if (uFlags & SNROM_FLAG_GAMEBOY)
		MapSuperGameBoy(); /* AURORA_SGB_RUNTIME_V0_4_20260904 */

	/* Indexed/16-bit accesses can transiently carry past $FFFFFF.  Publish
	   bank $00 into the overflow page after every cartridge/system override
	   has been installed, preserving the 24-bit bus wrap in the ASM core. */
	SNCPUMirror24BitBus(&m_Cpu);



#if CODE_DEBUG
//	DumpMemMap();
#endif

}


// (Re)mapeia os bancos $C0-$FF conforme os registradores de segmento do
// S-DD1 ($4804-$4807). Cada registrador escolhe um segmento de 1MB da ROM
// para um grupo de 16 bancos: $4804->$C0-$CF, $4805->$D0-$DF, $4806->$E0-$EF,
// $4807->$F0-$FF. Star Ocean troca esses segmentos para enxergar seus 6MB.
void SnesSystem::RemapSDD1(void)
{
	Uint8 *pRomData  = m_pRom->GetData();
	Uint32 uRomBytes = m_pRom->GetBytes();
	Uint32 g;

	if (!pRomData || uRomBytes == 0)
		return;

	for (g = 0; g < 4; g++)
	{
		Uint32 uSeg     = m_SDD1.BankSegment(g);
		Uint32 uRomOff  = (uSeg * 0x100000) % uRomBytes;
		Uint32 uBankBase = (0xC0 + g * 0x10) << 16;   // $C00000 / $D00000 / ...

		SNCPUSetMemSpeed(&m_Cpu, uBankBase, 0x100000, SNCPU_CYCLE_SLOW);
		SNCPUSetBank    (&m_Cpu, uBankBase, 0x100000, pRomData + uRomOff, FALSE);
	}

#if SNDBG_LOG
	{
		// loga so' quando a config de segmentos muda (evita flood)
		static Uint32 uLast = 0xFFFFFFFF;
		Uint32 uCur = (Uint32)(m_SDD1.BankSegment(0) | (m_SDD1.BankSegment(1) << 8)
		            | (m_SDD1.BankSegment(2) << 16) | (m_SDD1.BankSegment(3) << 24));
		if (uCur != uLast)
		{
			uLast = uCur;
			DLog("[sdd1] remap seg=%d,%d,%d,%d romBytes=%06X",
				(int)m_SDD1.BankSegment(0), (int)m_SDD1.BankSegment(1),
				(int)m_SDD1.BankSegment(2), (int)m_SDD1.BankSegment(3),
				(unsigned)uRomBytes);
		}
	}
#endif
}


void SnesSystem::SetFastRom()
{
	SNCPUSetRomSpeed(&m_Cpu, 0x800000, 0x800000, SNCPU_CYCLE_FAST);
}

void SnesSystem::SetSlowRom()
{
	/* AURORA_TOP_GEAR_FASTROM_V1: keep exact-CRC Top Gear/Top Racer ROM
	 * fast across hard/soft reset and MEMSEL=0 writes. */
	if (g_SnesCompatTopGearFastRom)
	{
		SNCPUSetRomSpeed(&m_Cpu, 0x000000, 0x1000000, SNCPU_CYCLE_FAST);
		return;
	}
	SNCPUSetRomSpeed(&m_Cpu, 0x800000, 0x800000, SNCPU_CYCLE_SLOW);
}

#if 0
void SnesSystem::MapLoRom()
{
	Uint32 uMemAddr;
	Uint32 uRomAddr;
	Uint8 *pRomData;
	Uint32 uRomBytes;
	Uint32 uSRAMBytes;

	SnesRom *pRom = m_pRom;
	SNCpuT *pCpu = &m_Cpu;
	Uint8 *pRam = m_Ram;
	Uint8 *pSRam = m_SRam;

	uRomBytes= pRom->GetBytes();
	pRomData = pRom->GetData();

	// determine size of SRAM in bytes 
	uSRAMBytes = pRom->GetSRAMBytes();
	if (uSRAMBytes > SNES_SRAMSIZE) uSRAMBytes = SNES_SRAMSIZE; 

//	uSRAMBytes = 1024 * 16;
//	uSRAMBytes = SNES_SRAMSIZE;

	// round up to banksize
	uSRAMBytes = (uSRAMBytes + SNCPU_BANK_SIZE - 1)  & ~(SNCPU_BANK_MASK);

	// set default traps
	SNCPUSetTrap(pCpu,     0, SNCPU_MEM_SIZE, ReadMem, WriteMem);
	SNCPUSetMemSpeed(pCpu, 0, SNCPU_MEM_SIZE, SNCPU_CYCLE_SLOW);

	// map slow rom at xx8000 -> xxFFFF
	uRomAddr=0x000000; 
	for (uMemAddr=0x000000; uMemAddr < 0x700000; uMemAddr+=0x10000)
	{
		// mirror 32K at 0000->7FFFF
		if (uMemAddr >= 0x400000)
			SNCPUSetBank(pCpu, uMemAddr | 0x000000, 0x8000, pRomData + uRomAddr, FALSE);
		SNCPUSetBank(pCpu, uMemAddr | 0x008000, 0x8000, pRomData + uRomAddr, FALSE);

		// increment/wrap rom address
		uRomAddr += 0x08000;
		if (uRomAddr >= uRomBytes) uRomAddr = 0;
	}

	// map fast rom at xx8000 -> xxFFFF
	uRomAddr=0x000000; 
	for (uMemAddr=0x800000; uMemAddr < 0xFE0000; uMemAddr+=0x10000)
	{
		// mirror 32K at 0000->7FFFF
		if (uMemAddr >= 0xC00000)
			SNCPUSetBank(pCpu, uMemAddr | 0x000000, 0x8000, pRomData + uRomAddr,  FALSE);
		SNCPUSetBank(pCpu, uMemAddr | 0x008000, 0x8000, pRomData + uRomAddr,  FALSE);

		// increment/wrap rom address
		uRomAddr += 0x08000;
		if (uRomAddr >= uRomBytes) uRomAddr = 0;
	}

	// map i/o area
	uMemAddr=0x000000; 
	while (uMemAddr < 0x400000)
	{
		// map loram at xx0000 -> xx1FFF
		SNCPUSetBank(pCpu, uMemAddr | 0x000000, 0x2000, pRam, TRUE);
		SNCPUSetBank(pCpu, uMemAddr | 0x800000, 0x2000, pRam, TRUE);


		SNCPUSetTrap(pCpu, uMemAddr | 0x002000, 0x2000, Read2000, Write2000);
		SNCPUSetTrap(pCpu, uMemAddr | 0x004000, 0x2000, Read4000, Write4000);
  
		SNCPUSetTrap(pCpu, uMemAddr | 0x802000, 0x2000, Read2000, Write2000);
		SNCPUSetTrap(pCpu, uMemAddr | 0x804000, 0x2000, Read4000, Write4000);
		#if SNES_RAMFAST
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x000000, 0x2000, SNCPU_CYCLE_FAST);
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x800000, 0x2000, SNCPU_CYCLE_FAST);
		#endif

		// i/o area is fast
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x002000, 0x2000, SNCPU_CYCLE_FAST);
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x802000, 0x2000, SNCPU_CYCLE_FAST);
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x004000, 0x2000, SNCPU_CYCLE_FAST);
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x804000, 0x2000, SNCPU_CYCLE_FAST);

		uMemAddr += 0x10000;
	}

	// map sram area 700000 -> 7DFFFF
	if (uSRAMBytes > 0)
	{
		uMemAddr = 0x700000;
		while (uMemAddr < 0x7E0000)
		{
			SNCPUSetBank(pCpu, uMemAddr, uSRAMBytes, pSRam, TRUE);
			uMemAddr += uSRAMBytes;
		}
	}

	// map ram at 7E0000 -> 7FFFFF
	//SNCPUSetTrap(pCpu, 0x7E0000, 0x20000, ReadMem, WriteMem);
	SNCPUSetBank(pCpu, 0x7E0000, 0x20000, pRam, TRUE);

	#if SNES_RAMFAST
	SNCPUSetMemSpeed(pCpu, 0x7E0000, 0x20000, SNCPU_CYCLE_FAST);
	#endif

}


void SnesSystem::MapHiRom()
{
	Uint32 uMemAddr;
	Uint32 uRomAddr;
	Uint8 *pRomData;
	Uint32 uRomBytes;
	Uint32 uSRAMBytes;

	SnesRom *pRom = m_pRom;
	SNCpuT *pCpu = &m_Cpu;
	Uint8 *pRam = m_Ram;
///	Uint8 *pSRam = m_SRam;

	uRomBytes= pRom->GetBytes();
	pRomData = pRom->GetData();

	// determine size of SRAM in bytes 
	uSRAMBytes = pRom->GetSRAMBytes();
	if (uSRAMBytes > SNES_SRAMSIZE) uSRAMBytes = SNES_SRAMSIZE; 
//	uSRAMBytes = SNES_SRAMSIZE;

	// round up to banksize
	uSRAMBytes = (uSRAMBytes + SNCPU_BANK_SIZE - 1)  & ~(SNCPU_BANK_MASK);


	// set default traps
	SNCPUSetTrap(pCpu,     0, SNCPU_MEM_SIZE, ReadMem, WriteMem);
	SNCPUSetMemSpeed(pCpu, 0, SNCPU_MEM_SIZE, SNCPU_CYCLE_SLOW);

	// map 32kb slow rom at 008000 -> 3FFFFF
	uRomAddr=0x000000; 
	for (uMemAddr=0x000000; uMemAddr < 0x400000; uMemAddr+=0x10000)
	{
		SNCPUSetBank(pCpu, uMemAddr | 0x008000, 0x8000, pRomData + uRomAddr + 0x8000, FALSE);

		// increment/wrap rom address
		uRomAddr += 0x10000;
		if (uRomAddr >= uRomBytes) uRomAddr = 0;
	}

	// map 64kb slow rom at 400000 -> xxFFFF
	uRomAddr=0x000000; 
	for (uMemAddr=0x400000; uMemAddr < 0x7E0000; uMemAddr+=0x10000)
	{
		SNCPUSetBank(pCpu, uMemAddr, 0x10000, pRomData + uRomAddr, FALSE);

		// increment/wrap rom address
		uRomAddr += 0x10000;
		if (uRomAddr >= uRomBytes) uRomAddr = 0;
	}


	// map fast 32kb rom at 808000 -> BFFFFF
	uRomAddr=0x000000; 
	for (uMemAddr=0x800000; uMemAddr < 0xC00000; uMemAddr+=0x10000)
	{
		SNCPUSetBank(pCpu, uMemAddr | 0x008000, 0x8000, pRomData + uRomAddr + 0x8000,  FALSE);

		// increment/wrap rom address
		uRomAddr += 0x10000;
		if (uRomAddr >= uRomBytes) uRomAddr = 0;
	}


	// map fast 64kb rom at C00000 -> FFFFFF
	uRomAddr=0x000000; 
	for (uMemAddr=0xC00000; uMemAddr < 0x1000000; uMemAddr+=0x10000)
	{
		SNCPUSetBank(pCpu, uMemAddr, 0x10000, pRomData + uRomAddr,  FALSE);

		// increment/wrap rom address
		uRomAddr += 0x10000;
		if (uRomAddr >= uRomBytes) uRomAddr = 0;
	}


	// map i/o area
	uMemAddr=0x000000; 
	while (uMemAddr < 0x400000)
	{
		// map loram at xx0000 -> xx1FFF
		SNCPUSetBank(pCpu, uMemAddr | 0x000000, 0x2000, pRam, TRUE);
		SNCPUSetBank(pCpu, uMemAddr | 0x800000, 0x2000, pRam, TRUE);


		SNCPUSetTrap(pCpu, uMemAddr | 0x002000, 0x2000, Read2000, Write2000);
		SNCPUSetTrap(pCpu, uMemAddr | 0x004000, 0x2000, Read4000, Write4000);

		SNCPUSetTrap(pCpu, uMemAddr | 0x802000, 0x2000, Read2000, Write2000);
		SNCPUSetTrap(pCpu, uMemAddr | 0x804000, 0x2000, Read4000, Write4000);
#if SNES_RAMFAST
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x000000, 0x2000, SNCPU_CYCLE_FAST);
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x800000, 0x2000, SNCPU_CYCLE_FAST);
#endif

		// i/o area is fast
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x002000, 0x2000, SNCPU_CYCLE_FAST);
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x802000, 0x2000, SNCPU_CYCLE_FAST);
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x004000, 0x2000, SNCPU_CYCLE_FAST);
		SNCPUSetMemSpeed(pCpu, uMemAddr | 0x804000, 0x2000, SNCPU_CYCLE_FAST);

		uMemAddr += 0x10000;
	}


/*

	// map sram area 700000 -> 7?????
	SNCPUSetBank(pCpu, 0x700000, uSRAMBytes, pSRam, TRUE);
*/

	// map ram at 7E0000 -> 7FFFFF
	//SNCPUSetTrap(pCpu, 0x7E0000, 0x20000, ReadMem, WriteMem);
	SNCPUSetBank(pCpu, 0x7E0000, 0x20000, pRam, TRUE);

}
#endif
