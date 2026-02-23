/*
 * helpers.h — Minimal test framework (header-only)
 */

#ifndef TEST_HELPERS_H
#define TEST_HELPERS_H

#include <stdio.h>
#include <string.h>
#include <math.h>

extern int test_pass_count;
extern int test_fail_count;

#define ASSERT_EQ(a, b) do { \
    long long _a = (long long)(a); \
    long long _b = (long long)(b); \
    if (_a != _b) { \
        printf("  FAIL: %s:%d: %s == %lld, expected %lld\n", \
            __FILE__, __LINE__, #a, _a, _b); \
        return 1; \
    } \
} while (0)

#define ASSERT_MEM_EQ(a, b, n) do { \
    if (memcmp((a), (b), (n)) != 0) { \
        printf("  FAIL: %s:%d: memcmp(%s, %s, %d) != 0\n", \
            __FILE__, __LINE__, #a, #b, (int)(n)); \
        return 1; \
    } \
} while (0)

#define ASSERT_STR_EQ(a, b) do { \
    if (strcmp((a), (b)) != 0) { \
        printf("  FAIL: %s:%d: \"%s\" != \"%s\"\n", \
            __FILE__, __LINE__, (a), (b)); \
        return 1; \
    } \
} while (0)

#define ASSERT_TRUE(cond) do { \
    if (!(cond)) { \
        printf("  FAIL: %s:%d: %s is false\n", \
            __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

#define ASSERT_FALSE(cond) do { \
    if ((cond)) { \
        printf("  FAIL: %s:%d: %s is true\n", \
            __FILE__, __LINE__, #cond); \
        return 1; \
    } \
} while (0)

#define ASSERT_NULL(ptr) do { \
    if ((ptr) != NULL) { \
        printf("  FAIL: %s:%d: %s is not NULL\n", \
            __FILE__, __LINE__, #ptr); \
        return 1; \
    } \
} while (0)

#define ASSERT_NOT_NULL(ptr) do { \
    if ((ptr) == NULL) { \
        printf("  FAIL: %s:%d: %s is NULL\n", \
            __FILE__, __LINE__, #ptr); \
        return 1; \
    } \
} while (0)

#define ASSERT_NEAR(a, b, eps) do { \
    double _a = (double)(a); \
    double _b = (double)(b); \
    if (fabs(_a - _b) > (eps)) { \
        printf("  FAIL: %s:%d: %s == %f, expected %f (eps=%f)\n", \
            __FILE__, __LINE__, #a, _a, _b, (double)(eps)); \
        return 1; \
    } \
} while (0)

#define RUN_TEST(fn) do { \
    printf("  %s... ", #fn); \
    if (fn() == 0) { \
        printf("ok\n"); \
        test_pass_count++; \
    } else { \
        test_fail_count++; \
    } \
} while (0)

#endif /* TEST_HELPERS_H */
