/*
 * internal.h — Internal shared definitions for liballtrax-usb
 *
 * Not part of the public API. Used by protocol.c, transport.c,
 * controller.c, and variables.c.
 */

#ifndef ALLTRAX_INTERNAL_H
#define ALLTRAX_INTERNAL_H

#include "alltrax.h"
#include <stdio.h>
#include <string.h>

typedef struct hid_device_ hid_device;

/* ------------------------------------------------------------------ */
/* Controller state                                                    */
/* ------------------------------------------------------------------ */

struct alltrax_controller {
    hid_device*  hid;
    uint16_t     vid;
    uint16_t     pid;
    alltrax_controller_type controller_type; /* Validated model family */
    uint32_t     boot_rev;     /* Read when the controller is identified */
    uint32_t     program_rev;
    bool         firmware_read_override;
    bool         allow_writes;
    char         error_detail[256];
};

/* ------------------------------------------------------------------ */
/* Protocol constants                                                  */
/* ------------------------------------------------------------------ */

#define PACKET_SIZE     64
#define MAX_PAYLOAD     56   /* 64 - 8 byte header */
#define REPORT_ID_READ    0x01
#define REPORT_ID_WRITE   0x02
#define REPORT_ID_SPECIAL 0x03
#define RESPONSE_ID       0x04
#define RESPONSE_TYPE_RW  0x01   /* data[4] for read/write responses */
#define RESPONSE_TYPE_UNK 0x02   /* semantics unknown */
#define RESPONSE_TYPE_SP  0x03   /* data[4] for special function responses */
#define RESULT_PASS       0x00

/* Special function codes (Report ID 3) */
#define FUNC_PAGE_ERASE            1
#define FUNC_ERASE_OPTION_BYTES    2   /* unused: FLASH option byte erase */
#define FUNC_GET_WRITE_PROTECTION  8   /* unused: read FLASH write protection */
#define FUNC_SET_WRITE_PROTECTION  9   /* unused: set FLASH write protection */
#define FUNC_GET_FLAG_STATUS       10
#define FUNC_CLEAR_FLAGS           11
#define FUNC_RESET                 12
#define FUNC_BMS_CLEAR_BATTERY     13  /* unused: BMS only */
#define FUNC_BMS_NEW_PROFILE       14  /* unused: BMS only */

/* FLASH flag bits (STM32 FLASH_SR register) */
#define FLASH_FLAG_BSY       1
#define FLASH_FLAG_OPTERR    2
#define FLASH_FLAG_PGERR     4
#define FLASH_FLAG_WRPRTERR  16
#define FLASH_FLAG_EOP       32

/* ------------------------------------------------------------------ */
/* Address constants                                                   */
/* ------------------------------------------------------------------ */

/* FLASH page size (XCT uses 2KB pages on STM32) */
#define FLASH_BASE      0x08000000
#define FLASH_PAGE_SIZE 2048
#define FLASH_END       (FLASH_BASE + 64 * FLASH_PAGE_SIZE) /* Exclusive XCT limit */

/* Named FLASH pages */
#define INFOSET_PAGE  1   /* 0x08000800 — controller identity */
#define FACTSET_PAGE  2   /* 0x08001000 — factory settings */
#define SWITCH_PAGE   3   /* 0x08001800 — mode switching */
#define VARSET_PAGE   4   /* 0x08002000 — user settings */

#define INFOSET_ADDR  (FLASH_BASE + INFOSET_PAGE * FLASH_PAGE_SIZE)
#define FACTSET_ADDR  (FLASH_BASE + FACTSET_PAGE * FLASH_PAGE_SIZE)
#define SWITCH_ADDR   (FLASH_BASE + SWITCH_PAGE  * FLASH_PAGE_SIZE)
#define VARSET_ADDR   (FLASH_BASE + VARSET_PAGE  * FLASH_PAGE_SIZE)
#define VARSET_SIZE   FLASH_PAGE_SIZE

/* GoodSet markers (first 2 bytes of each settings page) */
#define ADDR_V_GOODSET  VARSET_ADDR
#define ADDR_F_GOODSET  FACTSET_ADDR

/* Offsets within INFOSET */
#define ADDR_CONTROLLER_INFO  INFOSET_ADDR            /* 0x08000800 */
#define ADDR_HARDWARE_CONFIG  (INFOSET_ADDR + 0x80)   /* 0x08000880 */

/* Addresses in other FLASH regions */
#define ADDR_BOOT_REV         0x080067F0  /* end of page 12 (bootloader) */
#define ADDR_PRGM_REV         0x0801FFF0  /* last page (PRGM) */

/* RAM addresses */
#define ADDR_RUN_MODE  0x2000FFFA
#define RUN_MODE_RUN   0x00
#define RUN_MODE_CAL   0xFF
#define RAM_BASE       0x20000000
#define RAM_END        0x20010000

/* ------------------------------------------------------------------ */
/* IDs and other constants                                            */
/* ------------------------------------------------------------------ */

/* USB device IDs */
#define ALLTRAX_VID     0x23D4
#define ALLTRAX_PID_XCT 0x0002   /* XCT, SRX, NCT */
#define ALLTRAX_PID_SPM 0x0001   /* SPM, SPB, SR, BMS, BMS2 */

