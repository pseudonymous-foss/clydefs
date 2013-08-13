#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/tree.h>

#include "blinktree.h"
#include "stack.h"

/* 
 *  
 * FIXME (overall) 
 * - writing a sibling link into the node 
 *   (for reading the sibling, if any)
 *   -> need to keep this in mind in all algorithms
 *  
 * - split nodes ASSUMES we only split on overflow (ie=>2k+1 nodes) 
 *     if we want to preemptively split, we'd need to fix delete algorithms, too
 *  
 * - nid (node id) -- should be generated! 
 * - make_data missing a free_data variant 
 */ 

/* 
 *  --read/search algorithms should cope with child
 *    entries marked 'KEY_UNDER_UPDATE'
 *  
 *  --I'm structuring the code to reflect how I think
 *    the order of operations should be to give a lock-free
 *    read experience. Requiring just the serialization of writes
 *      HOWEVER-- the compiler may reorder code, the CPU may reorder operations
 *        and these things combined means that any such structuring is MOOT
 *        'volatile' won't really work past ensuring no restructuring of other
 *        'volatile' members, but memory barriers may. Easier still is just to
 *        allow many readers so long as no write lock is set.
 *  
 *  --use linux kernel slab functionality to create a cache of memory for nodes ?
 *      (ALSO: need to find a way to requisition ridiculous amounts of memory for data)
 * 
 *  FIXME:
 *      * free memory properly (check functions)
 *      * examine locking patterns
 *      * blinktree_scanleaf should be removed and replaced by node_haskey 
 */

/* 
 * NOT Handled (nor expected to be) 
 * 
 * - tree id issuer will not keep track of id's issued on last initialization, it's all in memory
 */



#define INTERNAL_NODE 0
#define LEAF_NODE 1
#define __BLINKTREE_EXPECTED_HEIGHT 10
#define KEY_UNDER_UPDATE 0
#define NODE_REMOVED_HK_ENTRY 1

/*u64_maxval*/
#define TREE_MAX_NID U64_MAX_VALUE
#define NO_SUCH_ENTRY U8_MAX_VALUE

#define NODE_LOCK(n) \
    do { \
        if(spin_is_locked(&(n)->lock)) \
            pr_debug("%s<%d>: encountered a lock, waiting...\n", __FUNCTION__, __LINE__); \
        spin_lock(&(n)->lock); \
    } while(0);

#define NODE_UNLOCK(n) spin_unlock(&(n)->lock)
#define TREE_LIST_LOCK() spin_lock(&t_list_lock);
#define TREE_LIST_UNLOCK() spin_unlock(&t_list_lock);

STATIC struct tree *tree_list_head = NULL;
STATIC u8 tid_counter = 1;
STATIC DEFINE_SPINLOCK(t_list_lock);
unsigned long t_list_lock_flags;

#ifdef DBG_FUNCS
static void blinktree_print_node(struct btn *node, int depth);
#else
#define blinktree_print_node(x, y) ;
#endif

static __always_inline int node_is_locked(struct btn *node)
{
    return spin_is_locked(&(node->lock));
}

/**
 * Return the node's high key. 
 * @description return the node's high key which is the 
 *              upper bound of hte subtree rooted in
 *              'node'.
 * @param node whose high key is to be returned 
 * @return the node's high key  
 */ 
static __always_inline u64 node_high_key(struct btn *node) 
{
    return node->child_keys[node->numkeys-1];
}

/* 
 * given a tree id, returns the root node 
 * --- 
 *  return:
 *      tree root
 */
static __always_inline struct tree *get_tree(u64 tid)
{
    
    struct tree *c = tree_list_head;
    while (c != NULL) {
        if ( c->tid == tid)
            return c;
        else
            c = c->nxt;
    }

    /*err out*/
    pr_warn("\n\nget_tree: could not find any tree with id(%llu)!\n", tid);
    return NULL;
}

/* 
 * given a valid tree id, supplants the old 
 * root node with a new one. 
 * --- 
 *   precondition: hold the t_list_lock 
 */
static __always_inline void set_tree_root(u64 tid, struct btn *new_root)
{
    struct tree *c = tree_list_head;
    for (c = tree_list_head; c != NULL; c = c->nxt) {
        if ( c->tid == tid) {
            c->root = new_root;
            return;
        }
    }
    /*err out*/
    pr_debug("\n\nset_tree_root: could not find any tree with id(%llu)!\n", tid);
    BUG();
}

/* 
 * Issue a new tree node id 
 *  
 * FIXME FIXME: I need a way of making this get the tree id, too 
 * NOTE: only (strictly speaking) need to issue these to nodes created as a 
 *  direct consequence of 
 * --- 
 * return: 
 *  a new tree node id 
 */
static __always_inline u64 acquire_nid(void)
{
    u64 v = 0;
    return v; /*FIXME: not implemented*/
}

/** 
 * Allocate a new tree node. 
 * @param tree the tree to which this node will belong 
 * @param node_ref pointer to the pointer which will hold a reference to the allocation, if successful
 * @param nid the node identifier given the new node 
 * @param is_leaf whether this node is to be a leaf node or not. 
 * @return -ENOMEM if allocation fails, 0 otherwise (success) 
 */
static noinline int make_node(struct tree *tree, struct btn **node_ref, u64 nid, u8 is_leaf){
    struct btn *n = NULL;
    CLYDE_ASSERT(tree != NULL);
    CLYDE_ASSERT(node_ref != NULL);
    CLYDE_ASSERT(*node_ref == NULL); /*ensure we're not losing a ptr*/
    
    n = (struct btn*)kmem_cache_alloc(tree->node_cache, GFP_ATOMIC);
    if (!n) {
        pr_debug("make_node: failed to allocate node\n");
        goto err_alloc;
    }
    /*
    Allocation is large enough to contain both struct and 2k+1 entries, 
    where each entry is 1 key and 1 child pointer. 
     
    I'll lay out the memory as follows: 
    |  struct btn  |  2k+1 ptrs  |  2k+1 keys  | 
    ^ node_offset  ^ key_offset  ^ ptr_offset
    */
    (*node_ref) = n;                                                /*node_offset*/
    n->child_keys = (u64*)(n+1);                                    /*key_offset*/
    n->child_nodes = (void**)(n->child_keys + 1 + (2 * tree->k));   /*node_offset*/


    n->nid = nid;
    n->sibling = NULL;
    n->is_leaf = is_leaf;
    n->numkeys = 0;
    spin_lock_init(&(*node_ref)->lock);
    return 0;

err_alloc:
    return -ENOMEM;
}

