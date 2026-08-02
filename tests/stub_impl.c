/* Minimal function bodies for the stub headers in tests/stub_include,
 * just enough to link a full test binary of the sources under src/
 * against them (never actually exercised at runtime beyond main()
 * returning immediately -- see tests/run_tests.sh). NOT part of the
 * shipped calculator build. */
#include <usbdrvce.h>
#include <msddrvce.h>
#include <fileioc.h>
#include <tice.h>
#include <graphx.h>

#include <stddef.h>
#include <stdio.h>
#include <string.h>

uint8_t gfx_vram_stub[GFX_LCD_HEIGHT][GFX_LCD_WIDTH];

usb_error_t usb_Init(usb_event_callback_t handler, usb_callback_data_t *data,
                      const void *descriptors, uint32_t flags)
{
    (void)handler; (void)data; (void)descriptors; (void)flags;
    return USB_NO_DEVICE;
}
usb_error_t usb_WaitForInterrupt(void) { return USB_SUCCESS; }
usb_error_t usb_ResetDevice(usb_device_t dev) { (void)dev; return USB_SUCCESS; }
void usb_HandleEvents(void) {}
void usb_Cleanup(void) {}

msd_error_t msd_Open(msd_t *msd, usb_device_t usb)
{ (void)msd; (void)usb; return MSD_SUCCESS; }
void msd_Close(msd_t *msd) { (void)msd; }
msd_error_t msd_Reset(msd_t *msd) { (void)msd; return MSD_SUCCESS; }
msd_error_t msd_Info(msd_t *msd, msd_info_t *info)
{ (void)msd; info->bsize = 512; info->bnum = 1000000; return MSD_SUCCESS; }
msd_error_t msd_ReadAsync(msd_transfer_t *xfer) { (void)xfer; return MSD_SUCCESS; }
msd_error_t msd_WriteAsync(msd_transfer_t *xfer) { (void)xfer; return MSD_SUCCESS; }
uint24_t msd_Read(msd_t *msd, uint32_t lba, uint24_t count, void *buffer)
{ (void)msd; (void)lba; memset(buffer, 0, (size_t)count * 512); return count; }
uint24_t msd_Write(msd_t *msd, uint32_t lba, uint24_t count, const void *buffer)
{ (void)msd; (void)lba; (void)buffer; return count; }
uint8_t msd_FindPartitions(msd_t *msd, msd_partition_t *partitions, uint8_t max)
{ (void)msd; (void)partitions; (void)max; return 0; }

uint8_t ti_Open(const char *name, const char *mode)
{ (void)name; (void)mode; return 0; }
int ti_Close(uint8_t handle) { (void)handle; return 1; }
size_t ti_Read(void *data, size_t size, size_t count, uint8_t handle)
{ (void)data; (void)handle; return size * count; }
size_t ti_Write(const void *data, size_t size, size_t count, uint8_t handle)
{ (void)data; (void)handle; return size * count; }
int ti_SetArchiveStatus(uint8_t archive, uint8_t handle)
{ (void)archive; (void)handle; return 1; }
void ti_SetGCBehavior(void (*before)(void), void (*after)(void))
{ (void)before; (void)after; }
int ti_Delete(const char *name) { (void)name; return 1; }
int ti_Seek(int offset, unsigned int origin, uint8_t handle)
{ (void)offset; (void)origin; (void)handle; return 0; }

void os_SetCursorPos(uint8_t row, uint8_t col) { (void)row; (void)col; }
void os_PutStrFull(char *str) { (void)str; }
void os_NewLine(void) {}
void os_ClrHome(void) {}
uint8_t os_GetCSC(void) { return sk_Clear; /* always "exit" so loops terminate */ }

void gfx_Begin(void) {}
void gfx_End(void) {}
void gfx_SwapDraw(void) {}
void gfx_Wait(void) {}
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
