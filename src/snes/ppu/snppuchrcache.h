#ifndef _SNPPUCHRCACHE_H
#define _SNPPUCHRCACHE_H

#include <string.h>
#include "types.h"

/* AURORA_TOPGEAR_HFLIP_CACHE
 * Cache a pre-flipped copy of decoded 4bpp CHR rows.
 * Both orientations share the same VRAM validity bitmap.
 */
#ifndef SNPPU_CHR_CACHE_HFLIP
#define SNPPU_CHR_CACHE_HFLIP 1
#endif

/*
 * Cache fisico de CHR decodificado.
 *
 * A VRAM do SNES pode ser interpretada como tiles 2bpp ou 4bpp dependendo
 * do modo/camada. Por isso mantemos uma tabela direta para cada formato:
 *
 *   2bpp: 32768 palavras /  8 palavras por tile = 4096 tiles
 *   4bpp: 32768 palavras / 16 palavras por tile = 2048 tiles
 *
 * Cada linha guarda oito indices de cor sem paleta. BG e OBJ 4bpp usam a
 * MESMA tabela; trocar CGRAM, prioridade ou paleta nao invalida os pixels.
 * A orientacao canonica e' sem H-flip. Com o switch H-flip ligado,
 * uma copia 4bpp pre-invertida acrescenta 147456 bytes; com =0, o
 * caminho canonico faz a reversao na saida como na base r29.
 *
 * Diferente dos caches experimentais de linha, um hit nao le a fonte da
 * VRAM, nao calcula hash e nao disputa uma entrada de 512 slots. Escritas em
 * $2118/$2119 invalidam diretamente os tiles fisicos afetados.
 */

#define SNPPU_CHR2_TILE_WORDS  8u
#define SNPPU_CHR4_TILE_WORDS 16u
#define SNPPU_CHR2_TILE_COUNT 4096u
#define SNPPU_CHR4_TILE_COUNT 2048u
#define SNPPU_VRAM_WORD_MASK  0x7FFFu

struct SnesPPUChrCacheT
{
	Uint64 uData2[SNPPU_CHR2_TILE_COUNT][8];
	Uint8  uOpaque2[SNPPU_CHR2_TILE_COUNT][8];
	Uint8  uValid2[SNPPU_CHR2_TILE_COUNT];

	Uint64 uData4[SNPPU_CHR4_TILE_COUNT][8];
	Uint8  uOpaque4[SNPPU_CHR4_TILE_COUNT][8];

#if SNPPU_CHR_CACHE_HFLIP
	Uint64 uData4HFlip[SNPPU_CHR4_TILE_COUNT][8];
	Uint8  uOpaque4HFlip[SNPPU_CHR4_TILE_COUNT][8];
#endif

	Uint8  uValid4[SNPPU_CHR4_TILE_COUNT];
};

_INLINE Uint64 SnesPPUChrCacheReverseBytes(Uint64 uData)
{
	uData = ((uData & 0x00FF00FF00FF00FFULL) << 8) |
	        ((uData & 0xFF00FF00FF00FF00ULL) >> 8);
	uData = ((uData & 0x0000FFFF0000FFFFULL) << 16) |
	        ((uData & 0xFFFF0000FFFF0000ULL) >> 16);
	return (uData << 32) | (uData >> 32);
}

_INLINE Uint8 SnesPPUChrCacheReverseMask(Uint8 uMask)
{
	uMask = (Uint8)(((uMask & 0x55u) << 1) | ((uMask & 0xAAu) >> 1));
	uMask = (Uint8)(((uMask & 0x33u) << 2) | ((uMask & 0xCCu) >> 2));
	return (Uint8)((uMask << 4) | (uMask >> 4));
}

_INLINE void SnesPPUChrCacheFlipRow(Uint64 *pData, Uint32 *pOpaque)
{
	*pData = SnesPPUChrCacheReverseBytes(*pData);
	*pOpaque = SnesPPUChrCacheReverseMask((Uint8)*pOpaque);
}

_INLINE Bool SnesPPUChrCacheLookup2(const SnesPPUChrCacheT *pCache,
	Uint32 uRowAddress, Bool bHFlip, Uint64 *pData, Uint32 *pOpaque)
{
	Uint32 uAddress = uRowAddress & SNPPU_VRAM_WORD_MASK;
	Uint32 uTile = uAddress >> 3;
	Uint32 uRow = uAddress & 7u;

	if (!(pCache->uValid2[uTile] & (1u << uRow)))
		return FALSE;

	*pData = pCache->uData2[uTile][uRow];
	*pOpaque = pCache->uOpaque2[uTile][uRow];
	if (bHFlip)
		SnesPPUChrCacheFlipRow(pData, pOpaque);
	return TRUE;
}

