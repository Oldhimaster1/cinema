#include "player_v2.h"
#include "fat32ro.h"
#include "msd_util.h"

#include <fileioc.h>
#include <graphx.h>
#include <msddrvce.h>
#include <tice.h>
#include <usbdrvce.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* 2, not 4: frames are no longer bit-packed (see frame_slot_t below),
 * so each slot's buffer doubled in size. Halving the slot count keeps
 * total slot RAM the same as before (2 * 15,362 =~ 4 * 7,682) instead of
 * doubling it. 2 slots is exactly the double-buffering depth Cinema's
 * v1 (legacy) player has always used successfully at this same frame
 * size, so this isn't a step into the unknown. */
#define SLOT_COUNT 2

#define V2_Y_OFFSET ((GFX_LCD_HEIGHT - CINEMA_V2_DEST_HEIGHT) / 2)

/* The video occupies rows [V2_Y_OFFSET, V2_Y_OFFSET + 192). The OSD
 * lives entirely in the letterbox margin *below* it, so showing the OSD
 * never overwrites video pixels and never forces a re-blit. */
#define V2_OSD_TOP  (V2_Y_OFFSET + CINEMA_V2_DEST_HEIGHT)
#define V2_OSD_ROWS (GFX_LCD_HEIGHT - V2_OSD_TOP)
#define V2_OSD_BAR_Y (V2_OSD_TOP + 3)
#define V2_OSD_BAR_H 4
#define V2_OSD_BAR_X 4
#define V2_OSD_BAR_W (GFX_LCD_WIDTH - 2 * V2_OSD_BAR_X)
#define V2_OSD_TEXT_Y (V2_OSD_TOP + 11)

/* How long the OSD stays up after a control press before auto-hiding. */
#define V2_OSD_LINGER_TICKS (3UL * CLOCKS_PER_SEC)

/* Seek step sizes, in seconds of movie time. */
#define V2_SEEK_SMALL 10
#define V2_SEEK_LARGE 60

/* A frame's data normally comes from one contiguous run of sectors, but
 * when the movie is a file on a FAT32 drive (see src/fat32ro.h) rather
 * than a raw whole-device image, a fragmented file can split a single
 * frame's sectors across more than one extent. CINEMA_MAX_PARTS_PER_FRAME
 * bounds how many separate reads one frame can require; a frame needing
 * more than this is a "too fragmented to play" condition, reported as an
 * error rather than silently reading the wrong data or growing the slot
 * structure unboundedly. In practice a contiguous (or lightly
 * fragmented) file needs exactly 1. */
#define CINEMA_MAX_PARTS_PER_FRAME 4

typedef enum {
    SLOT_EMPTY,
    SLOT_LOADING,
    SLOT_NEEDS_NEXT_PART, /* previous part done; another part remains to queue */
    SLOT_READY,
    SLOT_ERROR
} slot_state_t;

typedef struct {
    uint32_t lba;
    uint32_t sectors;
} frame_part_t;

typedef struct {
    /* gfx_sprite_t-shaped (2-byte width/height header + pixel data), and
     * used as one: a frame's bytes are read off USB directly into
     * sprite_data + 2 (see queue_slot_part), and render_frame passes
     * this straight to gfx_ScaledSprite_NoClip with no copy or unpack
     * step in between. This is deliberately the same zero-decode design
     * Cinema's v1 (legacy) player uses -- an earlier version of v2
     * stored frames bit-packed 2-per-byte to halve the USB read size,
     * but real-hardware testing traced most of the resulting slowdown to
     * the CPU cost of unpacking that packing back out every frame on the
     * ez80 core, which cost more than the packing saved. width/height
     * are set once per slot, in player_v2_run(); nothing after that ever
     * changes them, so there's no per-frame header-writing cost either. */
    uint8_t sprite_data[2 + CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT];
    uint32_t frame_number;
    volatile slot_state_t state;
    volatile msd_error_t error;
    msd_transfer_t transfer;

    /* Resolved once, when the frame is first queued (see
     * resolve_frame_parts), then serviced one at a time: the completion
     * callback only ever records state (see frame_read_callback), so
     * queueing the *next* part happens from the main loop (in
     * refill_empty_slots), the same place new frames get queued. */
    frame_part_t pending_parts[CINEMA_MAX_PARTS_PER_FRAME];
    uint8_t pending_part_count;
    uint8_t next_pending_part;
} frame_slot_t;

static gfx_sprite_t *slot_sprite(frame_slot_t *slot)
{
    return (gfx_sprite_t *)slot->sprite_data;
}