#if 0 /*will become useful once blinktree_remove actually cleans the tree*/
/** 
 * Free the tree node memory.
 * @param tree the tree to which the node belongs
 * @param node the node to release. 
 * @post node is a dangling ptr 
 */
static noinline void free_node(struct tree *tree, struct btn *node)
{
    kmem_cache_free(tree->node_cache,node);
}
#endif


/** 
 *  Allocate a data block of a variable size of bytes
 *  @param ret_block_ref pointer to the pointer which will hold
 *                   a reference to the allocation, if
 *                   successful.
 *  @param num_bytes: the size of the data block to allocate, in
 *                  bytes.
 *  @return 0 on success, error otherwise. e.g.
 *          TERR_ALLOC_FAILED => allocation failed.
 *  @post On success, *ret_block_ref points to the allocated
 *        data block. On failure, *ret_block_ref points to NULL.
 */ 
int data_block_alloc(struct btd **ret_block_ref, u32 num_bytes){
    CLYDE_ASSERT(ret_block_ref != NULL);
    CLYDE_ASSERT(*ret_block_ref == NULL); /*ensure we're not losing a ptr*/

    /*alloc leaf*/
    *ret_block_ref = (struct btd*)kmalloc(sizeof(struct btd), GFP_ATOMIC);
    if(!*ret_block_ref)
        goto err_alloc;

    (*ret_block_ref)->data = (u8*) kmalloc(sizeof(u8)*num_bytes, GFP_ATOMIC);
    if(!(*ret_block_ref)->data)
        goto err_data_alloc;
    (*ret_block_ref)->num_bytes = num_bytes;

    goto out;

err_data_alloc:
    kfree(*ret_block_ref);
err_alloc:
    return TERR_ALLOC_FAILED;
out:
    return 0;
}

void data_block_free(struct btd *block)
{
    kfree(block->data);
    kfree(block);
}

/** 
 * Create new tree with k as the node split/merge value. 
 * @param k determines when nodes are split/merged. Nodes are 
 *        split when they have 2k+1 nodes.
 * @return the tree identifier value. 0 on failure to create tree.
 */ 
u64 blinktree_create(u8 k)
{
    struct tree *t = NULL;
    struct btn *n = NULL;
    size_t node_size;
    t = (struct tree*)kmalloc(sizeof(struct tree), GFP_ATOMIC);
    if (!t) {
        pr_debug("blinktree_create failed to allocate memory\n");
        goto err_tree_alloc;
    }
    t->k = k;
    t->node_cache = NULL;

    node_size = sizeof(struct btn) +            /*node struct itself*/
        sizeof(u64) * ((t->k)*2+1) +            /*k key values*/
        sizeof(void*) * ((t->k)*2+1);           /*k pointers to other nodes*/

    t->node_cache = kmem_cache_create("btn", node_size, 0, 0, NULL);
    if (!t->node_cache) {
        pr_debug("blinktree_create: failed to allocate memcache for k=%u tree\n", k);
        goto err_cache_alloc;
    }

    if (make_node(t, &n, acquire_nid(), LEAF_NODE)){
        pr_debug("Failed allocating a root node for the creation of a new tree\n");
        goto err_node_alloc;
    }
    t->root = n;

    TREE_LIST_LOCK();
    t->tid = tid_counter++;
    /*root is a leaf by virtue of being the first and only node*/

    if (unlikely(tree_list_head == NULL)) {
        t->nxt = NULL;
        smp_mb(); /*because blinktree_remove is lockless*/
        tree_list_head = t;
    } else {
        t->nxt = tree_list_head->nxt;
        smp_mb(); /*because blinktree_remove is lockless*/
        tree_list_head->nxt = t;
    }
    TREE_LIST_UNLOCK();

    return t->tid; /*success*/
err_node_alloc:
    kmem_cache_destroy(t->node_cache);
err_cache_alloc:
    kfree(t);
err_tree_alloc:
    return 0;
}

/** 
 * Remove tree identified by 'tid'
 * @param tid the tree identifier.
 * @return 0 on success, 
 *  TERR_NO_SUCH_TREE => supplied ID didn't match a tree 
 */ 
int blinktree_remove(u64 tid)
{
    /* 
        Unlink tree, clean when not busy, may need a worker counter indicating
        how many threads work on the tree at any moment
    */
    struct tree *c, *p = NULL;
    pr_debug("blinktree_remove not yet implemented _properly_\n");

    c = tree_list_head;
    while(c != NULL) {
        if(c->tid == tid) {
            if (p) {
                p->nxt = c->nxt;
                smp_mb();
            }
            /*FIXME: wait for all workers to exit the tree, then begin cleaning it all up*/
            /*also clean up tree struct itself, kmem_cache and all*/
            return 0;
        }
        p = c;
        c = c->nxt;
    }
    return TERR_NO_SUCH_TREE;
}

/* 
 * returns the index of the node entry matching the key 
 * 'key' - or a special value if no entry matched 
 * return: 
 *   NO_SUCH_ENTRY: if the entry wasn't found in this node
 *   -otherwise- the entry's index
 */ 
