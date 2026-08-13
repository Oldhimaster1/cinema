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

# ----------------------------

include $(shell cedev-config --makefile)