typedef struct {
    global_t *global;
    const fat32ro_extent_map_t *movie_map;
    frame_slot_t slots[SLOT_COUNT];

    char filename[CIN2_RESUME_FILENAME_LEN]; /* "" for raw single-image mode */

    uint32_t next_frame_to_queue;
    uint32_t frame_count;
    uint32_t start_frame;
    bool has_presented;
    uint32_t last_frame_presented;

    uint32_t fps_num;
    uint32_t fps_den;

    clock_t start_tick;
    clock_t pause_tick;
    clock_t accumulated_pause_ticks;
    bool paused;

    uint32_t dropped_frames;
    uint32_t repeated_frames;

    /* --- OSD / controls --- */
    bool osd_pinned;          /* toggled on with [mode], stays until toggled off */
    clock_t osd_until_tick;   /* transient show-after-keypress deadline */
    uint8_t osd_clear_pending; /* frames left to scrub the OSD out of both buffers */
    uint8_t osd_fg;           /* brightest palette index, chosen at startup */
    uint8_t osd_bg;           /* darkest palette index */

    /* --- telemetry, reported on exit and in the OSD --- */
    uint32_t decode_ticks_total;
    uint32_t decode_samples;
    uint32_t fps_window_frames;
    clock_t fps_window_start;
    uint32_t fps_tenths;      /* measured presentation rate x10 */
} player_v2_t;

/* Callback only records what happened -- no graphics calls, no printing,
 * no LBA math, and (per the comment on pending_parts above) no queueing
 * of the next part either. The main loop decides what any of it means. */
static void frame_read_callback(msd_error_t error, struct msd_transfer *xfer)
{
    frame_slot_t *slot = (frame_slot_t *)xfer->userptr;

    slot->error = error;
    if (error != MSD_SUCCESS) {
        slot->state = SLOT_ERROR;
        return;
    }

    slot->next_pending_part++;
    slot->state = (slot->next_pending_part >= slot->pending_part_count)
        ? SLOT_READY : SLOT_NEEDS_NEXT_PART;
}

/* Splits frame_number's CIN2_FRAME_SECTORS-sector range into parts via
 * the movie's extent map, capped at CINEMA_MAX_PARTS_PER_FRAME. False
 * means the frame can't be resolved at all: either it needs more parts
 * than that bound (a pathologically fragmented file), or it reaches
 * past the mapped extent (which should not happen for any frame within
 * a header's own frame_count, since the map is sized to the file's
 * declared length -- checked here anyway rather than trusting that). */
static bool resolve_frame_parts(const fat32ro_extent_map_t *map, uint32_t frame_number,
                                 frame_slot_t *slot)
{
    uint32_t sector_offset = cin2_frame_lba(frame_number);
    uint32_t remaining = CIN2_FRAME_SECTORS;
    uint8_t count = 0;

    while (remaining > 0) {
        uint32_t lba, run;

        if (count >= CINEMA_MAX_PARTS_PER_FRAME) {
            return false;
        }
        if (!fat32ro_extent_lookup(map, sector_offset, &lba, &run)) {
            return false;
        }
        if (run > remaining) {
            run = remaining;
        }

        slot->pending_parts[count].lba = lba;
        slot->pending_parts[count].sectors = run;
        count++;

        sector_offset += run;
        remaining -= run;
    }

    slot->pending_part_count = count;
    slot->next_pending_part = 0;
    return true;
}

/* Queues slot->pending_parts[slot->next_pending_part] -- either the
 * first part of a freshly resolved frame, or the next part of one
 * already in progress (SLOT_NEEDS_NEXT_PART). */
static msd_error_t queue_slot_part(global_t *global, frame_slot_t *slot)
{
    uint8_t idx = slot->next_pending_part;
    uint32_t byte_offset = 0;
    uint8_t i;
    msd_error_t result;

    for (i = 0; i < idx; ++i) {
        byte_offset += slot->pending_parts[i].sectors * FAT32RO_SECTOR_BYTES;
    }

    slot->transfer.msd = &global->msd;
    slot->transfer.lba = slot->pending_parts[idx].lba;
    slot->transfer.count = slot->pending_parts[idx].sectors;
    slot->transfer.buffer = slot_sprite(slot)->data + byte_offset;
    slot->transfer.callback = frame_read_callback;
    slot->transfer.userptr = slot;

    slot->state = SLOT_LOADING;
    result = msd_ReadAsync(&slot->transfer);
    if (result != MSD_SUCCESS) {
        slot->error = result;
        slot->state = SLOT_ERROR;
    }

    return result;
}

