#ifndef __CLYDEFSCORE_UTILS_H
#define __CLYDEFSCORE_UTILS_H
#include <linux/kernel.h>

#define CLYDE_ASSERT(x)                                                 \
do {    if (x) break;                                                   \
        printk(KERN_EMERG "### ASSERTION FAILED %s: %s: %d: %s\n",      \
               __FILE__, __func__, __LINE__, #x); dump_stack(); BUG();  \
} while (0);

#ifdef CONFIG_CLYDEFS_CORE_DEBUG
    #define STATIC
    #define DBG_FUNCS 
#else
    /*disallow access to protected variables etc*/
    #define STATIC static
#endif //CONFIG_CLYDEFS_CORE_DEBUG

#define U64_MAX_VALUE 18446744073709551615ull
#define U8_MAX_VALUE 255

#endif //__CLYDEFSCORE_UTILS_H