static __always_inline u8 node_indexof_key(struct btn *node, u64 key)
{
    u8 i;
    for (i=0; i<node->numkeys; i++) {
        if (node->child_keys[i] == key)
            return i;
        if (unlikely(node->child_keys[i] > key))
            return NO_SUCH_ENTRY; /*we've moved past the value already*/
    }
    return NO_SUCH_ENTRY;
}

/* 
 * returns the index of the node entry pointing to child (c)
 * from parent (p) if any such entry exists.
 * ---
 *  return:
 *    - (success) the index of the entry
 *    - (failure) NO_SUCH_ENTRY, entry does not exist
 */
static __always_inline u8 node_indexof_node(struct btn *p, struct btn *c)
{
    u8 i;
    for (i=0; i<p->numkeys; i++) {
        if (p->child_nodes[i] == c)
            return i;
    }
    return NO_SUCH_ENTRY;
}

/* 
 * scans a node for the key in question, returning the address of the node if 
 * it was found, or NULL otherwise. 
 */
static __always_inline void *node_valueof(struct btn *node, u64 key)
{
    u8 entry_index = node_indexof_key(node,key);
    if (likely(entry_index != NO_SUCH_ENTRY)) {
        return node->child_nodes[entry_index];
    }
    return NULL; /*key not in this node*/
}

/* 
 * scans a node for the key in question, returning true if the
 * node has the key, false otherwise
 */
static __always_inline u8 node_haskey(struct btn *node, u64 key)
{
    return (node_indexof_key(node,key) != NO_SUCH_ENTRY);
}

/* 
 * scans a node (p) to determine if it links to node (c) in any
 * of its entries, if so, (p) is a parent of (c).
 */
static __always_inline u8 node_isparentof(struct btn *p, struct btn *c)
{
    u8 i;
    for (i=0; i<p->numkeys; i++) {
        if ( p->child_nodes[i] == c ) {
            return 1;
        }
    }
    return 0;
}

/* 
 * scans node (p) to determine the key of the entry linking from (p)
 * to (c), if any. If no link was found, it returns NO_SUCH_ENTRY
 * ---
 *   return:
 *    (success) key used in (p) to refer to subtree (c)
 *    (failure) NO_SUCH_ENTRY
 */
static __always_inline u64 node_keyof_node(struct btn *p, struct btn *c)
{
    u8 i;
    for (i=0; i<p->numkeys; i++) {
        if (p->child_nodes[i] == c)
            return p->child_keys[i];
    }
    return NO_SUCH_ENTRY;
}

/* 
 * Inserts a key pointing to a value into the node 'node'. 
 * (value can either be a leaf/data block or another node) 
 * --- 
 * node: the 'parent' node, the node the (key,value) pair is inserted into 
 * key: the key identifying the the value 
 * value: the value, should be either a data block or a subtree with upper bound 'key' 
 * --- 
 */
static __always_inline void node_insert_entry(struct btn *node, u64 key, void *value)
{
    /* 
    We go down the list of keys until finding a key LESS than (or equal to) 
    what we want to insert (at index 'i'). 
    This means our new key must be inserted at 'i+1' to maintain sorted 
    order. 
    (using '<=' ensures entries are sorted in ascending order with age as 
    the secondary criteria, ergo inserting an entry with key 'x' will be 
    inserted AFTER any existing entries with that value - we rely on this 
    when updating parent node entries after node splits.) 
    We then shift every key (and node) from j=numkeys-1 to i(excl) once 
      to the right to make room in 'i+1' for our (key,value) pair.
    */
    short i,j,ndx;

    /*this check only wards against using this function 
      when no one has a lock on the node.
      FIXME how to check that this thread owns the lock ?
    */
    CLYDE_ASSERT(node_is_locked(node));

    if ( node->numkeys == 0 ) {
        ndx = 0; /*where to insert entry*/
        goto insert;
    }

    /*need to sort things as we go, therefore assume everything is in order already*/
    i = node->numkeys-1;
    do {
        if ( unlikely(node->child_keys[i] == KEY_UNDER_UPDATE) ) continue;
        if (node->child_keys[i] <= key) {
            break;
        }
        i--;
    } while(i >= 0);
    
    /*shift all keys from i+1 to numkeys*/
    j = node->numkeys;
    while ( j > i ) { /*FIXME: what about here ? locks? barriers?*/
        j--;
        node->child_keys[j+1] = node->child_keys[j];
        node->child_nodes[j+1] = node->child_nodes[j];
    }
    ndx = i+1; /*where to insert entry*/

insert:
    /* 
      mark the altered element as under construction and
      ensure all elements will be read by incrementing the
      numkeys count immediately.
    */
    node->child_keys[ndx] = KEY_UNDER_UPDATE;
    node->numkeys++;

    node->child_nodes[ndx] = value;

    /* 
     * only the key value needs 
     * to be set for the entry to be valid
     */
    smp_mb();
    node->child_keys[ndx] = key; 
}


/* 
 * Deletes an entry from a node. 
 * --- 
 * node: the node to delete the entry from 
 * key: the key of the entry to remove 
 * --- 
 * return: 
 *  1: success, removed node's high key
 *  0: success
 * precondition:
 *   -the entry given by 'key' exists
 *   -the node is locked for writing
 * ---
 */
