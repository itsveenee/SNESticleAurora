/* AURORA_PPU_R5900_DIRECT_BG_V5_20260927
 * Exact PS2 host kernels for the existing 8-bit BG renderer.
 *
 * These routines do not advance emulated state.  They translate the same
 * already-selected VRAM rows, palette metadata and visibility masks as the
 * C++ reference path.  EABI is used explicitly: the first eight integer /
 * pointer arguments are $4..$11.  No incoming argument is read from stack.
 */

        .text
        .p2align 4
        .set    noreorder
        .set    noat

/* AURORA_C4_BG_COMPOSITOR_R5900_V7_1_FINAL_20261004
 * C4 final: keep only measured-winning BG compositor scheduling.
 * Memory-phase inlining and CHR2/CHR4 output scheduling are reverted.
 * No emulated timing/state/audio/frameskip behavior is changed. */

/*
 * struct AuroraPPUChrDirectAsmCtx {
 *   const Uint64 *planeNormal;   // +0
 *   const Uint64 *planeHFlip;    // +4
 *   const Uint8  *maskReverse;   // +8
 *   const Uint64 *palLookup;     // +12
 * };
 *
 * void AuroraPPUFetchCHR2Direct64PS2(
 *   const Uint16 *vram, Uint32 base, const SnesRenderTileT *tiles,
 *   Int32 nTiles, Uint32 scrollY, Uint8 *dest, Uint8 *mask,
 *   const AuroraPPUChrDirectAsmCtx *ctx);
 *
 * SnesRenderTileT layout checked by C++ wrapper:
 *   +0 uTile (u16), +2 uPal, +3 uFlip, +4 uOffsetY, size 6.
 */
/* AURORA_DKC_CHR24_CTX_HOIST_R5900_V22_20260928
 * AURORA_DKC_CHR24_SCHEDULE_R5900_V25_20260928
 *
 * Exact V22 CHR2 transform with R5900 scheduling only:
 * - fourth context load occupies the entry branch delay slot;
 * - H-flip lookup selection occupies its branch delay slot;
 * - first plane-index shift occupies the transparency branch delay slot;
 * - palette priority uses sign-expanded bit 3 instead of a branch;
 * - pMask increment occupies the loop-back delay slot.
 *
 * No source address, VRAM wrap, flip, palette, opacity or priority rule changes.
 * Leaf function; caller-saved registers only.
 */
        .globl  AuroraPPUFetchCHR2Direct64PS2
        .ent    AuroraPPUFetchCHR2Direct64PS2
/* AURORA_SNES_CHR24_R5900_V3_14_20261005
 * Exact CHR2 hot path. v3.10 scheduling/transparent exit preserved; adds a
 * zero-flip specialization so the dominant non-flipped row skips both the
 * V-flip and H-flip decision chains. */
AuroraPPUFetchCHR2Direct64PS2:
        lw      $12,0($11)
        lw      $13,4($11)
        lw      $14,8($11)
        blez    $7,.Lchr2_v314_done
        lw      $15,12($11)

.Lchr2_v314_loop:
        lhu     $2,0($6)
        lbu     $3,3($6)
        lbu     $11,4($6)
        beq     $3,$0,.Lchr2_v314_noflip
        sll     $2,$2,3

        /* General flip path: byte-identical v3.10 algebra. */
        addu    $2,$2,$5
        addu    $11,$11,$8
        andi    $24,$3,2
        bnel    $24,$0,.Lchr2_v314_no_vflip
        xori    $11,$11,7
.Lchr2_v314_no_vflip:
        addu    $2,$2,$11
        andi    $2,$2,0x7fff
        sll     $2,$2,1
        addu    $2,$4,$2
        lhu     $25,0($2)
        andi    $2,$25,0x00ff
        srl     $11,$25,8
        or      $24,$2,$11
        andi    $25,$3,1
        beq     $24,$0,.Lchr2_v314_transparent
        addiu   $7,$7,-1

        beql    $25,$0,.Lchr2_v314_normal
        move    $25,$12
        move    $25,$13
        sb      $24,0($10)
        b       .Lchr2_v314_mask_ready
        sll     $2,$2,3
.Lchr2_v314_normal:
        addu    $3,$14,$24
        lbu     $24,0($3)
        sll     $2,$2,3
        sb      $24,0($10)
        b       .Lchr2_v314_mask_ready
        nop

.Lchr2_v314_noflip:
        /* flip==0 => no vertical XOR, normal plane lookup, reversed mask. */
        addu    $2,$2,$5
        addu    $11,$11,$8
        addu    $2,$2,$11
        andi    $2,$2,0x7fff
        sll     $2,$2,1
        addu    $2,$4,$2
        lhu     $25,0($2)
        andi    $2,$25,0x00ff
        srl     $11,$25,8
        or      $24,$2,$11
        beq     $24,$0,.Lchr2_v314_transparent
        addiu   $7,$7,-1
        addu    $3,$14,$24
        lbu     $24,0($3)
        move    $25,$12
        sll     $2,$2,3
        sb      $24,0($10)