_INLINE Bool SnesPPUChrCacheLookup4(const SnesPPUChrCacheT *pCache,
	Uint32 uRowAddress, Bool bHFlip, Uint64 *pData, Uint32 *pOpaque)
{
	Uint32 uAddress = uRowAddress & SNPPU_VRAM_WORD_MASK;
	Uint32 uTile = uAddress >> 4;
	Uint32 uRow = uAddress & 7u;

	if (!(pCache->uValid4[uTile] & (1u << uRow)))
		return FALSE;

#if SNPPU_CHR_CACHE_HFLIP
	if (bHFlip)
	{
		*pData = pCache->uData4HFlip[uTile][uRow];
		*pOpaque = pCache->uOpaque4HFlip[uTile][uRow];
	}
	else
	{
		*pData = pCache->uData4[uTile][uRow];
		*pOpaque = pCache->uOpaque4[uTile][uRow];
	}
#else
	*pData = pCache->uData4[uTile][uRow];
	*pOpaque = pCache->uOpaque4[uTile][uRow];
	if (bHFlip)
		SnesPPUChrCacheFlipRow(pData, pOpaque);
#endif
	return TRUE;
}

_INLINE void SnesPPUChrCacheStore2(SnesPPUChrCacheT *pCache,
	Uint32 uRowAddress, Uint64 uData, Uint32 uOpaque)
{
	Uint32 uAddress = uRowAddress & SNPPU_VRAM_WORD_MASK;
	Uint32 uTile = uAddress >> 3;
	Uint32 uRow = uAddress & 7u;

	pCache->uData2[uTile][uRow] = uData;
	pCache->uOpaque2[uTile][uRow] = (Uint8)uOpaque;
	pCache->uValid2[uTile] |= (Uint8)(1u << uRow);
}

_INLINE void SnesPPUChrCacheStore4(SnesPPUChrCacheT *pCache,
	Uint32 uRowAddress, Uint64 uData, Uint32 uOpaque)
{
	Uint32 uAddress = uRowAddress & SNPPU_VRAM_WORD_MASK;
	Uint32 uTile = uAddress >> 4;
	Uint32 uRow = uAddress & 7u;

	pCache->uData4[uTile][uRow] = uData;
	pCache->uOpaque4[uTile][uRow] = (Uint8)uOpaque;

#if SNPPU_CHR_CACHE_HFLIP
	pCache->uData4HFlip[uTile][uRow] = SnesPPUChrCacheReverseBytes(uData);
	pCache->uOpaque4HFlip[uTile][uRow] =
		SnesPPUChrCacheReverseMask((Uint8)uOpaque);
#endif

	pCache->uValid4[uTile] |= (Uint8)(1u << uRow);
}

#if SNPPU_CHR_CACHE_HFLIP
/* AURORA_HFLIP_MISS_REUSE_V1
 * Store4() has already produced this mirrored row. The first H-flipped
 * consumer can load it directly instead of doing the byte/mask reversal
 * a second time.
 */
_INLINE void SnesPPUChrCacheLoad4HFlip(
	const SnesPPUChrCacheT *pCache,
	Uint32 uRowAddress,
	Uint64 *pData,
	Uint32 *pOpaque)
{
	Uint32 uAddress = uRowAddress & SNPPU_VRAM_WORD_MASK;
	Uint32 uTile = uAddress >> 4;
	Uint32 uRow = uAddress & 7u;

	*pData = pCache->uData4HFlip[uTile][uRow];
	*pOpaque = pCache->uOpaque4HFlip[uTile][uRow];
}
#endif

_INLINE void SnesPPUChrCacheInvalidateAll(SnesPPUChrCacheT *pCache)
{
	memset(pCache->uValid2, 0, sizeof(pCache->uValid2));
	memset(pCache->uValid4, 0, sizeof(pCache->uValid4));
}

_INLINE Uint32 SnesPPUChrCacheInvalidateRange(SnesPPUChrCacheT *pCache,
	Uint32 uWordAddress, Uint32 nWords)
{
	Uint32 nValidTiles = 0;

	if (!nWords)
		return 0;

	if (nWords >= 0x8000u)
	{
		SnesPPUChrCacheInvalidateAll(pCache);
		return SNPPU_CHR2_TILE_COUNT + SNPPU_CHR4_TILE_COUNT;
	}

	/* Avanca por limites de tile 2bpp. Isso visita no maximo 4097
	   posicoes ate em uma transferencia com wrap e tambem cobre cada tile
	   4bpp tocado, sem um laco por byte de DMA. */
	while (nWords)
	{
		Uint32 uAddress = uWordAddress & SNPPU_VRAM_WORD_MASK;
		Uint32 uTile2 = uAddress >> 3;
		Uint32 uTile4 = uAddress >> 4;
		Uint32 nStep = 8u - (uAddress & 7u);

		if (pCache->uValid2[uTile2])
		{
			pCache->uValid2[uTile2] = 0;
			nValidTiles++;
		}
		if (pCache->uValid4[uTile4])
		{
			pCache->uValid4[uTile4] = 0;
			nValidTiles++;
		}

		if (nStep > nWords)
			nStep = nWords;
		uWordAddress = (uAddress + nStep) & SNPPU_VRAM_WORD_MASK;
		nWords -= nStep;
	}

	return nValidTiles;
}

/* Implementado em snppurender8.cpp; chamado pelo caminho de escrita da PPU. */
void SnesPPUInvalidateChrCache(Uint32 uWordAddress, Uint32 nWords);

#endif // _SNPPUCHRCACHE_H
