/* Drives src/player_v1.c end-to-end against a synthetic in-memory
 * legacy-format "drive" (31 sectors/frame: 1 palette + 30 image, no
 * header), analogous to tests/stub_impl_sim.c for player_v2.c. Exit is
 * triggered by making the fake os_GetCSC() return a keypress after a
 * configurable number of calls, since player_v1_run's only exit
 * condition is os_GetCSC() returning nonzero. NOT part of the shipped
 * calculator build. */
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

static const uint8_t *g_drive;
static uint32_t g_drive_sectors;

void sim_set_drive(const uint8_t *drive, uint32_t sector_count)
{
    g_drive = drive;
    g_drive_sectors = sector_count;
}

static int g_getcsc_calls_before_exit = -1; /* -1: never exit */
static int g_getcsc_calls = 0;
void sim_exit_after_getcsc_calls(int n)
{
    g_getcsc_calls_before_exit = n;
    g_getcsc_calls = 0;
}

unsigned g_async_reads = 0;
static uint32_t g_fail_on_lba = 0xFFFFFFFFu;
void sim_inject_read_failure_at_lba(uint32_t lba) { g_fail_on_lba = lba; }

usb_error_t usb_Init(usb_event_callback_t handler, usb_callback_data_t *data,
                      const void *descriptors, uint32_t flags)
{ (void)handler; (void)data; (void)descriptors; (void)flags; return USB_SUCCESS; }
usb_error_t usb_WaitForInterrupt(void) { return USB_SUCCESS; }
usb_error_t usb_ResetDevice(usb_device_t dev) { (void)dev; return USB_SUCCESS; }
void usb_HandleEvents(void) {}
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
    if (lba + count > g_drive_sectors) return 0;
    memcpy(buffer, g_drive + (uint64_t)lba * 512, (size_t)count * 512);
    return count;
}
uint24_t msd_Write(msd_t *msd, uint32_t lba, uint24_t count, const void *buffer)
{ (void)msd; (void)lba; (void)buffer; return count; }
uint8_t msd_FindPartitions(msd_t *msd, msd_partition_t *partitions, uint8_t max)
{ (void)msd; (void)partitions; (void)max; return 0; }

msd_error_t msd_ReadAsync(msd_transfer_t *xfer)
{
    msd_error_t error = MSD_SUCCESS;

    g_async_reads++;
    if (xfer->lba == g_fail_on_lba) {
        error = MSD_ERROR_TIMEOUT;
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

static uint8_t g_appvar_data[256];
static size_t g_appvar_len = 0;
static int g_appvar_exists = 0;

uint8_t ti_Open(const char *name, const char *mode)
{
    (void)name;
    if (mode[0] == 'r') return g_appvar_exists ? 1 : 0;
    return 1;
}
int ti_Close(uint8_t handle) { (void)handle; return 1; }
size_t ti_Read(void *data, size_t size, size_t count, uint8_t handle)
{
    size_t bytes = size * count;
    (void)handle;
    if (bytes > g_appvar_len) return 0;
    memcpy(data, g_appvar_data, bytes);
    return count;
}
size_t ti_Write(const void *data, size_t size, size_t count, uint8_t handle)
{
    size_t bytes = size * count;
    (void)handle;
    if (bytes > sizeof(g_appvar_data)) return 0;
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

void os_SetCursorPos(uint8_t row, uint8_t col) { (void)row; (void)col; }
void os_PutStrFull(char *str) { printf("[calc] %s", str); }
void os_NewLine(void) { printf("\n"); }
void os_ClrHome(void) {}

uint8_t os_GetCSC(void)
{
    g_getcsc_calls++;
    if (g_getcsc_calls_before_exit >= 0 && g_getcsc_calls >= g_getcsc_calls_before_exit) {
        return sk_Clear;
    }
    return 0;
}

void putstr(const char *str) { printf("[calc] %s\n", str); }

unsigned g_frames_rendered = 0;

void gfx_Begin(void) {}
void gfx_End(void) {}
void gfx_SwapDraw(void) {}
void gfx_Wait(void) { g_frames_rendered++; }
void gfx_SetDraw(gfx_buffer_t mode) { (void)mode; }
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

int sim_get_appvar(uint8_t *out, size_t out_size)
{
    if (!g_appvar_exists || g_appvar_len > out_size) return 0;
    memcpy(out, g_appvar_data, g_appvar_len);
    return (int)g_appvar_len;
}