.Lchr2_v314_mask_ready:
        addu    $2,$25,$2
        ld      $3,0($2)
        sll     $11,$11,3
        addu    $11,$25,$11
        ld      $1,0($11)
        addiu   $6,$6,6
        lbu     $2,-4($6)
        dsll    $1,$1,1
        or      $3,$3,$1
        sll     $11,$2,28
        sll     $1,$2,3
        sra     $11,$11,31
        addu    $1,$15,$1
        and     $11,$24,$11
        sb      $11,48($10)
        ld      $1,0($1)
        addiu   $9,$9,8
        or      $3,$3,$1
        sd      $3,-8($9)
        bgtz    $7,.Lchr2_v314_loop
        addiu   $10,$10,1
        jr      $31
        nop

.Lchr2_v314_transparent:
        sb      $0,0($10)
        sb      $0,48($10)
        sd      $0,0($9)
        addiu   $6,$6,6
        addiu   $10,$10,1
        bgtz    $7,.Lchr2_v314_loop
        addiu   $9,$9,8
.Lchr2_v314_done:
        jr      $31
        nop
        .end    AuroraPPUFetchCHR2Direct64PS2
/* Same EABI argument map as the 2bpp kernel; ctx->palLookup is the 4bpp
 * palette table and base uses 16 VRAM words per tile. */
/* AURORA_DKC_CHR4_CTX_HOIST_R5900_V22_20260928
 * AURORA_DKC_CHR4_SCHEDULE_R5900_V25_20260928
 *
 * Exact V22 CHR4 row/wrap/flip transform with instruction scheduling only.
 * Same safe delay-slot/priority-mask strategy as CHR2.  No PPU decisions move
 * into assembly and no emulated timing/state is changed.
 */
        .globl  AuroraPPUFetchCHR4Direct64PS2
        .ent    AuroraPPUFetchCHR4Direct64PS2
/* AURORA_SNES_CHR24_R5900_V3_14_20261005
 * Exact CHR4 opaque-path follow-up. v3.10 transparent exit and latency-aware
 * decode are retained. A dedicated flip==0 path removes the V/H flip decision
 * chains from the common non-flipped opaque row while keeping the same VRAM
 * modulo, opacity reversal, LUTs, palette and priority output. */
AuroraPPUFetchCHR4Direct64PS2:
        lw      $12,0($11)
        lw      $13,4($11)
        lw      $14,8($11)
        blez    $7,.Lchr4_v314_done
        lw      $15,12($11)

.Lchr4_v314_loop:
        lhu     $2,0($6)
        lbu     $3,3($6)
        lbu     $11,4($6)
        beq     $3,$0,.Lchr4_v314_noflip
        sll     $2,$2,4

        /* General flip path: v3.10 address/opacity semantics unchanged. */
        addu    $2,$2,$5
        addu    $11,$11,$8
        andi    $24,$3,2
        bnel    $24,$0,.Lchr4_v314_no_vflip
        xori    $11,$11,7
.Lchr4_v314_no_vflip:
        addu    $2,$2,$11
        andi    $2,$2,0x7fff
        sll     $2,$2,1
        addu    $2,$4,$2
        lhu     $24,0($2)
        lhu     $25,16($2)
        andi    $3,$3,1
        or      $11,$24,$25
        srl     $2,$11,8
        or      $11,$11,$2
        andi    $11,$11,0x00ff
        beq     $11,$0,.Lchr4_v314_transparent
        addiu   $7,$7,-1

        beql    $3,$0,.Lchr4_v314_normal
        move    $2,$12
        move    $2,$13
        sb      $11,0($10)
        b       .Lchr4_v314_mask_ready
        andi    $3,$24,0x00ff
.Lchr4_v314_normal:
        addu    $3,$14,$11
        lbu     $11,0($3)
        andi    $3,$24,0x00ff
        sb      $11,0($10)
        b       .Lchr4_v314_mask_ready
        nop

.Lchr4_v314_noflip:
        /* flip==0: no vertical XOR and no H-flip selector branch. */
        addu    $2,$2,$5
        addu    $11,$11,$8
        addu    $2,$2,$11
        andi    $2,$2,0x7fff
        sll     $2,$2,1
        addu    $2,$4,$2
        lhu     $24,0($2)
        lhu     $25,16($2)
        or      $11,$24,$25
        srl     $2,$11,8
        or      $11,$11,$2
        andi    $11,$11,0x00ff
        beq     $11,$0,.Lchr4_v314_transparent
        addiu   $7,$7,-1
        addu    $3,$14,$11
        lbu     $11,0($3)
        move    $2,$12
        andi    $3,$24,0x00ff
        sb      $11,0($10)

