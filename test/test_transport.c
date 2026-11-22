/* Real connection/transport code against fake hidapi; no USB library linked. */
#include "helpers.h"
#include "internal.h"
#include <hidapi.h>

int test_pass_count;
int test_fail_count;

struct hid_device_ { size_t index; };

static struct {
    struct hid_device_info devs[3];
    struct hid_device_ handles[3];
    const char* models[3];
    bool fail_open;
    size_t count;
    unsigned enumerations, frees, opens, closes, reads, writes, mutations;
    unsigned legacy_opens;
    size_t opened_index;
    uint8_t request[PACKET_SIZE], mode;
    uint32_t program_rev;
    int model_reply_len; /* -1: timeout, -2: USB error; otherwise declared size */
    int write_result, read_result;
} mock;

static void mock_reset(void)
{
    memset(&mock, 0, sizeof(mock));
    mock.program_rev = 5005;
    mock.model_reply_len = 15;
    mock.write_result = mock.read_result = PACKET_SIZE;
}

static void add_device(const char* path, uint16_t vid, uint16_t pid, const char* model)
{
    size_t i = mock.count++;
    mock.devs[i].path = (char*)path;
    mock.devs[i].vendor_id = vid;
    mock.devs[i].product_id = pid;
    mock.handles[i].index = i;
    mock.models[i] = model;
    if (i > 0) mock.devs[i - 1].next = &mock.devs[i];
}

int hid_init(void) { return 0; }
int hid_exit(void) { return 0; }

struct hid_device_info* hid_enumerate(unsigned short vid, unsigned short pid)
{
    (void)vid;
    (void)pid;
    mock.enumerations++;
    /* Deliberately return all entries, so metadata validation is exercised. */
    return mock.count ? mock.devs : NULL;
}

void hid_free_enumeration(struct hid_device_info* devs)
{
    (void)devs;
    mock.frees++;
}

hid_device* hid_open_path(const char* path)
{
    mock.opens++;
    if (mock.fail_open) return NULL;
    for (size_t i = 0; i < mock.count; i++) {
        if (mock.devs[i].path && strcmp(path, mock.devs[i].path) == 0) {
            mock.opened_index = i;
            return &mock.handles[i];
        }
    }
    /* The OS could open an unenumerated path; the library must refuse it. */
    return &mock.handles[0];
}

hid_device* hid_open(unsigned short vid, unsigned short pid, const wchar_t* serial)
{
    (void)serial;
    mock.legacy_opens++;
    for (size_t i = 0; i < mock.count; i++) {
        if (vid == mock.devs[i].vendor_id && pid == mock.devs[i].product_id)
            return hid_open_path(mock.devs[i].path);
    }
    return NULL;
}

void hid_close(hid_device* hid)
{
    (void)hid;
    mock.closes++;
}

const wchar_t* hid_error(hid_device* hid)
{
    (void)hid;
    return L"simulated error";
}

