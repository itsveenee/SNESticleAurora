/* AURORA_SNES_MODE7_R5900_V36_20261015
 * R5900/EE Mode 7 8-bit VRAM fetch, historical Full/256 path.
 * Source reference: historical snppurender8.cpp 8b337578.
 * Output: one byte per pixel, no video-timing or emulated state change.
 * The *tilemap* byte is VRAM[tile*2], the *character* byte is
 * VRAM[(chrbase+pixel_offset)*2+1].  These parity bits must NOT swap.
 * The old repeated-tile lookup remains conditional on tile index changes.
 * Clamp OOB: tile 0, but low coordinate bits select that tile's pixel.
 * Black OOB: return transparent 0, do NOT touch VRAM.
 * The (x|y)>>10 OOB rule is exactly the historical signed expression.
 * The arithmetic is 32-bit two's-complement, as on the original R5900.
 */
        .text
        .p2align 4
        .set noreorder
        .set noat

        .p2align 4
        .globl AuroraPPUMode7RepeatPS2
        .ent AuroraPPUMode7RepeatPS2
AuroraPPUMode7RepeatPS2:
        /* nPixels<=0: no writes or VRAM reads. */
        blez    $5,.Lv36_repeat_done
        addiu   $13,$0,-1       /* lastTile = 0xffffffff */
        move    $14,$0          /* cachedChrBase = 0 */
.Lv36_repeat_loop:
        sra     $11,$7,8        /* signed x2 = x >> 8 */
        sra     $12,$8,8        /* signed y2 = y >> 8 */
        addu    $7,$7,$9        /* x += dx; AFTER sampling x2 */
        addu    $8,$8,$10       /* y += dy; AFTER sampling y2 */
        andi    $11,$11,0x3ff  /* x2 &= 1023 */
        andi    $12,$12,0x3ff  /* y2 &= 1023 */
.Lv36_repeat_inside:
        srl     $2,$12,3
        sll     $2,$2,7
        srl     $3,$11,3
        or      $2,$2,$3       /* tile = ((y2>>3)<<7)|(x2>>3) */
        beq     $2,$13,.Lv36_repeat_cache_hit
        nop
        sll     $3,$2,1
        addu    $3,$3,$6
        lbu     $14,0($3)      /* even byte: 8-bit tile index */
        sll     $14,$14,6      /* tile base in character bytes */
        move    $13,$2         /* do NOT update cache on OOB */
.Lv36_repeat_cache_hit:
        move    $15,$14
.Lv36_repeat_sample:
        andi    $3,$11,7       /* x2 & 7 */
        andi    $25,$12,7      /* y2 & 7 */
        sll     $25,$25,3
        addu    $3,$3,$25
        addu    $3,$3,$15      /* word = chrBase + xLow + yLow*8 */
        sll     $3,$3,1
        addu    $3,$3,$6
        lbu     $3,1($3)       /* odd byte: pixel value */
.Lv36_repeat_store:
        sb      $3,0($4)
        addiu   $4,$4,1
        addiu   $5,$5,-1
        bgtz    $5,.Lv36_repeat_loop
        nop
.Lv36_repeat_done:
        jr      $31
        nop
        .end    AuroraPPUMode7RepeatPS2

        .p2align 4
        .globl AuroraPPUMode7ClampPS2
        .ent AuroraPPUMode7ClampPS2
AuroraPPUMode7ClampPS2:
        /* nPixels<=0: no writes or VRAM reads. */
        blez    $5,.Lv36_clamp_done
        addiu   $13,$0,-1       /* lastTile = 0xffffffff */
        move    $14,$0          /* cachedChrBase = 0 */
.Lv36_clamp_loop:
        sra     $11,$7,8        /* signed x2 = x >> 8 */
        sra     $12,$8,8        /* signed y2 = y >> 8 */
        addu    $7,$7,$9        /* x += dx; AFTER sampling x2 */
        addu    $8,$8,$10       /* y += dy; AFTER sampling y2 */
        or      $24,$11,$12
        sra     $24,$24,10     /* exact signed (x2|y2)>>10 */
        bne     $24,$0,.Lv36_clamp_oob
        nop