.Lchr4_v314_mask_ready:
        srl     $24,$24,8
        sll     $3,$3,3
        sll     $24,$24,3
        addu    $3,$2,$3
        ld      $3,0($3)
        andi    $1,$25,0x00ff
        addu    $24,$2,$24
        srl     $25,$25,8
        ld      $24,0($24)
        sll     $1,$1,3
        sll     $25,$25,3
        addu    $1,$2,$1
        ld      $1,0($1)
        addu    $25,$2,$25
        dsll    $24,$24,1
        ld      $25,0($25)
        or      $3,$3,$24
        lbu     $2,2($6)
        dsll    $1,$1,2
        addiu   $6,$6,6
        or      $3,$3,$1
        sll     $24,$2,28
        dsll    $25,$25,3
        sll     $1,$2,3
        or      $3,$3,$25
        sra     $24,$24,31
        addu    $1,$15,$1
        and     $24,$11,$24
        sb      $24,48($10)
        ld      $1,0($1)
        addiu   $9,$9,8
        or      $3,$3,$1
        sd      $3,-8($9)
        bgtz    $7,.Lchr4_v314_loop
        addiu   $10,$10,1
        jr      $31
        nop

.Lchr4_v314_transparent:
        sb      $0,0($10)
        sb      $0,48($10)
        sd      $0,0($9)
        addiu   $6,$6,6
        addiu   $10,$10,1
        bgtz    $7,.Lchr4_v314_loop
        addiu   $9,$9,8
.Lchr4_v314_done:
        jr      $31
        nop
        .end    AuroraPPUFetchCHR4Direct64PS2


/* AURORA_C4_BG_CHR_NOOFFSET_R5900_V8_4_FINAL_20261004
 * Fast path for ordinary non-OPT BG rows only. FetchBG()/ordinary map kernels
 * publish uOffsetY=0 for these rows, so (uScrollY + uOffsetY) collapses to the
 * invariant uScrollY. Generic CHR2/CHR4 kernels above stay untouched and are
 * still used by OPT/offset paths. Mode 5/6 keep their dedicated C++ decoders.
 * No emulated state, timing, audio, frameskip, VRAM addressing or priority
 * rule changes. The ordinary path hoists the two possible row bases once per
 * 33-tile call, then uses R5900 MOVZ for the per-tile V-flip selection. */
        .globl  AuroraPPUFetchCHR2NoOffset64PS2
        .ent    AuroraPPUFetchCHR2NoOffset64PS2
AuroraPPUFetchCHR2NoOffset64PS2:
        lw      $12,0($11)               /* plane normal */
        lw      $13,4($11)               /* plane H-flip */
        lw      $14,8($11)               /* reverse-mask table */
        blez    $7,.Lchr2_nooff_done
        lw      $15,12($11)              /* palette LUT */

        /* Hoist both invariant row bases. Reuse $5/$8 after argument setup. */
        xori    $2,$8,7
        addu    $2,$5,$2                 /* flipped base + (Y^7) */
        addu    $5,$5,$8                 /* normal base + Y */
        move    $8,$2                    /* $8 = flipped row base */

.Lchr2_nooff_loop:
        lhu     $2,0($6)
        lbu     $3,3($6)
        sll     $2,$2,3

        /* Ordinary FetchBG rows have uOffsetY==0. MOVZ chooses normal base
         * when V-flip is clear; flipped base stays selected otherwise. */
        andi    $24,$3,2
        move    $11,$8
        movz    $11,$5,$24
        addu    $2,$2,$11
        andi    $2,$2,0x7fff
        sll     $2,$2,1
        addu    $2,$4,$2
        lhu     $25,0($2)
        andi    $2,$25,0x00ff            /* p0 */
        srl     $11,$25,8                /* p1 */
        or      $24,$2,$11               /* source opacity */

        /* AURORA_V24_SNES_CHR_FAST_EXACT_20261010: raw-zero reject.
         * The opacity mask is zero under both original orientations, so
         * transparent tiles bypass mask-table lookup and H-flip selection. */
        beq     $24,$0,.Lchr2_nooff_transparent
        andi    $25,$3,1
        beq     $25,$0,.Lchr2_nooff_normal
        move    $25,$13
        b       .Lchr2_nooff_mask_ready
        sb      $24,0($10)
.Lchr2_nooff_normal:
        move    $25,$12
        addu    $24,$14,$24
        lbu     $24,0($24)
        sb      $24,0($10)