static __always_inline int node_remove_entry(struct btn *node, u64 key)
{
    /* 
     * Marking the key as UNDER_UPDATE ensures others ignore 
     * it in their search. 
     * For internal nodes, this is fine as lookups will skip 
     * to the next entry which holds the subtree which is where 
     * requests directed to the now removed key should go now.  
     *  
     * For leaf nodes, it may render a failed lookup, but the 
     * scan continues without errors. Which is acceptable. 
     */
    
    u8 entry_ndx;
    u8 i;

    i = entry_ndx = node_indexof_key(node,key);
    if (i == NO_SUCH_ENTRY) {
        return NO_SUCH_ENTRY;
    }
    CLYDE_ASSERT(entry_ndx < node->numkeys);
    CLYDE_ASSERT(node_is_locked(node));
    
    /*shifts elements to the left to fill the void of the deleted element while 
      maintaining latch-free reading*/
    for (i=entry_ndx; i<node->numkeys-1; i++) {
        /*last iter works on second-to-last entry, copying data from the last entry*/
        node->child_keys[i] = KEY_UNDER_UPDATE;
        smp_mb(); /*for internal nodes, the order of operations would not matter.
        For leaf nodes, it may result in wrongfully overwriting an entry being shifted*/
        node->child_nodes[i] = node->child_nodes[i+1];
        smp_mb();
        node->child_keys[i] = node->child_keys[i+1];
        smp_mb();
    }

    node->numkeys--;
    /*node->numkeys == highest index of the node prior to entry deletion 
      => if following is true, we removed the high-key entry*/
    if (unlikely(entry_ndx == node->numkeys)) {
        return NODE_REMOVED_HK_ENTRY;
    } else {
        return 0;
    }
}



/* 
 * splits the node into two nodes and makes the old (overfull) 
 * node's sibling pointer point to the new node 
 * --- 
 * node: the node to split 
 * is_root: whether or not the node being split is the root of the tree 
 * --- 
 * return: 
 *   the newly created node holding the higher keys
 *  
 * postconditions: 
 *  - node is split in two, the new node is registered as the old node's
 *     sibling, the split retains (and maintains) tree consistency.
 *  - the new sibling node (node_right) is locked
 */
static __always_inline struct btn *node_split(struct tree *tree, struct btn *node, u8 is_root)
{
    /*
     * split the node into 
     *   old_node : k+1 elements
     *   node_right: k elements
     * ensure that 
     *   node_right's sibling points to the old node's sibling
     *   old_node's sibling now points to node_right
     */
    struct btn *node_right = NULL;
    printk("inside node_split (is_root:%u)\n", is_root);
    CLYDE_ASSERT(tree != NULL);
    CLYDE_ASSERT(node != NULL);
    /*'node' need to be protected from modications while we copy data from it*/
    CLYDE_ASSERT( node_is_locked(node) );
    /*ensure splitting is only done when there's enough nodes to 
      avoid subsequent compaction*/
    CLYDE_ASSERT( node->numkeys >= (tree->k*2 + 1) );

    printk("\t\tbefore make_node (node_right points to: %p)\n", node_right);
    if (make_node(tree, &node_right, acquire_nid(), node->is_leaf))
        return NULL;
    printk("\t\tafter make_node (node_right points to: %p), (node_right->lock points to %p)\n", node_right, &node_right->lock);
    printk("\t\tbefore memcopying node elements\n");
    printk("\t\tbefore splitting node:\n\t\t\t");
    blinktree_print_node(node,0);

    /*copy k keys from ndx k onwards (ergo, skipping k+1 elems)*/ 
    memcpy(node_right->child_keys, 
           &node->child_keys[tree->k+1],
           sizeof(u64)*tree->k);
    memcpy(node_right->child_nodes, 
           &node->child_nodes[tree->k+1], 
           sizeof(void*)*tree->k);
    printk("\t\t_after_ memcpy - node_right->lock points to %p\n", &node_right->lock);

    printk("\t\tbefore locking node_right splinlock\n");
    /*when splitting we need to insert a new entry for the 
      resulting node_right into a parent node. We need to
      ensure the high-key which we insert indeed matches
      the highest value in the node at the time.*/
    NODE_LOCK(node_right);
    printk("\t\t__after__ locking node_right splinlock\n");
    node_right->sibling = node->sibling;
    node->sibling = node_right;

    
    node_right->numkeys = tree->k;

    /* 
     * sibling to the right is fully initialised
     * update this node to effect the split.
     */
    smp_mb();
    node->numkeys = tree->k+1;
    printk("\t\tafter splitting node:\n\t\t\tnode: ");
    blinktree_print_node(node,0);
    printk("\n\t\t\tnode: ");
    blinktree_print_node(node_right,0);
    return node_right;
}

/** 
 *  Searches from the root node toward a leaf containing an
 *  entry for 'key'.
 *  @param root the root of the tree
 *  @param key the key of the entry to search for
 *  @param path a stack on which to record the path travelled
 *              down the tree. (minus nodes travelled via link
 *              pointers)
 *  @return a leaf-node identified by 'key' or NULL if no such
 *          path was found.
 *  @pre 'path' is an empty, initialised stack.
 *  @post 'path' contains each node traversed (root included,
 *        siblings excluded) to get to the leaf, pushed in
 *        order.
 */ 
static noinline struct btn *find_leaf(struct btn *root, u64 key, struct stack *path)
{
    /*note as per b-link algorithm, only the moves down to 
      child nodes are recorded in the path, moves to the right (to siblings)
      are ignored.
    */
    u8 i, advance_down;
    struct btn *c = root;
    printk("find_leaf begin\n");
    CLYDE_ASSERT(root != NULL);
    CLYDE_ASSERT(path != NULL);

    if (c->is_leaf)
        return c;

    printk("\tbefore while (!c->is_leaf) loop\n");
    do {
        advance_down = i = 0;

        /*Disable preemption while we examine this one node 
          to avoid issues regarding changes to the node while
          we slept, halfway through its keys.*/
        preempt_disable();
        /*go through each child ptr, to find the next edge down*/
        while ( i < c->numkeys ) {
            if (key <= c->child_keys[i]) {
                advance_down = 1;
                break;
            }
            i++;
        }
        
        if ( likely(advance_down) ) {
            /*found the right index, continue down this way*/
            if (likely(!c->is_leaf)) {
                /*printk("----------pushing node onto tree_path stack\n");
                printk("\t\t\tnodedbg: nid(%llu)\n", c->nid);
                printk("\t\t\tnodedbg: is_leaf(%u)\n", c->is_leaf);
                printk("\t\t\tnodedbg: numkeys(%u)\n", c->numkeys);
                printk("\t\t\tnodedbg: lock addr(%p)\n", c->lock);
                printk("\t\t\tnodedbg: is lock null ? (%d)\n", (c->lock==NULL));*/
                clydefscore_stack_push(path, c);
            }
            c = (struct btn*) c->child_nodes[i];
        } else {
            /*none of the child nodes actually matched, use link ptr*/
            /*node must've been split, advance to sibling, if possible, otherwise err out*/
            if ( c->sibling == NULL ) {
                printk("blinktree,findleaf: exiting with null for sibling\n");
                goto no_node;
            }
            c = c->sibling;
        }
        preempt_enable();
    } while (!c->is_leaf);


    printk("\tafter while (!c->is_leaf) loop (SUCCESS!)\n");
    return c;
no_node:
    preempt_enable();
    return NULL;
}

