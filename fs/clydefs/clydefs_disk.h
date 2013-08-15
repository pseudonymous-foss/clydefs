#ifndef __CLYDEFS_DISK
#define __CLYDEFS_DISK

/*
Holds various structures as they appear when persisted to disk
*/

/**how many superblocks are stored in the superblock tbl node ?*/
#define CLYDE_NUM_SB_ENTRIES 2U

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
    /**Generation number, used to determine newest superblock */ 
    __le32 generation;
    /**Identifier for tree holding inode information*/ 
    cfs_disk_node_addr inode_root;
    /**Identifier for tree holding file nodes*/
    __le64 file_tree_tid;
};

#endif /*__CLYDEFS_DISK*/