.Lchr2_nooff_mask_ready:
        sll     $2,$2,3

        addu    $2,$25,$2
        ld      $3,0($2)
        sll     $11,$11,3
        addu    $11,$25,$11
        ld      $2,0($11)
        dsll    $2,$2,1
        or      $3,$3,$2

        lbu     $11,2($6)
        sll     $2,$11,3
        addu    $2,$15,$2
        ld      $2,0($2)
        or      $3,$3,$2

        sll     $11,$11,28
        sra     $11,$11,31
        /* Hot opaque path: useful priority calculation in branch slot. */
        b       .Lchr2_nooff_output
        and     $2,$24,$11

.Lchr2_nooff_transparent:
        /* Exact mask output that the original pre-branch code produced. */
        sb      $0,0($10)
        move    $3,$0
        move    $2,$0

.Lchr2_nooff_output:
        sd      $3,0($9)
        sb      $2,48($10)
        addiu   $6,$6,6
        addiu   $9,$9,8
        addiu   $7,$7,-1
        bgtz    $7,.Lchr2_nooff_loop
        addiu   $10,$10,1

.Lchr2_nooff_done:
        jr      $31
        nop
        .end    AuroraPPUFetchCHR2NoOffset64PS2
        .globl  AuroraPPUFetchCHR4NoOffset64PS2
        .ent    AuroraPPUFetchCHR4NoOffset64PS2
AuroraPPUFetchCHR4NoOffset64PS2:
        lw      $12,0($11)               /* plane normal */
        lw      $13,4($11)               /* plane H-flip */
        lw      $14,8($11)               /* reverse-mask table */
        blez    $7,.Lchr4_nooff_done_v10_4
        lw      $15,12($11)              /* palette LUT */

        /* AURORA_C4_CHR4_OPAQUE_R5900_V10_4_FINAL_20261004
         * Preserve v8.4 word addressing and row-base hoist. The only changes
         * below are: zero-mask fast reject, independent opaque LUT-load
         * scheduling, and hot opaque fall-through output. */
        xori    $2,$8,7
        addu    $2,$5,$2                 /* flipped base + (Y^7) */
        addu    $5,$5,$8                 /* normal base + Y */
        move    $8,$2                    /* $8 = flipped row base */

.Lchr4_nooff_loop_v10_4:
        lhu     $2,0($6)
        lbu     $3,3($6)
        sll     $2,$2,4

        andi    $24,$3,2
        move    $11,$8
        movz    $11,$5,$24
        addu    $2,$2,$11

        andi    $2,$2,0x7fff
        sll     $2,$2,1
        addu    $2,$4,$2
        lhu     $24,0($2)
        lhu     $25,16($2)

        /* Raw opacity is OR(p0,p1,p2,p3). Bit reversal preserves zero,
         * so transparent rows can bypass H/normal mask selection entirely. */
        or      $11,$24,$25
        srl     $2,$11,8
        or      $11,$11,$2
        andi    $11,$11,0x00ff

        beq     $11,$0,.Lchr4_nooff_transparent_v10_4
        andi    $3,$3,1                  /* H-flip flag; useful delay slot */

        /* AURORA_V24_SNES_CHR_FAST_EXACT_20261010: branchless H choice.
         * LUT selection and opacity output are identical to v10.4. The
         * 256-byte mask table is read-only, always valid for raw 8-bit mask. */
        /* AURORA_V28_CHR4_MASK_LOAD_SCHEDULE_20261010
         * Same 6 opcodes, inputs, outputs and guards as v24; issue reverse
         * lookup earlier and hide its load-use delay behind independent MOVN.
         * Both H-flip choices retain exact palette/mask/priority semantics. */
        addu    $1,$14,$11
        move    $2,$12                  /* normal plane */
        lbu     $1,0($1)                 /* reverse mask lookup */
        movn    $2,$13,$3                /* H-flipped plane iff H bit set */
        movz    $11,$1,$3                /* only reverse if H bit clear */
        sb      $11,0($10)

