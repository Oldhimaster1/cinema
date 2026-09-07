; render_v2_asm.s -- eZ80 assembly for render_scaled_fixed_asm(), Cinema's
; CINEMA_RENDERER_FIXED_ASM candidate (see src/render_v2.h).
;
; *** UNVERIFIED BY EXECUTION ***
; This function has never actually run. There is no TI-84 Plus CE ROM
; image, emulator, or physical calculator available in the development
; environment it was produced in (cemu-autotester -- a real CE-emulator-
; based test runner that ships with the CE C/C++ toolchain -- exists and
; was investigated, but it requires a ROM dump only obtainable by
; legally dumping one from an owned physical calculator, which was not
; available either). Do not select CINEMA_RENDERER_FIXED_ASM for real
; playback until it has been tested -- ideally with a bounded render-
; only benchmark first, well before trusting it for normal movie
; playback. GraphX (CINEMA_RENDERER_GRAPHX, the default) is unaffected
; either way; this file is not linked into that path's execution, only
; (harmlessly) compiled and linked as dead code.
;
; Provenance: this is not hand-typed from scratch, and not blind
; "-S"-from-C-source output either (an earlier attempt at that used a
; different, incompatible assembly dialect -- see below). It is the
; verbatim GAS-syntax assembly this exact function compiles to *inside
; the real Cinema build*, extracted directly from the object the real
; toolchain produces:
;
;   make CINEMA_RENDERER=fixed_c   (a normal, complete build)
;   -> obj/lto.s, the whole-program-optimized assembly the real link
;      step actually assembles and links into bin/CINEMA.8xp
;   -> the _render_scaled_fixed_c section of that file, copied out
;      verbatim and renamed (render_scaled_fixed_c -> _asm,
;      .LBB37_* -> .LRSFA_BB_*, purely to avoid clashing with the real
;      render_scaled_fixed_c, which stays linked into every build
;      regardless of which renderer is selected) -- no instruction was
;      changed.
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
;     exactly like every other GraphX drawing call.
;   - Offsets 7680 and 8000 are 24*320 and 24*320+320 -- the two
;     destination row starts at y=24 and y=25, matching
;     RENDER_DST_Y_OFFSET * RENDER_DST_STRIDE and one row past it.
;   - 640 (2*320) is the per-source-row destination pointer advance,
;     160 is RENDER_SRC_W, 96 is RENDER_SRC_H -- all appear as constants,
;     never computed.
;   - No `call __imulu` (or any multiply) appears anywhere in this file.
;     An earlier, non-unrolled version of render_scaled_fixed_c compiled
;     to one `call __imulu` per source row (96 per frame); rewriting
;     that C to use running pointers instead of recomputing "y * stride"
;     each iteration removed it, confirmed by recompiling and grepping
;     the output both times.
;   - Each of the two per-row loops (.LRSFA_BB_2 / .LRSFA_BB_4) has
;     exactly 16 unrolled load/duplicate/store sequences (matching
;     RENDER_UNROLL=16, i.e. 160/16 = 10 loop iterations per destination
;     row instead of 160) -- no per-pixel conditional branch, only one
;     branch per 16-pixel block.
;   - .LRSFA_BB_2 and .LRSFA_BB_4 are structurally identical (row 1 vs.
;     row 2 of the same source row), each addressed from the same
;     source-row base, which is the "re-read the compact source row,
;     don't read the destination back" behavior the algorithm calls for.
;
; Verified mechanically, not just by eye: `z80-none-elf-objdump -d` both
; obj/src/render_v2_asm.s.o (this file, assembled standalone) and
; obj/lto.o (a real `make CINEMA_RENDERER=fixed_c` build's LTO object,
; which contains the compiler's own from-C compilation of the identical
; algorithm) and diffed the two disassemblies with the function names
; normalized to match -- zero differences, for every one of the 306
; disassembled lines. This file's machine code is byte-for-byte
; identical to what the real toolchain produces for render_scaled_fixed_c
; in a complete, whole-program-optimized build, not merely "the same
; algorithm" -- about as strong a static correctness argument as is
; possible without execution. (This also caught a real bug before it
; shipped: the first version of this file was missing the `.assume ADL =
; 1` directive above, seen once near the top of obj/lto.s and easy to
; lose when copying just one function's section out of the middle of
; it. Without it, z80-none-elf-as silently encoded `ld ix, 0` -- and
; presumably other affected immediates -- as 16-bit instead of the
; required 24-bit ADL-mode literal. It assembled and linked without any
; error either way; only the disassembly diff caught it.)
;
; What was *not* verified, and is exactly why this path is unverified
; overall: whether this is actually *fast* on real hardware, and whether
; the eZ80 codegen is correct in ways only execution (not reading it)
; can prove -- byte-identical to the compiler's own output rules out a
; transcription mistake in this file, but not a latent bug in the
; compiler's codegen itself. One visible inefficiency this review
; noticed but did not attempt to hand-fix (too much risk of introducing
; a real bug with no way to test the fix): the compiler shuttles values
; between `iy` and `hl`/`bc` via `push`/`pop` pairs rather than a direct
; register-to-register move, once per unrolled pixel -- plausibly
; because this target's ABI/register allocator only keeps one flexible
; offset-addressable index register comfortably live at a time. Whether
; that costs enough to matter next to the memory-access pattern itself
; is exactly the kind of question this development environment cannot
; answer; it would need real hardware or emulator timing.
; NOTE: `.assume ADL = 1` below is required and was almost lost when this
; function's section was extracted out of the middle of obj/lto.s -- the
; directive appears once, near the top of that file, ahead of every
; function's own section, and applies for the rest of the assembly. Its
; absence is a real, concrete example of the exact risk this whole file
; carries: without it, z80-none-elf-as silently encoded `ld ix, 0` (and
; presumably every other affected immediate) as a 16-bit literal instead
; of the required 24-bit ADL-mode one -- confirmed by objdump'ing the
; assembled object both with and without this line and comparing byte
; counts against the known-correct encoding in obj/lto.o for the
; equivalent render_scaled_fixed_c. It assembled *and linked* without
; any error either way -- nothing about the build catches this class of
; bug, only comparing the actual disassembled bytes did.
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
	lea	hl, ix - 18
	ld	sp, hl
	ld	bc, (ix + 6)
	ld	iy, (-1900524)
	ld	de, 7680
	lea	hl, iy + 0
	add	hl, de
	ld	(ix - 6), hl
	ld	de, 8000
	add	iy, de
	or	a, a
	sbc	hl, hl
	ld	(ix - 18), hl
	.local	.LRSFA_BB_1