static msd_error_t queue_frame(global_t *global, const fat32ro_extent_map_t *map,
                                frame_slot_t *slot, uint32_t frame_number)
{
    slot->frame_number = frame_number;
    slot->error = MSD_SUCCESS;

    if (!resolve_frame_parts(map, frame_number, slot)) {
        slot->error = MSD_ERROR_INVALID_PARAM;
        slot->state = SLOT_ERROR;
        return MSD_ERROR_INVALID_PARAM;
    }

    return queue_slot_part(global, slot);
}

/* Queues the next not-yet-read frame into every SLOT_EMPTY slot, and
 * queues the next part of every SLOT_NEEDS_NEXT_PART slot (a frame
 * that's split across a fragmentation boundary -- see
 * CINEMA_MAX_PARTS_PER_FRAME). Returns false only if msd_ReadAsync
 * itself failed to queue, or a frame couldn't be resolved to sectors at
 * all (not if a previously-queued transfer later errors out -- that's
 * caught via find_failed_slot in the main loop).
 *
 * A usb_HandleEvents() call after each individual slot's queueing call
 * (rather than only queueing all of them back to back and servicing
 * events afterward) is deliberate: real-hardware testing during
 * development hit an unexplained hard crash when several msd_ReadAsync
 * calls were issued in a tight loop with no event-servicing between
 * them, and reproducibly stopped happening once queueing was paced out
 * this way. The exact mechanism was never pinned down further, but the
 * cost of this is one extra call per slot (SLOT_COUNT of them, at most),
 * which is negligible next to the read itself. */
static bool refill_empty_slots(player_v2_t *player)
{
    uint8_t i;

    for (i = 0; i < SLOT_COUNT; ++i) {
        frame_slot_t *slot = &player->slots[i];

        if (slot->state == SLOT_NEEDS_NEXT_PART) {
            if (queue_slot_part(player->global, slot) != MSD_SUCCESS) {
                return false;
            }
            usb_HandleEvents();
            continue;
        }

        if (slot->state != SLOT_EMPTY) {
            continue;
        }
        if (player->next_frame_to_queue >= player->frame_count) {
            continue;
        }

        if (queue_frame(player->global, player->movie_map, slot,
                         player->next_frame_to_queue) != MSD_SUCCESS) {
            return false;
        }
        usb_HandleEvents();

        player->next_frame_to_queue++;
    }

    return true;
}

static frame_slot_t *find_failed_slot(player_v2_t *player)
{
    uint8_t i;

    for (i = 0; i < SLOT_COUNT; ++i) {
        if (player->slots[i].state == SLOT_ERROR) {
            return &player->slots[i];
        }
    }

    return NULL;
}

/* Queues frame_number into `slot` and blocks until that one transfer
 * reaches a terminal state (READY or ERROR), servicing any
 * SLOT_NEEDS_NEXT_PART continuation along the way -- so at most one
 * msd_ReadAsync transfer is ever outstanding while this runs. See
 * serialized_fill_slots for why that matters. context is just for
 * error messages ("prefill" or "seek"). */
static bool serialized_fill_slot(player_v2_t *player, frame_slot_t *slot,
                                   uint32_t frame_number, const char *context)
{
    char buffer[40];

    if (queue_frame(player->global, player->movie_map, slot, frame_number) != MSD_SUCCESS) {
        sprintf(buffer, "error queueing msd (%s)", context);
        putstr(buffer);
        return false;
    }

    for (;;) {
        if (slot->state == SLOT_READY) {
            return true;
        }
        if (slot->state == SLOT_ERROR) {
            put_msd_error(slot->error, context);
            return false;
        }
        if (slot->state == SLOT_NEEDS_NEXT_PART) {
            if (queue_slot_part(player->global, slot) != MSD_SUCCESS) {
                sprintf(buffer, "error queueing msd (%s)", context);
                putstr(buffer);
                return false;
            }
        }

        usb_HandleEvents();

        if (player->global->usb == NULL) {
            putstr("usb device disconnected");
            return false;
        }
        if (os_GetCSC()) {
            return false;
        }
    }
}

/* Queues every not-yet-read frame that fits in the slots, one at a
 * time, waiting for each to fully finish (or fail) before starting the
 * next. Used both for the initial prefill burst and right after a seek
 * resets all slots -- the two moments where up to SLOT_COUNT fresh
 * transfers would otherwise get queued back-to-back in a single
 * refill_empty_slots() pass. Real-hardware testing hit a hard crash
 * doing exactly that (several msd_ReadAsync calls with no gap between
 * them); this guarantees at most one transfer is ever outstanding
 * during these bursts. Steady-state playback (refill_empty_slots
 * called once per main-loop iteration) is left as it was: it only
 * rarely needs to queue more than one slot per call, so the same risk
 * doesn't really apply there. */
