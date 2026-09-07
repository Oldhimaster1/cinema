# Standalone render-only benchmark, NOT part of Cinema's own build.
#
# Measures gfx_ScaledSprite_NoClip() against whichever fixed renderer is
# selected (CINEMA_RENDERER=fixed_c or fixed_asm, same meaning as
# Cinema's own makefile), on a fixed, deterministic 160x96 test frame --
# no USB, no FAT, no scheduler, nothing but the render call itself timed
# in a tight loop. Bounded iteration count, explicit on-screen escape
# path (any key) before it exits -- see the renderer phase's report for
# why this exists as its own tiny program rather than a mode inside
# Cinema: keeping it minimal keeps what's being timed (and what's being
# risked, for the still execution-unverified fixed_asm path) as small
# and legible as possible.
#
# Build from this directory: `make CINEMA_RENDERER=fixed_asm` (or
# fixed_c, or omit for the graphx-only control -- see src/main.c).

NAME = RENDBNCH
ICON =
DESCRIPTION = "Cinema renderer benchmark (not for normal use)"
COMPRESSED = NO

CFLAGS = -Wall -Wextra -O3 -I../src
CXXFLAGS = -Wall -Wextra -O3 -I../src
LTOFLAGS = -Wall -Wextra -O3 -I../src

CINEMA_RENDERER ?= graphx

EXTRA_CSOURCES += ../src/render_v2.c

ifeq ($(CINEMA_RENDERER),fixed_c)
CFLAGS   += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_C
CXXFLAGS += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_C
LTOFLAGS += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_C
else ifeq ($(CINEMA_RENDERER),fixed_asm)
CFLAGS   += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_ASM
CXXFLAGS += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_ASM
LTOFLAGS += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_ASM
EXTRA_ASMSOURCES += ../src/render_v2_asm.s
else ifneq ($(CINEMA_RENDERER),graphx)
$(error Unknown CINEMA_RENDERER '$(CINEMA_RENDERER)' -- use graphx, fixed_c, or fixed_asm)
endif

include $(shell cedev-config --makefile)