.Lchr4_nooff_opaque_v10_4:
        /* v10.2: form all four independent lookup addresses first. */
        andi    $3,$24,0x00ff
        srl     $24,$24,8
        andi    $1,$25,0x00ff
        srl     $25,$25,8

        sll     $3,$3,3
        sll     $24,$24,3
        sll     $1,$1,3
        sll     $25,$25,3

        addu    $3,$2,$3
        addu    $24,$2,$24
        addu    $1,$2,$1
        addu    $25,$2,$25

        /* Issue the four scratchpad loads as one independent group. */
        ld      $3,0($3)
        ld      $24,0($24)
        ld      $1,0($1)
        ld      $25,0($25)

        /* Consume the oldest loads first; use the gap to launch palette. */
        dsll    $24,$24,1
        or      $3,$3,$24

        lbu     $24,2($6)
        sll     $2,$24,3
        addu    $2,$15,$2
        ld      $2,0($2)

        dsll    $1,$1,2
        dsll    $25,$25,3
        or      $1,$1,$25
        or      $3,$3,$1

        /* Palette load gets several independent ops before first use. */
        sll     $24,$24,28
        sra     $24,$24,31
        and     $24,$11,$24              /* priority mask */
        or      $3,$3,$2

        /* v10.3: opaque is the hot path. Fall directly into its output tail;
         * no unconditional branch+nop on every opaque tile. */
        sd      $3,0($9)
        sb      $24,48($10)
        addiu   $6,$6,6
        addiu   $9,$9,8
        addiu   $7,$7,-1
        bgtz    $7,.Lchr4_nooff_loop_v10_4
        addiu   $10,$10,1

.Lchr4_nooff_done_v10_4:
        jr      $31
        nop

        /* Cold transparent tail is duplicated so neither path needs a
         * per-tile join branch. Zero mask is orientation-invariant. */
.Lchr4_nooff_transparent_v10_4:
        sd      $0,0($9)
        sb      $0,0($10)
        sb      $0,48($10)
        addiu   $6,$6,6
        addiu   $9,$9,8
        addiu   $7,$7,-1
        bgtz    $7,.Lchr4_nooff_loop_v10_4
        addiu   $10,$10,1
        b       .Lchr4_nooff_done_v10_4
        nop
        .end    AuroraPPUFetchCHR4NoOffset64PS2
/* AURORA_OBJ_POSTFETCH_COMPACT_R5900_V3_40_20261008
 * Int32 AuroraPPUCompactOBJTransparentPS2(SnesRenderObj8T *line, Int32 n)
 *
 * SnesRenderObj8T is compile-time guarded in snppurender8.cpp:
 *   size = 16, uData[SNPPU_BGPLANE_OPAQUE] = byte 13.
 *
 * Phase 1 is the common no-transparent fast path: scan one byte per entry and
 * perform zero stores.  The source pointer advances in the beq delay slot.
 * Only after the first transparent row is found do we compact later opaque
 * quadwords forward.  Relative order is preserved, so the renderer's reverse
 * traversal observes exactly the same opaque ordering.
 */
        .p2align 4
        .globl  AuroraPPUCompactOBJTransparentPS2
        .ent    AuroraPPUCompactOBJTransparentPS2
AuroraPPUCompactOBJTransparentPS2:
        move    $2,$5                   /* default: all input rows survive */
        blez    $5,.Lobj_compact_done
        sll     $7,$5,4                 /* byte size; harmless for n<=0 */
        addu    $7,$4,$7                /* end = line + n*16 */
        move    $6,$4                   /* src */

.Lobj_compact_find_gap:
        lbu     $8,13($6)               /* uOpaque */
        beq     $8,$0,.Lobj_compact_gap
        addiu   $6,$6,16                /* always advance to next entry */
        bne     $6,$7,.Lobj_compact_find_gap
        nop
        b       .Lobj_compact_done      /* all opaque: zero stores */
        nop

.Lobj_compact_gap:
        /* beq delay slot advanced src; dst is the transparent entry just seen. */
        addiu   $9,$6,-16               /* dst = first transparent slot */
        subu    $2,$9,$4
        srl     $2,$2,4                 /* survivors before first gap */
        beq     $6,$7,.Lobj_compact_done
        nop

.Lobj_compact_loop:
        lbu     $8,13($6)
        beq     $8,$0,.Lobj_compact_skip
        addiu   $6,$6,16                /* src advances on both paths */
        lq      $10,-16($6)             /* previous entry; 16-byte aligned */
        sq      $10,0($9)
        addiu   $9,$9,16
        addiu   $2,$2,1
.Lobj_compact_skip:
        bne     $6,$7,.Lobj_compact_loop
        nop

.Lobj_compact_done:
        jr      $31
        nop
        .end    AuroraPPUCompactOBJTransparentPS2

/* AURORA_DKC_BGDATA_DELAY_SLOT_R5900_V26_20260928
 * Scheduling-only change: pointer increments occupy already-existing branch
 * delay slots.  Data, masks, qfsrv semantics and tile count are unchanged.
 */

/*
 * void AuroraPPURenderBGDataOPS2(Uint8 *line, Uint8 *src, const Uint8 *maskData,
 *                                Uint32 scrollX, Int32 nTiles,
 *                                const Uint64 *maskLookup);
 *
 * Same qfsrv/PlaneLookup[1] operation sequence as the previous inline-MMI
 * implementation, but the complete 16-pixel loop now stays in assembly.
 */
        .globl  AuroraPPURenderBGDataOPS2
        .ent    AuroraPPURenderBGDataOPS2
