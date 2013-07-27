#include "blinktreeinterface.h"
#include "blinktree.h"

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
static u64 nidcnt; /*nids are unsigned 64 bit values, atomic64_t uses signed values*/
static __always_inline u64 nidcnt_inc_get(void)
{
    spin_lock(&nidcnt_lock);
    return nidcnt++;
    spin_unlock(&nidcnt_lock);
}
static __always_inline void nidcnt_dec(void)
{
    spin_lock(&nidcnt_lock);
    nidcnt--;
    spin_unlock(&nidcnt_lock);
}

static int blinktreeinterface_node_read(u64 tid, u64 nid, u64 offset, u64 len, void *data)
{ /*implements treeinterface->node_read*/

    /*FIXME assuming single data node of 1MB size*/
    struct btd * db = blinktree_lookup(tid,nid);
    if ( unlikely(db == NULL) )
        return -ENOENT;

    if ((offset < db->num_bytes) && (len <= (db->num_bytes - offset))) {
        /*read request within the 1MB data range*/
        u8 *start = ((u8*)data) + offset;
        memcpy(data, start, len);
        return 0;
    } else {
        pr_warn("CANNOT SUPPORT READS OUTSIDE THE RANGE OF 1MB\n");
        BUG();
    }
}

static int blinktreeinterface_node_write(u64 tid, u64 nid, u64 offset, u64 len, void *data)
{ /*implements treeinterface->node_write*/

    /*FIXME assuming single data node of 1MB size*/
    struct btd * db = blinktree_lookup(tid,nid);
    if ( unlikely(db == NULL) )
        return -ENOENT;

    if ((offset < db->num_bytes) && (len <= (db->num_bytes - offset))) {
        /*read request within the 1MB data range*/
        u8 *start = ((u8*)data) + offset;
        memcpy(start, data, len);
        return 0;
    } else {
        pr_warn("CANNOT SUPPORT READS OUTSIDE THE RANGE OF 1MB\n");
        BUG();
    }
}

static int blinktreeinterface_node_insert(u64 tid, ssize_t len, void *data)
{ /*implements treeinterface->node_insert*/

    /*FIXME assuming single data node of 1MB size*/
    u64 nid;
    int retval = 0;
    struct btd *db = NULL;

    if ( (retval=data_block_alloc(&db, 1024)) ) {
        pr_warn("insert: failed to allocate data block\n");
        goto err_data_alloc;
    }
    if (len > 1024) {
        pr_warn("insert: present implementation do not support writing past 1MB range");
        goto err_data_cpy;
    }
    memcpy(db->data, data, len);

    nid = nidcnt_inc_get();
    if ( (retval=blinktree_node_insert(tid,nid,db)) ) {
        pr_warn("insert: insertion failed! tid[%llu], nid[%llu] with %ld bytes of data\n",tid,nid,len);
        goto err_insert;
    }
err_insert:
    nidcnt_dec();
err_data_cpy:
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
    ti->tree_create = blinktree_create;
    ti->tree_remove = blinktree_remove;

    /*FIXME: calling remove will not free associated data*/
    ti->node_remove = blinktree_node_remove;
    ti->node_insert = blinktreeinterface_node_insert;
    ti->node_read = blinktreeinterface_node_read;
    ti->node_write = blinktreeinterface_node_write;
    return 0;
}