static uint32_t request_address(void)
{
    const uint8_t* p = mock.request + 4;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

int hid_write(hid_device* hid, const unsigned char* data, size_t size)
{
    (void)hid;
    mock.writes++;
    if (size != PACKET_SIZE) return -1;
    if (mock.write_result != PACKET_SIZE) return mock.write_result;
    memcpy(mock.request, data, size);
    if (data[0] != 0x01) mock.mutations++;
    if (data[0] == 0x02 && request_address() == ADDR_RUN_MODE)
        mock.mode = data[8];
    return (int)size;
}

static uint8_t read_byte(size_t index, uint32_t addr)
{
    if (addr >= ADDR_CONTROLLER_INFO && addr < ADDR_CONTROLLER_INFO + 15) {
        size_t offset = addr - ADDR_CONTROLLER_INFO;
        return offset < strlen(mock.models[index]) ? (uint8_t)mock.models[index][offset] : 0;
    }
    if (addr >= ADDR_BOOT_REV && addr < ADDR_BOOT_REV + 4)
        return (uint8_t)(5002u >> ((addr - ADDR_BOOT_REV) * 8));
    if (addr >= ADDR_PRGM_REV && addr < ADDR_PRGM_REV + 4)
        return (uint8_t)(mock.program_rev >> ((addr - ADDR_PRGM_REV) * 8));
    if (addr == ADDR_RUN_MODE) return mock.mode;
    return 0;
}

static int make_response(hid_device* hid, unsigned char* buf, size_t size)
{
    if (size != PACKET_SIZE) return -1;
    memset(buf, 0, size);
    buf[0] = 0x04;
    buf[1] = mock.request[1];
    buf[4] = 0x01;
    if (mock.request[0] == 0x02) {
        memcpy(buf + 8, mock.request + 8, buf[1]);
        return (int)size;
    }
    uint32_t addr = request_address();
    if (addr == ADDR_CONTROLLER_INFO && mock.request[1] == 15) {
        if (mock.model_reply_len < 0) return mock.model_reply_len == -1 ? 0 : -1;
        buf[1] = (uint8_t)mock.model_reply_len;
    }
    for (size_t i = 0; i < buf[1] && i < MAX_PAYLOAD; i++)
        buf[8 + i] = read_byte(hid->index, addr + (uint32_t)i);
    return (int)size;
}

int hid_read_timeout(hid_device* hid, unsigned char* buf, size_t size, int timeout)
{
    (void)timeout;
    mock.reads++;
    uint8_t reply[PACKET_SIZE];
    int rc = make_response(hid, reply, size);
    if (rc <= 0) return rc;
    int n = mock.read_result;
    /* A short read really leaves the rest of the caller's buffer untouched. */
    if (n > 0) memcpy(buf, reply, (size_t)n < size ? (size_t)n : size);
    return n;
}

static int test_no_devices_and_unknown_paths(void)
{
    mock_reset();
    alltrax_controller* ctrl = NULL;
    ASSERT_EQ(alltrax_open(&ctrl, NULL, true), ALLTRAX_ERR_NO_DEVICE);
    ASSERT_NULL(ctrl);
    add_device("xct", 0x23D4, 2, "XCT48400-DCS");
    ASSERT_EQ(alltrax_open(&ctrl, "unlisted", true), ALLTRAX_ERR_NO_DEVICE);
    ASSERT_NULL(ctrl);
    ASSERT_EQ(mock.opens, 0);
    ASSERT_EQ(mock.legacy_opens, 0);
    ASSERT_EQ(mock.writes, 0);
    ASSERT_EQ(mock.frees, 2);
    return 0;
}

static int test_explicit_paths_require_supported_usb_identity(void)
{
    const uint16_t ids[][2] = { { 0x1234, 2 }, { 0x23D4, 1 }, { 0x23D4, 99 }, { 0, 0 } };
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        for (int writes = 0; writes <= 1; writes++) {
            mock_reset();
            add_device("unsupported", ids[i][0], ids[i][1], "XCT48400-DCS");
            alltrax_controller* ctrl = NULL;
            ASSERT_EQ(alltrax_open(&ctrl, "unsupported", writes != 0), ALLTRAX_ERR_UNSUPPORTED);
            ASSERT_NULL(ctrl);
            ASSERT_EQ(mock.opens, 0);
            ASSERT_EQ(mock.writes, 0);
            ASSERT_EQ(mock.frees, 1);
        }
    }
    return 0;
}

static int test_auto_detection_skips_unsupported_devices(void)
{
    mock_reset();
    add_device("spm", 0x23D4, 1, "SPM48400");
    alltrax_controller* ctrl = NULL;
    ASSERT_EQ(alltrax_open(&ctrl, NULL, true), ALLTRAX_ERR_UNSUPPORTED);
    ASSERT_NULL(ctrl);
    ASSERT_EQ(mock.opens, 0);
    ASSERT_EQ(mock.legacy_opens, 0);
    ASSERT_EQ(mock.writes, 0);
    add_device("wrong-vendor", 0x1234, 2, "XCT48400-DCS");
    add_device("xct", 0x23D4, 2, "XCT48400-DCS");
    ASSERT_EQ(alltrax_open(&ctrl, NULL, true), ALLTRAX_OK);
    ASSERT_EQ(mock.opened_index, 2);
    ASSERT_EQ(mock.opens, 1);
    ASSERT_EQ(mock.mutations, 0);
    alltrax_close(ctrl);
    ASSERT_EQ(mock.closes, 1);
    return 0;
}

