#ifndef CINEMA_MSD_UTIL_H
#define CINEMA_MSD_UTIL_H

/* Must pull in msddrvce.h via cinema.h, not directly: usbdrvce.h
 * defines usb_callback_data_t as void unless it's already defined
 * (#ifndef-guarded) at the point it's first included, and cinema.h is
 * what defines it as global_t. If a translation unit includes this
 * header before cinema.h, msddrvce.h's own include of usbdrvce.h would
 * lock the guard in as "void" first, and cinema.h's later #define
 * would then just be a harmless-looking redefinition that never
 * actually reaches usb_Init's already-parsed prototype in that file.
 * Routing through cinema.h here means the override always happens
 * first, regardless of which of our headers a .c file includes first. */
#include "cinema.h"

/* Returns a short static string describing error, e.g. "usb failed".
 * Never returns NULL. */
const char *msd_error_string(msd_error_t error);

#endif