AuroraPPURenderBGDataOPS2:
        andi    $2,$7,7
        addu    $5,$5,$2
        mtsab   $5,0
        blez    $8,.Lrender_o_done
        nop

.Lrender_o_loop:
        lhu     $2,0($6)
        addiu   $6,$6,2
        beql    $2,$0,.Lrender_o_advance
        sq      $0,0x00($4)              /* zero only when the mask is empty */

        lq      $10,0x00($5)
        lq      $11,0x10($5)
        qfsrv   $10,$11,$10
        ori     $3,$0,0xffff
        beql    $2,$3,.Lrender_o_advance
        sq      $10,0x00($4)             /* full mask: store in taken delay slot */

        andi    $3,$2,0x00ff
        sll     $3,$3,3
        addu    $3,$9,$3
        ld      $12,0($3)
        srl     $3,$2,8
        sll     $3,$3,3
        addu    $3,$9,$3
        ld      $13,0($3)
        pcpyld  $14,$13,$12
        pceqb   $14,$14,$0
        por     $10,$10,$14
        pxor    $10,$10,$14

.Lrender_o_store:
        sq      $10,0x00($4)
.Lrender_o_advance:
        addiu   $5,$5,0x10
        addiu   $8,$8,-2
        bgtz    $8,.Lrender_o_loop
        addiu   $4,$4,0x10

.Lrender_o_done:
        jr      $31
        nop
        .end    AuroraPPURenderBGDataOPS2

        .globl  AuroraPPURenderBGDataPS2
        .ent    AuroraPPURenderBGDataPS2
AuroraPPURenderBGDataPS2:
        andi    $2,$7,7
        addu    $5,$5,$2
        mtsab   $5,0
        blez    $8,.Lrender_done
        nop

.Lrender_loop:
        lhu     $2,0($6)
        beq     $2,$0,.Lrender_advance
        addiu   $6,$6,2

        lq      $10,0x00($5)
        lq      $11,0x10($5)
        qfsrv   $10,$11,$10
        ori     $3,$0,0xffff
        beql    $2,$3,.Lrender_advance
        sq      $10,0x00($4)             /* full mask: store in taken delay slot */

        andi    $3,$2,0x00ff
        sll     $3,$3,3
        addu    $3,$9,$3
        ld      $12,0($3)
        srl     $3,$2,8
        sll     $3,$3,3
        addu    $3,$9,$3
        ld      $13,0($3)
        lq      $15,0x00($4)
        pcpyld  $14,$13,$12
        pceqb   $14,$14,$0
        pand    $15,$15,$14
        por     $10,$10,$14
        pxor    $10,$10,$14
        por     $10,$10,$15

.Lrender_store:
        sq      $10,0x00($4)
.Lrender_advance:
        addiu   $5,$5,0x10
        addiu   $8,$8,-2
        bgtz    $8,.Lrender_loop
        addiu   $4,$4,0x10

.Lrender_done:
        jr      $31
        nop
        .end    AuroraPPURenderBGDataPS2
/* AURORA_PPU_R5900_CHR8_V16_20260928
 * Exact 8bpp sibling of the existing direct 2bpp/4bpp CHR kernels.
 *
 * void AuroraPPUFetchCHR8Direct64PS2(
 *   const Uint16 *vram, Uint32 base, const SnesRenderTileT *tiles,
 *   Int32 nTiles, Uint32 scrollY, Uint8 *dest, Uint8 *mask,
 *   const AuroraPPUChrDirectAsmCtx *ctx);
 *
 * EABI: $4..$11. No incoming stack arguments.
 * SnesRenderTileT: +0 tile(u16), +2 pal, +3 flip, +4 offsetY, size 6.
 * SnesPPUTile8T row pairs are at byte offsets 0x00/0x10/0x20/0x30.
 */
        .p2align 4
        .globl  AuroraPPUFetchCHR8Direct64PS2
        .ent    AuroraPPUFetchCHR8Direct64PS2
AuroraPPUFetchCHR8Direct64PS2:
        addiu   $29,$29,-0x80
        sq      $16,0x00($29)
        sq      $17,0x10($29)
        sq      $18,0x20($29)
        sq      $19,0x30($29)
        sq      $20,0x40($29)
        sq      $21,0x50($29)
        sq      $22,0x60($29)
        sq      $23,0x70($29)

        move    $16,$4                  /* vram */
        move    $17,$5                  /* base */
        move    $18,$6                  /* tiles */
        move    $19,$7                  /* nTiles */
        move    $20,$8                  /* scrollY */
        move    $21,$9                  /* dest */
        move    $22,$10                 /* mask */
        move    $23,$11                 /* ctx */

        blez    $19,.Lchr8_done
        nop

