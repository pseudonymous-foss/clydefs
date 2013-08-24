#ifndef __CLYDEFS_DISK
#define __CLYDEFS_DISK
#include "clydefs.h"

/*
Holds various structures as they appear when persisted to disk
*/

/**On-disk structure of a [c,m,a]time field*/
typedef __le64 cfsd_time_t; /*64bit suffices as we aim only to track time at a per-second granularity*/

/**how many superblocks are stored in the superblock tbl node ?*/
#define CLYDE_NUM_SB_ENTRIES 2U

struct cfsd_node_addr {
    __le64 tid;
    __le64 nid;
};

/** 
 * On-disk structure of ClydeFS superblock.
 */  
struct cfsd_sb {
    /**ClydeFS magic number */ 
    __le32 magic_ident;
    /**Generation number, used to determine newest superblock */ 
    __le32 generation;
    /**Identifier for tree holding file nodes*/
    __le64 file_tree_tid;
    /**Location of root directory inode table */ 
    struct cfsd_node_addr fs_inode_tbl;
};

struct __inode_unused {
    __le16 one;
};

/** 
 * On-disk structure of ClydeFS inode entry.
 */                                           
struct cfsd_ientry {
    /*These fields generally match fs.h 'struct inode' fields*/
    __le64 ino;
    __le32 uid_t;
    __le32 gid_t;
    cfsd_time_t mtime;
    cfsd_time_t ctime;
    __le64 size_bytes;
    __le16 mode;
    struct __inode_unused garbage;
    /**actual length of name*/
    u32 nlen;
    /**Address of inode's inode table, set iff. this inode is a 
     * directory */ 
    struct cfsd_node_addr inode_tbl;
    /**name of inode/dentry*/
    unsigned char name[CFS_NAME_LEN];
};


/** 
 * Copy node address from in-memory representation to disk 
 * representation 
 * @param dst the struct to populate with values from 'src' 
 * @param src the source of the values with which to populate 
 *            'dst'
 */ 
static __always_inline void __copy2d_nodeaddr(struct cfsd_node_addr *dst, struct cfs_node_addr const * const src)
{
    dst->tid = cpu_to_le64(src->tid);
    dst->nid = cpu_to_le64(src->nid);
}

/** 
 * Copy node-address from disk representation to in-memory 
 * representation. 
 * @param dst the struct to populate with values from 'src' 
 * @param src the source of the values with which to populate 
 *            'dst'
 */ 
static __always_inline void __copy2c_nodeaddr(struct cfs_node_addr *dst, struct cfsd_node_addr const * const src)
{
    dst->tid = le64_to_cpu(src->tid);
    dst->nid = le64_to_cpu(src->nid);
}

/** 
 * @param dst the struct to populate with values from 'src' 
 * @param src the source of the values with which to populate 
 *            'dst'
 */ 
static __always_inline void __copy2d_sb(struct cfsd_sb *dst, struct cfs_sb const * const src)
{
    dst->file_tree_tid = cpu_to_le64(src->file_tree_tid);
    __copy2d_nodeaddr(&dst->fs_inode_tbl, &src->fs_inode_tbl);
    dst->magic_ident = cpu_to_le32(src->magic_ident);
    dst->generation = cpu_to_le32(src->generation);
}

static __always_inline void __copy2c_sb(struct cfs_sb *dst, struct cfsd_sb const * const src)
{
    dst->file_tree_tid = le64_to_cpu(src->file_tree_tid);
    __copy2c_nodeaddr(&dst->fs_inode_tbl, &src->fs_inode_tbl);
    dst->magic_ident = le32_to_cpu(src->magic_ident);
    dst->generation = le32_to_cpu(src->generation);
}

#define CHUNK_NUMENTRIES 32U
#define CHUNK_BEGIN_OFF 400U

/**The size and layout of an inode chunk*/ 
struct cfsd_inode_chunk {
    struct cfsd_ientry entries[CHUNK_NUMENTRIES];
};

#endif /*__CLYDEFS_DISK*/
