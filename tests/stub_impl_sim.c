/* Drives src/player_v2.c end-to-end against a synthetic in-memory CIN2
 * "drive" instead of real USB/MSD hardware, so the slot state machine,
 * prefill, scheduler, and drop/repeat bookkeeping in player_v2.c
 * actually execute (not just link) without a real calculator. Distinct
 * from tests/stub_impl.c, which only needs to satisfy the linker for
 * main.c's USB bring-up path. NOT part of the shipped calculator build.
 */
#include <usbdrvce.h>
#include <msddrvce.h>
#include <fileioc.h>
#include <tice.h>
#include <graphx.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

uint8_t gfx_vram_stub[GFX_LCD_HEIGHT][GFX_LCD_WIDTH];

/* --- deterministic fake clock -------------------------------------
 * player_v2.c's scheduler is driven entirely by clock()/CLOCKS_PER_SEC.
 * Rather than making this test actually sleep in real time to match
 * 24fps pacing (slow, flaky under load), we intercept clock() at link
 * time (-Wl,--wrap=clock) and advance a fake counter by a fixed amount
 * every trip through the main loop (see usb_HandleEvents below), so the
 * scheduler sees deterministic, fast-but-still-realistic time passing
 * regardless of whether that particular iteration rendered a frame. */
static clock_t g_fake_clock = 0;

clock_t __wrap_clock(void) { return g_fake_clock; }
void sim_advance_clock(clock_t ticks) { g_fake_clock += ticks; }

/* How much simulated wall-clock time one trip through the player's main
 * loop consumes. Charged in usb_HandleEvents() (called unconditionally
 * every iteration) rather than in gfx_Wait() (called only when a frame
 * actually renders) -- charging it there instead would make elapsed
 * time depend on having already rendered a frame, which is backwards:
 * on real hardware an independent 32768 Hz timer keeps advancing
 * clock() regardless of whether anything was just drawn. */
#define SIM_TICKS_PER_LOOP_ITERATION (CLOCKS_PER_SEC / 1000) /* ~1ms */

/* --- synthetic drive ------------------------------------------------- */

static const uint8_t *g_drive;
static uint32_t g_drive_sectors;

void sim_set_drive(const uint8_t *drive, uint32_t sector_count)
{
    g_drive = drive;
    g_drive_sectors = sector_count;
}

/* Instrumentation the test asserts on. */
unsigned g_async_reads = 0;
unsigned g_async_read_failures_injected = 0;
static uint32_t g_fail_on_lba = 0xFFFFFFFFu; /* sentinel: never fail */

void sim_inject_read_failure_at_lba(uint32_t lba)
{
    g_fail_on_lba = lba;
}

/* --- usbdrvce/msddrvce stubs ------------------------------------------ */

usb_error_t usb_Init(usb_event_callback_t handler, usb_callback_data_t *data,
                      const void *descriptors, uint32_t flags)
{ (void)handler; (void)data; (void)descriptors; (void)flags; return USB_SUCCESS; }
usb_error_t usb_WaitForInterrupt(void) { return USB_SUCCESS; }
usb_error_t usb_ResetDevice(usb_device_t dev) { (void)dev; return USB_SUCCESS; }
void usb_HandleEvents(void) { sim_advance_clock(SIM_TICKS_PER_LOOP_ITERATION); }
void usb_Cleanup(void) {}

msd_error_t msd_Open(msd_t *msd, usb_device_t usb)
{ (void)msd; (void)usb; return MSD_SUCCESS; }
void msd_Close(msd_t *msd) { (void)msd; }
msd_error_t msd_Reset(msd_t *msd) { (void)msd; return MSD_SUCCESS; }
msd_error_t msd_Info(msd_t *msd, msd_info_t *info)
{ (void)msd; info->bsize = 512; info->bnum = g_drive_sectors; return MSD_SUCCESS; }