static int test_supported_models_open_and_read_info(void)
{
    const char* models[] = { "XCT48400-DCS", "SRX48400", "NCT48400" };
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        for (int writes = 0; writes <= 1; writes++) {
            mock_reset();
            add_device("xct", 0x23D4, 2, models[i]);
            alltrax_controller* ctrl = NULL;
            ASSERT_EQ(alltrax_open(&ctrl, writes ? "xct" : NULL, writes != 0), ALLTRAX_OK);
            ASSERT_TRUE(ctrl != NULL);
            ASSERT_EQ(ctrl->vid, 0x23D4);
            ASSERT_EQ(ctrl->pid, 2);
            ASSERT_EQ(ctrl->controller_type, ALLTRAX_CONTROLLER_XCT);
            ASSERT_EQ(ctrl->allow_writes, writes != 0);
            ASSERT_EQ(mock.reads, 1);
            ASSERT_EQ(mock.mutations, 0);
            alltrax_info info;
            ASSERT_EQ(alltrax_get_info(ctrl, &info), ALLTRAX_OK);
            ASSERT_STR_EQ(info.model, models[i]);
            ASSERT_TRUE(info.supported);
            alltrax_close(ctrl);
            ASSERT_EQ(mock.closes, 1);
            ASSERT_EQ(mock.frees, 1);
        }
    }
    return 0;
}

static int test_unsupported_models_fail_and_close(void)
{
    const char* models[] = { "SPM48400", "SR48400", "BMS", "BMS2", "Unknown", "" };
    for (size_t i = 0; i < sizeof(models) / sizeof(models[0]); i++) {
        mock_reset();
        add_device("bad-model", 0x23D4, 2, models[i]);
        alltrax_controller* ctrl = NULL;
        ASSERT_EQ(alltrax_open(&ctrl, "bad-model", true), ALLTRAX_ERR_UNSUPPORTED);
        ASSERT_NULL(ctrl);
        ASSERT_EQ(mock.reads, 1);
        ASSERT_EQ(mock.mutations, 0);
        ASSERT_EQ(mock.closes, 1);
        ASSERT_EQ(mock.frees, 1);
    }
    return 0;
}

static int test_identification_read_failure_closes_device(void)
{
    const int lengths[] = { -2, -1, 0, 14, 16, 57 };
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        mock_reset();
        add_device("xct", 0x23D4, 2, "XCT48400-DCS");
        mock.model_reply_len = lengths[i];
        alltrax_controller* ctrl = NULL;
        alltrax_error expected = lengths[i] == -2 ? ALLTRAX_ERR_USB :
                                 lengths[i] == -1 ? ALLTRAX_ERR_TIMEOUT : ALLTRAX_ERR_PROTOCOL;
        ASSERT_EQ(alltrax_open(&ctrl, "xct", true), expected);
        ASSERT_NULL(ctrl);
        ASSERT_EQ(mock.mutations, 0);
        ASSERT_EQ(mock.closes, 1);
        ASSERT_EQ(mock.frees, 1);
    }
    return 0;
}

static int test_auto_detection_continues_after_unrecognized_model(void)
{
    mock_reset();
    add_device("unknown", 0x23D4, 2, "Unknown");
    add_device("xct", 0x23D4, 2, "XCT48400-DCS");
    alltrax_controller* ctrl = NULL;
    ASSERT_EQ(alltrax_open(&ctrl, NULL, true), ALLTRAX_OK);
    ASSERT_EQ(mock.opened_index, 1);
    ASSERT_EQ(mock.opens, 2);
    ASSERT_EQ(mock.closes, 1);
    ASSERT_EQ(mock.mutations, 0);
    alltrax_close(ctrl);
    ASSERT_EQ(mock.closes, 2);
    return 0;
}

