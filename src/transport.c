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

    /* Use enumeration on all supported hidapi versions. An explicit path must
     * match a supported entry before it is opened or receives any traffic. */
    struct hid_device_info* devs = hid_enumerate(ALLTRAX_VID, 0);
    alltrax_error rc = ALLTRAX_ERR_NO_DEVICE;
    for (struct hid_device_info* d = devs; d; d = d->next) {
        if (!d->path || (device_path && strcmp(d->path, device_path) != 0))
            continue;
        if (d->vendor_id != ALLTRAX_VID || d->product_id != ALLTRAX_PID_XCT) {
            rc = ALLTRAX_ERR_UNSUPPORTED;
            if (device_path) break;
            continue;
        }

        hid_device* hid = hid_open_path(d->path);
        if (!hid) {
            rc = ALLTRAX_ERR_NO_DEVICE;
            if (device_path) break;
            continue;
        }
        alltrax_controller* ctrl = calloc(1, sizeof(*ctrl));
        if (!ctrl) {
            hid_close(hid);
            rc = ALLTRAX_ERR_NO_MEMORY;
            break;
        }

        ctrl->hid = hid;
        ctrl->vid = d->vendor_id;
        ctrl->pid = d->product_id;
        ctrl->allow_writes = allow_writes;
        rc = identify_controller(ctrl);
        if (!rc) {
            *out = ctrl;
            break;
        }
        alltrax_close(ctrl);
        if (device_path) break;
    }
    hid_free_enumeration(devs);
    return rc;
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
    alltrax_error rc = require_xct_usb(ctrl);
    if (rc) return rc;
    if (data[0] == REPORT_ID_WRITE || data[0] == REPORT_ID_SPECIAL) {
        rc = require_xct_controller(ctrl);
        if (rc) return rc;
    }

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
    if (written != PACKET_SIZE) {
        set_error_detail(ctrl,
            "USB write length mismatch for report 0x%02X: expected %d bytes, wrote %d",
            data[0], PACKET_SIZE, written);
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
    if (n != PACKET_SIZE) {
        set_error_detail(ctrl, "USB read length mismatch: expected %d bytes, got %d",
                         PACKET_SIZE, n);
        return ALLTRAX_ERR_PROTOCOL;
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
    case ALLTRAX_ERR_NO_DEVICE:   return "No Alltrax device found";
    case ALLTRAX_ERR_BLOCKED:     return "Write blocked (read-only mode)";
    case ALLTRAX_ERR_NO_MEMORY:   return "Memory allocation failed";
    case ALLTRAX_ERR_UNSUPPORTED: return "Unsupported device (requires Alltrax XCT family, USB 23d4:0002)";
    }
    return "Unknown error";
}

const char* alltrax_last_error_detail(const alltrax_controller* ctrl)
{
    if (!ctrl) return "";
    return ctrl->error_detail;
}