/* ------------------------------------------------------------------ */
/* Internal function declarations                                      */
/* ------------------------------------------------------------------ */

/* protocol.c */
void build_read_request(uint32_t addr, uint8_t num_bytes, uint8_t buf[PACKET_SIZE]);
void build_write_request(uint32_t addr, const uint8_t* data, uint8_t data_len,
    uint8_t buf[PACKET_SIZE]);
void build_page_erase_request(uint8_t page, uint8_t buf[PACKET_SIZE]);
void build_reset_request(uint8_t buf[PACKET_SIZE]);
alltrax_error parse_response(const uint8_t data[PACKET_SIZE],
    uint8_t expected_type, uint8_t* result, uint8_t* num_bytes,
    uint8_t payload[MAX_PAYLOAD]);

/* variables.c */
#define MAX_STRING_SIZE 32

/* Zero indicates an invalid type or size (strings: 1-32, other sizes: 0). */
size_t alltrax_var_byte_size(const alltrax_var_def* var);

/* True only for entries of the library's variable and curve tables. Device
 * operations accept only these. Their shapes and spans are checked by
 * test_catalog_invariants. */
bool catalog_has_var(const alltrax_var_def* var);
bool catalog_has_curve(const alltrax_curve_def* def);
alltrax_error alltrax_decode_var(const uint8_t* data, size_t data_len,
    const alltrax_var_def* var, uint32_t base_address, alltrax_var_value* out);
/* Returns encoded byte count, or 0 for invalid values/unsupported types.
 * On failure the output buffer is unchanged. */
int alltrax_encode_var(const alltrax_var_def* var, double value, uint8_t* buf);
/* Encode a string variable: trailing spaces trimmed, at most the field size,
 * printable ASCII (0x20-0x7E), space-padded to the field size. Returns the
 * field size, or 0 if invalid; on failure the buffer is unchanged. */
int alltrax_encode_string(const alltrax_var_def* var, const char* text,
    uint8_t* buf);

/* transport.c */
/* Success requires a complete 64-byte report. A short write is ERR_USB;
 * a positive short read is ERR_PROTOCOL; zero-byte reads are ERR_TIMEOUT. */
alltrax_error transport_write(alltrax_controller* ctrl,
    const uint8_t data[PACKET_SIZE]);
alltrax_error transport_read(alltrax_controller* ctrl,
    uint8_t buf[PACKET_SIZE]);

/* controller.c: read and validate the model and firmware revisions after USB
 * identity is established. */
alltrax_error identify_controller(alltrax_controller* ctrl);

/* Byte helpers */
/* Subtraction keeps both offset and absolute-address span checks overflow-safe. */
static inline bool span_fits(size_t offset, size_t length, size_t capacity)
{
    return length > 0 && offset <= capacity && length <= capacity - offset;
}

static inline bool address_span_fits(uint32_t address, size_t length,
    uint32_t base, size_t capacity)
{
    return address >= base && span_fits((size_t)(address - base), length, capacity);
}

static inline uint16_t get_le16(const uint8_t* buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

static inline uint32_t get_le32(const uint8_t* buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

/* Copy a space/null-padded string from a payload, null-terminate, trim */
/* Decode a fixed-size device string: NUL becomes a space, other bytes
 * outside printable ASCII (0x20-0x7E) become '?', and trailing spaces are
 * trimmed. The result is always printable ASCII. */
static inline void get_string(const uint8_t* src, size_t src_len,
    char* dst, size_t dst_size)
{
    size_t n = src_len < dst_size - 1 ? src_len : dst_size - 1;
    memset(dst, 0, dst_size);
    for (size_t i = 0; i < n; i++) {
        uint8_t c = src[i];
        dst[i] = c == 0 ? ' ' : (c < 0x20 || c > 0x7E) ? '?' : (char)c;
    }
    while (n > 0 && dst[n - 1] == ' ')
        dst[--n] = '\0';
}

/* Helper to set error detail */
#define set_error_detail(ctrl, ...) \
    snprintf((ctrl)->error_detail, sizeof((ctrl)->error_detail), __VA_ARGS__)

/* USB identity is required even for the initial model read. */
static inline alltrax_error require_xct_usb(alltrax_controller* ctrl)
{
    if (!ctrl)
        return ALLTRAX_ERR_INVALID_ARG;
    if (ctrl->vid != ALLTRAX_VID || ctrl->pid != ALLTRAX_PID_XCT) {
        set_error_detail(ctrl,
            "Unsupported USB identity %04X:%04X; requires Alltrax %04X:%04X",
            ctrl->vid, ctrl->pid, ALLTRAX_VID, ALLTRAX_PID_XCT);
        return ALLTRAX_ERR_UNSUPPORTED;
    }
    return ALLTRAX_OK;
}

/* Every public device operation requires a validated XCT-family model. */
static inline alltrax_error require_xct_controller(alltrax_controller* ctrl)
{
    alltrax_error rc = require_xct_usb(ctrl);
    if (rc) return rc;
    if (ctrl->controller_type != ALLTRAX_CONTROLLER_XCT) {
        set_error_detail(ctrl, "Unsupported or unidentified controller family; "
                         "requires a validated XCT-family model");
        return ALLTRAX_ERR_UNSUPPORTED;
    }
    return ALLTRAX_OK;
}

#endif /* ALLTRAX_INTERNAL_H */