static bool serialized_fill_slots(player_v2_t *player, const char *context)
{
    uint8_t i;

    for (i = 0; i < SLOT_COUNT; ++i) {
        frame_slot_t *slot = &player->slots[i];

        if (slot->state == SLOT_ERROR) {
            continue; /* left for the main loop to observe/report */
        }
        if (player->next_frame_to_queue >= player->frame_count) {
            break;
        }

        if (!serialized_fill_slot(player, slot, player->next_frame_to_queue, context)) {
            return false;
        }
        player->next_frame_to_queue++;
    }

    return true;
}

/* Fills every slot and blocks until each either finishes or errors, so
 * playback starts with a full read-ahead buffer instead of the single
 * frame the original player waited for. */
static bool prefill_frames(player_v2_t *player)
{
    return serialized_fill_slots(player, "prefill");
}

/* Frames that finished loading but are older than what the clock now
 * wants are stale -- we're behind schedule. Free their slots (counting
 * a drop) so refill_empty_slots can queue what actually comes next. */
static void discard_obsolete_frames(player_v2_t *player, uint32_t wanted)
{
    uint8_t i;

    for (i = 0; i < SLOT_COUNT; ++i) {
        frame_slot_t *slot = &player->slots[i];

        if (slot->state == SLOT_READY && slot->frame_number < wanted) {
            slot->state = SLOT_EMPTY;
            player->dropped_frames++;
        }
    }
}

static frame_slot_t *find_ready_frame(player_v2_t *player, uint32_t wanted)
{
    uint8_t i;

    for (i = 0; i < SLOT_COUNT; ++i) {
        frame_slot_t *slot = &player->slots[i];

        if (slot->state == SLOT_READY && slot->frame_number == wanted) {
            return slot;
        }
    }

    return NULL;
}

/* Which frame *should* be on screen right now, based on wall-clock time
 * elapsed since playback started (minus any time spent paused), offset
 * by start_frame so a resumed movie schedules relative to where it
 * resumed rather than relative to frame 0 (whose slots aren't even
 * being queued anymore). Using uint64_t here (rather than trying to
 * keep everything in 32 bits) is deliberate: fps_num can be up to
 * 24000 and elapsed ticks can run into the hundreds of millions for a
 * long movie, and that product overflows 32 bits. This math runs once
 * per main-loop iteration, not per pixel, so the extra cost of 64-bit
 * arithmetic on ez80 is not a hot path.
 */
static uint32_t desired_frame(const player_v2_t *player, clock_t now)
{
    clock_t elapsed = now - player->start_tick - player->accumulated_pause_ticks;
    uint64_t numerator = (uint64_t)elapsed * player->fps_num;
    uint64_t denominator = (uint64_t)CLOCKS_PER_SEC * player->fps_den;
    uint64_t frame = (uint64_t)player->start_frame + numerator / denominator;

    if (frame > 0xFFFFFFFFu) {
        frame = 0xFFFFFFFFu;
    }

    return (uint32_t)frame;
}

/* Relative brightness of an RGB1555 entry (gfx_SetPalette's real 5-5-5
 * format -- see docs/CIN2_FORMAT.md). Only used to compare palette
 * entries against each other, so the weights just need to be sane and
 * the arithmetic integer -- the absolute scale is meaningless. */
static uint16_t luminance1555(uint16_t color)
{
    uint16_t r = (color >> 10) & 0x1F;
    uint16_t g = (color >> 5) & 0x1F;
    uint16_t b = color & 0x1F;

    return (uint16_t)(2u * r + 3u * g + b);
}

/* The OSD has no palette of its own -- CIN2 stores exactly 16 colors and
 * they all belong to the movie. So pick the brightest entry for text and
 * the darkest for the backdrop; that keeps the overlay readable whatever
 * the movie's palette happens to be. */
static void choose_osd_colors(player_v2_t *player, const cin2_header_t *header)
{
    uint16_t best = 0;
    uint16_t worst = 0xFFFF;
    uint8_t i;

    player->osd_fg = 0;
    player->osd_bg = 0;

    for (i = 0; i < 16; ++i) {
        uint16_t lum = luminance1555(header->palette[i]);

        if (lum >= best) {
            best = lum;
            player->osd_fg = i;
        }
        if (lum <= worst) {
            worst = lum;
            player->osd_bg = i;
        }
    }
}