static int test_null_enumeration_path_is_ignored(void)
{
    mock_reset();
    add_device(NULL, 0x23D4, 2, "XCT48400-DCS");
    alltrax_controller* ctrl = NULL;
    ASSERT_EQ(alltrax_open(&ctrl, NULL, true), ALLTRAX_ERR_NO_DEVICE);
    ASSERT_EQ(alltrax_open(&ctrl, "xct", true), ALLTRAX_ERR_NO_DEVICE);
    ASSERT_NULL(ctrl);
    ASSERT_EQ(mock.opens, 0);
    ASSERT_EQ(mock.writes, 0);
    return 0;
}

static int test_info_rejects_changed_model_and_revokes_write_access(void)
{
    mock_reset();
    add_device("xct", 0x23D4, 2, "XCT48400-DCS");
    alltrax_controller* ctrl = NULL;
    ASSERT_EQ(alltrax_open(&ctrl, NULL, true), ALLTRAX_OK);
    mock.models[0] = "SPM48400";
    alltrax_info info;
    ASSERT_EQ(alltrax_get_info(ctrl, &info), ALLTRAX_ERR_UNSUPPORTED);
    ASSERT_EQ(mock.reads, 2); /* Model check, then info; no XCT revision reads. */
    unsigned requests = mock.writes;
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    double value = 1;
    alltrax_write_opts opts = { .skip_fw_check = true };
    ASSERT_EQ(alltrax_write_vars(ctrl, &hpd, &value, 1, &opts), ALLTRAX_ERR_UNSUPPORTED);
    ASSERT_EQ(alltrax_reset_device(ctrl), ALLTRAX_ERR_UNSUPPORTED);
    ASSERT_EQ(mock.writes, requests);
    ASSERT_EQ(mock.mutations, 0);
    alltrax_close(ctrl);
    return 0;
}

static int test_failed_open_and_invalid_output_handle(void)
{
    mock_reset();
    add_device("xct", 0x23D4, 2, "XCT48400-DCS");
    mock.fail_open = true;
    alltrax_controller* ctrl = NULL;
    ASSERT_EQ(alltrax_open(&ctrl, "xct", true), ALLTRAX_ERR_NO_DEVICE);
    ASSERT_NULL(ctrl);
    ASSERT_EQ(mock.reads, 0);
    ASSERT_EQ(mock.closes, 0);
    ASSERT_EQ(mock.frees, 1);
    ASSERT_EQ(alltrax_open(NULL, NULL, true), ALLTRAX_ERR_INVALID_ARG);
    alltrax_controller existing = {0};
    ctrl = &existing;
    ASSERT_EQ(alltrax_open(&ctrl, NULL, true), ALLTRAX_ERR_INVALID_ARG);
    ASSERT_TRUE(ctrl == &existing);
    ASSERT_EQ(mock.enumerations, 1);
    return 0;
}

static int test_xct_ram_write_reset_and_read_only_protection(void)
{
    for (int writes = 0; writes <= 1; writes++) {
        mock_reset();
        add_device("xct", 0x23D4, 2, "XCT48400-DCS");
        alltrax_controller* ctrl = NULL;
        ASSERT_EQ(alltrax_open(&ctrl, NULL, writes != 0), ALLTRAX_OK);
        const alltrax_var_def* fan = alltrax_find_var("Fan_On");
        double value = 1;
        alltrax_error expected = writes ? ALLTRAX_OK : ALLTRAX_ERR_BLOCKED;
        ASSERT_EQ(alltrax_write_vars(ctrl, &fan, &value, 1, NULL), expected);
        ASSERT_EQ(alltrax_reset_device(ctrl), expected);
        ASSERT_EQ(mock.mode, RUN_MODE_RUN);
        ASSERT_EQ(mock.mutations, writes ? 4 : 0); /* CAL, Fan_On, RUN, reset */
        alltrax_close(ctrl);
    }
    return 0;
}

