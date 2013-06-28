/* 
 
*/
#include <linux/module.h>

short dodebug = 0;

extern int tests_init(void);

static int __init init_clydefscore(void)
{
    printk(KERN_INFO "ClydeFS core loaded\n");
    if (dodebug) {
        printk("\t->debug enabled\n");
        tests_init();
    }

    return 0;
}

static void __exit exit_clydefscore(void)
{
    printk(KERN_INFO "ClydeFS core unloaded\n");
}

module_param(dodebug, short, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(dodebug, "do debug or not..");

module_init(init_clydefscore)
module_exit(exit_clydefscore)
MODULE_LICENSE("GPL");