.Lchr8_loop:
        lhu     $2,0($18)               /* tile */
        lbu     $3,3($18)               /* flip: bit0 H, bit1 V */
        lbu     $4,4($18)               /* offsetY */
        sll     $2,$2,5                 /* tile * 32 words */
        addu    $2,$2,$17
        andi    $2,$2,0x7fff
        addu    $4,$4,$20
        andi    $5,$3,2
        beq     $5,$0,.Lchr8_no_vflip
        nop
        xori    $4,$4,7
.Lchr8_no_vflip:
        addu    $2,$2,$4                /* row word address */
        andi    $2,$2,0x7fff
        sll     $2,$2,1                 /* words -> bytes */
        addu    $2,$16,$2

        /* Preserve the reference SnesPPUTile8T layout exactly. */
        lhu     $8,0x00($2)             /* p0 | p1<<8 */
        lhu     $9,0x10($2)             /* p2 | p3<<8 */
        lhu     $10,0x20($2)            /* p4 | p5<<8 */
        lhu     $11,0x30($2)            /* p6 | p7<<8 */

        /* One source-opacity byte. */
        or      $12,$8,$9
        or      $12,$12,$10
        or      $12,$12,$11
        srl     $13,$12,8
        or      $12,$12,$13
        andi    $12,$12,0x00ff

        andi    $13,$3,1
        beq     $13,$0,.Lchr8_normal
        nop
        lw      $24,4($23)              /* H-flipped plane lookup */
        move    $14,$12                 /* identity opacity mask */
        beq     $0,$0,.Lchr8_lookup_ready
        nop
.Lchr8_normal:
        lw      $24,0($23)              /* canonical plane lookup */
        lw      $25,8($23)              /* bit-reverse mask table */
        addu    $25,$25,$12
        lbu     $14,0($25)
.Lchr8_lookup_ready:
        beq     $14,$0,.Lchr8_transparent
        nop

        /* Decode planes 0..7 into one 64-bit 8-pixel indexed row. */
        andi    $12,$8,0x00ff
        sll     $25,$12,3
        addu    $25,$24,$25
        ld      $15,0($25)

        srl     $12,$8,8
        sll     $25,$12,3
        addu    $25,$24,$25
        ld      $13,0($25)
        dsll    $13,$13,1
        or      $15,$15,$13

        andi    $12,$9,0x00ff
        sll     $25,$12,3
        addu    $25,$24,$25
        ld      $13,0($25)
        dsll    $13,$13,2
        or      $15,$15,$13

        srl     $12,$9,8
        sll     $25,$12,3
        addu    $25,$24,$25
        ld      $13,0($25)
        dsll    $13,$13,3
        or      $15,$15,$13

        andi    $12,$10,0x00ff
        sll     $25,$12,3
        addu    $25,$24,$25
        ld      $13,0($25)
        dsll    $13,$13,4
        or      $15,$15,$13

        srl     $12,$10,8
        sll     $25,$12,3
        addu    $25,$24,$25
        ld      $13,0($25)
        dsll    $13,$13,5
        or      $15,$15,$13

        andi    $12,$11,0x00ff
        sll     $25,$12,3
        addu    $25,$24,$25
        ld      $13,0($25)
        dsll    $13,$13,6
        or      $15,$15,$13

        srl     $12,$11,8
        sll     $25,$12,3
        addu    $25,$24,$25
        ld      $13,0($25)
        dsll    $13,$13,7
        or      $15,$15,$13

        lbu     $12,2($18)              /* palette field carries priority bit */
        andi    $12,$12,8
        move    $13,$0
        beq     $12,$0,.Lchr8_output
        nop
        move    $13,$14
        beq     $0,$0,.Lchr8_output
        nop

.Lchr8_transparent:
        move    $15,$0
        move    $13,$0

.Lchr8_output:
        sd      $15,0($21)
        sb      $14,0($22)
        sb      $13,48($22)
        addiu   $18,$18,6
        addiu   $21,$21,8
        addiu   $22,$22,1
        addiu   $19,$19,-1
        bgtz    $19,.Lchr8_loop
        nop

.Lchr8_done:
        lq      $16,0x00($29)
        lq      $17,0x10($29)
        lq      $18,0x20($29)
        lq      $19,0x30($29)
        lq      $20,0x40($29)
        lq      $21,0x50($29)
        lq      $22,0x60($29)
        lq      $23,0x70($29)
        addiu   $29,$29,0x80
        jr      $31
        nop
        .end    AuroraPPUFetchCHR8Direct64PS2
