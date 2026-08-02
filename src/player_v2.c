#include "player_v2.h"
#include "decode.h"
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

#define SLOT_COUNT 4

/* 4 slots * 1/24s per frame = ~166ms of read-ahead buffer. */

#define V2_Y_OFFSET ((GFX_LCD_HEIGHT - CINEMA_V2_DEST_HEIGHT) / 2)

typedef enum {
    SLOT_EMPTY,
    SLOT_LOADING,
    SLOT_READY,
    SLOT_ERROR
} slot_state_t;

typedef struct {
    uint8_t packed[CINEMA_V2_PACKED_BYTES];
    uint32_t frame_number;
    volatile slot_state_t state;
    volatile msd_error_t error;
    msd_transfer_t transfer;
} frame_slot_t;

typedef struct {
    global_t *global;
    frame_slot_t slots[SLOT_COUNT];

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
} player_v2_t;

/* Callback only records what happened -- no graphics calls, no printing,
 * no LBA math. The main loop decides what any of it means. */
static void frame_read_callback(msd_error_t error, struct msd_transfer *xfer)
{
    frame_slot_t *slot = (frame_slot_t *)xfer->userptr;

    slot->error = error;
    slot->state = (error == MSD_SUCCESS) ? SLOT_READY : SLOT_ERROR;
}

static msd_error_t queue_frame(global_t *global, frame_slot_t *slot,
                                uint32_t frame_number)
{
    msd_error_t result;

    slot->frame_number = frame_number;
    slot->error = MSD_SUCCESS;
    slot->state = SLOT_LOADING;

    slot->transfer.msd = &global->msd;
    slot->transfer.lba = cin2_frame_lba(frame_number);
    slot->transfer.count = CIN2_FRAME_SECTORS;
    slot->transfer.buffer = slot->packed;
    slot->transfer.callback = frame_read_callback;
    slot->transfer.userptr = slot;

    result = msd_ReadAsync(&slot->transfer);
    if (result != MSD_SUCCESS) {
        slot->error = result;
        slot->state = SLOT_ERROR;
    }

    return result;
}

/* Queues the next not-yet-read frame into every SLOT_EMPTY slot. Returns
 * false only if msd_ReadAsync itself failed to queue (not if a
 * previously-queued transfer later errors out -- that's caught via
 * find_failed_slot in the main loop). */
static bool refill_empty_slots(player_v2_t *player)
{
    uint8_t i;

    for (i = 0; i < SLOT_COUNT; ++i) {
        frame_slot_t *slot = &player->slots[i];

        if (slot->state != SLOT_EMPTY) {
            continue;
        }
        if (player->next_frame_to_queue >= player->frame_count) {
            continue;
        }

        if (queue_frame(player->global, slot, player->next_frame_to_queue)
            != MSD_SUCCESS) {
            return false;
        }
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

static bool all_queued_slots_resolved(player_v2_t *player)
{
    uint8_t i;

    for (i = 0; i < SLOT_COUNT; ++i) {
        if (player->slots[i].state == SLOT_LOADING) {
            return false;
        }
    }

    return true;
}

/* Fills every slot and blocks until each either finishes or errors, so
 * playback starts with a full read-ahead buffer instead of the single
 * frame the original player waited for. */
static bool prefill_frames(player_v2_t *player)
{
    if (!refill_empty_slots(player)) {
        putstr("error queueing msd (prefill)");
        return false;
    }

    while (!all_queued_slots_resolved(player)) {
        usb_HandleEvents();

        if (player->global->usb == NULL) {
            putstr("usb device disconnected");
            return false;
        }
        {
            frame_slot_t *failed = find_failed_slot(player);
            if (failed != NULL) {
                put_msd_error(failed->error, "prefill");
                return false;
            }
        }
        if (os_GetCSC()) {
            return false;
        }
    }

    return true;
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

static void render_frame(frame_slot_t *slot)
{
    gfx_SetDrawBuffer();
    cinema_draw_packed4_scaled2x(slot->packed, &gfx_vbuffer[0][0],
                                  GFX_LCD_WIDTH, V2_Y_OFFSET);
    gfx_SwapDraw();
    gfx_Wait();
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
        if (key == sk_Clear) {
            return true;
        }
        if (key == sk_2nd) {
            if (!player->paused) {
                player->paused = true;
                player->pause_tick = clock();
            } else {
                player->paused = false;
                player->accumulated_pause_ticks +=
                    (clock_t)(clock() - player->pause_tick);
            }
        }

        if (player->paused) {
            continue;
        }

        {
            uint32_t wanted = desired_frame(player, clock());
            frame_slot_t *slot;

            discard_obsolete_frames(player, wanted);
            slot = find_ready_frame(player, wanted);

            if (slot != NULL) {
                render_frame(slot);
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
                    uint32_t start_frame)
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
    player.frame_count = header->frame_count;
    player.fps_num = header->fps_num;
    player.fps_den = header->fps_den;
    player.start_frame = start_frame;
    player.next_frame_to_queue = start_frame;

    gfx_Begin();
    graphics_active = true;
    gfx_SetPalette(header->palette, sizeof(header->palette), 0);
    gfx_SwapDraw();
    gfx_SetDrawBuffer();
    gfx_ZeroScreen();
    gfx_SwapDraw();
    gfx_SetDrawBuffer();
    gfx_ZeroScreen();

    ok = prefill_frames(&player);
    if (ok) {
        player.start_tick = clock();
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
