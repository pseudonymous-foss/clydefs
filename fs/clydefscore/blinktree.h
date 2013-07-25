#ifndef __CLYDEFSCORE_BLINKTREE_H
#define __CLYDEFSCORE_BLINKTREE_H

#include <linux/spinlock.h>
#include "utils.h"
#include "stack.h"

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

    spinlock_t *lock;
};

/* 
 * Tree data, contains information 
 * about length(in bytes) and address of 
 * a contiguous section of memory 
 * --- 
 * num_bytes: number of bytes in this data element
 * data: ptr to start of data
 */
struct btd{
    u8 num_bytes;
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
};

u64 blinktree_create(u8 k);
int blinktree_insert(u64 tid, u64 key, void *data);
int blinktree_remove(u64 tid, u64 key);
struct btd *blinktree_lookup(struct tree *tree, u64 key);

#ifdef DBG_FUNCS
void dbg_blinktree_print_inorder(u64 tid);
void dbg_blinktree_getkeys(u64 tid, struct stack *s);
void dbg_blinktree_getnodes(u64 tid, struct stack *s);
#endif //DBG_FUNCS

#endif //__CLYDEFSCORE_BLINKTREE_H
