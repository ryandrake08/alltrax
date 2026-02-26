/*
 * transport.c — USB HID transport wrapping hidapi
 *
 * Uses the hidraw backend on Linux (works with /dev/hidraw* in LXC
 * containers without needing /dev/bus/usb passthrough).
 */

#include "internal.h"
#include <hidapi.h>
#include <stdlib.h>

/* Default read timeout in milliseconds */
#define DEFAULT_TIMEOUT_MS 1000

alltrax_error alltrax_init(void)
{
    return hid_init() == 0 ? ALLTRAX_OK : ALLTRAX_ERR_USB;
}

void alltrax_exit(void)
{
    hid_exit();
}

alltrax_error alltrax_open(alltrax_controller** out,
    const char* device_path, bool allow_writes)
{
    if (!out || *out)
        return ALLTRAX_ERR_INVALID_ARG;

    hid_device* hid = NULL;
    uint16_t pid = 0;

    if (device_path) {
        /* Open specific device by OS path */
        hid = hid_open_path(device_path);
        if (!hid)
            return ALLTRAX_ERR_NO_DEVICE;

        /* Determine PID by enumerating Alltrax devices and matching path.
         * hid_get_device_info() requires hidapi >= 0.12; enumerate works
         * with all versions. */
        struct hid_device_info* devs = hid_enumerate(ALLTRAX_VID, 0);
        for (struct hid_device_info* d = devs; d; d = d->next) {
            if (strcmp(d->path, device_path) == 0) {
                pid = d->product_id;
                break;
            }
        }
        hid_free_enumeration(devs);
    } else {
        /* Auto-detect: try XCT (PID 0x0002) first, then SPM (PID 0x0001) */
        pid = ALLTRAX_PID_XCT;
        hid = hid_open(ALLTRAX_VID, ALLTRAX_PID_XCT, NULL);
        if (!hid) {
            pid = ALLTRAX_PID_SPM;
            hid = hid_open(ALLTRAX_VID, ALLTRAX_PID_SPM, NULL);
        }
    }

    if (!hid)
        return ALLTRAX_ERR_NO_DEVICE;

    alltrax_controller* ctrl = calloc(1, sizeof(*ctrl));
    if (!ctrl) {
        hid_close(hid);
        return ALLTRAX_ERR_NO_MEMORY;
    }

    ctrl->hid = hid;
    ctrl->pid = pid;
    ctrl->allow_writes = allow_writes;
    *out = ctrl;
    return ALLTRAX_OK;
}

void alltrax_close(alltrax_controller* ctrl)
{
    if (!ctrl)
        return;

    if (ctrl->hid) {
        hid_close(ctrl->hid);
        ctrl->hid = NULL;
    }

    free(ctrl);
}

alltrax_error transport_write(alltrax_controller* ctrl,
    const uint8_t data[PACKET_SIZE])
{
    if (!ctrl->hid) {
        set_error_detail(ctrl, "Device not open");
        return ALLTRAX_ERR_USB;
    }

    /* Safety: block writes if not allowed */
    if (!ctrl->allow_writes) {
        uint8_t report_id = data[0];
        if (report_id == REPORT_ID_WRITE || report_id == REPORT_ID_SPECIAL) {
            set_error_detail(ctrl,
                "Write blocked: Report ID 0x%02X not allowed in read-only mode",
                report_id);
            return ALLTRAX_ERR_BLOCKED;
        }
    }

    int written = hid_write(ctrl->hid, data, PACKET_SIZE);
    if (written < 0) {
        set_error_detail(ctrl, "USB write failed: %ls",
                         hid_error(ctrl->hid));
        return ALLTRAX_ERR_USB;
    }

    return ALLTRAX_OK;
}

alltrax_error transport_read(alltrax_controller* ctrl,
    uint8_t buf[PACKET_SIZE])
{
    if (!ctrl->hid) {
        set_error_detail(ctrl, "Device not open");
        return ALLTRAX_ERR_USB;
    }

    int n = hid_read_timeout(ctrl->hid, buf, PACKET_SIZE, DEFAULT_TIMEOUT_MS);
    if (n < 0) {
        set_error_detail(ctrl, "USB read failed: %ls",
                         hid_error(ctrl->hid));
        return ALLTRAX_ERR_USB;
    }
    if (n == 0) {
        set_error_detail(ctrl, "USB read timed out after %dms", DEFAULT_TIMEOUT_MS);
        return ALLTRAX_ERR_TIMEOUT;
    }

    return ALLTRAX_OK;
}

const char* alltrax_strerror(alltrax_error err)
{
    switch (err) {
    case ALLTRAX_OK:              return "Success";
    case ALLTRAX_ERR_USB:         return "USB communication failure";
    case ALLTRAX_ERR_TIMEOUT:     return "No response from device";
    case ALLTRAX_ERR_PROTOCOL:    return "Malformed response";
    case ALLTRAX_ERR_DEVICE_FAIL: return "Device reported failure";
    case ALLTRAX_ERR_VERIFY:      return "Write verification failed";
    case ALLTRAX_ERR_FIRMWARE:    return "Unknown firmware version";
    case ALLTRAX_ERR_FLASH:       return "FLASH operation failed";
    case ALLTRAX_ERR_NOT_RUN:     return "Controller not in RUN mode";
    case ALLTRAX_ERR_INVALID_ARG: return "Invalid argument";
    case ALLTRAX_ERR_ADDRESS:     return "Address out of range";
    case ALLTRAX_ERR_NO_DEVICE:   return "No Alltrax device found";
    case ALLTRAX_ERR_BLOCKED:     return "Write blocked (read-only mode)";
    case ALLTRAX_ERR_NO_MEMORY:   return "Memory allocation failed";
    }
    return "Unknown error";
}

const char* alltrax_last_error_detail(const alltrax_controller* ctrl)
{
    if (!ctrl) return "";
    return ctrl->error_detail;
}
