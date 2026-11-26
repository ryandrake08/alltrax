/*
 * transport.c — USB HID transport wrapping hidapi
 *
 * Uses the hidraw backend on Linux (works with /dev/hidraw* in LXC
 * containers without needing /dev/bus/usb passthrough).
 */

#define _POSIX_C_SOURCE 200809L
#include "internal.h"
#include <errno.h>
#include <fcntl.h>
#include <hidapi.h>
#include <stdlib.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_TIMEOUT_MS 750
#define HID_WRITE_ATTEMPTS 4
#define DRAIN_QUIET_MS 32
#define DRAIN_MAX_REPORTS 16
#define RECONNECT_POLL_MS 250

static const char* lock_path = "/tmp/alltrax.lock";

void transport_set_lock_path(const char* path)
{
    lock_path = path;
}

/* Take the exclusive process lock. A read-only descriptor can hold a flock(),
 * so a lock file created by another user still works. */
static alltrax_error acquire_lock(int* fd_out)
{
    int fd = open(lock_path, O_RDONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0666);
    if (fd < 0)
        return ALLTRAX_ERR_LOCK;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        alltrax_error rc = errno == EWOULDBLOCK ? ALLTRAX_ERR_BUSY : ALLTRAX_ERR_LOCK;
        close(fd);
        return rc;
    }
    *fd_out = fd;
    return ALLTRAX_OK;
}

static bool is_xct_device(const struct hid_device_info* d)
{
    return d->vendor_id == ALLTRAX_VID && d->product_id == ALLTRAX_PID_XCT;
}

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
    int lock_fd;
    alltrax_error lock_rc = acquire_lock(&lock_fd);
    if (lock_rc)
        return lock_rc;

    /* Use enumeration on all supported hidapi versions. An explicit path must
     * match a supported entry before it is opened or receives any traffic. */
    struct hid_device_info* devs = hid_enumerate(ALLTRAX_VID, 0);
    alltrax_error rc = ALLTRAX_ERR_NO_DEVICE;
    for (struct hid_device_info* d = devs; d; d = d->next) {
        if (!d->path || (device_path && strcmp(d->path, device_path) != 0))
            continue;
        if (!is_xct_device(d)) {
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
        ctrl->device_path = strdup(d->path);
        if (!ctrl->device_path) {
            alltrax_close(ctrl);
            rc = ALLTRAX_ERR_NO_MEMORY;
            break;
        }
        ctrl->vid = d->vendor_id;
        ctrl->pid = d->product_id;
        ctrl->allow_writes = allow_writes;
        rc = identify_controller(ctrl);
        if (!rc) {
            ctrl->locked = true;
            ctrl->lock_fd = lock_fd;
            *out = ctrl;
            break;
        }
        alltrax_close(ctrl);
        if (device_path) break;
    }
    hid_free_enumeration(devs);
    if (rc)
        close(lock_fd);
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
    if (ctrl->locked)
        close(ctrl->lock_fd);

    free(ctrl->device_path);
    free(ctrl);
}

/* Discovery needs only the model and serial. Use a single bounded read
 * probe per candidate, with no retries, so stale nodes do not stall a scan. */
static bool probe_identity(const char* path, int timeout_ms, char model[16],
    uint32_t* serial)
{
    hid_device* hid = hid_open_path(path);
    if (!hid) return false;

    uint8_t packet[PACKET_SIZE];
    const uint8_t size = SERIAL_NUMBER_OFFSET + 4;
    build_read_request(ADDR_CONTROLLER_INFO, size, packet);
    int n = hid_write(hid, packet, sizeof(packet));
    if (n == PACKET_SIZE)
        n = hid_read_timeout(hid, packet, sizeof(packet), timeout_ms);
    else
        n = -1;
    hid_close(hid);
    if (n != PACKET_SIZE) return false;

    uint8_t result, length, data[MAX_PAYLOAD];
    if (parse_response(packet, RESPONSE_TYPE_RW, &result, &length, data) ||
        result != RESULT_PASS || length != size)
        return false;
    get_string(data, 15, model, 16);
    if (detect_controller_type(model) != ALLTRAX_CONTROLLER_XCT)
        return false;
    *serial = get_le32(data + SERIAL_NUMBER_OFFSET);
    return true;
}