/* AURORA_PPU_BG_MAP_R5900_V17_20260928
 * Ordinary BG tilemap fetch kernels. These are deliberately leaf-only:
 * no emulated clock/state is advanced and no PPU ownership decision occurs.
 *
 * EABI:
 *   BG8 : $4 uAddr, $5 pTile, $6 nTiles, $7 ppScreen
 *   BG16: $4 uAddr, $5 pTile, $6 nTiles, $7 ppScreen, $8 uFlipXOR
 *
 * SnesRenderTileT is compile-time checked by snppubg.cpp:
 *   +0 uTile(u16), +2 uPal, +3 uFlip, +4 uOffsetY, size 6.
 */
        .p2align 4
        .globl  AuroraPPUFetchBG8x8MapPS2
        .ent    AuroraPPUFetchBG8x8MapPS2
AuroraPPUFetchBG8x8MapPS2:
        /* AURORA_DKC_BGMAP_DELAY_SCHEDULE_R5900_V29_20260928
         * V17 semantics exactly; only safe delay slots are occupied.
         */
        /* v20: callers provide the exact 12-bit abstract map address. */
        blez    $6,.Lbg8_v29_done
        srl     $8,$4,10

        sll     $8,$8,2
        addu    $8,$7,$8
        lw      $8,0($8)

.Lbg8_v29_loop:
        andi    $9,$4,0x03ff
        sll     $9,$9,1
        addu    $9,$8,$9
        lhu     $10,0($9)
        addiu   $4,$4,1

        andi    $11,$4,0x001f
        bnez    $11,.Lbg8_v29_no_wrap
        andi    $9,$10,0x03ff
        addiu   $4,$4,-32
        xori    $4,$4,0x0400
        srl     $8,$4,10
        sll     $8,$8,2
        addu    $8,$7,$8
        lw      $8,0($8)

.Lbg8_v29_no_wrap:
        sh      $9,0($5)
        srl     $9,$10,10
        andi    $9,$9,0x000f
        sb      $9,2($5)
        /* lhu makes word>>14 intrinsically 0..3. */
        srl     $9,$10,14
        sb      $9,3($5)
        sb      $0,4($5)

        addiu   $6,$6,-1
        bgtz    $6,.Lbg8_v29_loop
        addiu   $5,$5,6

.Lbg8_v29_done:
        jr      $31
        nop
        .end    AuroraPPUFetchBG8x8MapPS2
        .globl  AuroraPPUFetchBG16x16MapPS2
        .ent    AuroraPPUFetchBG16x16MapPS2
AuroraPPUFetchBG16x16MapPS2:
        /* AURORA_DKC_BG16MAP_DELAY_SCHEDULE_R5900_V29_20260928
         * Same V17 map wrap, 16x16 quadrant, flip/palette metadata and
         * nTiles accounting. Only branch delay slots carry invariant work.
         */
        /* v20: callers provide the exact 12-bit abstract map address. */
        blez    $6,.Lbg16_v29_done
        srl     $9,$4,10

        sll     $9,$9,2
        addu    $9,$7,$9
        lw      $9,0($9)

.Lbg16_v29_loop:
        andi    $10,$4,0x03ff
        sll     $10,$10,1
        addu    $10,$9,$10
        lhu     $10,0($10)
        addiu   $4,$4,1

        andi    $11,$4,0x001f
        bnez    $11,.Lbg16_v29_no_wrap
        andi    $11,$10,0x03ff
        addiu   $4,$4,-32
        xori    $4,$4,0x0400
        srl     $9,$4,10
        sll     $9,$9,2
        addu    $9,$7,$9
        lw      $9,0($9)

.Lbg16_v29_no_wrap:
        /* lhu makes word>>14 intrinsically 0..3. */
        srl     $12,$10,14
        srl     $13,$10,10
        andi    $13,$13,0x000f

        xor     $14,$12,$8
        andi    $15,$14,2
        sll     $15,$15,3
        andi    $24,$14,1
        addu    $25,$15,$24
        addu    $25,$11,$25
        andi    $25,$25,0x03ff
        sh      $25,0($5)
        sb      $13,2($5)
        sb      $12,3($5)
        sb      $0,4($5)
        addiu   $5,$5,6
        addiu   $6,$6,-1

        andi    $25,$8,1
        bnez    $25,.Lbg16_v29_after_second
        xori    $24,$24,1
        addu    $25,$15,$24
        addu    $25,$11,$25
        andi    $25,$25,0x03ff
        sh      $25,0($5)
        sb      $13,2($5)
        sb      $12,3($5)
        sb      $0,4($5)
        addiu   $5,$5,6
        addiu   $6,$6,-1

.Lbg16_v29_after_second:
        addiu   $25,$0,-2
        bgtz    $6,.Lbg16_v29_loop
        and     $8,$8,$25

.Lbg16_v29_done:
        jr      $31
        nop
        .end    AuroraPPUFetchBG16x16MapPS2
