#include "player_v1.h"
#include "msd_util.h"

#include <fileioc.h>
#include <graphx.h>
#include <msddrvce.h>
#include <tice.h>
#include <usbdrvce.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Legacy v1 on-disk layout, unchanged from the original Cinema:
 * palette (1 sector, 256 x RGB565) + image (30 sectors, 160x96 8bpp) =
 * 31 sectors/frame, no header. */
#define V1_SECTORS_PER_FRAME 31
#define V1_PALETTE_SECTORS    1
#define V1_IMAGE_SECTORS     30
#define V1_IMAGE_WIDTH       160
#define V1_IMAGE_HEIGHT       96

/* Fix for diagnosis "critical issue 1": the original player only waited
 * on the image transfer's completion flag and assumed the palette
 * transfer -- queued first -- would always finish first too, rather than
 * proving it. Track both independently and require both before treating
 * a frame as ready. */
typedef struct {
    volatile bool palette_done;
    volatile bool image_done;
    volatile msd_error_t palette_error;
    volatile msd_error_t image_error;
} v1_io_state_t;

/* Fix for diagnosis "critical issue 4": callbacks used to call gfx_End()
 * and putstr() directly and could report a failed transfer as ready.
 * Callbacks now only record state; the main loop decides what to do. */
static void v1_palette_callback(msd_error_t error, struct msd_transfer *xfer)
{
    v1_io_state_t *state = (v1_io_state_t *)xfer->userptr;

    xfer->lba += V1_SECTORS_PER_FRAME;
    state->palette_error = error;
    state->palette_done = true;
}

static void v1_image_callback(msd_error_t error, struct msd_transfer *xfer)
{
    v1_io_state_t *state = (v1_io_state_t *)xfer->userptr;

    xfer->lba += V1_SECTORS_PER_FRAME;
    state->image_error = error;
    state->image_done = true;
}

/* Must be called before queueing a palette+image transfer pair, not
 * after -- resetting these flags after msd_ReadAsync() has already been
 * called would race against (and could silently clobber) a completion
 * the callback already recorded. */
static void v1_arm(v1_io_state_t *state)
{
    state->palette_done = false;
    state->image_done = false;
}

static bool v1_wait_for_frame(v1_io_state_t *state)
{
    while (!(state->palette_done && state->image_done)) {
        usb_HandleEvents();
    }

    return state->palette_error == MSD_SUCCESS
        && state->image_error == MSD_SUCCESS;
}

static void v1_report_io_error(const v1_io_state_t *state)
{
    char buffer[64];

    if (state->palette_error != MSD_SUCCESS) {
        sprintf(buffer, "%s (palette)", msd_error_string(state->palette_error));
        putstr(buffer);
    }
    if (state->image_error != MSD_SUCCESS) {
        sprintf(buffer, "%s (image)", msd_error_string(state->image_error));
        putstr(buffer);
    }
}

