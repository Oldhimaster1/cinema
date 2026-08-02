#ifndef CINEMA_H
#define CINEMA_H

/* Shared types/constants used by main.c and both players. */

typedef struct global global_t;
#define usb_callback_data_t global_t

#include <usbdrvce.h>
#include <msddrvce.h>
#include <stdbool.h>
#include <stdint.h>

#define BLOCK_SIZE 512

/* v1 (legacy) resume appvar: 4-byte raw LBA, unchanged from the original
 * Cinema so existing v1 resume state keeps working. */
#define APPVAR_V1 "SSCINEMA"
/* v2 resume appvar: see docs/CIN2_FORMAT.md for the record layout. Kept
 * separate from APPVAR_V1 so the two formats' resume state never collide. */
#define APPVAR_V2 "SSCINEV2"

struct global
{
    usb_device_t usb;
    msd_t msd;
};

enum { USB_RETRY_INIT = USB_USER_ERROR };

/* Prints str followed by a newline via the OS text routines. */
void putstr(const char *str);

/* Prints a human-readable line describing an msd_error_t, prefixed with
 * context (e.g. "image"), e.g. "usb failed (image)". */
void put_msd_error(msd_error_t error, const char *context);

usb_error_t handleUsbEvent(usb_event_t event, void *event_data,
                            usb_callback_data_t *global);

#endif