static bool osd_should_draw(const player_v2_t *player)
{
    /* Signed difference so the comparison stays correct across a clock_t
     * wraparound: negative means "now is still before the deadline". */
    return player->osd_pinned
        || (int32_t)((uint32_t)clock() - (uint32_t)player->osd_until_tick) < 0;
}

static void osd_poke(player_v2_t *player)
{
    player->osd_until_tick = clock() + (clock_t)V2_OSD_LINGER_TICKS;
}

static void format_timecode(char *out, uint32_t frame,
                             uint32_t fps_num, uint32_t fps_den)
{
    uint32_t seconds = (uint32_t)(((uint64_t)frame * fps_den) / fps_num);

    sprintf(out, "%lu:%02lu",
            (unsigned long)(seconds / 60u), (unsigned long)(seconds % 60u));
}

/* Drawn with GraphX primitives rather than memset on gfx_vbuffer so the
 * OSD lands on whichever target gfx_SetDraw() currently selects. That
 * lets the paused path overlay it straight onto the *visible* screen
 * without a buffer swap (and so without flickering between two frames),
 * while normal playback draws it into the offscreen buffer. */
static void osd_fill_rect(uint24_t x, uint8_t y, uint24_t width, uint8_t height,
                           uint8_t color)
{
    gfx_SetColor(color);
    gfx_FillRectangle_NoClip(x, y, width, height);
}

/* Draws into the letterbox margin of the current draw target. During
 * playback this is called after the frame blit and before
 * gfx_SwapDraw(); while paused it targets the visible screen. */
static void draw_osd(player_v2_t *player)
{
    /* Generous enough for every field at its theoretical uint32_t
     * maximum simultaneously (compilers' -Wformat-overflow checks that
     * worst case, not the realistic one). */
    char line[96];
    char position[16];
    char total[16];
    uint32_t filled;

    osd_fill_rect(0, V2_OSD_TOP, GFX_LCD_WIDTH, V2_OSD_ROWS, player->osd_bg);

    /* Progress bar: elapsed portion in the text color, the remainder
     * left as backdrop. */
    filled = player->frame_count > 1
        ? (uint32_t)(((uint64_t)player->last_frame_presented * V2_OSD_BAR_W)
                      / (player->frame_count - 1))
        : V2_OSD_BAR_W;
    if (filled > V2_OSD_BAR_W) {
        filled = V2_OSD_BAR_W;
    }
    if (filled > 0) {
        osd_fill_rect(V2_OSD_BAR_X, V2_OSD_BAR_Y, (uint24_t)filled,
                       V2_OSD_BAR_H, player->osd_fg);
    }

    format_timecode(position, player->last_frame_presented,
                     player->fps_num, player->fps_den);
    format_timecode(total, player->frame_count, player->fps_num, player->fps_den);

    if (player->paused) {
        sprintf(line, "PAUSED  %s/%s", position, total);
    } else {
        uint32_t decode_ms = player->decode_samples
            ? (uint32_t)(((uint64_t)player->decode_ticks_total * 1000u)
                          / ((uint64_t)player->decode_samples * CLOCKS_PER_SEC))
            : 0u;

        sprintf(line, "%s/%s  %lu.%luFPS  DEC%lums  DR%lu",
                position, total,
                (unsigned long)(player->fps_tenths / 10u),
                (unsigned long)(player->fps_tenths % 10u),
                (unsigned long)decode_ms,
                (unsigned long)player->dropped_frames);
    }

    gfx_SetTextFGColor(player->osd_fg);
    gfx_SetTextBGColor(player->osd_bg);
    gfx_PrintStringXY(line, 4, V2_OSD_TEXT_Y);
}

/* Waits until no slot has a transfer in flight. Required before reusing
 * slot buffers on a seek: msd_ReadAsync owns slot->sprite_data until its
 * callback fires, and there is no cancel in the msddrvce API. */
static void drain_loading_slots(player_v2_t *player)
{
    while (true) {
        bool any_loading = false;
        uint8_t i;

        for (i = 0; i < SLOT_COUNT; ++i) {
            if (player->slots[i].state == SLOT_LOADING) {
                any_loading = true;
                break;
            }
        }
        if (!any_loading) {
            return;
        }
        if (player->global->usb == NULL) {
            /* Device is gone; the callbacks are never coming. The main
             * loop detects the disconnect and bails on the next pass. */
            return;
        }

        usb_HandleEvents();
    }
}

/* Returns false only on a fatal I/O error/disconnect while refilling
 * the slots the seek just invalidated (mirroring prefill_frames' fatal
 * error handling) -- player_v2_loop treats that exactly like any other
 * fatal error and stops. */