/* 
 * Searches from root node toward a leaf. 
 * --- 
 * root: root node
 * key: key to search for
 * --- 
 * returns: 
 *  - a node to the leaf, NULL if no results 
 * preconditions: 
 *  - an empty, initialised stack
 * postconditions: 
 *  - returns the leaf node itself as the return value
 */
static noinline struct btn *find_leaf_no_path(struct tree *tree, u64 key)
{
    /*note as per b-link algorithm, only the moves down to 
      child nodes are recorded in the path, moves to the right (to siblings)
      are ignored.
    */
    u8 i, advance_down;
    struct btn *c;
    printk("find_leaf_no_path begin\n");
    CLYDE_ASSERT(tree != NULL);
    CLYDE_ASSERT(tree->root != NULL);

    c = tree->root;
    if (c->is_leaf)
        return c;

    printk("\tbefore while (!c->is_leaf) loop\n");
    do {
        advance_down = i = 0;

        /*Disable preemption while we examine this one node 
          to avoid issues regarding changes to the node while
          we slept, halfway through its keys.*/
        preempt_disable();
        /*go through each child ptr, to find the next edge down*/
        while ( i < c->numkeys ) {
            if (key <= c->child_keys[i]) {
                advance_down = 1;
                break;
            }
            i++;
        }
        
        if ( likely(advance_down) ) {
            /*found the right index, continue down this way*/
            c = (struct btn*) c->child_nodes[i];
        } else {
            /*none of the child nodes actually matched, use link ptr*/
            /*node must've been split, advance to sibling, if possible, otherwise err out*/
            if ( c->sibling == NULL ) {
                printk("blinktree,findleaf_no_path: exiting with null for sibling\n");
                goto no_node;
            }
            c = c->sibling;
        }
        preempt_enable();
    } while (!c->is_leaf);


    printk("\tafter while (!c->is_leaf) loop (SUCCESS!)\n");
    return c;
no_node:
    preempt_enable();
    return NULL;
}

/* 
 * scans a leaf node for the particular record/data block associated 
 * with 'key'. 
 * --- 
 *  return:
 *   NULL: if no record for supplied key was found (unlikely/erroneous)
 *   -otherwise- the record/data block associated with 'key'
 */
static __always_inline struct btd *blinktree_scanleaf(struct btn *node, u64 key)
{
    u8 i;

    CLYDE_ASSERT(node != NULL);
    while (node) {
        for(i=0; i < node->numkeys; i++) {
            if (node->child_keys[i] == key)
                return (struct btd*)node->child_nodes[i];
        }
        node = node->sibling; /*not in this node, check sibling*/
    }

    /*no child with matching key found in 'node' or its siblings to the right*/
    return NULL; 
}


/** 
 * Locate the data node identified by 'nid' 
 * @param ret_btd a pointer to where a pointer to the leaf node 
 *                should be stored.
 * @param tid the tree identifier 
 * @param nid the node identifier 
 * @return 0 on success, TERR_NO_SUCH_TREE or TERR_NO_SUCH_NODE 
 *         on failure to locate the node.
 * @post '*ret_btd' points to the blinktree data node if a leaf 
 *       was found, or NULL if the lookup failed.
 */
int blinktree_lookup(struct btd **ret_btd, u64 tid, u64 nid)
{
    struct btn *c = NULL;
    int retval = 0;
    struct tree *tree = get_tree(tid);
    *ret_btd = NULL;

    if (unlikely(tree == NULL)) {
        retval |= TERR_NO_SUCH_TREE;
        goto out;
    }
    CLYDE_ASSERT(ret_btd != NULL);
    CLYDE_ASSERT(tree->root != NULL);

    c = find_leaf_no_path(tree, nid);
    if ( unlikely(c == NULL) ) {
        retval |= TERR_NO_SUCH_NODE;
        goto out;
    }

    *ret_btd = blinktree_scanleaf(c, nid);
out:
    return retval;
}


/** 
 * examines the node, searching for a subtree containing the
 * supplied key. If it is not found, it moves to the sibling
 * node (moving right) and continues the search.
 * Acquires and releases node locks as needed.
 * @post (*node) points to a subtree/leaf related to key
 * @post (*node* is locked by this thread 
 * @post if either of the two other post conditions fail, 
 *       BUG() out. 
*/
static __always_inline void move_right(struct btn **node, u64 key)
{
    struct btn *next_node;
    u8 i=0;

    CLYDE_ASSERT(node != NULL);
    CLYDE_ASSERT((*node) != NULL);
    CLYDE_ASSERT(node_is_locked(*node));

    printk("inside move_right - after assert checks\n");

    if (unlikely((*node)->numkeys == 0))
        return;

    next_node = *node; /*for first loop iteration*/
    
    do {
        printk("\t\tmove_right loop iter\n");
        *node = next_node;
        next_node = NULL;
        for (i=0; i < (*node)->numkeys; i++) {
            if (key <= (*node)->child_keys[i] ) {
                /*entry is bounded by this node, stop*/
                return;
            }
        }

        /*no subtree was found in this node, try sibling*/
        printk("\t\tmove_right: no next_node found\n...");
        
        if ((*node)->sibling) {
            next_node = (*node)->sibling;

            /*move to sibling, handle locks*/
            NODE_LOCK(next_node);
            NODE_UNLOCK((*node));
        }
        if (!(*node)->sibling && !(*node)->is_leaf) {
            printk("move_right: (*node)->sibling==NULL, reached the last internal node without finding");
            printk("a key greater than '%llu' (should ALWAYS have an inf key as the rightmost key for internal nodes)\n", key);
            BUG();
        }
    } while (next_node);
    /*to get here, we must be at the rightmost leaf node of the tree*/
}

