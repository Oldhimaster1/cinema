/* Stub of the real CE-toolchain msddrvce.h -- see ce_types.h header
 * comment. msd_transfer_t field layout is transcribed verbatim from
 * https://github.com/CE-Programming/toolchain src/msddrvce/msddrvce.h
 * since src/player_v2.c relies on its exact field names. */
#ifndef CINEMA_TEST_MSDDRVCE_H
#define CINEMA_TEST_MSDDRVCE_H

#include "ce_types.h"
#include "usbdrvce.h"
#include <stdint.h>

typedef struct { uint8_t opaque[64]; } msd_t;

typedef enum {
    MSD_SUCCESS = 0,
    MSD_ERROR_INVALID_PARAM,
    MSD_ERROR_USB_FAILED,
    MSD_ERROR_SCSI_FAILED,
    MSD_ERROR_SCSI_CHECK_CONDITION,
    MSD_ERROR_NOT_SUPPORTED,
    MSD_ERROR_INVALID_DEVICE,
    MSD_ERROR_TIMEOUT
} msd_error_t;

typedef struct {
    uint32_t bsize;
    uint32_t bnum;
} msd_info_t;

typedef struct msd_transfer {
    msd_t *msd;
    uint32_t lba;
    void *buffer;
    uint24_t count;
    void (*callback)(msd_error_t error, struct msd_transfer *xfer);
    void *userptr;
    uint8_t priv[76];
} msd_transfer_t;

typedef struct { uint8_t opaque[16]; } msd_partition_t;

msd_error_t msd_Open(msd_t *msd, usb_device_t usb);
void msd_Close(msd_t *msd);
msd_error_t msd_Reset(msd_t *msd);
msd_error_t msd_Info(msd_t *msd, msd_info_t *info);
msd_error_t msd_ReadAsync(msd_transfer_t *xfer);
msd_error_t msd_WriteAsync(msd_transfer_t *xfer);
uint24_t msd_Read(msd_t *msd, uint32_t lba, uint24_t count, void *buffer);
uint24_t msd_Write(msd_t *msd, uint32_t lba, uint24_t count, const void *buffer);
uint8_t msd_FindPartitions(msd_t *msd, msd_partition_t *partitions, uint8_t max);

#endif