uint24_t msd_Read(msd_t *msd, uint32_t lba, uint24_t count, void *buffer)
{
    (void)msd;
    if (lba + count > g_drive_sectors) {
        return 0;
    }
    memcpy(buffer, g_drive + (uint64_t)lba * 512, (size_t)count * 512);
    return count;
}
uint24_t msd_Write(msd_t *msd, uint32_t lba, uint24_t count, const void *buffer)
{ (void)msd; (void)lba; (void)buffer; return count; }
uint8_t msd_FindPartitions(msd_t *msd, msd_partition_t *partitions, uint8_t max)
{ (void)msd; (void)partitions; (void)max; return 0; }

/* Real hardware calls back asynchronously later; our synthetic drive has
 * no I/O latency to model, so we fire the callback immediately, which is
 * exactly the "callback can run before the next line of caller code"
 * case player_v2.c's slot state machine has to be correct under. */
msd_error_t msd_ReadAsync(msd_transfer_t *xfer)
{
    msd_error_t error = MSD_SUCCESS;

    g_async_reads++;

    if (xfer->lba == g_fail_on_lba) {
        error = MSD_ERROR_TIMEOUT;
        g_async_read_failures_injected++;
    } else if (xfer->lba + xfer->count > g_drive_sectors) {
        error = MSD_ERROR_INVALID_PARAM;
    } else {
        memcpy(xfer->buffer, g_drive + (uint64_t)xfer->lba * 512,
               (size_t)xfer->count * 512);
    }

    xfer->callback(error, xfer);
    return MSD_SUCCESS;
}
msd_error_t msd_WriteAsync(msd_transfer_t *xfer) { (void)xfer; return MSD_SUCCESS; }

/* --- fileioc stub: in-memory appvar store ----------------------------- */

static uint8_t g_appvar_data[256];
static size_t g_appvar_len = 0;
static int g_appvar_exists = 0;
static size_t g_appvar_pos = 0;

uint8_t ti_Open(const char *name, const char *mode)
{
    (void)name;
    if (mode[0] == 'r') {
        return g_appvar_exists ? 1 : 0;
    }
    g_appvar_pos = 0;
    return 1; /* "w" always succeeds */
}
int ti_Close(uint8_t handle) { (void)handle; return 1; }
size_t ti_Read(void *data, size_t size, size_t count, uint8_t handle)
{
    size_t bytes = size * count;
    (void)handle;
    if (g_appvar_pos + bytes > g_appvar_len) {
        return 0;
    }
    memcpy(data, g_appvar_data + g_appvar_pos, bytes);
    g_appvar_pos += bytes;
    return count;
}
size_t ti_Write(const void *data, size_t size, size_t count, uint8_t handle)
{
    size_t bytes = size * count;
    (void)handle;
    if (bytes > sizeof(g_appvar_data)) {
        return 0;
    }
    memcpy(g_appvar_data, data, bytes);
    g_appvar_len = bytes;
    g_appvar_exists = 1;
    return count;
}
int ti_SetArchiveStatus(uint8_t archive, uint8_t handle)
{ (void)archive; (void)handle; return 1; }
void ti_SetGCBehavior(void (*before)(void), void (*after)(void))
{ (void)before; (void)after; }
int ti_Delete(const char *name) { (void)name; g_appvar_exists = 0; return 1; }
int ti_Seek(int offset, unsigned int origin, uint8_t handle)
{ (void)offset; (void)origin; (void)handle; return 0; }

/* --- tice stub ---------------------------------------------------------
 * os_GetCSC returns 0 (no key) by default, except for keys scheduled via
 * sim_inject_key_at_call (fires once, on the Nth call to os_GetCSC,
 * counting from 1) -- used to drive control-key sequences (pause, seek)
 * through the real player_v2_loop() for end-to-end testing. */
#define SIM_MAX_INJECTED_KEYS 8
static struct { int call_index; uint8_t key; } g_injected_keys[SIM_MAX_INJECTED_KEYS];
static int g_injected_key_count = 0;
static int g_getcsc_calls = 0;