/** 
 * Given the "old" parent, writes the new key for node_right, then modifies the old key
 * for the new contents of node_left to ensure consistency throughout the update procedure.
 * @return parent node of node_right
 * @post the two child nodes (node_left, node_right) are 
 *       unlocked.
 * @post parent node(s) are updated to reflect the split of the child node
 * @note parent of node_right is the node into which a new insertion has been made, thus
 *       also where the insert algorithm must continue
 */
static __always_inline struct btn* patch_parents_children_entries(struct btn *parent_start, struct btn *node_left, struct btn *node_right)
{
   /* 
    * When updating the "parent", we need to ensure reachability to all nodes, except the newest 
    * node entry, throughout the update process - otherwise the latch-free reading functionality 
    * is broken. 
    *  
    * Furthermore, we must take into account the possibility of the parent node itself having 
    * been split since we descended down through it. Thus the old node entry which needs updating 
    * and the new node entry which we need to insert may potentially be distributed among two 
    * different, new nodes.  
    */
   struct btn *node_parent, *pn, *cn;
   u64 hk_nl, hk_nr;
   u8 nl_entry_ndx;

   /*Checking preconditions*/
   CLYDE_ASSERT(node_is_locked(parent_start));
   CLYDE_ASSERT(node_is_locked(node_left));
   CLYDE_ASSERT(node_is_locked(node_right));
   printk("inside patch_parents_children_entries\n\t\tbefore assignments\n");

   node_parent = NULL;
   cn = parent_start;

   printk("\t\tbefore grabbing first nl_p\n");
   hk_nr = node_high_key(node_right); /*temporary val, is equal to node_left's old high key*/
   /*find old node entry, will have old node's high key which is new node's hk*/
   while (1) {
       /* 
        * old node's high key will always be at most equal to the highkey of the parent 
        * - if the node is the rightmost leafnode, its parent entry has key TREE_MAX_NID 
        * - if the node is the rightmost non-leaf node of that level, its HK is TREE_MAX_NID and its parent entry has key TREE_MAX_NID
        * - if the node is not the rightmost node, whether leaf or internal, its HK is less or equal to the parent's HK 
        */
       if (hk_nr <= node_high_key(cn)) {
           node_parent = cn;
           break;
       }
       pn = cn;
       cn = cn->sibling;
       if (likely(cn != NULL)) {
           NODE_LOCK(cn);
       }else{
           u8 i;
           pr_err(" (nl_p) proceded through all internal nodes until hitting the last node - should NEVER happen (rightmost node should have inf as last key)\n");
           for (i=0;i<pn->numkeys; i++)
               printk("k(%llu), ", pn->child_keys[i]);
           printk("\n");
           BUG();
       }
       NODE_UNLOCK(pn);
   }

   /*determine high keys*/
   hk_nl = node_high_key(node_left);
   /*old node's high key will always be the hk of the new rightmost node, => get the hk which 
     the parent referenced the old node by.*/
   hk_nr = node_keyof_node(node_parent, node_left);
   printk("determined high keys: nl_hk(%llu), nr_hk(%llu)\n", hk_nl, hk_nr);

   printk("\t\tbefore grabbing first nr_p\n");
   /*find node into which the entry for node_right needs to be inserted*/
   

   printk("\t\tbefore checking if nl_p and nl_r are set\n");
   if ( !node_parent ) {
       printk("blinktree,patch_parents_children_entries: did not find 'both' parents\n");
       BUG();
   }
   
   /*node_left, node_right, nl_p and nr_p are locked and found*/

   /*to achieve consistency, the entry for node_right needs to be inserted before the 
     entry for node_left is adjusted to a lower high-key. Otherwise certain keys would
     be unreachable for a moment => inconsistent tree*/
   printk("\t\tbefore patching\n");
   node_insert_entry(node_parent, hk_nr, node_right); /*can in itself trigger a new split*/
   nl_entry_ndx = node_indexof_node(node_parent, node_left);
   smp_mb();
   node_parent->child_keys[nl_entry_ndx] = hk_nl; /*update nl entry's hk to reflect what's left in nl node*/


   printk("\t\tbefore unlocking\n");
   NODE_UNLOCK(node_right);
   NODE_UNLOCK(node_left);
   
   return node_parent; /*retain lock on node and return it*/
}

/** 
 * Insert a node into the tree identified by the id in 'tid'. 
 * @param tid the tree identifier 
 * @param nid the node id of the new node 
 * @param data the data to insert. 
 * @return 0 on success, negative on error. E.g. 
 *         TERR_ALLOC_FAILED => allocation errors
 *         TERR_NO_SUCH_TREE => no tree by 'tid'
 */
