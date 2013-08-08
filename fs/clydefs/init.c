#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include "super.h"
#include "io.h"

short dodebug = 0; /*module param*/
char *dbg_dev = NULL;

#ifdef CONFIG_CLYDEFS_DEBUG
extern int tests_init(void);
#else
void tests_init(void)
{
    printk("ERROR! Module compiled without debugging support!\n");
}
#endif

static int __init clydefs_init(void)
{

    if (unlikely(dodebug)) {
        tests_init();
        return 0;
    }
    
    return -1;

    /*not testing*/
    if ( cfsio_init() ){
        return -1;
    }
    return super_init();
}

static void __exit clydefs_exit(void)
{
    super_exit();
    cfsio_exit();
}

module_param(dodebug, short, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(dodebug, "do debug or not..");

module_param(dbg_dev, charp, 0);
MODULE_PARM_DESC(dbg_dev, "device to issue tests against\n");

module_init(clydefs_init);
module_exit(clydefs_exit);
MODULE_DESCRIPTION("ClydeFS prototype");
MODULE_LICENSE("GPL");