.Lv36_clamp_inside:
        srl     $2,$12,3
        sll     $2,$2,7
        srl     $3,$11,3
        or      $2,$2,$3       /* tile = ((y2>>3)<<7)|(x2>>3) */
        beq     $2,$13,.Lv36_clamp_cache_hit
        nop
        sll     $3,$2,1
        addu    $3,$3,$6
        lbu     $14,0($3)      /* even byte: 8-bit tile index */
        sll     $14,$14,6      /* tile base in character bytes */
        move    $13,$2         /* do NOT update cache on OOB */
.Lv36_clamp_cache_hit:
        b       .Lv36_clamp_sample
        move    $15,$14        /* current tile base */
.Lv36_clamp_oob:
        move    $15,$0         /* tile 0, preserve cached in-range tile */
.Lv36_clamp_sample:
        andi    $3,$11,7       /* x2 & 7 */
        andi    $25,$12,7      /* y2 & 7 */
        sll     $25,$25,3
        addu    $3,$3,$25
        addu    $3,$3,$15      /* word = chrBase + xLow + yLow*8 */
        sll     $3,$3,1
        addu    $3,$3,$6
        lbu     $3,1($3)       /* odd byte: pixel value */
.Lv36_clamp_store:
        sb      $3,0($4)
        addiu   $4,$4,1
        addiu   $5,$5,-1
        bgtz    $5,.Lv36_clamp_loop
        nop
.Lv36_clamp_done:
        jr      $31
        nop
        .end    AuroraPPUMode7ClampPS2

        .p2align 4
        .globl AuroraPPUMode7BlackPS2
        .ent AuroraPPUMode7BlackPS2
AuroraPPUMode7BlackPS2:
        /* nPixels<=0: no writes or VRAM reads. */
        blez    $5,.Lv36_black_done
        addiu   $13,$0,-1       /* lastTile = 0xffffffff */
        move    $14,$0          /* cachedChrBase = 0 */
.Lv36_black_loop:
        sra     $11,$7,8        /* signed x2 = x >> 8 */
        sra     $12,$8,8        /* signed y2 = y >> 8 */
        addu    $7,$7,$9        /* x += dx; AFTER sampling x2 */
        addu    $8,$8,$10       /* y += dy; AFTER sampling y2 */
        or      $24,$11,$12
        sra     $24,$24,10     /* exact signed (x2|y2)>>10 */
        bne     $24,$0,.Lv36_black_oob
        nop
.Lv36_black_inside:
        srl     $2,$12,3
        sll     $2,$2,7
        srl     $3,$11,3
        or      $2,$2,$3       /* tile = ((y2>>3)<<7)|(x2>>3) */
        beq     $2,$13,.Lv36_black_cache_hit
        nop
        sll     $3,$2,1
        addu    $3,$3,$6
        lbu     $14,0($3)      /* even byte: 8-bit tile index */
        sll     $14,$14,6      /* tile base in character bytes */
        move    $13,$2         /* do NOT update cache on OOB */
.Lv36_black_cache_hit:
        b       .Lv36_black_sample
        nop
.Lv36_black_oob:
        b       .Lv36_black_store
        move    $3,$0          /* transparent; no VRAM read */
.Lv36_black_sample:
        move    $15,$14
        andi    $3,$11,7       /* x2 & 7 */
        andi    $25,$12,7      /* y2 & 7 */
        sll     $25,$25,3
        addu    $3,$3,$25
        addu    $3,$3,$15      /* word = chrBase + xLow + yLow*8 */
        sll     $3,$3,1
        addu    $3,$3,$6
        lbu     $3,1($3)       /* odd byte: pixel value */
.Lv36_black_store:
        sb      $3,0($4)
        addiu   $4,$4,1
        addiu   $5,$5,-1
        bgtz    $5,.Lv36_black_loop
        nop
.Lv36_black_done:
        jr      $31
        nop
        .end    AuroraPPUMode7BlackPS2
