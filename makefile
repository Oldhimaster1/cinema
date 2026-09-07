# ----------------------------
# Makefile Options
# ----------------------------

NAME = CINEMA
ICON = icon.png
DESCRIPTION = "USB Video Player -- William Wierzbowski"
COMPRESSED = YES

# -O3, not the CE default -Oz. Cinema is a real-time video player: the
# per-frame decode/blit and the USB service loop are the whole product,
# and -Oz (optimize for minimum *size*) costs speed for bytes we have
# plenty of -- an .8xp a few KB larger is free, dropped frames are not.
# LTOFLAGS must be set too: CEdev's makefile.mk defaults it to $(CFLAGS),
# but the LTO recompile is where the hot loops actually get their final
# codegen, so leaving it at -Oz would undo most of this.
CFLAGS = -Wall -Wextra -O3
CXXFLAGS = -Wall -Wextra -O3
LTOFLAGS = -Wall -Wextra -O3

# Which renderer src/player_v2.c uses for its per-frame 160x96->320x192
# 2x scale-up -- see src/render_v2.h for what each one is.
#   graphx    (default) -- gfx_ScaledSprite_NoClip(). Hardware-proven;
#             what every prior release shipped. Use this unless you have
#             a specific reason not to.
#   fixed_c   -- Cinema's own specialized C implementation. Host-tested
#               for pixel correctness; not yet tested on real hardware.
#   fixed_asm -- same algorithm in hand-reviewed eZ80 assembly
#               (src/render_v2_asm.s), generated from and checked
#               against fixed_c. UNVERIFIED BY EXECUTION -- no ROM,
#               emulator, or calculator was available to actually run it
#               during development. Do not use for real playback without
#               testing it yourself first (see that file's header
#               comment and docs/RENDERER.md).
# Override on the command line, e.g. `make CINEMA_RENDERER=fixed_c`.
CINEMA_RENDERER ?= graphx

ifeq ($(CINEMA_RENDERER),fixed_c)
CFLAGS   += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_C
CXXFLAGS += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_C
LTOFLAGS += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_C
else ifeq ($(CINEMA_RENDERER),fixed_asm)
CFLAGS   += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_ASM
CXXFLAGS += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_ASM
LTOFLAGS += -DCINEMA_RENDERER=CINEMA_RENDERER_FIXED_ASM
else ifneq ($(CINEMA_RENDERER),graphx)
$(error Unknown CINEMA_RENDERER '$(CINEMA_RENDERER)' -- use graphx, fixed_c, or fixed_asm)
endif

# ----------------------------

include $(shell cedev-config --makefile)