bool player_v1_run(global_t *global, uint32_t start_lba)
{
    gfx_sprite_t *sprite_buffer_1 = NULL;
    gfx_sprite_t *sprite_buffer_2 = NULL;
    static uint16_t palette_buffer_1[256];
    static uint16_t palette_buffer_2[256];
    msd_transfer_t xfer_palette;
    msd_transfer_t xfer_image;
    v1_io_state_t io_state;
    msd_error_t msderr;
    bool graphics_active = false;
    bool ok = true;

    memset(palette_buffer_1, 0, sizeof(palette_buffer_1));
    memset(&io_state, 0, sizeof(io_state));

    xfer_palette.msd = &global->msd;
    xfer_palette.lba = start_lba;
    xfer_palette.count = V1_PALETTE_SECTORS;
    xfer_palette.callback = v1_palette_callback;
    xfer_palette.userptr = &io_state;

    xfer_image.msd = &global->msd;
    xfer_image.lba = start_lba + V1_PALETTE_SECTORS;
    xfer_image.count = V1_IMAGE_SECTORS;
    xfer_image.callback = v1_image_callback;
    xfer_image.userptr = &io_state;

    gfx_Begin();
    graphics_active = true;
    gfx_SwapDraw();
    gfx_SetDrawBuffer();
    gfx_ZeroScreen();
    gfx_SwapDraw();
    gfx_SetDrawBuffer();
    gfx_ZeroScreen();

    /* Fix for diagnosis's "check sprite/buffer allocation": the original
     * never checked gfx_MallocSprite() for failure before dereferencing
     * the result. */
    sprite_buffer_1 = gfx_MallocSprite(V1_IMAGE_WIDTH, V1_IMAGE_HEIGHT);
    sprite_buffer_2 = gfx_MallocSprite(V1_IMAGE_WIDTH, V1_IMAGE_HEIGHT);
    if (sprite_buffer_1 == NULL || sprite_buffer_2 == NULL) {
        putstr("frame buffer allocation failed");
        ok = false;
        goto cleanup;
    }

    xfer_palette.buffer = palette_buffer_1;
    xfer_image.buffer = &sprite_buffer_1->data;

    v1_arm(&io_state);
    msderr = msd_ReadAsync(&xfer_palette);
    if (msderr != MSD_SUCCESS) {
        putstr("error queueing msd (palette)");
        ok = false;
        goto cleanup;
    }
    msderr = msd_ReadAsync(&xfer_image);
    if (msderr != MSD_SUCCESS) {
        putstr("error queueing msd (image)");
        ok = false;
        goto cleanup;
    }

    if (!v1_wait_for_frame(&io_state)) {
        v1_report_io_error(&io_state);
        ok = false;
        goto cleanup;
    }

    /* Main loop -- unchanged cadence from the original: one outer
     * iteration displays two frames (alternating the two buffer sets),
     * and the exit key is polled once per iteration, matching the
     * original's exact timing so this stays a pure safety/correctness
     * refactor rather than a behavior change. */
    while (!os_GetCSC()) {
        gfx_SetDrawBuffer();
        xfer_image.buffer = &sprite_buffer_2->data;
        xfer_palette.buffer = palette_buffer_2;

        v1_arm(&io_state);
        msderr = msd_ReadAsync(&xfer_palette);
        if (msderr != MSD_SUCCESS) {
            putstr("error queueing msd (palette)");
            ok = false;
            goto cleanup;
        }
        msderr = msd_ReadAsync(&xfer_image);
        if (msderr != MSD_SUCCESS) {
            putstr("error queueing msd (image)");
            ok = false;
            goto cleanup;
        }

        gfx_ScaledSprite_NoClip(sprite_buffer_1, 0, 24, 2, 2);
        gfx_SwapDraw();
        gfx_Wait();
        gfx_SetPalette(palette_buffer_1, 512, 0);

        if (!v1_wait_for_frame(&io_state)) {
            v1_report_io_error(&io_state);
            ok = false;
            goto cleanup;
        }

        gfx_SetDrawBuffer();
        xfer_image.buffer = &sprite_buffer_1->data;
        xfer_palette.buffer = palette_buffer_1;

        v1_arm(&io_state);
        msderr = msd_ReadAsync(&xfer_palette);
        if (msderr != MSD_SUCCESS) {
            putstr("error queueing msd (palette)");
            ok = false;
            goto cleanup;
        }
        msderr = msd_ReadAsync(&xfer_image);
        if (msderr != MSD_SUCCESS) {
            putstr("error queueing msd (image)");
            ok = false;
            goto cleanup;
        }

        gfx_ScaledSprite_NoClip(sprite_buffer_2, 0, 24, 2, 2);
        gfx_SwapDraw();
        gfx_Wait();
        gfx_SetPalette(palette_buffer_2, 512, 0);

        if (!v1_wait_for_frame(&io_state)) {
            v1_report_io_error(&io_state);
            ok = false;
            goto cleanup;
        }
    }

cleanup:
    /* Fix for diagnosis's "track whether graphics started": a single
     * cleanup path instead of calling gfx_End() from inside callbacks or
     * forgetting it on an error goto. */
    if (graphics_active) {
        gfx_End();
    }
    free(sprite_buffer_1);
    free(sprite_buffer_2);

    if (ok) {
        /* Matches the original's resume semantics exactly: the callback
         * has already advanced xfer_palette.lba past every frame that
         * finished reading, which is what gets saved. Only save on a
         * clean user-initiated exit, not after an I/O error. */
        uint8_t var = ti_Open(APPVAR_V1, "w");
        if (var) {
            ti_SetGCBehavior(NULL, NULL);
            ti_SetArchiveStatus(0, var);
            ti_Write(&xfer_palette.lba, sizeof(uint32_t), 1, var);
            ti_SetArchiveStatus(1, var);
            ti_Close(var);
        }
    }

    return ok;
}
