#ifndef CINEMA_MSD_UTIL_H
#define CINEMA_MSD_UTIL_H

#include <msddrvce.h>

/* Returns a short static string describing error, e.g. "usb failed".
 * Never returns NULL. */
const char *msd_error_string(msd_error_t error);

#endif
