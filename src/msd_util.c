#include "msd_util.h"
#include "cinema.h"

#include <stdio.h>

void put_msd_error(msd_error_t error, const char *context)
{
    char buffer[64];

    sprintf(buffer, "%s (%s)", msd_error_string(error), context);
    putstr(buffer);
}

const char *msd_error_string(msd_error_t error)
{
    switch (error) {
        case MSD_SUCCESS:
            return "success";
        case MSD_ERROR_INVALID_PARAM:
            return "invalid param";
        case MSD_ERROR_USB_FAILED:
            return "usb failed";
        case MSD_ERROR_SCSI_FAILED:
            return "scsi failed";
        case MSD_ERROR_SCSI_CHECK_CONDITION:
            return "scsi check condition";
        case MSD_ERROR_NOT_SUPPORTED:
            return "not supported";
        case MSD_ERROR_INVALID_DEVICE:
            return "invalid device";
        case MSD_ERROR_TIMEOUT:
            return "timeout";
        default:
            return "unknown error";
    }
}
