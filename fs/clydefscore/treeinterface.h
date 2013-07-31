#ifndef __TREEINTERFACE_H
#define __TREEINTERFACE_H

#include <linux/types.h>

struct treeinterface {
    /**
     * Create a new tree.
     * @param k the k-value, nodes are split when there are 2k 
     *          children elements in them.
     * @return the tid (tree identifier) of the tree. 
     */
    u64 (*tree_create)(u8 k);
    /**
     * Remove a tree and any child elements. 
     * @param tid the tree identifier 
     * @return 0 on success, negative on errors, positive values as 
     *         status codes. 1 => no such tree
     */
    int (*tree_remove)(u64 tid);
    /**
     * Create a new node containing the data identified by 'data'. 
     * @param tid the tree identifier 
     * @param len the length of the data in bytes 
     * @param data a pointer to the data itself 
     * @return 0 on success. Negative values indicate errors. 
     *         -ENOMEM in particular if out of memory.
     *         -ENOENT => no tree by 'tid'
     * @post provided function returns successfully. 'nid' will be 
     *       set to the assigned node identifier.
     */
    int (*node_insert)(u64 tid, u64 *nid);
    /**
     * @param tid the tree identifier
     * @param nid the node identifier
     * @return 0 on success. Negative values on errors. Positive 
     *         values as status codes. -ENOENT => no such node
     */
    int (*node_remove)(u64 tid, u64 nid);
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
    int (*node_read)(u64 tid, u64 nid, u64 offset, u64 len, void *data);
    /** 
     * Write a sequence of bytes to the node. 
     * @param tid the identifier of the tree containing the node 
     * @param nid the node identifier 
     * @param offset the offset within the node from which to begin writing. In bytes. 
     * @param len length of data buffer which is to be written. 
     * @param data a buffer containing the data to write. 
     * @return 0 on success. Negative values on errors. -ENOENT if 
     *         the node doesn't exist, -ENOMEM if space allocation
     *         for the data failed. 
     */
    int (*node_write)(u64 tid, u64 nid, u64 offset, u64 len, void *data);
};


int treeinterface_init(void);

void treeinterface_exit(void);

#endif //__TREEINTERFACE_H
