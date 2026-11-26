/*
 * test_internal.h — Declarations for library-internal functions used by tests
 *
 * These functions are not called across library .c files — they only need
 * external linkage so that test code can exercise them directly.
 */

#ifndef TEST_INTERNAL_H
#define TEST_INTERNAL_H

#include "internal.h"

/* protocol.c */
void build_flash_get_flag_request(uint8_t flag, uint8_t buf[PACKET_SIZE]);
void build_flash_clear_flags_request(uint8_t flags, uint8_t buf[PACKET_SIZE]);

/* identity.c */
char* format_rev(uint32_t rev, char* buf, size_t buflen);
bool firmware_in_bounds(alltrax_controller_type type,
    uint32_t boot_rev, uint32_t program_rev);

/* variables.c */
alltrax_error encode_curve_array(const double* values, double scale,
    int16_t raw_max, uint8_t* buf);
void type_range(alltrax_var_type type, int64_t* lo, int64_t* hi);
void effective_bounds(const alltrax_var_def* var, int64_t* lo, int64_t* hi);

#endif /* TEST_INTERNAL_H */
