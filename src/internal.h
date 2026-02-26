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
    uint16_t     pid;
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
#define RESPONSE_TYPE_SP  0x02   /* data[4] for special function responses */
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
#define ADDR_BOOT_REV         0x080067F0  /* page 13 (DATA) */
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

/* Validated firmware version */
#define VALIDATED_FIRMWARE 5005

/* ------------------------------------------------------------------ */
/* Internal function declarations                                      */
/* ------------------------------------------------------------------ */

/* protocol.c */
void build_read_request(uint32_t addr, uint8_t num_bytes, uint8_t buf[PACKET_SIZE]);
void build_write_request(uint32_t addr, const uint8_t* data, uint8_t data_len,
    uint8_t buf[PACKET_SIZE]);
void build_page_erase_request(uint8_t page, uint8_t buf[PACKET_SIZE]);
void build_reset_request(uint8_t buf[PACKET_SIZE]);
void build_flash_get_flag_request(uint8_t flag, uint8_t buf[PACKET_SIZE]);
void build_flash_clear_flags_request(uint8_t flags, uint8_t buf[PACKET_SIZE]);
alltrax_error parse_response(const uint8_t data[PACKET_SIZE],
    uint8_t expected_type, uint8_t* result, uint8_t* num_bytes,
    uint8_t payload[MAX_PAYLOAD]);

/* variables.c */
size_t alltrax_var_byte_size(const alltrax_var_def* var);
alltrax_error alltrax_decode_var(const uint8_t* data, size_t data_len,
    const alltrax_var_def* var, uint32_t base_address, alltrax_var_value* out);
int alltrax_encode_var(const alltrax_var_def* var, double value, uint8_t* buf);

/* controller.c */
alltrax_error validate_voltage_link(
    double ksi, double under_volt, double over_volt,
    double bms_missing, char* err_msg, size_t err_msg_size);

/* controller.c */
alltrax_controller_type detect_controller_type(const char* model);

/* transport.c */
alltrax_error transport_write(alltrax_controller* ctrl,
    const uint8_t data[PACKET_SIZE]);
alltrax_error transport_read(alltrax_controller* ctrl,
    uint8_t buf[PACKET_SIZE]);

/* Byte helpers */
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
static inline void get_string(const uint8_t* src, size_t src_len,
    char* dst, size_t dst_size)
{
    size_t n = src_len < dst_size - 1 ? src_len : dst_size - 1;
    memset(dst, 0, dst_size);
    memcpy(dst, src, n);
    for (size_t i = n; i > 0; i--) {
        if (dst[i - 1] == ' ' || dst[i - 1] == '\0')
            dst[i - 1] = '\0';
        else
            break;
    }
}

/* Helper to set error detail */
#define set_error_detail(ctrl, ...) \
    snprintf((ctrl)->error_detail, sizeof((ctrl)->error_detail), __VA_ARGS__)

#endif /* ALLTRAX_INTERNAL_H */
