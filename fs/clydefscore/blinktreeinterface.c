#include <linux/kernel.h>
#include <linux/slab.h>

#include "blinktreeinterface.h"
#include "blinktree.h"

/*Size of each node being allocated for the tree, this is 
  meant as a temporary measure in lieu of an actual allocation
  system.*/
#define NODE_ALLOC_SIZE 1024u*1024u*4u

/* 
 *  FIXME:
 *  ------
 *  * beat blinktree into shape
 *      - tree_create needs to have the possibility of returning an error code (255 ? )
 *      - change tree identifiers to 64bit
 *  * consider approach
 *      - either let kvblade lookup nodes itself and keep track of actively used nodes
 *          -OR- keep track from within, hashing a unique request number to keep track
 *  * implement actual node identifier assignment. Presently the counter is shared for
 *      all trees leading to two drawbacks:
 *      (1) there can at most be 2^64 nodes across all trees (unecessarily restrictive)
 *      (2) there can only be 2^64 nodes, ever. Unused nid's are never released
 *          (bad but maybe not a problem I want to solve in a prototype)
 */

DEFINE_SPINLOCK(nidcnt_lock);
static u64 nidcnt = 1; /*nids are unsigned 64 bit values, atomic64_t uses signed values*/
static __always_inline u64 nidcnt_inc_get(void)
{
    int reserved_val = 0;
    spin_lock(&nidcnt_lock);
    reserved_val = nidcnt++;
    spin_unlock(&nidcnt_lock);
    return reserved_val;
}

static u8 *disk[1500];
static struct btd *disk_meta[1500];
DEFINE_SPINLOCK(blk_alloc_lock);
int nxt_blk_ndx = 0;
int blocks = 0;

static __always_inline void dealloc_disk(int i) {
    while( --i >= 0 ) {
        kfree(disk_meta[i]);
        kfree(disk[i]);
    }
}

static __always_inline int prealloc_disk(int prealloc_blocks)
{
    int i;
    printk("prealloc_disk running..\n");

    if (prealloc_blocks > 1500) {
        pr_debug("cannot allocate more than 1500 blocks\n");
        return 1;
    }

    /*zero out lists*/
    memset(disk, 0, sizeof(u8 *)*1500);
    memset(disk_meta, 0, sizeof(struct btd *)*1500);

    for (i = 0; i < prealloc_blocks; i++) {

        disk_meta[i] = (struct btd*)kmalloc(sizeof(struct btd), GFP_ATOMIC);
        if(! disk_meta[i] )
            goto unwind;

        disk[i] = (u8*) kmalloc(sizeof(u8)*NODE_ALLOC_SIZE, GFP_ATOMIC);
        disk_meta[i]->data = disk[i];
        if(! disk[i]){
            kfree(disk_meta[i]);
            goto unwind;
        }
        disk_meta[i]->num_bytes = NODE_ALLOC_SIZE;
    }
    blocks = prealloc_blocks;
    printk("prealloc successful, allocated %d blocks\n", i);
    return 0;

unwind:
    pr_debug("Allocation of block#%d failed\n", i);
    printk("prealloc_disk failed at block#%d\n", i);
    dealloc_disk(i);
    return 1;
}


static __always_inline int get_blk(struct btd **ret_blk)
{
    int ndx = 0;
    CLYDE_ASSERT(ret_blk != NULL);
    CLYDE_ASSERT(*ret_blk == NULL); /*ensure we're not losing a ptr*/

    spin_lock(&blk_alloc_lock);
    ndx = nxt_blk_ndx++;
    if (unlikely(ndx >= blocks)) {
        nxt_blk_ndx--;
        return 1;
    }
    spin_unlock(&blk_alloc_lock);
    *ret_blk = disk_meta[ndx];
    return 0;
}

#if 0
/*wrong way to release a failed nid, hold a buffer of 10 spaces or something*/
static __always_inline void nidcnt_dec(void)
{
    spin_lock(&nidcnt_lock);
    nidcnt--;
    spin_unlock(&nidcnt_lock);
}
#endif