.LRSFA_BB_1:                               ; =>This Loop Header: Depth=1
                                        ;     Child Loop RSFA_BB_2 Depth 2
                                        ;     Child Loop RSFA_BB_4 Depth 2
	ld	(ix - 9), iy
	ld	(ix - 3), bc
	ld	de, 0
	push	de
	pop	bc
	.local	.LRSFA_BB_2
.LRSFA_BB_2:                               ;   Parent Loop RSFA_BB_1 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	ld	(ix - 12), bc
	ld	iy, (ix - 6)
	add	iy, de
	ld	hl, (ix - 3)
	add	hl, bc
	push	hl
	pop	bc
	ld	a, (hl)
	ld	(iy), a
	ld	(iy + 1), a
	lea	hl, iy + 0
	push	bc
	pop	iy
	ld	a, (iy + 1)
	push	hl
	pop	iy
	ld	(iy + 2), a
	ld	(iy + 3), a
	push	bc
	pop	iy
	ld	a, (iy + 2)
	push	hl
	pop	iy
	ld	(iy + 4), a
	ld	(iy + 5), a
	push	bc
	pop	iy
	ld	a, (iy + 3)
	push	hl
	pop	iy
	ld	(iy + 6), a
	ld	(iy + 7), a
	push	bc
	pop	iy
	ld	a, (iy + 4)
	push	hl
	pop	iy
	ld	(iy + 8), a
	ld	(iy + 9), a
	push	bc
	pop	iy
	ld	a, (iy + 5)
	push	hl
	pop	iy
	ld	(iy + 10), a
	ld	(iy + 11), a
	push	bc
	pop	iy
	ld	a, (iy + 6)
	push	hl
	pop	iy
	ld	(iy + 12), a
	ld	(iy + 13), a
	push	bc
	pop	iy
	ld	a, (iy + 7)
	push	hl
	pop	iy
	ld	(iy + 14), a
	ld	(iy + 15), a
	push	bc
	pop	iy
	ld	a, (iy + 8)
	push	hl
	pop	iy
	ld	(iy + 16), a
	ld	(iy + 17), a
	push	bc
	pop	iy
	ld	a, (iy + 9)
	push	hl
	pop	iy
	ld	(iy + 18), a
	ld	(iy + 19), a
	push	bc
	pop	iy
	ld	a, (iy + 10)
	push	hl
	pop	iy
	ld	(iy + 20), a
	ld	(iy + 21), a
	push	bc
	pop	iy
	ld	a, (iy + 11)
	push	hl
	pop	iy
	ld	(iy + 22), a
	ld	(iy + 23), a
	push	bc
	pop	iy
	ld	a, (iy + 12)
	push	hl
	pop	iy
	ld	(iy + 24), a
	ld	(iy + 25), a
	push	bc
	pop	iy
	ld	a, (iy + 13)
	push	hl
	pop	iy
	ld	(iy + 26), a
	ld	(iy + 27), a
	push	bc
	pop	iy
	ld	a, (iy + 14)
	push	hl
	pop	iy
	ld	(iy + 28), a
	ld	(iy + 29), a
	push	bc
	pop	iy
	ld	a, (iy + 15)
	push	hl
	pop	iy
	ld	(iy + 30), a
	ld	(iy + 31), a
	ex	de, hl
	ld	de, 32
	add	hl, de
	ld	(ix - 15), hl
	ld	iy, (ix - 12)
	ld	de, 16
	add	iy, de
	lea	hl, iy + 0
	ld	de, 160
	or	a, a
	sbc	hl, de
	lea	bc, iy + 0
	ld	de, (ix - 15)
	jp	nz, .LRSFA_BB_2
