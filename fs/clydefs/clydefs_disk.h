#ifndef __CLYDEFS_DISK
#define __CLYDEFS_DISK

/*
Holds various structures as they appear when persisted to disk
*/

struct cfs_disk_node_addr {
    __le64 tid;
    __le64 nid;
};

/** 
 * On-disk structure of ClydeFS superblock.
 */  
struct cfs_disk_sb {
    /**ClydeFS magic number */ 
    __le32 magic_ident;
    /**Identifier for tree holding inode information*/ 
    __le64 inode_tree_tid;
    /**Identifier for tree holding file nodes*/
    __le64 file_tree_tid;
};

#endif /*__CLYDEFS_DISK*/
