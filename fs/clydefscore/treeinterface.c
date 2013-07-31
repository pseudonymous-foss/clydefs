#include <linux/printk.h>
#include <linux/errno.h>
#include "treeinterface.h"
#include "blinktreeinterface.h"

/* 
 * Read the 'struct treeinterface' documentation to see what each function does. 
 */

static struct treeinterface ti;

u64 clydefscore_tree_create(u8 k)
{
    return ti.tree_create(k);
}
EXPORT_SYMBOL(clydefscore_tree_create);


int clydefscore_tree_remove(u64 tid)
{
    return ti.tree_remove(tid);
}
EXPORT_SYMBOL(clydefscore_tree_remove);


u64 clydefscore_node_insert(u64 tid, u64 *nid)
{
    return ti.node_insert(tid,nid);
}
EXPORT_SYMBOL(clydefscore_node_insert);


int clydefscore_node_remove(u64 tid, u64 nid)
{
    return ti.node_remove(tid,nid);
}
EXPORT_SYMBOL(clydefscore_node_remove);


int clydefscore_node_read(u64 tid, u64 nid, u64 offset, u64 len, void *data)
{
    return ti.node_read(tid,nid,offset,len,data);
}
EXPORT_SYMBOL(clydefscore_node_read);

int clydefscore_node_write(u64 tid, u64 nid, u64 offset, u64 len, void *data)
{
    return ti.node_write(tid,nid,offset,len,data);
}
EXPORT_SYMBOL(clydefscore_node_write);

/**
 * initialise tree interface subsystem. 
 * @description initializes the tree interface subsystem, 
 *              allowing modules depending on the externally
 *              exported interface to utilise this back end.
 * @return 0 on success
 */
int treeinterface_init(void)
{
    return blinktree_treeinterface_init(&ti);
}

/**
 * terminate the tree interface subsystem.
 */
void treeinterface_exit(void)
{
    return; /*nothing to do yet*/
}
