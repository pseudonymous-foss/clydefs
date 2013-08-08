#include <embUnit/embUnit.h>
#include <linux/kernel.h>
extern TestRef stack_tests(TestCaller *test);
extern TestRef blinktree_tests(TestCaller *test);

/* 
 * function referenced by embUnit, but never implemented. 
 * Properly wrapped by this function. 
 * --- 
 * string: the C-string to print.
 */
void stdimpl_print(const char *string)
{
    printk(string);
}

int tests_init(void)
{
    static TestCaller test;
    /*TestRunner_runTest(stack_tests(&test));*/
    TestRunner_runTest(blinktree_tests(&test));
    TestRunner_end();
    return 0;
}