static int test_unknown_firmware_does_not_block_identity_or_reads(void)
{
    mock_reset();
    add_device("xct", 0x23D4, 2, "XCT48400-DCS");
    mock.program_rev = 6001;
    alltrax_controller* ctrl = NULL;
    ASSERT_EQ(alltrax_open(&ctrl, NULL, false), ALLTRAX_OK);
    alltrax_info info;
    ASSERT_EQ(alltrax_get_info(ctrl, &info), ALLTRAX_OK);
    ASSERT_EQ(info.program_rev, 6001);
    ASSERT_FALSE(info.supported);
    const alltrax_var_def* hpd = alltrax_find_var("HPD");
    alltrax_var_value value;
    ASSERT_EQ(alltrax_read_vars(ctrl, &hpd, 1, &value), ALLTRAX_OK);
    ASSERT_EQ(mock.mutations, 0);
    alltrax_close(ctrl);
    return 0;
}

static int test_transport_blocks_mutations_without_verified_identity(void)
{
    mock_reset();
    alltrax_controller ctrl = {
        .hid = &mock.handles[0], .vid = 0x23D4, .pid = 2,
        .controller_type = ALLTRAX_CONTROLLER_XCT, .allow_writes = true,
    };
    for (int bad = 0; bad < 4; bad++) {
        ctrl.vid = bad == 0 ? 0x1234 : 0x23D4;
        ctrl.pid = bad == 1 ? 1 : 2;
        ctrl.controller_type = bad == 2 ? ALLTRAX_CONTROLLER_UNKNOWN :
                               bad == 3 ? ALLTRAX_CONTROLLER_SPM : ALLTRAX_CONTROLLER_XCT;
        uint8_t packet[PACKET_SIZE];
        uint8_t value = 1;
        build_write_request(0x2000F204, &value, 1, packet);
        ASSERT_EQ(transport_write(&ctrl, packet), ALLTRAX_ERR_UNSUPPORTED);
        build_page_erase_request(VARSET_PAGE, packet);
        ASSERT_EQ(transport_write(&ctrl, packet), ALLTRAX_ERR_UNSUPPORTED);
        build_reset_request(packet);
        ASSERT_EQ(transport_write(&ctrl, packet), ALLTRAX_ERR_UNSUPPORTED);
    }
    ASSERT_EQ(mock.writes, 0);
    return 0;
}

static int test_transport_rejects_incomplete_writes(void)
{
    mock_reset();
    alltrax_controller ctrl = {
        .hid = &mock.handles[0], .vid = 0x23D4, .pid = 2,
        .controller_type = ALLTRAX_CONTROLLER_XCT, .allow_writes = true,
    };
    uint8_t packets[4][PACKET_SIZE], value = 1;
    build_read_request(ADDR_CONTROLLER_INFO, 15, packets[0]);
    build_write_request(0x2000F204, &value, 1, packets[1]);
    build_reset_request(packets[2]);
    build_page_erase_request(VARSET_PAGE, packets[3]);
    for (int n = 0; n <= 65; n++) {
        if (n == 64) continue;
        mock.write_result = n;
        for (size_t p = 0; p < 4; p++) {
            unsigned writes = mock.writes;
            ASSERT_EQ(transport_write(&ctrl, packets[p]), ALLTRAX_ERR_USB);
            ASSERT_EQ(mock.writes, writes + 1);
            ASSERT_EQ(mock.reads, 0);
            ASSERT_TRUE(strstr(ctrl.error_detail, "write length mismatch") != NULL);
            ASSERT_TRUE(strstr(ctrl.error_detail, "expected 64 bytes") != NULL);
            char actual[32];
            snprintf(actual, sizeof(actual), "wrote %d", n);
            ASSERT_TRUE(strstr(ctrl.error_detail, actual) != NULL);
        }
    }
    return 0;
}

