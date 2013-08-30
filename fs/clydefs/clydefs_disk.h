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
    /**Location of the INO reclamation table */
    struct cfs_node_addr fs_ino_tbl;

    /**Next free INO to give, only use iff ino_tbl_entries == 0 */ 
    u64 ino_nxt_free;
    /***Start offset into ino tbl containing reclaimable ino's */
    u64 ino_tbl_start;
    /**End offset of area containing reclaimable ino's*/
    u64 ino_tbl_end;
};

/** 
 * On-disk structure of ClydeFS inode entry.
 */                                           
struct cfsd_ientry {
    /**inode number, unique to the inode */ 
    __le64 ino;
    /**inode owner user id */ 
    __le32 uid;
    /**inode owner group id */ 
    __le32 gid;
    /**time of last modification */ 
    cfsd_time_t mtime;
    /**time of creation*/ 
    cfsd_time_t ctime;
    /**size, in bytes, of associated data */ 
    __le64 size_bytes;
    /**id of associated data node. If a file, contains the file 
     * data, if a directory, contains the directory's inode tbl*/ 
    __le64 data_nid;
    /**inode usage count, i_count */ 
    __le32 icount;
    /**actual length of name, at most CFS_NAME_LEN*/
    __le16 nlen;
    /**access mode 0ugo*/ 
    __le16 mode;
    /**name of inode/dentry */ 
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
 * Copy data from an in-memory superblock representation into an
 * on-disk representation. 
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
    dst->ino_nxt_free = cpu_to_le64(src->ino_nxt_free);
    dst->ino_tbl_start = cpu_to_le64(src->ino_tbl_start);
    dst->ino_tbl_end = cpu_to_le64(src->ino_tbl_end);
}

/** 
 * Copy data from on-disk superblock representation to an 
 * in-memory representation. 
 * @param dst the struct to populate with values from 'src' 
 * @param src the source of the values with which to populate 
 *            'dst'.
 */ 
static __always_inline void __copy2c_sb(struct cfs_sb *dst, struct cfsd_sb const * const src)
{
    dst->file_tree_tid = le64_to_cpu(src->file_tree_tid);
    __copy2c_nodeaddr(&dst->fs_inode_tbl, &src->fs_inode_tbl);
    dst->magic_ident = le32_to_cpu(src->magic_ident);
    dst->generation = le32_to_cpu(src->generation);
    dst->ino_nxt_free = le64_to_cpu(src->ino_nxt_free);
    dst->ino_tbl_start = le64_to_cpu(src->ino_tbl_start);
    dst->ino_tbl_end = le64_to_cpu(src->ino_tbl_end);
}

static __always_inline void __copy2c_timespec(struct timespec *dst, __le64 const * const src)
{ /*set inode timespec value from disk value*/
    dst->tv_sec = le64_to_cpu(*src);
}

static __always_inline void __copy2d_timespec(__le64 *dst, struct timespec const * const src)
{
    *dst = cpu_to_le64(src->tv_sec);
}

/**The size and layout of an inode chunk*/ 
struct cfsd_inode_chunk {
    /*u8 slack[CHUNK_LEAD_SLACK_BYTES]; -- skipped in inode.c code*/
    u8 entries_used;
    u8 last_chunk;
    struct cfsd_ientry entries[CHUNK_NUMENTRIES];
};

#endif /*__CLYDEFS_DISK*/
