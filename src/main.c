#include "cinema.h"
#include "cin2.h"
#include "decode.h"
#include "player_v1.h"
#include "player_v2.h"

#include <fileioc.h>
#include <msddrvce.h>
#include <tice.h>
#include <usbdrvce.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void putstr(const char *str)
{
    os_PutStrFull((char *)str);
    os_NewLine();
}

usb_error_t handleUsbEvent(usb_event_t event, void *event_data,
                            usb_callback_data_t *global)
{
    switch (event)
    {
        case USB_DEVICE_DISCONNECTED_EVENT:
            putstr("usb device disconnected");
            if (global->usb)
                msd_Close(&global->msd);
            global->usb = NULL;
            break;
        case USB_DEVICE_CONNECTED_EVENT:
            putstr("usb device connected");
            return usb_ResetDevice(event_data);
        case USB_DEVICE_ENABLED_EVENT:
            global->usb = event_data;
            putstr("usb device enabled");
            break;
        case USB_DEVICE_DISABLED_EVENT:
            putstr("usb device disabled");
            return USB_RETRY_INIT;
        default:
            break;
    }

    return USB_SUCCESS;
}

/* Prompts "resume where you left off?" and, if the user says yes and a
 * valid v1 resume record exists, returns the saved palette LBA.
 * Otherwise returns 0 (start of movie). Mirrors the original Cinema's
 * inline resume menu exactly -- same appvar, same 4-byte raw-LBA
 * format, same prompt -- so existing v1 resume state keeps working. */
static uint32_t v1_resume_menu(void)
{
    uint32_t start_lba = 0;
    uint8_t var = ti_Open(APPVAR_V1, "r+");

    if (var) {
        putstr("Resume where you left off?");
        putstr("Other - YES        0 - NO");

        while (1) {
            uint8_t key = os_GetCSC();
            if (key) {
                if (key != sk_0) {
                    ti_Read(&start_lba, sizeof(uint32_t), 1, var);
                    ti_SetGCBehavior(NULL, NULL);
                    ti_SetArchiveStatus(1, var);
                }
                break;
            }
        }
        ti_Close(var);
    }

    return start_lba;
}

/* Same idea as v1_resume_menu but for the v2 (CIN2) resume record. Only
 * offers to resume if a record exists, parses successfully, and was
 * saved against a movie with the same frame count -- otherwise a stale
 * or mismatched record is silently discarded and playback starts from
 * frame 0, rather than risking seeking into a differently-encoded
 * movie. */
static uint32_t v2_resume_menu(const cin2_header_t *header)
{
    uint8_t var;
    cin2_resume_t resume;
    bool have_resume = false;

    var = ti_Open(APPVAR_V2, "r+");
    if (var) {
        uint8_t raw[CIN2_RESUME_BYTES];

        if (ti_Read(raw, 1, CIN2_RESUME_BYTES, var) == CIN2_RESUME_BYTES
            && cin2_parse_resume_record(raw, &resume)
            && resume.frame_count == header->frame_count
            && resume.last_presented_frame + 1 < header->frame_count) {
            have_resume = true;
        }
        ti_Close(var);
    }

    if (!have_resume) {
        return 0;
    }

    putstr("Resume where you left off?");
    putstr("Other - YES        0 - NO");

    while (1) {
        uint8_t key = os_GetCSC();
        if (key) {
            if (key == sk_0) {
                return 0;
            }
            return resume.last_presented_frame + 1;
        }
    }
}

int main(void)
{
    static char buffer[212];
    static global_t global;
    static uint8_t header_sector[BLOCK_SIZE];
    usb_error_t usberr;
    msd_error_t msderr;
    msd_info_t msdinfo;
    cin2_header_t v2_header;
    bool is_v2 = false;
    bool player_ok;

    memset(&global, 0, sizeof(global_t));
    os_SetCursorPos(1, 0);

    /* usb initialization loop; waits for something to be plugged in */
    do
    {
        global.usb = NULL;

        usberr = usb_Init(handleUsbEvent, &global, NULL, USB_DEFAULT_INIT_FLAGS);
        if (usberr != USB_SUCCESS)
        {
            putstr("usb init error.");
            goto usb_error;
        }

        while (usberr == USB_SUCCESS)
        {
            if (global.usb != NULL)
                break;

            if (os_GetCSC())
            {
                putstr("exiting cinema, press a key");
                goto usb_error;
            }

            usberr = usb_WaitForInterrupt();
        }
    } while (usberr == USB_RETRY_INIT);

    if (usberr != USB_SUCCESS)
    {
        putstr("usb enable error.");
        goto usb_error;
    }

    msderr = msd_Open(&global.msd, global.usb);
    if (msderr != MSD_SUCCESS)
    {
        putstr("failed opening msd");
        goto usb_error;
    }

    putstr("opened msd");

    msderr = msd_Info(&global.msd, &msdinfo);
    if (msderr != MSD_SUCCESS)
    {
        putstr("error getting msd info");
        goto msd_error;
    }

    sprintf(buffer, "block size: %u bytes", (uint24_t)msdinfo.bsize);
    putstr(buffer);
    sprintf(buffer, "num blocks: %u", (uint24_t)msdinfo.bnum);
    putstr(buffer);

    if (msdinfo.bsize != BLOCK_SIZE)
    {
        putstr("unsupported block size");
        goto msd_error;
    }

    if (msd_Read(&global.msd, 0, 1, header_sector) != 1)
    {
        putstr("error reading drive header");
        goto msd_error;
    }

    is_v2 = cin2_has_magic(header_sector);
    if (is_v2)
    {
        if (!cin2_parse_header(header_sector, &v2_header))
        {
            putstr("corrupt or unsupported CIN2 header");
            goto msd_error;
        }
        if (v2_header.width != CINEMA_V2_WIDTH
            || v2_header.height != CINEMA_V2_HEIGHT)
        {
            putstr("unsupported CIN2 resolution");
            goto msd_error;
        }
        if (!cin2_frame_count_fits_drive(v2_header.frame_count, msdinfo.bnum))
        {
            putstr("movie extends beyond drive capacity");
            goto msd_error;
        }

        os_ClrHome();
        putstr("Cinema v2 (CIN2) detected");
        {
            uint32_t start_frame = v2_resume_menu(&v2_header);
            player_ok = player_v2_run(&global, &v2_header, start_frame);
        }
    }
    else
    {
        os_ClrHome();
        putstr("Cinema v1 (legacy) drive detected");
        {
            uint32_t start_lba = v1_resume_menu();
            player_ok = player_v1_run(&global, start_lba);
        }
    }

    msd_Close(&global.msd);
    usb_Cleanup();

    if (!player_ok) {
        while (!os_GetCSC());
    }

    return 0;

msd_error:
    msd_Close(&global.msd);
    usb_Cleanup();

    while (!os_GetCSC());

    return 0;

usb_error:
    usb_Cleanup();

    while (!os_GetCSC());

    return 0;
}
