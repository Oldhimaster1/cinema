; render_v2_asm.s -- eZ80 assembly for render_scaled_fixed_asm(), Cinema's
; CINEMA_RENDERER_FIXED_ASM candidate (see src/render_v2.h).
;
; *** UNVERIFIED BY EXECUTION AS PLAYBACK; RENDER-ONLY BENCHMARK CANDIDATE
; ONLY (see bench/) ***
; A prior physical hardware test of this same benchmark/playback pair
; (with the algorithm this file used to contain, before the rewrite
; documented below) measured normal playback at ~85.510ms/frame versus
; GraphX's ~34.027-34.088ms -- roughly 2.51x SLOWER, not faster. Per
; that result, and per explicit instruction, this candidate must NOT be
; selected for normal playback again until:
;   1. renderer routing is proven from disassembly and the
;      g_render_calls[] counters (see render_v2.h) -- done for THIS
;      file, see "Routing-proof instrumentation" below;
;   2. the render-only benchmark's timed region matches normal
;      playback's timed region (per-frame render call, nothing else
;      timed) -- true for both by construction, see bench/src/main.c
;      and player_v2.c's render_frame();
;   3. the render-only benchmark (bench/, CINEMA_RENDERER=fixed_asm)
;      physically beats GraphX's ~34.027-34.088ms average on real
;      hardware -- NOT YET MEASURED for this rewritten version;
;   4. output is pixel-correct -- byte-identical-to-compiler-output is
;      argued below, but this file has never actually executed;
;   5. the benchmark exits safely (bench/src/main.c always does, via an
;      explicit bounded loop and wait_for_key());
;   6. all host tests, ASan, UBSan, and CEdev builds pass.
; Do not treat a render-only benchmark win alone as license to ship this
; for playback either -- that was the previous round's mistake target;
; a normal-playback candidate needs its own physical measurement.
;
; GraphX (CINEMA_RENDERER_GRAPHX, the default) is unaffected either way;
; this file is not linked into that path's execution, only (harmlessly)
; compiled and linked as dead code when CINEMA_RENDERER != fixed_asm.
;
; What changed in this rewrite, and why: the previous version of this
; file (and of render_scaled_fixed_c, which it was extracted from) read
; each compact 160-byte source row TWICE -- once per destination row --
; addressing the destination through a *second* offset-addressable
; pointer that had to share the eZ80's only spare offset-addressable
; index register (iy; ix is the frame pointer) with the source pointer,
; via a push/pop swap *every unrolled pixel*. Disassembling that
; version counted 66 push + 66 pop instructions per 16-pixel unrolled
; block, executed 1,920 times/frame (2 blocks/row-pair x 10 blocks/row x
; 96 source rows) -- this is the leading suspect for the measured 2.51x
; slowdown, though it was never isolated from other effects on real
; hardware. render_scaled_fixed_c (src/render_v2.c) was redesigned to
; write BOTH destination rows from a SINGLE source-row pass instead:
; three plain, sequentially-advancing pointers (source row, destination
; row 0, destination row 1) fit directly in hl/de/bc with no offset
; addressing and no register-swap needed at all. Recompiling and
; disassembling the new shape confirms push+pop dropped from 132 to 42
; per 16-pixel block (68% fewer) and the redundant second full source-row
; read is gone entirely. Whether this net-wins on real hardware -- new
; register/stack-spill traffic elsewhere in the loop could partially
; offset the gain -- is exactly what item 3 above still needs to answer;
; this file cannot answer that on its own.
;
; Routing-proof instrumentation: this function's FIRST action increments
; g_render_calls[CINEMA_RENDERER_ID_FIXED_ASM] (id 3) -- see the
; "_g_render_calls+12 / +15" edit described below. This exists so a
; physical test can read this counter (and the other two, which must
; stay at 0) directly off the benchmark's/player's on-screen summary,
; instead of inferring which renderer actually ran from timing numbers
; alone -- see render_v2.h's own comment for the full rationale (a
; previous physical test misread results from a similarly-named but
; wrong .8xp file, which looked exactly like a routing bug from the
; numbers alone until this was added).
;
; Provenance: this is not hand-typed from scratch, and not blind
; "-S"-from-C-source output either (an earlier attempt at that used a
; different, incompatible assembly dialect -- see below). It is the
; verbatim GAS-syntax assembly this exact function (the rewritten
; render_scaled_fixed_c, WITH its record_render_call() instrumentation
; already compiled in) compiles to *inside the real Cinema build*,
; extracted directly from the object the real toolchain produces:
;
;   make CINEMA_RENDERER=fixed_c   (a normal, complete build)
;   -> obj/lto.s, the whole-program-optimized assembly the real link
;      step actually assembles and links into bin/CINEMA.8xp
;   -> the _render_scaled_fixed_c section of that file, copied out
;      verbatim and renamed (render_scaled_fixed_c -> _asm,
;      .LBB39_* -> .LRSFA_BB_*, purely to avoid clashing with the real
;      render_scaled_fixed_c, which stays linked into every build
;      regardless of which renderer is selected) -- no instruction was
;      changed except one deliberate, mechanical edit: the compiler
;      inlined record_render_call(CINEMA_RENDERER_ID_FIXED_C) (id 2)
;      directly into render_scaled_fixed_c's body as raw manipulation of
;      _g_render_calls+8 (the low bytes of g_render_calls[2], since
;      sizeof(unsigned long)==4 and 2*4==8) and _g_render_calls+11 (the
;      split-out high byte, +3 more). This file's copy needs to record
;      CINEMA_RENDERER_ID_FIXED_ASM (id 3) instead, so both offsets were
;      changed by exactly the same arithmetic the compiler itself used:
;      +8 -> +12 (3*4) and +11 -> +15 (+3 more), a well-defined, low-risk
;      renumbering, not free-form authored logic. Nothing else in the
;      instrumentation preamble (the __ladd call, the register
;      sequencing) was touched.
;
; (clang version 19.1.0, https://github.com/CE-Programming/llvm-project
; ef28e9c54cd1333a6091ab2ffbd315b465fc5090 -- `make version` reports
; "CE C/C++ Toolchain ca0936c9" for the toolchain this was built with.)
;
; Why this route and not `ez80-clang -S src/render_v2.c` directly: that
; command emits a *different*, incompatible assembly dialect (bare
; `assume adl=1` / `public` / `private` directives) than what this
; toolchain's actual .s-file assembler (z80-none-elf-as, GNU binutils --
; the same one the Makefile always uses for any hand-written .s file
; under src/) accepts (`.assume ADL=1` / `.globl` / `.local` / `.type` /
; `.size`, standard GNU-assembler syntax) -- confirmed by literally
; trying it and getting "Unknown instruction `assume`/`public`/
; `private`" from the assembler. The *lto.s* pipeline is the one that
; actually produces GAS-syntax output (`$(CC) -S` from linked LLVM
; bitcode, not from a .c file directly), which is also the exact code
; path that produces the real shipped binary -- so extracting from
; there is both the only syntax that assembles standalone here, and
; more representative than a separate non-LTO compile would have been.
;
; What was reviewed by hand, reading this exact output against the
; algorithm it came from (see render_scaled_fixed_c's own comment in
; src/render_v2.c for the algorithm itself):
;   - `ld iy, (-1900524)` loads gfx_vbuffer's current-draw-buffer
;     pointer from address 0xE30014 (-1900524 is 0xE30014 as a signed
;     24-bit literal, i.e. 0xE30014 - 0x1000000) -- confirms this reads
;     GraphX's *current* buffer, not a hardcoded framebuffer address,
;     exactly like every other GraphX drawing call. (In this rewritten
;     version gfx_vbuffer is loaded once into a plain register, not
;     re-read per row, since the destination is now reached via three
;     running pointers rather than recomputed offsets.)
;   - 351 appears as the destination row-0/row-1 split point relative to
;     the buffer read above combined with the fixed y-offset -- consistent
;     with RENDER_DST_Y_OFFSET*RENDER_DST_STRIDE (24*320=7680) plus the
;     one-row (320-byte) gap between the two destination rows this block
;     writes, reached via running +1 addressing rather than a second
;     recomputed base.
;   - 160 is RENDER_SRC_W, 96 is RENDER_SRC_H, 640 (2*320) is the
;     per-source-row destination pointer advance, 16 is RENDER_UNROLL --
;     all appear as constants, never computed.
;   - No `call __imulu` (or any multiply) appears anywhere in this file.
;   - Exactly ONE `call` appears: `call __ladd`, the inlined 32-bit
;     g_render_calls[3]++ this file's instrumentation preamble performs
;     (see "Provenance" above) -- confirmed by grepping this file for
;     "imulu\|call" before writing it out.
;   - The single per-row loop (.LRSFA_BB_2) has exactly 16 unrolled
;     load/duplicate-to-both-rows/store sequences per iteration (matching
;     RENDER_UNROLL=16, i.e. 160/16 = 10 loop iterations per source row
;     instead of 160) -- no per-pixel conditional branch, only one
;     branch per 16-pixel block -- and each iteration writes FOUR bytes
;     (two into each of the two destination rows) per source byte read,
;     matching the both-rows-in-one-pass algorithm; there is no second,
;     structurally-identical loop the way the previous version had
;     (.LBB_2 and .LBB_4 for row 0 and row 1 separately) -- one loop now
;     produces both rows.
;   - push+pop pairs in the per-pixel body dropped from the previous
;     version's 8 per unrolled pixel (66 total per block) to a much
;     smaller, non-offset-addressing-driven count -- confirmed by
;     instruction-histogram diff against the previous extraction: 66
;     push+66 pop -> 21 push+21 pop per 16-pixel block (68% reduction),
;     with the redundant second full-source-row read eliminated too (167
;     ld/38 add/32 inc/21 push/21 pop/5 lea/2 sbc/2 or/2 jp/2 ex/1 ret,
;     294 lines total, versus the previous 136 ld/66 push/66 pop/14 add/
;     8 lea/4 sbc/4 or/3 jp/2 ex/1 ret/1 inc, 306 lines).
;
; Verified mechanically, not just by eye: `z80-none-elf-objdump -d` both
; obj/src/render_v2_asm.s.o (this file, assembled standalone) and
; obj/lto.o (a real `make CINEMA_RENDERER=fixed_c` build's LTO object,
; which contains the compiler's own from-C compilation of the identical
; algorithm) and diffed the two disassemblies with the function names
; and the deliberate id-offset edit normalized to match -- see the build
; log for this candidate for the exact diff output. This file's machine
; code is byte-for-byte identical to what the real toolchain produces
; for render_scaled_fixed_c in a complete, whole-program-optimized
; build (aside from that one deliberate, documented offset edit), not
; merely "the same algorithm" -- about as strong a static correctness
; argument as is possible without execution.
;
; What was *not* verified, and is exactly why this path is unverified
; overall: whether this is actually *fast* on real hardware (see the
; 2.51x-slower measurement of the PREVIOUS version at the top of this
; file -- this rewrite is a response to that, not yet a proven fix), and
; whether the eZ80 codegen is correct in ways only execution (not
; reading it) can prove.
; NOTE: `.assume ADL = 1` below is required and was almost lost when this
; function's section was extracted out of the middle of obj/lto.s -- the
; directive appears once, near the top of that file, ahead of every
; function's own section, and applies for the rest of the assembly. Its
; absence is a real, concrete example of the exact risk this whole file
; carries: without it, z80-none-elf-as silently encoded `ld ix, 0` (and
; presumably every other affected immediate) as a 16-bit literal instead
; of the required 24-bit ADL-mode one -- confirmed in the previous
; extraction round by objdump'ing the assembled object both with and
; without this line and comparing byte counts against the known-correct
; encoding in obj/lto.o for the equivalent render_scaled_fixed_c. It
; assembled *and linked* without any error either way -- nothing about
; the build catches this class of bug, only comparing the actual
; disassembled bytes did. Reapplied here from scratch for this rewrite.
	.section	.text,"ax",@progbits
	.assume	ADL = 1
	.section	.text._render_scaled_fixed_asm,"ax",@progbits
	.globl	_render_scaled_fixed_asm          ; -- Begin function render_scaled_fixed_c
	.type	_render_scaled_fixed_asm,@function
