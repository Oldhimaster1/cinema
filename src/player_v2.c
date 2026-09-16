#include "player_v2.h"
static player_v2_result_t g_player_v2_result=PLAYER_V2_INVALID;
player_v2_result_t player_v2_last_result(void){return g_player_v2_result;}
#include "fat32ro.h"
#include "msd_util.h"
#include "render_v2.h"
#include "cinema_subtitle.h"

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
#define PRIMARY_SLOT_COUNT 2
#define MAX_SLOT_COUNT 3

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

/* How long the wanted frame can go unavailable before showing
 * "Buffering...": long enough that ordinary frame-to-frame jitter never
 * triggers it, short enough that a genuine USB-throughput stall (see
 * README's Performance section) is reported quickly rather than just
 * looking like a frozen picture. */
#define V2_BUFFERING_THRESHOLD_TICKS (CLOCKS_PER_SEC / 2)

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
    uint8_t *sprite_data;
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
    uint8_t frame_span;
    uint8_t frames_consumed;
    uint8_t present_index;
    struct player_v2_t *owner;
    clock_t read_submit_tick;
} frame_slot_t;

static gfx_sprite_t *slot_sprite(frame_slot_t *slot)
{
    return (gfx_sprite_t *)slot->sprite_data;
}

static uint8_t *g_packed_extra_storage;
static uint32_t g_packed_extra_storage_size;

void player_v2_set_packed_extra_storage(uint8_t *storage, uint32_t size)
{
    g_packed_extra_storage = storage;
    g_packed_extra_storage_size = size;
}

typedef struct player_v2_t {
    global_t *global;
    const fat32ro_extent_map_t *movie_map;
    frame_slot_t slots[MAX_SLOT_COUNT];
    uint8_t primary_storage[PRIMARY_SLOT_COUNT][2 + CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT];
    uint8_t slot_count;

    char filename[CIN2_RESUME_FILENAME_LEN]; /* "" for raw single-image mode */

    uint32_t next_frame_to_queue;
    uint32_t frame_count;
    uint32_t start_frame;
    bool has_presented;
    uint32_t last_frame_presented;

    uint32_t fps_num;
    uint32_t fps_den;
    uint8_t format_flags;
    uint8_t frame_sectors;

    clock_t start_tick;
    clock_t pause_tick;
    clock_t accumulated_pause_ticks;
    bool paused;
    bool loop_enabled;       /* toggled with [graph]; restarts from frame 0 at the end instead of stopping */
    bool pause_after_render; /* one-shot: re-pause right after the next frame shows (frame-step) */

    uint32_t dropped_frames;
    uint32_t repeated_frames;
    uint32_t session_frames_presented;
    uint32_t max_schedule_lag;
    clock_t playback_start_tick;

    /* --- stall / buffering feedback --- */
    clock_t last_progress_tick; /* clock() as of the last frame actually shown */
    bool buffering_shown;       /* is the "Buffering..." overlay currently up? */

    /* --- OSD / controls --- */
    bool osd_pinned;          /* toggled on with [mode], stays until toggled off */
    clock_t osd_until_tick;   /* transient show-after-keypress deadline */
    bool osd_was_visible;     /* was the OSD drawn on the previous frame? */
    uint8_t osd_clear_pending; /* frames left to scrub the OSD out of both buffers */
    uint8_t osd_fg;           /* brightest palette index, chosen at startup */
    uint8_t osd_bg;           /* darkest palette index */

    /* --- telemetry, reported on exit and in the OSD --- */
    uint32_t decode_ticks_total;
    uint32_t decode_samples;
    uint32_t fps_window_frames;
    clock_t fps_window_start;
    uint32_t fps_tenths;      /* measured presentation rate x10 */
    uint32_t read_ticks_total;
    uint32_t read_ticks_max;
    uint32_t read_sectors_total;
    uint32_t read_completions;
    uint32_t read_submissions;
    uint32_t reads_over_111ms;
    uint32_t reads_over_250ms;
    uint32_t reads_over_500ms;
    uint32_t reads_over_1000ms;
    uint32_t direct_map_hits;
    uint32_t fragmented_frame_resolves;
    uint32_t packed_pair_commands;
    uint32_t packed_single_commands;
    uint32_t max_concurrent_reads;
    uint8_t active_reads;

    const fat32ro_extent_map_t *subtitle_map;
    uint32_t subtitle_size;
    uint32_t subtitle_movie_id;
    csu_header_t subtitle_header;
    uint8_t *subtitle_sector;
    uint32_t subtitle_cached_sector;
    bool subtitle_sector_valid;
    csu_cue_t subtitle_cue;
    int32_t subtitle_cue_index;
    bool subtitle_cue_valid;
    bool subtitle_end_reached;
    bool subtitle_available;
    bool subtitles_enabled;
    uint32_t subtitle_sector_reads;
    uint32_t subtitle_runtime_validations;
    int32_t subtitle_failure_index;
    uint8_t subtitle_failure_reason;
    bool menu_back_buffer_repair;
    /* Validation-only read-ahead. This borrows the static packed/thumbnail
     * scratch buffer before playback slots begin using it, so Phase 1 adds no
     * stack pressure and no permanent BSS allocation. */
    uint8_t *subtitle_bulk_buffer;
    uint32_t subtitle_bulk_capacity;
    uint32_t subtitle_bulk_first_sector;
    uint32_t subtitle_bulk_sector_count;
    bool subtitle_validation_mode;
    uint32_t subtitle_validation_commands;
    uint32_t subtitle_validation_sectors;
    uint32_t subtitle_validation_max_sectors;
    uint32_t subtitle_validation_extent_stops;
    clock_t subtitle_validation_ticks;
    int32_t subtitle_delay_ms;
    uint8_t subtitle_style;
    uint8_t subtitle_size_mode;
    uint8_t subtitle_position;
} player_v2_t;

/* Callback only records what happened -- no graphics calls, no printing,
 * no LBA math, and (per the comment on pending_parts above) no queueing
 * of the next part either. The main loop decides what any of it means. */