; %bb.3:                                ; %.preheader.preheader
                                        ;   in Loop: Header=RSFA_BB_1 Depth=1
	ld	de, 0
	push	de
	pop	bc
	.local	.LRSFA_BB_4
.LRSFA_BB_4:                               ; %.preheader
                                        ;   Parent Loop RSFA_BB_1 Depth=1
                                        ; =>  This Inner Loop Header: Depth=2
	ld	(ix - 12), bc
	ld	iy, (ix - 9)
	add	iy, de
	ld	hl, (ix - 3)
	add	hl, bc
	push	hl
	pop	bc
	ld	a, (hl)
	ld	(iy), a
	ld	(iy + 1), a
	lea	hl, iy + 0
	push	bc
	pop	iy
	ld	a, (iy + 1)
	push	hl
	pop	iy
	ld	(iy + 2), a
	ld	(iy + 3), a
	push	bc
	pop	iy
	ld	a, (iy + 2)
	push	hl
	pop	iy
	ld	(iy + 4), a
	ld	(iy + 5), a
	push	bc
	pop	iy
	ld	a, (iy + 3)
	push	hl
	pop	iy
	ld	(iy + 6), a
	ld	(iy + 7), a
	push	bc
	pop	iy
	ld	a, (iy + 4)
	push	hl
	pop	iy
	ld	(iy + 8), a
	ld	(iy + 9), a
	push	bc
	pop	iy
	ld	a, (iy + 5)
	push	hl
	pop	iy
	ld	(iy + 10), a
	ld	(iy + 11), a
	push	bc
	pop	iy
	ld	a, (iy + 6)
	push	hl
	pop	iy
	ld	(iy + 12), a
	ld	(iy + 13), a
	push	bc
	pop	iy
	ld	a, (iy + 7)
	push	hl
	pop	iy
	ld	(iy + 14), a
	ld	(iy + 15), a
	push	bc
	pop	iy
	ld	a, (iy + 8)
	push	hl
	pop	iy
	ld	(iy + 16), a
	ld	(iy + 17), a
	push	bc
	pop	iy
	ld	a, (iy + 9)
	push	hl
	pop	iy
	ld	(iy + 18), a
	ld	(iy + 19), a
	push	bc
	pop	iy
	ld	a, (iy + 10)
	push	hl
	pop	iy
	ld	(iy + 20), a
	ld	(iy + 21), a
	push	bc
	pop	iy
	ld	a, (iy + 11)
	push	hl
	pop	iy
	ld	(iy + 22), a
	ld	(iy + 23), a
	push	bc
	pop	iy
	ld	a, (iy + 12)
	push	hl
	pop	iy
	ld	(iy + 24), a
	ld	(iy + 25), a
	push	bc
	pop	iy
	ld	a, (iy + 13)
	push	hl
	pop	iy
	ld	(iy + 26), a
	ld	(iy + 27), a
	push	bc
	pop	iy
	ld	a, (iy + 14)
	push	hl
	pop	iy
	ld	(iy + 28), a
	ld	(iy + 29), a
	push	bc
	pop	iy
	ld	a, (iy + 15)
	push	hl
	pop	iy
	ld	(iy + 30), a
	ld	(iy + 31), a
	ex	de, hl
	ld	de, 32
	add	hl, de
	ld	(ix - 15), hl
	ld	iy, (ix - 12)
	ld	de, 16
	add	iy, de
	lea	hl, iy + 0
	ld	de, 160
	or	a, a
	sbc	hl, de
	lea	bc, iy + 0
	ld	de, (ix - 15)
	jp	nz, .LRSFA_BB_4
; %bb.5:                                ;   in Loop: Header=RSFA_BB_1 Depth=1
	ld	hl, (ix - 3)
	ld	de, 160
	add	hl, de
	push	hl
	pop	bc
	ld	hl, (ix - 6)
	ld	de, 640
	add	hl, de
	ld	(ix - 6), hl
	ld	iy, (ix - 9)
	add	iy, de
	ld	hl, (ix - 18)
	inc	hl
	ld	(ix - 18), hl
	ld	de, 96
	or	a, a
	sbc	hl, de
	jp	nz, .LRSFA_BB_1
; %bb.6:
	ld	sp, ix
	pop	ix
	ret
	.local	.Lrsfa_func_end
.Lrsfa_func_end:
	.size	_render_scaled_fixed_asm, .Lrsfa_func_end-_render_scaled_fixed_asm