static bool player_seek_to_frame(player_v2_t *player, uint32_t target)
{
    uint8_t i;

    if (player->frame_count == 0) {
        return true;
    }
    if (target >= player->frame_count) {
        target = player->frame_count - 1;
    }

    drain_loading_slots(player);

    for (i = 0; i < SLOT_COUNT; ++i) {
        /* SLOT_ERROR is left intact: the main loop still has to observe
         * and report it. Everything else is safe to reuse now that no
         * transfer is outstanding. */
        if (player->slots[i].state != SLOT_ERROR) {
            player->slots[i].state = SLOT_EMPTY;
        }
    }

    player->next_frame_to_queue = target;
    player->start_frame = target;
    player->start_tick = clock();
    player->accumulated_pause_ticks = 0;
    /* A seek always resumes playback (standard player behavior, and it
     * sidesteps a real gap otherwise: the paused path only repaints via
     * the OSD overlay on the still-visible old frame, since the slot
     * that was on screen was just invalidated above -- staying paused
     * here would update the OSD's position readout without ever
     * actually showing the frame that was jumped to). */
    player->paused = false;

    /* Position the "already shown" marker just before the target so the
     * scheduler asks for `target` next. Seeking to 0 means nothing has
     * been presented yet at all, which also keeps the resume record from
     * claiming a frame we never displayed. */
    if (target > 0) {
        player->last_frame_presented = target - 1;
    } else {
        player->last_frame_presented = 0;
        player->has_presented = false;
    }

    player->fps_window_frames = 0;
    player->fps_window_start = player->start_tick;

    /* Every slot just got reset to EMPTY above -- refilling all of them
     * one at a time (rather than leaving it to the main loop's usual
     * one-pass refill_empty_slots() call) avoids the same back-to-back
     * multi-transfer burst that prefill_frames now avoids. See
     * serialized_fill_slots. */
    return serialized_fill_slots(player, "seek");
}

static bool player_seek_seconds(player_v2_t *player, int32_t delta_seconds)
{
    uint32_t magnitude = (uint32_t)(delta_seconds < 0
        ? -(int32_t)delta_seconds : delta_seconds);
    uint32_t delta_frames = (uint32_t)(((uint64_t)magnitude * player->fps_num)
                                        / player->fps_den);
    uint32_t base = player->has_presented ? player->last_frame_presented : 0;

    if (delta_seconds < 0) {
        return player_seek_to_frame(player, delta_frames >= base ? 0 : base - delta_frames);
    } else {
        uint64_t target = (uint64_t)base + delta_frames;

        return player_seek_to_frame(player, target >= player->frame_count
            ? player->frame_count - 1 : (uint32_t)target);
    }
}

static void render_frame(player_v2_t *player, frame_slot_t *slot)
{
    clock_t decode_start = clock();

    /* No gfx_SetDrawBuffer() here: per GraphX's own documentation,
     * "makes graphics routines act on the non-visible buffer" is a
     * persistent mode, not reset by gfx_SwapDraw() -- it only needs to
     * be set once, which player_v2_run() already does during setup.
     *
     * There is no unpack/decode step at all: slot's buffer already IS
     * the sprite gfx_ScaledSprite_NoClip draws from (see frame_slot_t),
     * frame bytes landed there straight from the USB read. The 2x
     * scale-up is GraphX's own library routine, the same one Cinema's
     * v1 (legacy) player uses successfully at this exact scale factor. */
    gfx_ScaledSprite_NoClip(slot_sprite(slot), 0, V2_Y_OFFSET, 2, 2);

    /* "decode" is a bit of a misnomer now (there's nothing left to
     * decode) -- this measures the blit alone, not the OSD or the swap,
     * so it's still the number to watch for GraphX-scaling cost
     * specifically, separate from USB read time. */
    player->decode_ticks_total += (uint32_t)(clock() - decode_start);
    player->decode_samples++;

    if (osd_should_draw(player)) {
        draw_osd(player);
    } else if (player->osd_clear_pending > 0) {
        /* Scrub the OSD out of the margin. Runs twice so both swap
         * buffers get cleaned, not just the one in hand. */
        osd_fill_rect(0, V2_OSD_TOP, GFX_LCD_WIDTH, V2_OSD_ROWS, player->osd_bg);
        player->osd_clear_pending--;
    }

    /* No gfx_Wait() here, deliberately: graphx.h's own documentation
     * says gfx_SwapDraw() does not block -- instead "the next invocation
     * of a graphx drawing function will block... waiting for this
     * event", and explicitly recommends scheduling non-drawing logic
     * (for us: usb_HandleEvents()/refill_empty_slots() back in
     * player_v2_loop) in the gap where a drawing call would otherwise
     * block, rather than an explicit gfx_Wait() that just burns that
     * same window doing nothing. The next frame's first draw call
     * (gfx_ScaledSprite_NoClip, at the top of the next render_frame) still
     * waits correctly if the LCD genuinely hasn't caught up yet -- this
     * only removes the case where we blocked for no reason while a
     * background USB read could have been making progress instead. */
    gfx_SwapDraw();

    player->fps_window_frames++;
    {
        uint32_t window = (uint32_t)(clock() - player->fps_window_start);

        if (window >= (uint32_t)CLOCKS_PER_SEC) {
            player->fps_tenths = (uint32_t)(((uint64_t)player->fps_window_frames
                                              * 10u * CLOCKS_PER_SEC) / window);
            player->fps_window_frames = 0;
            player->fps_window_start = clock();
        }
    }
}