int blinktree_node_insert(u64 tid, u64 nid, void *data)
{
    /*
        FIXME
           Still theoretically vulnerable to the following:
           1) reads tree root, begins data insertion
           2) reads same tree root, begins data insertion
           2) finishes insertion by splitting the nodes all the way to and including root
           --some other work happens--
           1) splits nodes all the way up to the old root which both (1) and (2) started with
             -- realises it needs to split the "old_root" again, thus, to its mind, forming a new
                root.
           (maybe reading the root node from the list, comparing to ensure no new root as appeared
            since this thread started the insertion -- if so, split and insert key into the already created root)
     
       FIXME:
         clean up memory ?
       FIXME:
         check functions which can legitimately fail and act on it
       FIXME:
         ensure non-zero (error) codes actually communicate something
    */
    struct btn *node;
    struct stack tree_path;
    int retval = 0;
    struct tree *tree;

    CLYDE_ASSERT(nid != TREE_MAX_NID); /*reserved value, read definition*/
    
    printk("before get_tree\n");
    tree = get_tree(tid);
    if (unlikely(tree == NULL)) {
        /*No tree found by id 'tid'.*/
        retval |= TERR_NO_SUCH_TREE;
        goto err_no_such_tree;
    }
    CLYDE_ASSERT(tree->root != NULL);
    CLYDE_ASSERT(data != NULL);

    printk("before clydefscore_stack_init\n");
    if (clydefscore_stack_init(&tree_path, __BLINKTREE_EXPECTED_HEIGHT)) {
        retval |= TERR_ALLOC_FAILED; /* stack allocation failed */
        goto err_stack_alloc;
    }

    printk("before find_leaf\n");
    node = find_leaf(tree->root, nid, &tree_path);
    if (node == NULL) {
        /*find_leaf got stuck somewhere on an internal node*/
        pr_warn("blinktree_insert: find_leaf got stuck on an internal node and returned NULL\n");
        retval |= TERR; 
        goto out;
    }

    printk("after find_leaf (is lock contended?: %d)\n", node_is_locked(node));
    NODE_LOCK(node);
    printk("before move_right\n");
    move_right(&node,nid);

    printk("before blinktree_scanleaf\n");
    if (blinktree_scanleaf(node, nid)){
        NODE_UNLOCK(node);
        retval = 0;
        goto out; /*nid already in tree*/
    }

    printk("blinktree_insert: node_insert(node, %llu, data)\n", nid);
    node_insert_entry(node,nid,data);

split_or_done:
    printk("\t\tinside 'split_or_done'\n");
    
    if (likely(node->numkeys <= (tree->k * 2))) {
        /* 
         * node may be filled, but it doesn't require splitting 
         * => SAFE INSERT 
         */
        NODE_UNLOCK(node);
        retval = 0;
        goto out;
    } else {
        /*
         * Node requires splitting
         * => UNSAFE INSERT
         */
        struct btn *node_right;
        
        /*empty stack=> root node (splitting the root is unique)*/
        u8 is_root = (clydefscore_stack_size(&tree_path) == 0);
        printk("\t\t_after_ is_root assignment\n");

        printk("\t\tbefore node_split\n");
        node_right = node_split(tree, node, is_root); 
        if (unlikely(node_right == NULL)) {
            pr_warn("blinktree_insert: unsafe insert requiring split failed, could not allocate new sibling node\n");
            retval |= TERR_ALLOC_FAILED;
            goto out;
        }
        

        /*
         * INVARIANT: newly inserted key was strictly less than the 
         *    old node's high key (as if it had been higher, the operation
         *    had taken place in the sibling node, had it been equal, the
         *    algorithm would have terminated before this step)
         */
        if (likely(!is_root)) {
            struct btn *node_left = node;
            printk("\t\tis_root check: node is NOT root\n");
           /* printk("\t\t (!is_root) - stack size: %u\n", clydefscore_stack_size(&tree_path));
            printk("\t\tbefore popping (parent)node\n");*/
            node = (struct btn*)clydefscore_stack_pop(&tree_path);
            /*printk("\t\t\tnodedbg: nid(%llu)\n", node->nid);
            printk("\t\t\tnodedbg: is_leaf(%u)\n", node->is_leaf);
            printk("\t\t\tnodedbg: numkeys(%u)\n", node->numkeys);
            printk("\t\t\tnodedbg: lock addr(%p)\n", node->lock);
            printk("\t\t\tnodedbg: is lock null ? (%d)\n", (node->lock==NULL));
            printk("\t\tbefore locking (parent)node\n");*/
            NODE_LOCK(node);
            /*have both parent (node) and child (node_left & node_right) locks.*/
            printk("\t\tbefore patch_parent_children_entries nl_hk(%llu), nr_hk(%llu)\n", node_high_key(node_left), node_high_key(node_right));
            node = patch_parents_children_entries(node, node_left, node_right);
            printk("before goto split_or_done\n");
            /* 
             * block postcondition: retain lock for 'node' 
             */
            goto split_or_done;
            
        } else {
            /*just split the root node, make a new root node*/
            struct btn *root = NULL;
            printk("\t\tis_root check: node is root\n");
            printk("\t\tbefore make_node\n");
            if (make_node(tree, &root, acquire_nid(), INTERNAL_NODE)) {
                /*failed to create new node*/
                printk("blinktree_insert: Failed to create new root node, presumably allocation failed.\n");
                retval |= TERR_ALLOC_FAILED;
                goto out;
            }
            printk("\t\tbefore locking root->lock\n");
            NODE_LOCK(root); /*required by node_insert, even if not strictly necessary*/
            printk("\t\tbefore inserting elements into root\n");
            node_insert_entry(root, node_high_key(node), node);
            node_insert_entry(root, TREE_MAX_NID, node_right); /*Ensure any possible key can enter this sub-tree*/
            printk("\t\tbefore updating root tree\n");
            TREE_LIST_LOCK();
            set_tree_root(tid,root);
            TREE_LIST_UNLOCK();

            NODE_UNLOCK(node_right);
            NODE_UNLOCK(node);
            NODE_UNLOCK(root);
            retval = 0;
            goto out; /*all done*/
        }
    }

out:
    /*precondition: all locks have been released*/
    clydefscore_stack_free(&tree_path);
err_stack_alloc:
err_no_such_tree:
    return retval;
}

/**
 * Remove node identified by the supplied key. 
 * @description locates the node identified by the key 'key' 
 *  in the tree identified by 'tid'
 * @param tid the tree identifier, identifiying the tree 
 *        in which the node resides.
 * @param nid the node identifier, uniquely identifying 
 *        the node within the tree. 
 * @return 0 on success, errors otherwise, e.g. 
 *         TERR_NO_SUCH_NODE => no such node
 *         TERR_NO_SUCH_TREE => supplied tid didn't match a tree
 *         TERR_ALLOC_FAILED => allocation of stack to remember
 *         tree path failed.
 */
