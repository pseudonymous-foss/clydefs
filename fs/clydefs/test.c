#include <embUnit/embUnit.h>
#include <linux/kernel.h>

extern TestRef io_tests(TestCaller *test);
extern TestRef chunk_tests(TestCaller *test);

extern char *dbg_dev;
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

static __always_inline void run_test(TestRef tf, char *err_msg)
{
    if (tf) {
        TestRunner_runTest(tf);
    } else {
        printk("ERR: %s failed to initialise\n", err_msg);
    }
}

int tests_init(void)
{

    if (dbg_dev) {
        static TestCaller test;
        /*run_test(io_tests(&test), "io_tests");*/
        run_test(chunk_tests(&test), "chunk_tests");       
        TestRunner_end();
    } else {
        printk("ERROR: dbg_dev not supplied when loading the module, no device to test against!\n");
        return 1;
    }
    return 0;
}