static void save_resume_state(const player_v2_t *player)
{
    uint8_t var;

    if (!player->has_presented) {
        return;
    }

    var = ti_Open(APPVAR_V2, "w");
    if (var) {
        uint8_t raw[CIN2_RESUME_BYTES];
        cin2_resume_t state;

        state.frame_count = player->frame_count;
        state.last_presented_frame = player->last_frame_presented;
        memcpy(state.filename, player->filename, sizeof(state.filename));
        cin2_build_resume_record(raw, &state);

        ti_SetGCBehavior(NULL, NULL);
        ti_SetArchiveStatus(0, var);
        ti_Write(raw, 1, CIN2_RESUME_BYTES, var);
        ti_SetArchiveStatus(1, var);
        ti_Close(var);
    }
}

static void print_playback_summary(const player_v2_t *player)
{
    char buffer[64];

    sprintf(buffer, "frames shown: %lu", (unsigned long)(player->has_presented
        ? player->last_frame_presented + 1 : 0));
    putstr(buffer);
    sprintf(buffer, "dropped: %lu  repeated: %lu",
            (unsigned long)player->dropped_frames,
            (unsigned long)player->repeated_frames);
    putstr(buffer);

    /* Decode cost is the number to watch when tuning playback speed: it
     * is the per-frame CPU work the player itself controls, separate
     * from however long the USB reads take. */
    if (player->decode_samples > 0) {
        uint32_t avg_ticks = player->decode_ticks_total / player->decode_samples;
        uint32_t avg_us = (uint32_t)(((uint64_t)avg_ticks * 1000000u)
                                      / CLOCKS_PER_SEC);

        sprintf(buffer, "decode avg: %lu.%03lu ms (%lu frames)",
                (unsigned long)(avg_us / 1000u),
                (unsigned long)(avg_us % 1000u),
                (unsigned long)player->decode_samples);
        putstr(buffer);
        if (avg_us > 0) {
            sprintf(buffer, "decode ceiling: ~%lu fps",
                    (unsigned long)(1000000u / avg_us));
            putstr(buffer);
        }
    }
}

/* True once the last frame of the movie has been presented. */
static bool playback_finished(const player_v2_t *player)
{
    return player->has_presented
        && player->last_frame_presented + 1 >= player->frame_count;
}

