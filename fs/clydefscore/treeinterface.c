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
 */ 

/**
 * Create a new tree.
 * @param k the k-value, nodes are split when there are 2k 
 *          children elements in them.
 * @return the tid (tree identifier) of the tree. 
 */
u64 clydefscore_tree_create(u8 k)
{
    return 255;/*FIXME: decide code for indicating no tree could be made*/
}
EXPORT_SYMBOL_GPL(clydefscore_tree_create);

/**
 * Remove a tree and any child elements. 
 * @param tid the tree identifier 
 * @return 0 on success, negative on errors, positive values as 
 *         status codes. 1 => no such tree
 */
int clydefscore_tree_remove(u64 tid)
{
    return 1;
}
EXPORT_SYMBOL_GPL(clydefscore_tree_remove);

/**
 * Create a new node containing the data identified by 'data'. 
 * @param tid the tree identifier 
 * @param len the length of the data in bytes 
 * @param data a pointer to the data itself 
 * @return 0 on success. Negative values indicate errors. 
 *         -ENOMEM in particular if out of memory.
 */
int clydefscore_node_insert(u64 tid, ssize_t len, void *data)
{
    return -ENOMEM;
}
EXPORT_SYMBOL_GPL(clydefscore_node_insert);

/**
 * @param tid the tree identifier
 * @param nid the node identifier
 * @return 0 on success. Negative values on errors. Positive 
 *         values as status codes. 1 => no such node
 */
int clydefscore_node_remove(u64 tid, u64 nid)
{
    return 1;
}
EXPORT_SYMBOL_GPL(clydefscore_node_remove);

/**
 * Read a sequence of data from the node 
 * @param tid the tree identifier 
 * @param nid the node identifier 
 * @param offset the offset within the node 
 * @param len amount to read in bytes 
 * @param data a buffer guaranteed to be at least large enough 
 *         to hold the requested data
 * @return 0 on success. Negative values on errors: -ENOENT if 
 *         the node doesn't exist
 */
int clydefscore_node_read(u64 tid, u64 nid, u64 offset, void *data)
{
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(clydefscore_node_read);

/*update(tid, nid, offset, [len,data])*/
/** 
 * Write a sequence of bytes to the node. 
 * @param tid the identifier of the tree containing the node 
 * @param nid the node identifier 
 * @param offset the offset within the node from which to begin writing. In bytes. 
 * @param data a buffer containing the data to write. 
 * @return 0 on success. Negative values on errors. -ENOENT if 
 *         the node doesn't exist, -ENOMEM if space allocation
 *         for the data failed. 
 */
int clydefscore_node_write(u64 tid, u64 nid, u64 offset, void *data)
{
    return -ENOENT;
}
EXPORT_SYMBOL_GPL(clydefscore_node_write);