static int test_transport_rejects_incomplete_reads(void)
{
    mock_reset();
    add_device("xct", 0x23D4, 2, "XCT48400-DCS");
    alltrax_controller ctrl = { .hid = &mock.handles[0] };
    build_read_request(ADDR_CONTROLLER_INFO, 15, mock.request);
    for (int n = 1; n <= 65; n++) {
        if (n == 64) continue;
        mock.read_result = n;
        uint8_t buf[PACKET_SIZE];
        memset(buf, 0xA5, sizeof(buf));
        unsigned reads = mock.reads;
        ASSERT_EQ(transport_read(&ctrl, buf), ALLTRAX_ERR_PROTOCOL);
        ASSERT_EQ(mock.reads, reads + 1);
        ASSERT_EQ(mock.writes, 0);
        ASSERT_TRUE(strstr(ctrl.error_detail, "read length mismatch") != NULL);
        ASSERT_TRUE(strstr(ctrl.error_detail, "expected 64 bytes") != NULL);
        char actual[32];
        snprintf(actual, sizeof(actual), "got %d", n);
        ASSERT_TRUE(strstr(ctrl.error_detail, actual) != NULL);
        if (n < 64) ASSERT_EQ(buf[n], 0xA5);
    }
    return 0;
}

static int test_read_errors_timeouts_and_full_reports_remain_distinct(void)
{
    mock_reset();
    add_device("xct", 0x23D4, 2, "XCT48400-DCS");
    alltrax_controller ctrl = { .hid = &mock.handles[0] };
    build_read_request(ADDR_CONTROLLER_INFO, 15, mock.request);
    uint8_t buf[PACKET_SIZE];
    mock.read_result = -1;
    ASSERT_EQ(transport_read(&ctrl, buf), ALLTRAX_ERR_USB);
    ASSERT_TRUE(strstr(ctrl.error_detail, "USB read failed: simulated error") != NULL);
    mock.read_result = 0;
    ASSERT_EQ(transport_read(&ctrl, buf), ALLTRAX_ERR_TIMEOUT);
    ASSERT_TRUE(strstr(ctrl.error_detail, "timed out after 1000ms") != NULL);
    mock.read_result = 64;
    ASSERT_EQ(transport_read(&ctrl, buf), ALLTRAX_OK);
    ASSERT_EQ(buf[0], 0x04);
    ASSERT_EQ(buf[1], 15);
    ASSERT_MEM_EQ(buf + 8, "XCT48400-DCS", 12);
    ASSERT_EQ(mock.reads, 3);
    return 0;
}

static int test_reset_requires_a_complete_write(void)
{
    mock_reset();
    add_device("xct", 0x23D4, 2, "XCT48400-DCS");
    alltrax_controller* ctrl = NULL;
    ASSERT_EQ(alltrax_open(&ctrl, NULL, true), ALLTRAX_OK);
    const int results[] = { 0, 1, 63, 65, -1, 64 };
    unsigned reads = mock.reads;
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        mock.write_result = results[i];
        unsigned writes = mock.writes;
        ASSERT_EQ(alltrax_reset_device(ctrl), results[i] == 64 ? ALLTRAX_OK : ALLTRAX_ERR_USB);
        ASSERT_EQ(mock.writes, writes + 1);
        ASSERT_EQ(mock.reads, reads); /* Reset has no acknowledgement. */
        if (results[i] < 0)
            ASSERT_TRUE(strstr(ctrl->error_detail, "USB write failed: simulated error") != NULL);
        else if (results[i] != 64)
            ASSERT_TRUE(strstr(ctrl->error_detail, "report 0x03") != NULL);
    }
    alltrax_close(ctrl);
    return 0;
}

static int test_partial_reports_never_reach_value_decoding(void)
{
    const int lengths[] = { 63, 9, 8, 1 };
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        mock_reset();
        add_device("xct", 0x23D4, 2, "XCT48400-DCS");
        alltrax_controller* ctrl = NULL;
        ASSERT_EQ(alltrax_open(&ctrl, NULL, false), ALLTRAX_OK);
        mock.read_result = lengths[i];
        const alltrax_var_def* var = alltrax_find_var("HPD");
        alltrax_var_value out = { .display = 4321 }, original;
        memcpy(&original, &out, sizeof(out));
        unsigned reads = mock.reads, writes = mock.writes;
        ASSERT_EQ(alltrax_read_vars(ctrl, &var, 1, &out), ALLTRAX_ERR_PROTOCOL);
        ASSERT_MEM_EQ(&out, &original, sizeof(out));
        ASSERT_EQ(mock.reads, reads + 1);
        ASSERT_EQ(mock.writes, writes + 1);
        ASSERT_TRUE(strstr(ctrl->error_detail, "read length mismatch") != NULL);
        alltrax_close(ctrl);
    }
    return 0;
}