static bool player_v2_loop(player_v2_t *player)
{
    while (true) {
        uint8_t key;

        usb_HandleEvents();

        if (player->global->usb == NULL) {
            putstr("usb device disconnected");
            return false;
        }

        {
            frame_slot_t *failed = find_failed_slot(player);
            if (failed != NULL) {
                put_msd_error(failed->error, "frame read");
                return false;
            }
        }

        if (!refill_empty_slots(player)) {
            putstr("error queueing msd (frame)");
            return false;
        }

        key = os_GetCSC();
        if (key != 0) {
            /* Any control press wakes the OSD for a few seconds, the way
             * a normal video player surfaces its scrubber on input. */
            osd_poke(player);
        }

        switch (key) {
            case sk_Clear:
                return true;

            case sk_2nd:
            case sk_Enter:
                if (!player->paused) {
                    player->paused = true;
                    player->pause_tick = clock();
                } else {
                    player->paused = false;
                    player->accumulated_pause_ticks +=
                        (clock_t)(clock() - player->pause_tick);
                }
                break;

            case sk_Left:
                if (!player_seek_seconds(player, -V2_SEEK_SMALL)) {
                    return false;
                }
                break;
            case sk_Right:
                if (!player_seek_seconds(player, V2_SEEK_SMALL)) {
                    return false;
                }
                break;
            case sk_Down:
                if (!player_seek_seconds(player, -V2_SEEK_LARGE)) {
                    return false;
                }
                break;
            case sk_Up:
                if (!player_seek_seconds(player, V2_SEEK_LARGE)) {
                    return false;
                }
                break;

            case sk_0:
                if (!player_seek_to_frame(player, 0)) {
                    return false;
                }
                break;

            case sk_Mode:
                player->osd_pinned = !player->osd_pinned;
                if (!player->osd_pinned) {
                    /* Clear it out of both swap buffers, and don't let
                     * the keypress-linger immediately redraw it. */
                    player->osd_clear_pending = 2;
                    player->osd_until_tick = clock();
                }
                break;

            default:
                break;
        }

        if (player->paused) {
            /* The correct frame is already on the visible screen, so
             * rather than re-blitting it (the presented slot has already
             * been released, and swapping buffers here would flicker
             * between two different frames), overlay the OSD straight
             * onto the visible screen and don't swap at all. Only redraw
             * on an actual control press -- otherwise a pause would spin
             * this every iteration for no visible change. */
            if (key != 0) {
                gfx_SetDrawScreen();
                if (osd_should_draw(player)) {
                    draw_osd(player);
                } else {
                    osd_fill_rect(0, V2_OSD_TOP, GFX_LCD_WIDTH, V2_OSD_ROWS,
                                   player->osd_bg);
                }
                gfx_SetDrawBuffer();
            }
            continue;
        }

        {
            uint32_t wanted = desired_frame(player, clock());
            frame_slot_t *slot;

            discard_obsolete_frames(player, wanted);
            slot = find_ready_frame(player, wanted);

            if (slot != NULL) {
                render_frame(player, slot);
                player->has_presented = true;
                player->last_frame_presented = slot->frame_number;
                slot->state = SLOT_EMPTY;

                if (playback_finished(player)) {
                    return true;
                }
            } else if (player->has_presented
                       && wanted > player->last_frame_presented) {
                /* Wanted frame isn't ready yet -- hold the currently
                 * displayed frame rather than show nothing. */
                player->repeated_frames++;
            }
        }
    }
}

bool player_v2_run(global_t *global, const cin2_header_t *header,
                    uint32_t start_frame, const fat32ro_extent_map_t *movie_map,
                    const char *filename)
{
    static player_v2_t player;
    bool graphics_active = false;
    bool ok;

    if (header->frame_count == 0) {
        putstr("movie has no frames");
        return false;
    }

    memset(&player, 0, sizeof(player));
    player.global = global;
    player.movie_map = movie_map;
    {
        size_t name_len = strlen(filename);

        if (name_len >= sizeof(player.filename)) {
            name_len = sizeof(player.filename) - 1;
        }
        memcpy(player.filename, filename, name_len);
        player.filename[name_len] = '\0';
    }
    player.frame_count = header->frame_count;
    player.fps_num = header->fps_num;
    player.fps_den = header->fps_den;
    player.start_frame = start_frame;
    player.next_frame_to_queue = start_frame;
    choose_osd_colors(&player, header);
    {
        uint8_t i;

        for (i = 0; i < SLOT_COUNT; ++i) {
            gfx_sprite_t *sprite = slot_sprite(&player.slots[i]);

            sprite->width = CINEMA_V2_WIDTH;
            sprite->height = CINEMA_V2_HEIGHT;
        }
    }

    /* Prefill runs here, before gfx_Begin() -- it doesn't touch graphics
     * at all, and keeping it out of graphics mode means a prefill error
     * (e.g. a read failure) shows as a plain text message instead of
     * being immediately replaced by a graphics-mode screen. */
    ok = prefill_frames(&player);

    if (ok) {
        gfx_Begin();
        graphics_active = true;
        gfx_SetPalette(header->palette, sizeof(header->palette), 0);
        gfx_SwapDraw();
        gfx_SetDrawBuffer();
        gfx_ZeroScreen();
        gfx_SwapDraw();
        gfx_SetDrawBuffer();
        gfx_ZeroScreen();

        player.start_tick = clock();
        player.fps_window_start = player.start_tick;
        /* Surface the controls/scrubber briefly on start, the way a
         * video player does, then let it auto-hide. */
        osd_poke(&player);
        ok = player_v2_loop(&player);
    }

    if (graphics_active) {
        gfx_End();
    }

    print_playback_summary(&player);

    if (ok) {
        save_resume_state(&player);
    }

    return ok;
}