alltrax_error alltrax_list_devices(const char* path, alltrax_device_visitor visit,
    void* user, size_t* count_out)
{
    if (!count_out) return ALLTRAX_ERR_INVALID_ARG;
    *count_out = 0;
    int lock_fd;
    alltrax_error rc = acquire_lock(&lock_fd);
    if (rc) return rc;
    size_t count = 0;
    struct hid_device_info* devs = hid_enumerate(ALLTRAX_VID, 0);
    for (const struct hid_device_info* d = devs; d; d = d->next) {
        if (!d->path || !is_xct_device(d) || (path && strcmp(path, d->path) != 0))
            continue;
        char model[16];
        uint32_t serial;
        if (probe_identity(d->path, DEFAULT_TIMEOUT_MS, model, &serial)) {
            if (visit) visit(d->path, model, serial, user);
            count++;
        }
    }
    hid_free_enumeration(devs);
    close(lock_fd);
    *count_out = count;
    return ALLTRAX_OK;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

alltrax_error alltrax_wait_for_reconnect(alltrax_controller* old,
    unsigned timeout_ms)
{
    if (!old || !old->reset_sent) {
        if (old) set_error_detail(old, "No reset was sent through this handle");
        return ALLTRAX_ERR_INVALID_ARG;
    }
    int64_t now = monotonic_ms();
    if (now < 0 || !old->hid || !old->device_path) return ALLTRAX_ERR_TIMEOUT;
    int64_t deadline = now + timeout_ms;
    bool disconnected = false;
    bool has_serial = old->reset_serial != 0 && old->reset_serial != UINT32_MAX;

    while ((now = monotonic_ms()) >= 0 && now < deadline) {
        uint8_t ignored[PACKET_SIZE];
        if (!disconnected && hid_read_timeout(old->hid, ignored, sizeof(ignored), 0) < 0)
            disconnected = true;

        bool old_listed = false, found = false;
        struct hid_device_info* devs = hid_enumerate(ALLTRAX_VID, 0);
        for (const struct hid_device_info* d = devs; d; d = d->next) {
            if (!d->path || !is_xct_device(d)) continue;
            bool same_path = strcmp(d->path, old->device_path) == 0;
            old_listed |= same_path;
            if (!same_path && (!disconnected || !has_serial)) continue;

            now = monotonic_ms();
            if (now < 0 || now >= deadline) break;
            int64_t remaining = deadline - now;
            int wait_ms = remaining < DEFAULT_TIMEOUT_MS ? (int)remaining : DEFAULT_TIMEOUT_MS;
            char model[16];
            uint32_t serial;
            bool match = probe_identity(d->path, wait_ms, model, &serial) &&
                serial == old->reset_serial && strcmp(model, old->reset_model) == 0;
            if (match && disconnected) {
                found = true;
                break;
            }
            /* The host may recover a rebooting controller in place, keeping
             * its path and the old handle; it stops answering meanwhile. */
            if (!match) disconnected = true;
        }
        hid_free_enumeration(devs);
        if (found) return ALLTRAX_OK;
        disconnected |= !old_listed;

        now = monotonic_ms();
        if (now < 0 || now >= deadline) break;
        int64_t delay_ms = deadline - now;
        if (delay_ms > RECONNECT_POLL_MS) delay_ms = RECONNECT_POLL_MS;
        struct timespec delay = { .tv_nsec = (long)delay_ms * 1000000L };
        nanosleep(&delay, NULL);
    }
    set_error_detail(old, "Controller did not reappear within %u ms", timeout_ms);
    return ALLTRAX_ERR_TIMEOUT;
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

    /* Reset has no echo and a failed send could already have rebooted it. */
    bool reset = data[0] == REPORT_ID_SPECIAL && data[5] == 0 &&
                 data[6] == 0 && data[7] == FUNC_RESET;
    int written = hid_write(ctrl->hid, data, PACKET_SIZE);
    for (int attempt = 1; written < 0 && !reset && attempt < HID_WRITE_ATTEMPTS;
         attempt++) {
        struct timespec delay = { .tv_nsec = 125000000 };
        while (nanosleep(&delay, &delay) == -1 && errno == EINTR) {}
        written = hid_write(ctrl->hid, data, PACKET_SIZE);
    }
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

/* Reports carry no transaction ID. Wait for one quiet USB polling interval
 * and bound the drain in case the device keeps sending unsolicited reports. */
alltrax_error transport_drain(alltrax_controller* ctrl)
{
    uint8_t discarded[PACKET_SIZE];
    for (int i = 0; i < DRAIN_MAX_REPORTS; i++) {
        int n = hid_read_timeout(ctrl->hid, discarded, sizeof(discarded),
                                 DRAIN_QUIET_MS);
        if (n == 0) return ALLTRAX_OK;
        if (n < 0) {
            set_error_detail(ctrl, "USB read failed while draining replies: %ls",
                             hid_error(ctrl->hid));
            return ALLTRAX_ERR_USB;
        }
    }
    set_error_detail(ctrl, "USB input did not become quiet after draining replies");
    return ALLTRAX_ERR_PROTOCOL;
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
    case ALLTRAX_ERR_INVALID_ARG: return "Invalid argument";
    case ALLTRAX_ERR_NO_DEVICE:   return "No Alltrax device found";
    case ALLTRAX_ERR_BLOCKED:     return "Write blocked (read-only mode)";
    case ALLTRAX_ERR_NO_MEMORY:   return "Memory allocation failed";
    case ALLTRAX_ERR_UNSUPPORTED: return "Unsupported device (requires Alltrax XCT family, USB 23d4:0002)";
    case ALLTRAX_ERR_BOOTLOADER:  return "Controller is in bootloader mode";
    case ALLTRAX_ERR_BUSY:        return "Controller in use by another alltrax process";
    case ALLTRAX_ERR_LOCK:        return "Cannot use the alltrax lock file (/tmp/alltrax.lock)";
    }
    return "Unknown error";
}

const char* alltrax_last_error_detail(const alltrax_controller* ctrl)
{
    if (!ctrl) return "";
    return ctrl->error_detail;
}
