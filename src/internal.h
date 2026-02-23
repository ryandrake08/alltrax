/*
 * internal.h — Internal shared definitions for liballtrax-usb
 *
 * Not part of the public API. Used by protocol.c, transport.c,
 * controller.c, and variables.c.
 */

#ifndef ALLTRAX_INTERNAL_H
#define ALLTRAX_INTERNAL_H

#include "alltrax.h"
#include <hidapi.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Controller state                                                    */
/* ------------------------------------------------------------------ */

struct alltrax_controller {
    hid_device*  hid;
    bool         allow_writes;
    char         error_detail[256];
    alltrax_info info;
    bool         info_valid;
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
#define RESULT_PASS       0x00

/* Special function codes */
#define FUNC_PAGE_ERASE       1
#define FUNC_GET_FLAG_STATUS  10
#define FUNC_CLEAR_FLAGS      11
#define FUNC_RESET            12

/* FLASH flag bits (STM32 FLASH_SR register) */
#define FLASH_FLAG_PGERR     4
#define FLASH_FLAG_WRPRTERR  16
#define FLASH_FLAG_EOP       32

/* ------------------------------------------------------------------ */
/* Address constants                                                   */
/* ------------------------------------------------------------------ */

#define ADDR_CONTROLLER_INFO  0x08000800
#define ADDR_HARDWARE_CONFIG  0x08000880
#define ADDR_FACT_GOODSET     0x08001800
#define ADDR_BOOT_REV         0x080067F0
#define ADDR_PRGM_REV        0x0801FFF0

/* VARSET page (user settings in FLASH) */
#define VARSET_ADDR    0x08002000
#define VARSET_SIZE    2048
#define VARSET_PAGE    4
#define ADDR_V_GOODSET 0x08002000
#define ADDR_F_GOODSET 0x08001000

/* RAM addresses */
#define ADDR_RUN_MODE  0x2000FFFA
#define RUN_MODE_RUN   0x00
#define RUN_MODE_CAL   0xFF
#define RAM_BASE       0x20000000
#define RAM_END        0x20010000

/* USB device IDs */
#define ALLTRAX_VID    0x23D4
#define ALLTRAX_PID    0x0002

/* Validated firmware version */
#define VALIDATED_FIRMWARE 5005

/* ------------------------------------------------------------------ */
/* Internal function declarations                                      */
/* ------------------------------------------------------------------ */

/* protocol.c */
void build_read_request(uint32_t addr, uint8_t num_bytes, uint8_t buf[PACKET_SIZE]);
alltrax_error parse_read_response(const uint8_t data[PACKET_SIZE],
    uint8_t* result, uint8_t* num_bytes, uint8_t payload[MAX_PAYLOAD]);
void build_write_request(uint32_t addr, const uint8_t* data, uint8_t data_len,
    uint8_t buf[PACKET_SIZE]);
alltrax_error parse_write_response(const uint8_t data[PACKET_SIZE],
    uint8_t* result, uint8_t* num_bytes, uint8_t echo[MAX_PAYLOAD]);
void build_page_erase_request(uint8_t page, uint8_t buf[PACKET_SIZE]);
void build_reset_request(uint8_t buf[PACKET_SIZE]);
void build_flash_get_flag_request(uint8_t flag, uint8_t buf[PACKET_SIZE]);
void build_flash_clear_flags_request(uint8_t flags, uint8_t buf[PACKET_SIZE]);
alltrax_error parse_special_response(const uint8_t data[PACKET_SIZE],
    uint8_t* result, uint8_t* num_bytes, uint8_t payload[MAX_PAYLOAD]);
alltrax_error parse_controller_info(const uint8_t* payload, size_t len,
    alltrax_info* info);
alltrax_error parse_hardware_config(const uint8_t* payload, size_t len,
    alltrax_info* info);

/* transport.c */
alltrax_error transport_write(alltrax_controller* ctrl,
    const uint8_t data[PACKET_SIZE]);
alltrax_error transport_read(alltrax_controller* ctrl,
    uint8_t buf[PACKET_SIZE], int timeout_ms);

/* Helper to set error detail */
#define set_error_detail(ctrl, ...) \
    snprintf((ctrl)->error_detail, sizeof((ctrl)->error_detail), __VA_ARGS__)

#endif /* ALLTRAX_INTERNAL_H */
