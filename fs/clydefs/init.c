#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include "super.h"
#include "io.h"
#include "chunk.h"
#include "inode.h"
#include "sysfs.h"
#include "pagecache.h"

/* 
    FIXME:
        - remove the random 
*/ 

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
    int retval;

    if (unlikely(dodebug)) {
        tests_init();
        return 0;
    }

    /*not testing*/

    /*init io module*/
    retval = cfsio_init();
    if (retval)
        goto err;

    /*init page cache module */
    cfspc_init();
    if (retval)
        goto err_pagecache_init;

    /*init chunk module*/
    retval = cfsc_init();
    if (retval)
        goto err_chunk_init;

    /*init inode module*/
    retval = cfsi_init();
    if (retval)
        goto err_inode_init;

    /*init filesystem proper*/
    retval = super_init();
    if (retval)
        goto err_super_init;

    /*init sysfs*/
    retval = cfssys_init();
    if (retval)
        goto err_cfssys_init;

    return 0; /*succes*/

err_cfssys_init:
    super_exit();
err_super_init:
    cfsi_exit();
err_inode_init:
    cfsc_exit();
err_chunk_init:
    cfspc_exit();
err_pagecache_init:
    cfsio_exit();
err:
    return retval;
}

static void __exit clydefs_exit(void)
{
    cfssys_exit();
    super_exit();
    cfsi_exit();
    cfsc_exit();
    cfspc_exit();
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