void sim_inject_key_at_call(int call_index, uint8_t key)
{
    if (g_injected_key_count < SIM_MAX_INJECTED_KEYS) {
        g_injected_keys[g_injected_key_count].call_index = call_index;
        g_injected_keys[g_injected_key_count].key = key;
        g_injected_key_count++;
    }
}

void sim_reset_injected_keys(void)
{
    g_injected_key_count = 0;
    g_getcsc_calls = 0;
}

void os_SetCursorPos(uint8_t row, uint8_t col) { (void)row; (void)col; }
void os_PutStrFull(char *str) { printf("[calc] %s", str); }
void os_NewLine(void) { printf("\n"); }
void os_ClrHome(void) {}

uint8_t os_GetCSC(void)
{
    int i;

    g_getcsc_calls++;
    for (i = 0; i < g_injected_key_count; ++i) {
        if (g_injected_keys[i].call_index == g_getcsc_calls) {
            return g_injected_keys[i].key;
        }
    }
    return 0;
}

/* --- cinema.h's putstr --------------------------------------------
 * Normally defined once in src/main.c; this test links player_v2.c
 * directly without main.c, so it needs its own definition to satisfy
 * the linker (put_msd_error() and player_v2.c itself both call it). */
void putstr(const char *str) { printf("[calc] %s\n", str); }

/* --- graphx stub --------------------------------------------------- */

unsigned g_frames_rendered = 0;

/* Tracks which target (screen vs offscreen buffer) is currently
 * selected, and how many rectangle fills landed on each -- lets tests
 * confirm the paused-OSD-overlay path really does target gfx_screen
 * (via gfx_SetDrawScreen()) rather than the offscreen buffer. */
static gfx_buffer_t g_draw_mode = gfx_buffer;
unsigned g_screen_mode_fills = 0;
unsigned g_buffer_mode_fills = 0;

void gfx_Begin(void) {}
void gfx_End(void) {}
void gfx_SwapDraw(void) {}
void gfx_Wait(void) { g_frames_rendered++; }
void gfx_SetDraw(gfx_buffer_t mode) { g_draw_mode = mode; }
void gfx_ZeroScreen(void) {}
void gfx_SetPalette(const void *palette, uint24_t size, uint8_t offset)
{ (void)palette; (void)size; (void)offset; }
gfx_sprite_t *gfx_AllocSprite(uint8_t width, uint8_t height,
                               void *(*alloc_routine)(size_t))
{
    gfx_sprite_t *s = alloc_routine(sizeof(gfx_sprite_t) + (size_t)width * height);
    if (s) { s->width = width; s->height = height; }
    return s;
}
void gfx_ScaledSprite_NoClip(const gfx_sprite_t *sprite, uint24_t x,
                              uint8_t y, uint8_t width_scale,
                              uint8_t height_scale)
{ (void)sprite; (void)x; (void)y; (void)width_scale; (void)height_scale; }

/* --- test accessors for the appvar store ------------------------------ */

int sim_get_resume_record(uint8_t *out, size_t out_size)
{
    if (!g_appvar_exists || g_appvar_len > out_size) {
        return 0;
    }
    memcpy(out, g_appvar_data, g_appvar_len);
    return (int)g_appvar_len;
}

uint8_t gfx_SetColor(uint8_t index) { (void)index; return 0; }
void gfx_FillRectangle_NoClip(uint24_t x, uint8_t y, uint24_t width,
                               uint8_t height)
{
    (void)x; (void)y; (void)width; (void)height;
    if (g_draw_mode == gfx_screen) {
        g_screen_mode_fills++;
    } else {
        g_buffer_mode_fills++;
    }
}
uint8_t gfx_SetTextFGColor(uint8_t color) { (void)color; return 0; }
uint8_t gfx_SetTextBGColor(uint8_t color) { (void)color; return 0; }
void gfx_PrintStringXY(const char *string, int x, int y)
{ (void)string; (void)x; (void)y; }
