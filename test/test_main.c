#include <stdio.h>
#include "helpers.h"

int test_pass_count;
int test_fail_count;

extern void run_protocol_tests(void);
extern void run_variables_tests(void);

int main(void)
{
    test_pass_count = 0;
    test_fail_count = 0;

    run_protocol_tests();
    run_variables_tests();

    printf("\n%d passed, %d failed\n", test_pass_count, test_fail_count);
    return test_fail_count > 0 ? 1 : 0;
}