static int test_incomplete_identification_transfers_close_device(void)
{
    const int results[] = { 0, 1, 23, 63, 65, -1 };
    for (int writing = 0; writing < 2; writing++) {
        for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
            mock_reset();
            add_device("xct", 0x23D4, 2, "XCT48400-DCS");
            if (writing) mock.write_result = results[i];
            else mock.read_result = results[i];
            alltrax_controller* ctrl = NULL;
            alltrax_error expected = writing || results[i] < 0 ? ALLTRAX_ERR_USB :
                                     results[i] == 0 ? ALLTRAX_ERR_TIMEOUT : ALLTRAX_ERR_PROTOCOL;
            ASSERT_EQ(alltrax_open(&ctrl, "xct", true), expected);
            ASSERT_NULL(ctrl);
            ASSERT_EQ(mock.closes, 1);
            ASSERT_EQ(mock.frees, 1);
            ASSERT_EQ(mock.writes, 1);
            ASSERT_EQ(mock.reads, writing ? 0 : 1);
        }
    }
    return 0;
}

static int test_short_ram_write_does_not_wait_for_an_echo(void)
{
    mock_reset();
    add_device("xct", 0x23D4, 2, "XCT48400-DCS");
    alltrax_controller* ctrl = NULL;
    ASSERT_EQ(alltrax_open(&ctrl, NULL, true), ALLTRAX_OK);
    mock.write_result = 1;
    const alltrax_var_def* fan = alltrax_find_var("Fan_On");
    double value = 1;
    const alltrax_write_opts opts = { .skip_cal = true };
    unsigned reads = mock.reads, writes = mock.writes;
    ASSERT_EQ(alltrax_write_vars(ctrl, &fan, &value, 1, &opts), ALLTRAX_ERR_USB);
    ASSERT_EQ(mock.writes, writes + 1);
    ASSERT_EQ(mock.reads, reads);
    ASSERT_TRUE(strstr(ctrl->error_detail, "report 0x02") != NULL);
    alltrax_close(ctrl);
    return 0;
}

int main(void)
{
    printf("transport (simulated hidapi):\n");
    RUN_TEST(test_no_devices_and_unknown_paths);
    RUN_TEST(test_explicit_paths_require_supported_usb_identity);
    RUN_TEST(test_auto_detection_skips_unsupported_devices);
    RUN_TEST(test_supported_models_open_and_read_info);
    RUN_TEST(test_unsupported_models_fail_and_close);
    RUN_TEST(test_identification_read_failure_closes_device);
    RUN_TEST(test_auto_detection_continues_after_unrecognized_model);
    RUN_TEST(test_null_enumeration_path_is_ignored);
    RUN_TEST(test_info_rejects_changed_model_and_revokes_write_access);
    RUN_TEST(test_failed_open_and_invalid_output_handle);
    RUN_TEST(test_xct_ram_write_reset_and_read_only_protection);
    RUN_TEST(test_unknown_firmware_does_not_block_identity_or_reads);
    RUN_TEST(test_transport_blocks_mutations_without_verified_identity);
    RUN_TEST(test_transport_rejects_incomplete_writes);
    RUN_TEST(test_transport_rejects_incomplete_reads);
    RUN_TEST(test_read_errors_timeouts_and_full_reports_remain_distinct);
    RUN_TEST(test_reset_requires_a_complete_write);
    RUN_TEST(test_partial_reports_never_reach_value_decoding);
    RUN_TEST(test_incomplete_identification_transfers_close_device);
    RUN_TEST(test_short_ram_write_does_not_wait_for_an_echo);
    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);
    return test_fail_count > 0 ? 1 : 0;
}
