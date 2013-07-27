#include <linux/module.h>
#include "utils.h"
#include "treeinterface.h"

short dodebug = 0;

#ifdef DBG_FUNCS
extern int tests_init(void);
#else
void tests_init(void)
{
    printk("ERROR! Module compiled without debugging support!\n");
}
#endif

static int __init init_clydefscore(void)
{
    printk(KERN_INFO "ClydeFS core loaded\n");
    if (dodebug) {
        printk("\t->dodebug=1\n");
        tests_init();
    }
    if (treeinterface_init())
        return 1;
    return 0;
}

static void __exit exit_clydefscore(void)
{
    treeinterface_exit();
    printk(KERN_INFO "ClydeFS core unloaded\n");
}

module_param(dodebug, short, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(dodebug, "do debug or not..");

module_init(init_clydefscore)
module_exit(exit_clydefscore)
MODULE_LICENSE("GPL");
