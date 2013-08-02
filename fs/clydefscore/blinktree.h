#ifndef __CLYDEFSCORE_BLINKTREE_H
#define __CLYDEFSCORE_BLINKTREE_H

#include <linux/spinlock.h>
#include "utils.h"
#include "stack.h"
#include "treeinterface.h"

/* 
 * Tree node
 */
struct btn{
    /* nid - node id, should be unique to this tree, can be used to link objects */
    u64 nid;
    struct btn *sibling;
    u8 is_leaf;
    /*FIXME no longer NULL'ing @ node creation but assert nulls for alloc of data structs*/

    /*number of entries inserted into the node*/
    u8 numkeys;

    /* 
     * arrays of 2*k+1 length for the temporary 
     * overflow leading to a node split 
     */ 
    u64 *child_keys;
    void **child_nodes;

    spinlock_t lock;
};

/** 
 * Describes a single tree data block.
 * @description describes a single tree data block.
 *              Contains information about length
 *              (in bytes) and address of a
 *              contiguous section of memory.
 */
struct btd{
    /** number of contiguous bytes in this block. */
    u16 num_bytes;
    /** start of data block */ 
    u8 *data;
};

struct tree {
    /* unique identifier given to the tree */ 
    u64 tid;
    struct btn *root;
    /* 
     * the value of K used for this tree 
     * which decides when to merge (node 
     * elements < k) and when to split 
     * (node elements > k*2) nodes 
     */ 
    u8 k;
    struct tree *nxt;
    struct kmem_cache *node_cache;
};

u64 blinktree_create(u8 k);
int blinktree_remove(u64 tid);

int blinktree_node_insert(u64 tid, u64 nid, void *data);
int blinktree_node_remove(u64 tid, u64 nid);
struct btd *blinktree_lookup(u64 tid, u64 nid);


/*static noinline */ int data_block_alloc(struct btd **block_ref, u16 num_bytes);
void data_block_free(struct btd *block);

#ifdef DBG_FUNCS
void dbg_blinktree_print_inorder(u64 tid);
void dbg_blinktree_getkeys(u64 tid, struct stack *s);
void dbg_blinktree_getnodes(u64 tid, struct stack *s);
#endif //DBG_FUNCS

#endif //__CLYDEFSCORE_BLINKTREE_H