int blinktree_node_remove(u64 tid, u64 nid)
{
    /* 
     * Scenarios 
     * --simple 
     *  remove the entry, there's still k entries left, no merging required,
     *  nid was not the node high-key, no further adjustments needed in path,stop
     * --not-so-simple 
     *  remove the entry, the entry is the node's high-key.
     *  - Still k entries left, no merging required,
     *  - Restart algorithm @ parent level (we might have removed the parent's HK entry, or might
     *      have removed sufficient nodes to warrant a merger)
     * --"complex" 
     *  remove the entry (may be the high key, no matter), less than k nodes left,
     *  merging required unless node is root or rightmost entry of the level.
     *      (could merge left if the rightmost element, but the added complexity is unwanted)
     *  
     *  Merging (described above)
     *  "Balancing"
     *  
     *  Is *not* handled in classic b-link trees. Yielding good performance at the cost
     *  of poorer space utilisation (and conceivably degraded performance later on)
     */ 

    /*FIXME: this simplistic approach can use find_leaf_no_path*/
    struct tree *tree;
    struct btn *node;
    struct stack tree_path;
    int retval = 0;

    CLYDE_ASSERT(nid != TREE_MAX_NID); /*reserved value, read definition*/

    tree = get_tree(tid);
    if (unlikely(tree == NULL)) {
        retval |= TERR_NO_SUCH_TREE;
        goto out;
    }
    CLYDE_ASSERT(tree->root != NULL);

    printk("before clydefscore_stack_init\n");
    if (clydefscore_stack_init(&tree_path, __BLINKTREE_EXPECTED_HEIGHT)) {
        retval |= TERR_ALLOC_FAILED; /* stack allocation failed */
        goto err_stack_alloc;
    }

    printk("before find_leaf\n");
    node = find_leaf(tree->root, nid, &tree_path);
    if (node == NULL) {
        /*find_leaf got stuck somewhere on an internal node*/
        printk("blinktree_node_remove: find_leaf got stuck on an internal node and returned NULL\n");
        retval |= TERR_NO_SUCH_NODE; 
        goto out;
    }

    printk("after find_leaf (is lock contended?: %d)\n", node_is_locked(node));
    NODE_LOCK(node);
    printk("before move_right\n");
    move_right(&node,nid);
    /*FIXME: if this entry is data, I'd need to free the associated structure */
    if (node_remove_entry(node, nid) == NO_SUCH_ENTRY) {
        NODE_UNLOCK(node);
        retval = NO_SUCH_ENTRY;
        goto out;
    }
    NODE_UNLOCK(node);

out:
    clydefscore_stack_free(&tree_path);
err_stack_alloc:
    return retval;
}

#ifdef DBG_FUNCS

static void blinktree_print_node(struct btn *node, int depth)
{
    u8 i, d;
    d = depth + 1;
    if (node->is_leaf) {
        if(depth == 0)
            printk("root-l: ");
        else
            printk("n-l(%u):", depth);
        for(i=0; i<node->numkeys; i++) {
            printk("%llu, ", node->child_keys[i]);
        }
    } else {
        if(depth == 0)
            printk("  root: ");
        else
            printk("n(%u): ", depth);
        for(i=0; i < node->numkeys; i++) {
            blinktree_print_node(node->child_nodes[i], d);
        }
    }
}

/*
 * prints the nodes of the tree sorted by
 * their key values.
 */
void dbg_blinktree_print_inorder(u64 tid)
{
    struct tree *tree;
    tree = get_tree(tid);
    CLYDE_ASSERT(tree != NULL);
    CLYDE_ASSERT(tree->root != NULL);

    blinktree_print_node(tree->root, 0);
    printk("\n");
}


static void blinktree_get_node_keys(struct btn *node, struct stack *s)
{
    int i;
    printk("before if-check\n");
    if (node->is_leaf) {
        /*printk("node->is_leaf => TRUE\n");*/
        for(i=node->numkeys-1; i >= 0; i--) {
            /*printk("leaf iter\n");*/
            clydefscore_stack_push(s,&(node->child_keys[i]));
        }
    } else {
        /*printk("node->is_leaf => FALSE\n");*/
        for(i=node->numkeys-1; i >= 0; i--) {
            /*printk("non-leaf iter\n");*/
            blinktree_get_node_keys(node->child_nodes[i],s);
        }
    }
}

/*
 * visits the nodes of the tree, pushing all node keys onto the stack
 * --- 
 *  postcondition:
 *      - stack 's' holds all the nodes such that popping the stack
 *      yields the node keys in order.
*/
void dbg_blinktree_getkeys(u64 tid, struct stack *s)
{
    struct tree *tree = get_tree(tid);
    CLYDE_ASSERT(tree != NULL);
    CLYDE_ASSERT(tree->root != NULL);

    blinktree_get_node_keys(tree->root, s);
}

static void blinktree_get_nodes(struct btn *node, struct stack *s)
{
    int i;
    if (! node->is_leaf) {
        for(i=node->numkeys-1; i>= 0; i--) {
            blinktree_get_nodes(node->child_nodes[i], s);
        }
    } else {
        clydefscore_stack_push(s, node);
    }
}

/*
 * visits the nodes of the tree, pushing all node keys onto 
 * the stack in reverse order.
 * ---
 * postcondition:
 *  stack 's' holds all nodes, popping the stack yields an in-order
 *  traversal.
 */
void dbg_blinktree_getnodes(u64 tid, struct stack *s)
{
    struct tree *tree = get_tree(tid);
    CLYDE_ASSERT(tree != NULL);
    CLYDE_ASSERT(tree->root != NULL);

    blinktree_get_nodes(tree->root, s);
}
#endif //DBG_FNCS
