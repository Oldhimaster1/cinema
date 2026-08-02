/* Stub of the real CE-toolchain usbdrvce.h -- see ce_types.h header
 * comment. Signatures below are transcribed from
 * https://github.com/CE-Programming/toolchain src/usbdrvce/usbdrvce.h. */
#ifndef CINEMA_TEST_USBDRVCE_H
#define CINEMA_TEST_USBDRVCE_H

#include "ce_types.h"
#include <stdint.h>

typedef void *usb_device_t;

typedef enum {
    USB_SUCCESS = 0,
    USB_IGNORE,
    USB_USER_ERROR,
    USB_NO_DEVICE,
    USB_ERROR_SYSTEM,
    USB_ERROR_INVALID_PARAM,
    USB_ERROR_SCHEDULE_FULL,
    USB_ERROR_NO_MEMORY,
    USB_ERROR_NO_DEVICE,
    USB_ERROR_TIMEOUT,
    USB_ERROR_FAILED,
    USB_ERROR_INVALID_MSG,
    USB_ERROR_NOT_SUPPORTED
} usb_error_t;

typedef enum {
    USB_ROLE_CHANGED_EVENT = 1,
    USB_DEVICE_DISCONNECTED_EVENT,
    USB_DEVICE_CONNECTED_EVENT,
    USB_DEVICE_DISABLED_EVENT,
    USB_DEVICE_ENABLED_EVENT
} usb_event_t;

#define USB_DEFAULT_INIT_FLAGS 0u

#ifndef usb_callback_data_t
typedef void usb_callback_data_t;
#endif

typedef usb_error_t (*usb_event_callback_t)(usb_event_t event,
                                             void *event_data,
                                             usb_callback_data_t *data);

usb_error_t usb_Init(usb_event_callback_t handler, usb_callback_data_t *data,
                      const void *descriptors, uint32_t flags);
usb_error_t usb_WaitForInterrupt(void);
usb_error_t usb_ResetDevice(usb_device_t dev);
void usb_HandleEvents(void);
void usb_Cleanup(void);

#endif
