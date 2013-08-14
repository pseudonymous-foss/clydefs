#ifndef __LINUX_TREE_H
#define __LINUX_TREE_H

/*minimum values given to tree and node identifiers*/
#define TREE_MIN_TID 1
#define TREE_MIN_NID 1

enum TERR {
    /**general error, always set if an error of any kind 
     * occurred. */ 
    TERR = 1,
    /**one or more allocations failed */ 
    TERR_ALLOC_FAILED = ((1 << 2) | 1),
    /**if the id of a non-existing tree was supplied to an 
     * operation, this is set */
    TERR_NO_SUCH_TREE = ((1 << 3) | 1),
    /**if the id of a non-existing node was supplied to an 
     * operation, this is set */ 
    TERR_NO_SUCH_NODE = ((1 << 4) | 1),
    /**if an operation is temporarily impossible to fullfil but 
     * expected possible shortly, this is set*/ 
    TERR_BUSY = ((1 << 5) | 1),
    /**if an operation fails because one or more bio's return an 
    error*/
    TERR_IO_ERR = ((1 << 6) |1), 
};

/** 
 * extended bio data for the tree-based
 * interface.
 */ 
struct tree_iface_data {
    u8 cmd;         /*one of the vendor-specific AOECMD_* codes*/
    u64 tid;
    u64 nid;
    u64 off;
    u64 len;
    u64 err;
};

/**TREE interface vendor-specific AOE commands*/
enum AOE_CMD {
    /*Support our vendor-specific codes*/
    AOECMD_CREATETREE = 0xF0,   /*create a new tree*/
    AOECMD_REMOVETREE,          /*remove a tree and all its child nodes*/
    AOECMD_READNODE,            /*read data from an node*/
    AOECMD_INSERTNODE,          /*create a new node with some initial data*/
    AOECMD_UPDATENODE,          /*update the data of an existing node*/
    AOECMD_REMOVENODE,          /*remove the node and associated data*/
};

#endif //__LINUX_TREE_H