_render_scaled_fixed_asm:                 ; @render_scaled_fixed_c
; %bb.0:
	push	ix
	ld	ix, 0
	add	ix, sp
	lea	hl, ix - 21
	ld	sp, hl
	ld	hl, (ix + 6)
	ld	(ix - 6), hl
	ld	iy, _g_render_calls+12
	ld	bc, 1
	xor	a, a
	ld	hl, (_g_render_calls+12)
	lea	iy, iy + 3
	ld	e, (iy)
	call	__ladd
	ld	a, e
	ld	(_g_render_calls+12), hl
	ld	(_g_render_calls+15), a
	ld	iy, (-1900524)
	ld	de, 8031
	add	iy, de
	or	a, a
	sbc	hl, hl
	ld	(ix - 18), hl
	.local	.LRSFA_BB_1
.LRSFA_BB_1:                               ; =>This Loop Header: Depth=1
                                        ;     Child Loop RSFA_BB_2 Depth 2
	ld	(ix - 21), iy
	ld	bc, 0
	.local	.LRSFA_BB_2
.LRSFA_BB_2:                               ;   Parent Loop RSFA_BB_1 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	ld	(ix - 9), bc
	ld	(ix - 3), iy
	ld	hl, (ix - 6)
	add	hl, bc
	push	hl
	pop	bc
	lea	hl, iy + 0
	ld	de, -351
	add	hl, de
	ld	(ix - 12), hl
	push	bc
	pop	hl
	ld	a, (hl)
	lea	hl, iy + 0
	inc	de
	add	hl, de
	ld	(ix - 15), hl
	ld	hl, (ix - 12)
	ld	(hl), a
	lea	hl, iy + 0
	inc	de
	add	hl, de
	ld	(ix - 12), hl
	ld	hl, (ix - 15)
	ld	(hl), a
	ld	(iy - 31), a
	ld	(iy - 30), a
	push	bc
	pop	iy
	ld	a, (iy + 1)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	hl, (ix - 12)
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 29), a
	ld	iy, (ix - 3)
	ld	(iy - 28), a
	push	bc
	pop	iy
	ld	a, (iy + 2)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 27), a
	ld	iy, (ix - 3)
	ld	(iy - 26), a
	push	bc
	pop	iy
	ld	a, (iy + 3)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 25), a
	ld	iy, (ix - 3)
	ld	(iy - 24), a
	push	bc
	pop	iy
	ld	a, (iy + 4)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 23), a
	ld	iy, (ix - 3)
	ld	(iy - 22), a
	push	bc
	pop	iy
	ld	a, (iy + 5)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 21), a
	ld	iy, (ix - 3)
	ld	(iy - 20), a
	push	bc
	pop	iy
	ld	a, (iy + 6)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 19), a
	ld	iy, (ix - 3)
	ld	(iy - 18), a
	push	bc
	pop	iy
	ld	a, (iy + 7)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 17), a
	ld	iy, (ix - 3)
	ld	(iy - 16), a
	push	bc
	pop	iy
	ld	a, (iy + 8)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 15), a
	ld	iy, (ix - 3)
	ld	(iy - 14), a
	push	bc
	pop	iy
	ld	a, (iy + 9)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 13), a
	ld	iy, (ix - 3)
	ld	(iy - 12), a
	push	bc
	pop	iy
	ld	a, (iy + 10)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 11), a
	ld	iy, (ix - 3)
	ld	(iy - 10), a
	push	bc
	pop	iy
	ld	a, (iy + 11)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 9), a
	ld	iy, (ix - 3)
	ld	(iy - 8), a
	push	bc
	pop	iy
	ld	a, (iy + 12)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 7), a
	ld	iy, (ix - 3)
	ld	(iy - 6), a
	push	bc
	pop	iy
	ld	a, (iy + 13)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 5), a
	ld	iy, (ix - 3)
	ld	(iy - 4), a
	push	bc
	pop	iy
	ld	a, (iy + 14)
	ld	iy, (ix - 3)
	inc	de
	add	iy, de
	ld	(hl), a
	ld	hl, (ix - 3)
	inc	de
	add	hl, de
	ld	(iy), a
	ld	iy, (ix - 3)
	ld	(iy - 3), a
	ld	iy, (ix - 3)
	ld	(iy - 2), a
	push	bc
	pop	iy
	ld	a, (iy + 15)
	ld	bc, (ix - 3)
	push	bc
	pop	iy
	inc	de
	add	iy, de
	ld	de, 160
	ld	(hl), a
	ld	(iy), a
	push	bc
	pop	iy
	ld	(iy - 1), a
	ld	(iy), a
	ld	hl, (ix - 9)
	ld	bc, 16
	add	hl, bc
	push	hl
	pop	bc
	lea	iy, iy + 32
	or	a, a
	sbc	hl, de
	jp	nz, .LRSFA_BB_2
; %bb.3:                                ;   in Loop: Header=RSFA_BB_1 Depth=1
	ld	hl, (ix - 6)
	add	hl, de
	ld	(ix - 6), hl
	ld	hl, (ix - 18)
	inc	hl
	ld	iy, (ix - 21)
	ld	bc, 640
	add	iy, bc
	ld	(ix - 18), hl
	ld	bc, 96
	or	a, a
	sbc	hl, bc
	jp	nz, .LRSFA_BB_1
; %bb.4:
	ld	sp, ix
	pop	ix
	ret
	.local	.Lrsfa_func_end
.Lrsfa_func_end:
	.size	_render_scaled_fixed_asm, .Lrsfa_func_end-_render_scaled_fixed_asm