static int blinktreeinterface_node_read(u64 tid, u64 nid, u64 offset, u64 len, void *data)
{ /*implements treeinterface->node_read*/
    struct btd *db = NULL;
    int retval;

    /*FIXME assuming single data node of NODE_ALLOC_SIZE size*/
    retval = blinktree_lookup(&db, tid, nid);
    if ( unlikely(db == NULL) )
        return retval;

    if ((offset < db->num_bytes) && (len <= (db->num_bytes - offset))) {
        /*read request within the NODE_ALLOC_SIZE data range*/
        u8 *data_block = ((u8*)db->data) + offset;
        u8 *buf = ((u8*)data);
        memcpy(buf, data_block, len);
        return 0;
    } else {
        pr_emerg("CANNOT SUPPORT READS OUTSIDE THE RANGE (%db => %dkb) [attempted to read %llub from offset %llub]\n",
                 NODE_ALLOC_SIZE, NODE_ALLOC_SIZE/1024, len, offset);
        BUG();
    }
}

static int blinktreeinterface_node_write(u64 tid, u64 nid, u64 offset, u64 len, void *data)
{ /*implements treeinterface->node_write*/
    struct btd *db = NULL;
    int retval;

    /*FIXME assuming single data node of NODE_ALLOC_SIZE size*/
    retval = blinktree_lookup(&db, tid, nid);
    if ( unlikely(db == NULL) )
        return -ENOENT;

    if ((offset < db->num_bytes) && (len <= (db->num_bytes - offset))) {
        /*read request within the NODE_ALLOC_SIZE data range*/
        u8 *buf = ((u8*)data);
        u8 *data_block = ((u8*)db->data) + offset;
        memcpy(data_block, buf, len);
        return 0;
    } else {
        pr_emerg("CANNOT SUPPORT WRITES OUTSIDE THE RANGE (%db => %dkb) [attempted to write %llub from offset %llub]\n",
                 NODE_ALLOC_SIZE, NODE_ALLOC_SIZE/1024, len, offset);
        BUG();
    }
}

static int blinktreeinterface_node_insert(u64 tid, u64 *nid)
{ /*implements treeinterface->node_insert*/

    /*FIXME assuming single data node of NODE_ALLOC_SIZE size*/
    u64 tmp;
    int retval = 0;
    struct btd *db = NULL;
    pr_debug("blinktreeinterface_node_insert => 1\n");
    if ( (retval=get_blk(&db)) ) {
        pr_warn("insert: failed to allocate data block\n");
        goto err_data_alloc;
    }

    pr_debug("blinktreeinterface_node_insert => 2\n");
    memset(db->data,0,NODE_ALLOC_SIZE); /*clear data*/
    pr_debug("blinktreeinterface_node_insert => 3\n");
    tmp = nidcnt_inc_get();
    pr_debug("blinktreeinterface_node_insert => 4\n");
    if ( (retval=blinktree_node_insert(tid,tmp,db)) ) {
        pr_warn("insert: insertion failed! tid[%llu], nid[%llu]\n",tid,tmp);
        goto err_insert;
    }
    pr_debug("blinktreeinterface_node_insert => 5(done)\n");

    /* success */
    *nid = tmp;
    return 0; 
err_insert:
    /*nidcnt_dec(); -- FIXME, current implementation erroneous*/
    data_block_free(db);
err_data_alloc:
    return retval;
}

/**
 * Expose blinktree external interface via the supplied struct.
 * @description populates the supplied treeinterface struct with
 *              the blinktree implementations.
 * @return 0 on success;
 */
int blinktree_treeinterface_init(struct treeinterface *ti)
{
    if (prealloc_disk(1500)) {
        pr_debug("blinktree_treeinterface_init: failed to preallocate disk.\n");
        return 1;
    }
    ti->tree_create = blinktree_create;
    ti->tree_remove = blinktree_remove;

    /*FIXME: calling remove will not free associated data*/
    ti->node_remove = blinktree_node_remove;
    ti->node_insert = blinktreeinterface_node_insert;
    ti->node_read = blinktreeinterface_node_read;
    ti->node_write = blinktreeinterface_node_write;
    return 0;
}

void blinktree_treeinterface_exit(void) {
    dealloc_disk(blocks);
}