static void frame_read_callback(msd_error_t error, struct msd_transfer *xfer)
{
    frame_slot_t *slot = (frame_slot_t *)xfer->userptr;

    if (slot->owner != NULL) {
        uint32_t elapsed = (uint32_t)(clock() - slot->read_submit_tick);
        slot->owner->read_ticks_total += elapsed;
        if (elapsed > slot->owner->read_ticks_max) slot->owner->read_ticks_max = elapsed;
        if ((uint64_t)elapsed * 1000u > (uint64_t)CLOCKS_PER_SEC * 111u) slot->owner->reads_over_111ms++;
        if ((uint64_t)elapsed * 1000u > (uint64_t)CLOCKS_PER_SEC * 250u) slot->owner->reads_over_250ms++;
        if ((uint64_t)elapsed * 1000u > (uint64_t)CLOCKS_PER_SEC * 500u) slot->owner->reads_over_500ms++;
        if ((uint64_t)elapsed * 1000u > (uint64_t)CLOCKS_PER_SEC * 1000u) slot->owner->reads_over_1000ms++;
        slot->owner->read_completions++;
        slot->owner->read_sectors_total += xfer->count;
        if (slot->owner->active_reads > 0) slot->owner->active_reads--;
    }
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
    uint32_t sector_offset = cin2_frame_lba_for(frame_number, slot->owner->format_flags);
    uint32_t remaining = (uint32_t)slot->owner->frame_sectors * slot->frame_span;
    uint8_t count = 0;

    /* Most prepared/cached movies are one physical extent. Avoid the generic
     * 256-entry lookup loop and part builder for this hot per-frame case. */
    if (map->extent_count == 1 && sector_offset < map->extents[0].sectors
        && remaining <= map->extents[0].sectors - sector_offset) {
        slot->pending_parts[0].lba = map->extents[0].lba + sector_offset;
        slot->pending_parts[0].sectors = remaining;
        slot->pending_part_count = 1;
        slot->next_pending_part = 0;
        if (slot->owner != NULL) slot->owner->direct_map_hits++;
        return true;
    }
    if (slot->owner != NULL) slot->owner->fragmented_frame_resolves++;

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
    slot->read_submit_tick = clock();
    if (slot->owner != NULL) {
        slot->owner->read_submissions++;
        slot->owner->active_reads++;
        if (slot->owner->active_reads > slot->owner->max_concurrent_reads)
            slot->owner->max_concurrent_reads = slot->owner->active_reads;
    }
    result = msd_ReadAsync(&slot->transfer);
    if (result != MSD_SUCCESS) {
        if (slot->owner != NULL && slot->owner->active_reads > 0) slot->owner->active_reads--;
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
    slot->frames_consumed = 0;
    slot->present_index = 0;
    slot->frame_span = ((slot->owner->format_flags & CIN2_FLAG_PACKED4)
        && frame_number + 1u < slot->owner->frame_count) ? 2u : 1u;

    if (!resolve_frame_parts(map, frame_number, slot)) {
        /* A fragmented extent boundary may make the two-frame request exceed
         * the bounded part list. Fall back safely to one packed frame. */
        if (slot->frame_span == 2u) {
            slot->frame_span = 1u;
            if (resolve_frame_parts(map, frame_number, slot)) {
                slot->owner->packed_single_commands++;
                return queue_slot_part(global, slot);
            }
        }
        slot->error = MSD_ERROR_INVALID_PARAM;
        slot->state = SLOT_ERROR;
        return MSD_ERROR_INVALID_PARAM;
    }

    if (slot->owner->format_flags & CIN2_FLAG_PACKED4) {
        if (slot->frame_span == 2u) slot->owner->packed_pair_commands++;
        else slot->owner->packed_single_commands++;
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
static bool any_slot_loading(const player_v2_t *player)
{
    uint8_t i;
    for (i = 0; i < player->slot_count; ++i)
        if (player->slots[i].state == SLOT_LOADING) return true;
    return false;
}

static bool refill_empty_slots(player_v2_t *player)
{
    uint8_t i;

    /* msddrvce is most stable and fastest with one bulk command in flight.
     * The old loop could keep both 30-sector slot transfers outstanding,
     * which physical Cars telemetry showed collapsing sustained throughput.
     * Double buffering is retained: one READY frame can render while one
     * transfer progresses, but never two competing USB commands. */
    if (any_slot_loading(player)) return true;

    for (i = 0; i < player->slot_count; ++i) {
        frame_slot_t *slot = &player->slots[i];

        if (slot->state == SLOT_NEEDS_NEXT_PART) {
            if (queue_slot_part(player->global, slot) != MSD_SUCCESS) {
                return false;
            }
            usb_HandleEvents();
            return true;
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

        player->next_frame_to_queue += slot->frame_span;
        return true;
    }

    return true;
}

static frame_slot_t *find_failed_slot(player_v2_t *player)
{
    uint8_t i;

    for (i = 0; i < player->slot_count; ++i) {
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
                                   uint32_t frame_number, const char *context,
                                   bool allow_user_cancel)
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
        /* Initial prefill may still be canceled by a key. A seek refill must
         * not poll the keyboard here: the key that requested the seek can
         * legitimately remain held while the synchronous refill runs. Treating
         * that same held key as cancellation made player_seek_to_frame return
         * false, which the playback loop classified as fatal and followed with
         * the normal post-playback diagnostics pages. */
        if (allow_user_cancel && os_GetCSC()) {
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
static bool serialized_fill_slots(player_v2_t *player, const char *context,
                                    bool allow_user_cancel)
{
    uint8_t i;

    for (i = 0; i < player->slot_count; ++i) {
        frame_slot_t *slot = &player->slots[i];

        if (slot->state == SLOT_ERROR) {
            continue; /* left for the main loop to observe/report */
        }
        if (player->next_frame_to_queue >= player->frame_count) {
            break;
        }

        if (!serialized_fill_slot(player, slot, player->next_frame_to_queue,
                                  context, allow_user_cancel)) {
            return false;
        }
        player->next_frame_to_queue += slot->frame_span;
    }

    return true;
}

/* Fills every slot and blocks until each either finishes or errors, so
 * playback starts with a full read-ahead buffer instead of the single
 * frame the original player waited for. */
static bool prefill_frames(player_v2_t *player)
{
    return serialized_fill_slots(player, "prefill", true);
}

static frame_slot_t *find_ready_frame(player_v2_t *player, uint32_t wanted)
{
    uint8_t i;

    for (i = 0; i < player->slot_count; ++i) {
        frame_slot_t *slot = &player->slots[i];

        if (slot->state == SLOT_READY
            && wanted >= slot->frame_number + slot->frames_consumed
            && wanted < slot->frame_number + slot->frame_span) {
            slot->present_index = (uint8_t)(wanted - slot->frame_number);
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
        sprintf(line, "PAUSED  %s/%s%s  B%u", position, total,
                player->loop_enabled ? "  LOOP" : "",
                (unsigned)boot_GetBatteryStatus());
    } else {
        uint32_t decode_ms = player->decode_samples
            ? (uint32_t)(((uint64_t)player->decode_ticks_total * 1000u)
                          / ((uint64_t)player->decode_samples * CLOCKS_PER_SEC))
            : 0u;

        sprintf(line, "%s/%s  %lu.%luFPS  DEC%lums  DR%lu%s  B%u",
                position, total,
                (unsigned long)(player->fps_tenths / 10u),
                (unsigned long)(player->fps_tenths % 10u),
                (unsigned long)decode_ms,
                (unsigned long)player->dropped_frames,
                player->loop_enabled ? "  LOOP" : "",
                (unsigned)boot_GetBatteryStatus());
    }

    gfx_SetTextFGColor(player->osd_fg);
    gfx_SetTextBGColor(player->osd_bg);
    gfx_PrintStringXY(line, 4, V2_OSD_TEXT_Y);
}

/* Draws "Buffering..." straight onto the visible screen, the same
 * gfx_SetDrawScreen()/gfx_SetDrawBuffer() trick the paused-OSD path
 * uses: there's no freshly rendered frame to swap in while stalled (by
 * definition -- that's what "stalled" means here), so this has to paint
 * over whatever's already showing rather than going through the normal
 * offscreen-buffer-then-swap pipeline. Called only when the wanted
 * frame has been unavailable past V2_BUFFERING_THRESHOLD_TICKS -- see
 * player_v2_loop. Clearing it back out when a frame becomes ready again
 * is handled by forcing osd_clear_pending, reusing the exact mechanism
 * render_frame() already uses to scrub the OSD out of both swap
 * buffers. */
static void show_buffering_overlay(player_v2_t *player)
{
    gfx_SetDrawScreen();
    osd_fill_rect(0, V2_OSD_TOP, GFX_LCD_WIDTH, V2_OSD_ROWS, player->osd_bg);
    gfx_SetTextFGColor(player->osd_fg);
    gfx_SetTextBGColor(player->osd_bg);
    gfx_PrintStringXY("Buffering...", 4, V2_OSD_TEXT_Y);
    gfx_SetDrawBuffer();
}

/* Waits until no slot has a transfer in flight. Required before reusing
 * slot buffers on a seek: msd_ReadAsync owns slot->sprite_data until its
 * callback fires, and there is no cancel in the msddrvce API. */
static void drain_loading_slots(player_v2_t *player)
{
    while (true) {
        bool any_loading = false;
        uint8_t i;

        for (i = 0; i < player->slot_count; ++i) {
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
static void subtitle_invalidate_cue(player_v2_t *player);

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

    for (i = 0; i < player->slot_count; ++i) {
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
    player->last_progress_tick = player->start_tick;
    player->accumulated_pause_ticks = 0;
    subtitle_invalidate_cue(player);
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
    return serialized_fill_slots(player, "seek", false);
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


#define SUBTITLE_BULK_SECTORS 16u
#define SUBTITLE_BULK_BYTES (SUBTITLE_BULK_SECTORS * 512u)

static bool subtitle_stream_read_single_sector(void *ctx,uint32_t offset,uint8_t *out,size_t size)
{
    player_v2_t *player=(player_v2_t *)ctx;
    if(!player||!player->subtitle_sector)return false;
    while(size){
        uint32_t sector=offset/512u,in=offset%512u,lba,run;
        size_t take=512u-in;
        if(take>size)take=size;

        if(player->subtitle_validation_mode&&player->subtitle_bulk_buffer
           &&player->subtitle_bulk_capacity>=512u){
            bool hit=player->subtitle_bulk_sector_count!=0u
                &&sector>=player->subtitle_bulk_first_sector
                &&sector-player->subtitle_bulk_first_sector<player->subtitle_bulk_sector_count;
            if(!hit){
                uint32_t total_sectors=(player->subtitle_size+511u)/512u;
                uint32_t count;
                if(player->global->usb==NULL
                   ||!fat32ro_extent_lookup(player->subtitle_map,sector,&lba,&run))return false;
                count=player->subtitle_bulk_capacity/512u;
                if(count>SUBTITLE_BULK_SECTORS)count=SUBTITLE_BULK_SECTORS;
                if(count>run){count=run;player->subtitle_validation_extent_stops++;}
                if(count>total_sectors-sector)count=total_sectors-sector;
                if(count==0u||msd_Read(&player->global->msd,lba,count,
                                       player->subtitle_bulk_buffer)!=count)return false;
                player->subtitle_bulk_first_sector=sector;
                player->subtitle_bulk_sector_count=count;
                player->subtitle_validation_commands++;
                player->subtitle_validation_sectors+=count;
                if(count>player->subtitle_validation_max_sectors)
                    player->subtitle_validation_max_sectors=count;
            }
            {
                uint32_t cached=sector-player->subtitle_bulk_first_sector;
                memcpy(out,player->subtitle_bulk_buffer+cached*512u+in,take);
            }
        }else{
            if(!player->subtitle_sector_valid||player->subtitle_cached_sector!=sector){
                /* Never submit a synchronous sidecar command on top of an async movie command. */
                drain_loading_slots(player);
                if(player->global->usb==NULL
                   ||!fat32ro_extent_lookup(player->subtitle_map,sector,&lba,&run)
                   ||msd_Read(&player->global->msd,lba,1,player->subtitle_sector)!=1)return false;
                player->subtitle_cached_sector=sector;player->subtitle_sector_valid=true;
                player->subtitle_sector_reads++;
            }
            memcpy(out,player->subtitle_sector+in,take);
        }
        out+=take;offset+=(uint32_t)take;size-=take;
    }
    return true;
}

/* CINEMA_SUBTITLE_V15_CROSS_SECTOR
 * Arbitrary CSU byte reads are split at physical 512-byte boundaries.
 * Each chunk is delegated to the original cache-aware one-sector reader,
 * preserving extent mapping, cache reuse, serialized USB access, and the
 * existing subtitle_sector_reads counter. */
#ifdef CINEMA_DIAGNOSTIC
static uint32_t g_subtitle_read_requests;
static uint32_t g_subtitle_cross_sector_requests;
static uint32_t g_subtitle_read_bytes;
static uint32_t g_subtitle_draw_calls;
static uint32_t g_subtitle_active_frames;
static uint32_t g_subtitle_toggle_count;
static uint8_t g_subtitle_open_stage;
#endif

static bool subtitle_stream_read(void *ctx, uint32_t offset,
                                 uint8_t *out, size_t size)
{
    size_t remaining = size;
#ifdef CINEMA_DIAGNOSTIC
    g_subtitle_read_requests++;
    g_subtitle_read_bytes += (uint32_t)size;
    if (size > 0u && (offset / 512u) != ((offset + (uint32_t)size - 1u) / 512u)) {
        g_subtitle_cross_sector_requests++;
    }
#endif
    while (remaining > 0u) {
        uint32_t within = offset & 511u;
        size_t chunk = 512u - (size_t)within;
        if (chunk > remaining) {
            chunk = remaining;
        }
        if (!subtitle_stream_read_single_sector(ctx, offset, out, chunk)) {
            return false;
        }
        offset += (uint32_t)chunk;
        out += chunk;
        remaining -= chunk;
    }
    return true;
}

static void subtitle_invalidate_cue(player_v2_t *player)
{
    player->subtitle_cue_valid=false;
    player->subtitle_cue_index=-1;
    player->subtitle_end_reached=false;
}

enum { SUB_FAIL_NONE=0, SUB_FAIL_READ=1, SUB_FAIL_BOUNDS=2, SUB_FAIL_ORDER=3 };
static void subtitle_fail(player_v2_t *player,int32_t index,uint8_t reason)
{
    player->subtitle_failure_index=index;
    player->subtitle_failure_reason=reason;
    player->subtitle_available=false;
    player->subtitles_enabled=false;
    subtitle_invalidate_cue(player);
}
static bool subtitle_read_validated_cue(player_v2_t *player,uint32_t index,
                                        const csu_cue_t *previous,csu_cue_t *out)
{
    if(index>=player->subtitle_header.cue_count
       ||!csu_stream_read_cue(out,subtitle_stream_read,player,index)){
        subtitle_fail(player,(int32_t)index,SUB_FAIL_READ);return false;
    }
    player->subtitle_runtime_validations++;
    if(out->end_frame>player->frame_count){
        subtitle_fail(player,(int32_t)index,SUB_FAIL_BOUNDS);return false;
    }
    if(previous!=NULL&&out->start_frame<previous->end_frame){
        subtitle_fail(player,(int32_t)index,SUB_FAIL_ORDER);return false;
    }
    if(previous==NULL&&index>0u){
        csu_cue_t prev;
        if(!csu_stream_read_cue(&prev,subtitle_stream_read,player,index-1u)){
            subtitle_fail(player,(int32_t)index,SUB_FAIL_READ);return false;
        }
        player->subtitle_runtime_validations++;
        if(prev.end_frame>player->frame_count||out->start_frame<prev.end_frame){
            subtitle_fail(player,(int32_t)index,SUB_FAIL_ORDER);return false;
        }
    }
    return true;
}

static bool subtitle_locate(player_v2_t *player,uint32_t frame)
{
    int32_t i;
    if(!player->subtitle_available||player->subtitle_end_reached)return false;

    if(player->subtitle_cue_valid){
        /* A future cue is useful cached state.  Every frame in the gap before
         * it must return without touching USB. */
        if(frame<player->subtitle_cue.start_frame)return false;
        if(frame<player->subtitle_cue.end_frame)return true;

        /* Sequential presentation normally advances one cue at a time.  Keep
         * walking only if a frame jump crossed multiple short cues. */
        while(player->subtitle_cue_index+1<(int32_t)player->subtitle_header.cue_count){
            csu_cue_t next;
            if(!subtitle_read_validated_cue(player,
                    (uint32_t)(player->subtitle_cue_index+1),&player->subtitle_cue,&next))
                return false;
            player->subtitle_cue=next;
            player->subtitle_cue_index++;
            if(frame<next.start_frame)return false;
            if(frame<next.end_frame)return true;
        }
        player->subtitle_cue_valid=false;
        player->subtitle_end_reached=true;
        return false;
    }

    /* Initial playback and post-seek lookup: find either the active cue or
     * the next future cue.  Caching a future cue eliminates repeated binary
     * searches throughout subtitle-free intervals. */
    i=csu_stream_find_at_or_after(&player->subtitle_header,
                                  subtitle_stream_read,player,frame);
    if(i==-1){
        player->subtitle_end_reached=true;
        return false;
    }
    if(i<0||!subtitle_read_validated_cue(player,(uint32_t)i,NULL,
                                          &player->subtitle_cue)) return false;
    player->subtitle_cue_index=i;
    player->subtitle_cue_valid=true;
    return frame>=player->subtitle_cue.start_frame
        &&frame<player->subtitle_cue.end_frame;
}
#define SUB_STYLE_BOX 0u
#define SUB_STYLE_COMPACT 1u
#define SUB_STYLE_TEXT 2u
#define SUB_SIZE_NORMAL 0u
#define SUB_SIZE_TIGHT 1u
#define SUB_POS_BOTTOM 0u
#define SUB_POS_TOP 1u

static uint32_t subtitle_adjusted_frame(const player_v2_t *player,uint32_t frame)
{
    int64_t delta=(int64_t)player->subtitle_delay_ms*player->fps_num;
    int64_t divisor=(int64_t)1000*player->fps_den;
    int64_t adjusted;
    if(delta>=0)delta=(delta+divisor/2)/divisor;
    else delta=-((-delta+divisor/2)/divisor);
    /* Positive delay means show text later, so inspect an earlier subtitle time. */
    adjusted=(int64_t)frame-delta;
    if(adjusted<0)return 0;
    if(adjusted>=(int64_t)player->frame_count)return player->frame_count-1;
    return (uint32_t)adjusted;
}

static void draw_centered_subtitle_line(player_v2_t *player,const char *text,uint8_t y)
{
    uint24_t width=(uint24_t)strlen(text)*8u;
    uint24_t x=width<GFX_LCD_WIDTH?(GFX_LCD_WIDTH-width)/2:0;
    uint8_t pad=player->subtitle_size_mode==SUB_SIZE_TIGHT?1u:2u;
    uint24_t box_x=x>pad?x-pad:0;
    uint24_t box_w=width+2u*pad;
    if(box_w>GFX_LCD_WIDTH)box_w=GFX_LCD_WIDTH;
    if(player->subtitle_style!=SUB_STYLE_TEXT){
        gfx_SetColor(player->osd_bg);
        gfx_FillRectangle_NoClip(box_x,y>pad?y-pad:0,box_w,8u+2u*pad);
    }else{
        /* One cheap shadow draw gives legibility without a full box. */
        gfx_SetTextFGColor(player->osd_bg);
        gfx_PrintStringXY(text,x+1u,y+1u);
    }
    gfx_SetTextFGColor(player->osd_fg);
    gfx_PrintStringXY(text,x,y);
}

static void draw_subtitle(player_v2_t *player,uint32_t frame)
{
#ifdef CINEMA_DIAGNOSTIC
    g_subtitle_draw_calls++;
#endif
    const csu_cue_t *cue;
    uint32_t lookup;
    uint8_t y1,y2;
    if(!player->subtitle_available||!player->subtitles_enabled)return;
    lookup=subtitle_adjusted_frame(player,frame);
    if(!subtitle_locate(player,lookup))return;
    cue=&player->subtitle_cue;
    if(player->subtitle_position==SUB_POS_TOP){y1=V2_Y_OFFSET+4u;y2=V2_Y_OFFSET+14u;}
    else {y1=V2_Y_OFFSET+CINEMA_V2_DEST_HEIGHT-22u;y2=V2_Y_OFFSET+CINEMA_V2_DEST_HEIGHT-12u;}
    if(player->subtitle_size_mode==SUB_SIZE_TIGHT){if(player->subtitle_position==SUB_POS_TOP)y2=y1+9u;else y1=y2-9u;}
    if(cue->line_count==2){
#ifdef CINEMA_DIAGNOSTIC
        g_subtitle_active_frames++;
#endif
        draw_centered_subtitle_line(player,cue->line1,y1);
        draw_centered_subtitle_line(player,cue->line2,y2);
    }else draw_centered_subtitle_line(player,cue->line1,y2);
}

static const char *subtitle_style_name(uint8_t v)
{return v==SUB_STYLE_COMPACT?"COMPACT":v==SUB_STYLE_TEXT?"TEXT":"BOX";}
static const char *subtitle_spacing_name(uint8_t v)
{return v==SUB_SIZE_TIGHT?"TIGHT":"NORMAL";}
static const char *subtitle_position_name(uint8_t v)
{return v==SUB_POS_TOP?"TOP":"BOTTOM";}

static void subtitle_options_draw(player_v2_t *player,uint8_t row)
{
    char line[40];
    gfx_SetDrawScreen();gfx_SetColor(player->osd_bg);gfx_FillRectangle_NoClip(0,0,GFX_LCD_WIDTH,GFX_LCD_HEIGHT);gfx_SetTextFGColor(player->osd_fg);
    gfx_PrintStringXY("SUBTITLE OPTIONS",8,8);
    snprintf(line,sizeof(line),"%c Delay: %+ld ms",row==0?'>':' ',(long)player->subtitle_delay_ms);gfx_PrintStringXY(line,8,30);
    snprintf(line,sizeof(line),"%c Style: %s",row==1?'>':' ',subtitle_style_name(player->subtitle_style));gfx_PrintStringXY(line,8,44);
    snprintf(line,sizeof(line),"%c Spacing: %s",row==2?'>':' ',subtitle_spacing_name(player->subtitle_size_mode));gfx_PrintStringXY(line,8,58);
    snprintf(line,sizeof(line),"%c Position: %s",row==3?'>':' ',subtitle_position_name(player->subtitle_position));gfx_PrintStringXY(line,8,72);
    gfx_PrintStringXY("Up/Down select",8,96);gfx_PrintStringXY("Left/Right change",8,108);
    gfx_PrintStringXY("0 reset  Del exit",8,120);
    gfx_PrintStringXY("Y= toggles subtitles",8,136);
    gfx_PrintStringXY("2nd pauses  Clear exits",8,148);
}

static void subtitle_options_menu(player_v2_t *player)
{
    uint8_t row=0,key;
    bool changed=false;
    drain_loading_slots(player);
    do{
        subtitle_options_draw(player,row);
        do{usb_HandleEvents();key=os_GetCSC();}while(!key&&player->global->usb!=NULL);
        if(player->global->usb==NULL)break;
        if(key==sk_Up&&row)row--;
        else if(key==sk_Down&&row<3)row++;
        else if(key==sk_0){player->subtitle_delay_ms=0;player->subtitle_style=SUB_STYLE_BOX;player->subtitle_size_mode=SUB_SIZE_NORMAL;player->subtitle_position=SUB_POS_BOTTOM;changed=true;}
        else if(key==sk_Left||key==sk_Right){
            int dir=key==sk_Right?1:-1;
            if(row==0){player->subtitle_delay_ms+=dir*100;if(player->subtitle_delay_ms>10000)player->subtitle_delay_ms=10000;if(player->subtitle_delay_ms< -10000)player->subtitle_delay_ms=-10000;}
            else if(row==1)player->subtitle_style=(uint8_t)((player->subtitle_style+3u+dir)%3u);
            else if(row==2)player->subtitle_size_mode^=1u;
            else player->subtitle_position^=1u;
            changed=true;
        }
    }while(key!=sk_Del&&key!=sk_Clear);
    if(changed)subtitle_invalidate_cue(player);
    gfx_SetDrawBuffer();
    player->osd_clear_pending=2;
    player->osd_was_visible=false;
    /* Repaint a real movie frame after the menu, even if playback was paused. */
    if(player->has_presented){
        player->pause_after_render=player->paused;
        player->menu_back_buffer_repair=true;
        player_seek_to_frame(player,player->last_frame_presented);
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
     * the sprite data the renderer draws from (see frame_slot_t), frame
     * bytes landed there straight from the USB read. Which renderer
     * actually does the 2x scale-up is chosen at compile time -- see
     * render_v2.h. Default is GraphX's own library routine (the same
     * one Cinema's v1 (legacy) player uses successfully at this exact
     * scale factor); CINEMA_RENDERER_FIXED_C/_FIXED_ASM are Cinema's own
     * specialized replacements, opt-in via the Makefile. */
#if CINEMA_RENDERER == CINEMA_RENDERER_FIXED_C
    render_scaled_fixed_c(slot_sprite(slot)->data);
#elif CINEMA_RENDERER == CINEMA_RENDERER_FIXED_ASM
    if (player->format_flags & CIN2_FLAG_PACKED4)
        render_scaled_packed4_asm(slot_sprite(slot)->data
            + (uint32_t)slot->present_index * CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT / 2u);
    else
        render_scaled_fixed_asm(slot_sprite(slot)->data);
#else
    render_scaled_graphx((const struct gfx_sprite_t *)slot_sprite(slot));
#endif

    /* "decode" is a bit of a misnomer now (there's nothing left to
     * decode) -- this measures the blit alone, not the OSD or the swap,
     * so it's still the number to watch for GraphX-scaling cost
     * specifically, separate from USB read time. */
    player->decode_ticks_total += (uint32_t)(clock() - decode_start);
    player->decode_samples++;

    draw_subtitle(player, slot->frame_number + slot->present_index);

    {
        bool showing = osd_should_draw(player);

        if (showing) {
            draw_osd(player);
        } else {
            if (player->osd_was_visible) {
                /* Just transitioned from showing to hidden -- either the
                 * keypress linger timer expired naturally, or Mode just
                 * unpinned it (which also forces osd_should_draw() false
                 * immediately, so this one path handles both). Without
                 * this, the OSD's last-drawn content (e.g. a stale
                 * timecode) would just sit there forever: nothing else
                 * ever repaints the letterbox margin once draw_osd()
                 * stops being called. */
                player->osd_clear_pending = 2;
            }
            if (player->osd_clear_pending > 0) {
                /* Scrub the OSD out of the margin. Runs twice so both
                 * swap buffers get cleaned, not just the one in hand. */
                osd_fill_rect(0, V2_OSD_TOP, GFX_LCD_WIDTH, V2_OSD_ROWS, player->osd_bg);
                player->osd_clear_pending--;
            }
        }
        player->osd_was_visible = showing;
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
    if(player->menu_back_buffer_repair){
        /* The swap above makes the freshly restored movie frame visible.
         * Copy that exact completed screen into the other draw buffer so the
         * next swap cannot reveal stale SUBTITLE OPTIONS pixels. */
        gfx_BlitScreen();
        player->menu_back_buffer_repair=false;
    }
    player->session_frames_presented++;

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

static bool save_resume_state(const player_v2_t *player)
{
    uint8_t var;
    uint8_t store[CIN2_RESUME_STORE_BYTES];
    cin2_resume_t state;
    int slot;

    if (!player->has_presented) {
        return false;
    }

    /* Read-modify-write: ti_Write always rewrites the whole appvar, and
     * the store holds several movies' resume records side by side (see
     * cin2.h), so writing this one must not clobber the others. A
     * missing appvar (first run ever) or one shorter than the current
     * store size (an older single-slot build's leftover 33-byte appvar)
     * both leave `store` all-zero, which cin2_resume_store_find/
     * slot_for already treat as "no resume recorded yet" -- so an
     * upgrade from the old format just quietly starts a fresh store
     * rather than misreading it. */
    memset(store, 0, sizeof(store));
    var = ti_Open(APPVAR_V2, "r");
    if (var) {
        ti_Read(store, 1, sizeof(store), var);
        ti_Close(var);
    }

    state.frame_count = player->frame_count;
    state.last_presented_frame = player->last_frame_presented;
    memcpy(state.filename, player->filename, sizeof(state.filename));
    slot = cin2_resume_store_slot_for(store, player->filename);
    cin2_resume_store_write_slot(store, slot, &state);


    var = ti_Open(APPVAR_V2, "w");
    if (var) {
        bool wrote;
        ti_SetArchiveStatus(0, var);
        wrote = ti_Write(store, 1, sizeof(store), var) == sizeof(store);
        if (wrote) ti_SetArchiveStatus(1, var);
        ti_Close(var);
        if (wrote) {
            uint8_t verify[CIN2_RESUME_STORE_BYTES];
            var=ti_Open(APPVAR_V2,"r");
            if(var){bool same=ti_Read(verify,1,sizeof(verify),var)==sizeof(verify)&&memcmp(verify,store,sizeof(store))==0;ti_Close(var);return same;}
        }
    }
    return false;
}

static void diagnostic_wait_key(void)
{
#if CINEMA_RENDERER != CINEMA_RENDERER_FIXED_ASM
    /* Host simulations do not provide an interactive post-playback key.
     * Do not block their deterministic test sequence. The physical
     * fixed-ASM calculator build retains full release/press/release
     * debouncing below. */
    return;
#else
    while (os_GetCSC()) { }
    while (!os_GetCSC()) { }
    while (os_GetCSC()) { }
#endif
}

static void print_playback_summary(const player_v2_t *player)
{
    char buffer[64];
    uint32_t active = 1;
    uint32_t actual10 = 0;
    uint32_t target10 = (uint32_t)(((uint64_t)player->fps_num * 10u) / player->fps_den);
    uint32_t read_avg_whole = 0, read_avg_frac = 0;
    uint32_t read_max_whole = 0, read_max_frac = 0;
    uint32_t decode_avg_whole = 0, decode_avg_frac = 0;
    uint32_t kib10 = 0;

    if (player->session_frames_presented && clock() > player->playback_start_tick) {
        active = (uint32_t)(clock() - player->playback_start_tick
                            - player->accumulated_pause_ticks);
        if (!active) active = 1;
        actual10 = (uint32_t)(((uint64_t)player->session_frames_presented
                               * 10u * CLOCKS_PER_SEC) / active);
    }
    if (player->read_submissions) {
        uint64_t avg1000 = (uint64_t)player->read_ticks_total * 1000u
                            / player->read_submissions;
        avg1000 = avg1000 * 1000u / CLOCKS_PER_SEC;
        read_avg_whole = (uint32_t)(avg1000 / 1000u);
        read_avg_frac = (uint32_t)(avg1000 % 1000u);
        {
            uint64_t max1000 = (uint64_t)player->read_ticks_max * 1000000u
                                / CLOCKS_PER_SEC;
            read_max_whole = (uint32_t)(max1000 / 1000u);
            read_max_frac = (uint32_t)(max1000 % 1000u);
        }
    }
    if (player->decode_samples) {
        uint64_t davg1000 = (uint64_t)player->decode_ticks_total * 1000000u
                             / player->decode_samples / CLOCKS_PER_SEC;
        decode_avg_whole = (uint32_t)(davg1000 / 1000u);
        decode_avg_frac = (uint32_t)(davg1000 % 1000u);
    }
    if (player->read_ticks_total) {
        kib10 = (uint32_t)(((uint64_t)player->read_sectors_total * 512u
                            * 10u * CLOCKS_PER_SEC)
                           / player->read_ticks_total / 1024u);
    }

    /* Page 1: exactly nine printed lines. */
    os_ClrHome();
    putstr("DIAGNOSTICS 1/2");
    sprintf(buffer, "renderer id %u calls %lu", (unsigned)CINEMA_RENDERER_ACTIVE_ID,
            (unsigned long)g_render_calls[CINEMA_RENDERER_ACTIVE_ID]); putstr(buffer);
    sprintf(buffer, "session renders %lu", (unsigned long)player->session_frames_presented); putstr(buffer);
    sprintf(buffer, "movie position %lu", (unsigned long)(player->has_presented
            ? player->last_frame_presented + 1u : 0u)); putstr(buffer);
    sprintf(buffer, "actual %lu.%lu target %lu.%lu", (unsigned long)(actual10 / 10u),
            (unsigned long)(actual10 % 10u), (unsigned long)(target10 / 10u),
            (unsigned long)(target10 % 10u)); putstr(buffer);
    sprintf(buffer, "speed %lu.%02lux lag %lu", (unsigned long)(target10 ? actual10 * 100u / target10 / 100u : 0u),
            (unsigned long)(target10 ? actual10 * 100u / target10 % 100u : 0u),
            (unsigned long)player->max_schedule_lag); putstr(buffer);
    sprintf(buffer, "drop %lu wait %lu", (unsigned long)player->dropped_frames,
            (unsigned long)player->repeated_frames); putstr(buffer);
    sprintf(buffer, "read %lu.%03lu max %lu.%03lu", (unsigned long)read_avg_whole,
            (unsigned long)read_avg_frac, (unsigned long)read_max_whole,
            (unsigned long)read_max_frac); putstr(buffer);
    putstr("Press key for page 2");
    diagnostic_wait_key();

    /* Page 2: at most nine printed lines. */
    os_ClrHome();
    putstr("DIAGNOSTICS 2/2");
    sprintf(buffer, "read %lu.%lu KiB/s", (unsigned long)(kib10 / 10u),
            (unsigned long)(kib10 % 10u)); putstr(buffer);
    sprintf(buffer, "cmd %lu maxQ %u", (unsigned long)player->read_submissions,
            (unsigned)player->max_concurrent_reads); putstr(buffer);
    sprintf(buffer, "map direct %lu gen %lu", (unsigned long)player->direct_map_hits,
            (unsigned long)player->fragmented_frame_resolves); putstr(buffer);
    sprintf(buffer, "pair %lu single %lu", (unsigned long)player->packed_pair_commands,
            (unsigned long)player->packed_single_commands); putstr(buffer);
    sprintf(buffer, "slots %u supplied %lu", (unsigned)player->slot_count,
            (unsigned long)(player->packed_pair_commands * 2u
                            + player->packed_single_commands)); putstr(buffer);
    sprintf(buffer, ">111 %lu >250 %lu", (unsigned long)player->reads_over_111ms,
            (unsigned long)player->reads_over_250ms); putstr(buffer);
    sprintf(buffer, ">500 %lu >1000 %lu", (unsigned long)player->reads_over_500ms,
            (unsigned long)player->reads_over_1000ms); putstr(buffer);
    sprintf(buffer, "render %lu.%03lu ms (%lu)", (unsigned long)decode_avg_whole,
            (unsigned long)decode_avg_frac, (unsigned long)player->decode_samples); putstr(buffer);
    putstr("Press key to exit");
    diagnostic_wait_key();

#ifdef CINEMA_DIAGNOSTIC
    {
        char line[32];

        /*
         * Page 1 contains subtitle availability and startup status.
         * Keep output short enough for the TI-OS text display.
         */
        os_ClrHome();
        puts("SUBTITLE DIAG 1/2");

        switch (g_subtitle_open_stage) {
            case 1:
                puts("open NO MAP");
                break;

            case 2:
                puts("open TOO SHORT");
                break;

            case 3:
                puts("open FAST FAIL");
                break;

            case 4:
                puts("open FAST READY");
                break;

            default:
                puts("open UNKNOWN");
                break;
        }

        puts(
            player->subtitle_available
                ? "available YES"
                : "available NO"
        );

        puts(
            player->subtitles_enabled
                ? "enabled YES"
                : "enabled NO"
        );

        snprintf(
            line,
            sizeof(line),
            "file %lu bytes",
            (unsigned long)player->subtitle_size
        );
        puts(line);

        snprintf(
            line,
            sizeof(line),
            "cues %lu",
            (unsigned long)
                player->subtitle_header.cue_count
        );
        puts(line);

        snprintf(
            line,
            sizeof(line),
            "open %lu ms",
            (unsigned long)(
                player->subtitle_validation_ticks
                * 1000u
                / CLOCKS_PER_SEC
            )
        );
        puts(line);

        snprintf(
            line,
            sizeof(line),
            "cue index %ld",
            (long)player->subtitle_cue_index
        );
        puts(line);

        puts("Key: next page");

        /*
         * Reuse the existing press-and-release diagnostic wait.
         * This prevents one held key from skipping both pages.
         */
        diagnostic_wait_key();


        /*
         * Page 2 contains runtime I/O and contained failure data.
         */
        os_ClrHome();
        puts("SUBTITLE DIAG 2/2");

        snprintf(
            line,
            sizeof(line),
            "runtime sec %lu",
            (unsigned long)
                player->subtitle_sector_reads
        );
        puts(line);

        snprintf(
            line,
            sizeof(line),
            "cue checks %lu",
            (unsigned long)
                player->subtitle_runtime_validations
        );
        puts(line);

        snprintf(
            line,
            sizeof(line),
            "failure %ld/%u",
            (long)player->subtitle_failure_index,
            (unsigned)player->subtitle_failure_reason
        );
        puts(line);

        snprintf(
            line,
            sizeof(line),
            "requests %lu",
            (unsigned long)
                g_subtitle_read_requests
        );
        puts(line);

        snprintf(
            line,
            sizeof(line),
            "cross reads %lu",
            (unsigned long)
                g_subtitle_cross_sector_requests
        );
        puts(line);

        snprintf(
            line,
            sizeof(line),
            "read bytes %lu",
            (unsigned long)
                g_subtitle_read_bytes
        );
        puts(line);

        snprintf(
            line,
            sizeof(line),
            "draw calls %lu",
            (unsigned long)
                g_subtitle_draw_calls
        );
        puts(line);

        snprintf(
            line,
            sizeof(line),
            "active %lu tog %lu",
            (unsigned long)
                g_subtitle_active_frames,
            (unsigned long)
                g_subtitle_toggle_count
        );
        puts(line);

        puts("Key: exit");
        diagnostic_wait_key();
    }
#endif
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
                    /* Don't let the keypress-linger immediately redraw
                     * it -- render_frame()'s own showing/was_visible
                     * transition check clears it out of both swap
                     * buffers, the same as a natural linger expiry. */
                    player->osd_until_tick = clock();
                }
                break;

            case sk_Graph:
                player->loop_enabled = !player->loop_enabled;
                break;

            case sk_Del:
                if(player->subtitle_available)subtitle_options_menu(player);
                break;

            case sk_Window: /* frame-step forward, only while paused */
                if (player->paused && player->has_presented
                    && player->last_frame_presented + 1 < player->frame_count) {
                    player->pause_after_render = true;
                    if (!player_seek_to_frame(player, player->last_frame_presented + 1)) {
                        return false;
                    }
                }
                break;

            case sk_Yequ: /* paused: step back; playing: toggle subtitles */
                if (player->paused && player->has_presented
                    && player->last_frame_presented > 0) {
                    player->pause_after_render = true;
                    if (!player_seek_to_frame(player, player->last_frame_presented - 1)) {
                        return false;
                    }
                } else if (!player->paused && player->subtitle_available) {
                    player->subtitles_enabled = !player->subtitles_enabled;
#ifdef CINEMA_DIAGNOSTIC
                g_subtitle_toggle_count++;
#endif
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
            /* Deliberately sequential, never skip-ahead: the frame we
             * look for is always the very next one after whatever's on
             * screen, not "whatever the wall clock says right now". A
             * wall-clock target sounds more correct, but it isn't --
             * if the queue ever falls even slightly behind (real USB
             * throughput dips below what the frame rate needs, even
             * briefly), every frame that finishes loading is already
             * older than the ever-advancing clock target by the time it
             * arrives, so it gets thrown away, and the gap between
             * "what's loaded" and "what the clock wants" only ever
             * grows -- a real hardware-confirmed failure mode (near-total
             * frame loss, playback trickling out roughly one lucky frame
             * every few seconds, recoverable only by a manual seek since
             * that's the only place anything resyncs the queue).
             *
             * Asking for next_frame strictly in order instead can't
             * diverge like that by construction: the queue can only ever
             * be "caught up to" this target or still working on it, never
             * hopelessly behind a target that keeps moving out from under
             * it. `wanted` is still computed and still gates *early*
             * presentation (see the wanted < next_frame check below) so
             * fast hardware still paces itself to the real frame rate
             * instead of racing through the movie -- it just never causes
             * a frame to be skipped or discarded. The cost is that if the
             * hardware genuinely can't sustain the encoded rate, playback
             * runs slower than real time instead of dropping content to
             * keep up -- the only sane option once you rule out being
             * able to invent bytes that haven't arrived yet. */
            uint32_t wanted = desired_frame(player, clock());
            uint32_t next_frame = player->has_presented
                ? player->last_frame_presented + 1 : player->start_frame;
            frame_slot_t *slot;
            if (wanted > next_frame && wanted-next_frame > player->max_schedule_lag)
                player->max_schedule_lag=wanted-next_frame;
            slot = find_ready_frame(player, next_frame);

            if (slot != NULL && wanted >= next_frame) {
                if (player->buffering_shown) {
                    /* Stall resolved -- scrub the stale "Buffering..."
                     * text out of both swap buffers via the same
                     * mechanism render_frame()'s own OSD logic uses (see
                     * its showing/osd_was_visible handling). Whichever
                     * path fires next call (a real OSD redraw, or the
                     * clear-only fill) repaints the whole letterbox rect
                     * regardless, so this is safe either way. */
                    player->buffering_shown = false;
                    player->osd_clear_pending = 2;
                }

                render_frame(player, slot);
                player->has_presented = true;
                player->last_frame_presented = slot->frame_number + slot->present_index;
                player->last_progress_tick = clock();
                slot->frames_consumed = (uint8_t)(slot->present_index + 1u);
                if (slot->frames_consumed >= slot->frame_span)
                    slot->state = SLOT_EMPTY;

                if (player->pause_after_render) {
                    /* A paused frame-step (sk_Window/sk_Yequ): the seek
                     * that got us this frame always resumes playback
                     * (see player_seek_to_frame), so undo that here, now
                     * that the stepped-to frame has actually been drawn.
                     * Skipping playback_finished below is deliberate --
                     * stepping onto the last frame while paused should
                     * just sit there, not trigger end-of-movie/looping. */
                    player->pause_after_render = false;
                    player->paused = true;
                    player->pause_tick = clock();
                    continue;
                }

                if (playback_finished(player)) {
                    if (!player->loop_enabled) {
                        return true;
                    }
                    /* player_seek_to_frame(..., 0) is exactly the same
                     * reset the [0] restart key uses -- slots, timing,
                     * and the "already shown" marker all go back to a
                     * fresh start, so looping is indistinguishable from
                     * the user pressing 0 right as the last frame ends. */
                    if (!player_seek_to_frame(player, 0)) {
                        return false;
                    }
                }
            } else if (slot == NULL && player->has_presented) {
                /* next_frame isn't ready yet -- hold the currently
                 * displayed frame rather than show nothing. (The other
                 * remaining case -- slot != NULL but wanted < next_frame,
                 * i.e. a frame is ready ahead of schedule -- matches
                 * neither branch and does nothing, which is correct:
                 * there's a frame in hand, just not its turn yet, so
                 * quietly wait for the clock rather than count it as a
                 * stall.) */
                player->repeated_frames++;

                if (!player->buffering_shown
                    && (clock_t)(clock() - player->last_progress_tick)
                           > (clock_t)V2_BUFFERING_THRESHOLD_TICKS) {
                    show_buffering_overlay(player);
                    player->buffering_shown = true;
                }
            }
        }
    }
}

static const fat32ro_extent_map_t *g_next_subtitle_map;
static uint32_t g_next_subtitle_size,g_next_subtitle_movie_id;
static bool g_next_subtitles_enabled;
static uint8_t *g_next_subtitle_sector;
void player_v2_set_subtitle_stream(const fat32ro_extent_map_t *map,uint32_t size,
                                   uint32_t movie_id,bool enabled,uint8_t *sector_cache)
{
    g_next_subtitle_map=map; g_next_subtitle_size=size; g_next_subtitle_movie_id=movie_id;
    g_next_subtitles_enabled=enabled; g_next_subtitle_sector=sector_cache;
}

bool player_v2_run(global_t *global, const cin2_header_t *header,
                    uint32_t start_frame, const fat32ro_extent_map_t *movie_map,
                    const char *filename)
{
    static player_v2_t player;
    bool graphics_active = false;
    bool ok;

    g_player_v2_result=PLAYER_V2_INVALID;
    if (header->frame_count == 0) {
        putstr("movie has no frames");
        return false;
    }

    memset(&player, 0, sizeof(player));
    player.slot_count = PRIMARY_SLOT_COUNT;
    player.slots[0].sprite_data = player.primary_storage[0];
    player.slots[1].sprite_data = player.primary_storage[1];
    if ((header->flags & CIN2_FLAG_PACKED4) && g_packed_extra_storage != NULL
        && g_packed_extra_storage_size >= 2u + CINEMA_V2_WIDTH * CINEMA_V2_HEIGHT) {
        player.slots[2].sprite_data = g_packed_extra_storage;
        player.slot_count = MAX_SLOT_COUNT;
    }
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
    player.format_flags = header->flags;
    player.frame_sectors = (uint8_t)cin2_frame_sectors(header->flags);
    player.subtitle_map=g_next_subtitle_map;player.subtitle_size=g_next_subtitle_size;
    player.subtitle_sector=g_next_subtitle_sector;
    player.subtitle_movie_id=g_next_subtitle_movie_id;player.subtitles_enabled=g_next_subtitles_enabled;
    player.subtitle_cached_sector=0;player.subtitle_sector_valid=false;
    player.subtitle_failure_index=-1;player.subtitle_failure_reason=SUB_FAIL_NONE;
    subtitle_invalidate_cue(&player);
    /* Reuse the existing static thumbnail/packed-slot scratch only during
     * validation. Playback has not initialized slot 2 yet, and validation
     * mode is disabled before the storage can become a frame slot. */
    if(g_packed_extra_storage!=NULL&&g_packed_extra_storage_size>=SUBTITLE_BULK_BYTES){
        player.subtitle_bulk_buffer=g_packed_extra_storage;
        player.subtitle_bulk_capacity=SUBTITLE_BULK_BYTES;
    }
    player.subtitle_validation_mode=false;
    {
        clock_t validation_start=clock();
        if(player.subtitle_map!=NULL&&player.subtitle_sector!=NULL
           &&player.subtitle_size>=CSU_HEADER_SIZE){
            os_ClrHome();
            putstr("Opening subtitles...");
            player.subtitle_available=csu_stream_open_fast(&player.subtitle_header,
                subtitle_stream_read,&player,player.subtitle_size,0u,
                header->frame_count,header->fps_num,header->fps_den);
        }else{
            player.subtitle_available=false;
        }
        player.subtitle_validation_ticks=(clock_t)(clock()-validation_start);
    }
    player.subtitle_validation_mode=false;
    player.subtitle_bulk_sector_count=0;
    player.subtitle_bulk_buffer=NULL;
    player.subtitle_bulk_capacity=0;
    /* Validation traffic is reported separately. Runtime starts with an
     * empty one-sector cache so no pointer into borrowed scratch survives. */
    player.subtitle_sector_valid=false;
    player.subtitle_sector_reads=0;
    #ifdef CINEMA_DIAGNOSTIC
    g_subtitle_read_requests = 0;
    g_subtitle_cross_sector_requests = 0;
    g_subtitle_read_bytes = 0;
    g_subtitle_draw_calls = 0;
    g_subtitle_active_frames = 0;
    g_subtitle_toggle_count = 0;
    if (player.subtitle_map == NULL) g_subtitle_open_stage = 1;
    else if (player.subtitle_size < CSU_HEADER_SIZE) g_subtitle_open_stage = 2;
    else if (!player.subtitle_available) g_subtitle_open_stage = 3;
    else g_subtitle_open_stage = 4;
#endif
    g_next_subtitle_map=NULL;g_next_subtitle_size=0;g_next_subtitle_movie_id=0;g_next_subtitles_enabled=false; g_next_subtitle_sector=NULL;
    player.start_frame = start_frame;
    player.next_frame_to_queue = start_frame;
    choose_osd_colors(&player, header);
    {
        uint8_t i;

        for (i = 0; i < player.slot_count; ++i) {
            gfx_sprite_t *sprite = slot_sprite(&player.slots[i]);
            player.slots[i].owner = &player;

            sprite->width = CINEMA_V2_WIDTH;
            sprite->height = CINEMA_V2_HEIGHT;
        }
    }

    /* Prefill runs here, before gfx_Begin() -- it doesn't touch graphics
     * at all, and keeping it out of graphics mode means a prefill error
     * (e.g. a read failure) shows as a plain text message instead of
     * being immediately replaced by a graphics-mode screen. */
    ok = prefill_frames(&player);
    if(!ok) g_player_v2_result=PLAYER_V2_PREFILL_FAILED;

    if (ok) {
        /* A movie can easily run longer than TI-OS's idle auto-power-
         * down timer, which only resets on a keypress -- watching one
         * without touching a key for 5+ minutes would otherwise get cut
         * off mid-playback. Re-enabled below regardless of how the loop
         * exits. */
        os_DisableAPD();

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
        player.playback_start_tick = player.start_tick;
        player.fps_window_start = player.start_tick;
        player.last_progress_tick = player.start_tick;
        /* Surface the controls/scrubber briefly on start, the way a
         * video player does, then let it auto-hide. */
        osd_poke(&player);
        ok = player_v2_loop(&player);
        g_player_v2_result=ok?PLAYER_V2_USER_EXIT:(global->usb?PLAYER_V2_READ_FAILED:PLAYER_V2_DISCONNECTED);

        os_EnableAPD();
    }

    if (graphics_active) {
        gfx_End();
    }

#ifdef CINEMA_DIAGNOSTIC
    print_playback_summary(&player);
#endif

    if (player.has_presented) {
        putstr(save_resume_state(&player) ? "resume saved and verified" : "resume save failed");
    }

    return ok;
}
