#ifndef __CLYDEFS_GLOBAL_H
#define __CLYDEFS_GLOBAL_H

#include <linux/fs.h>

/*FIXME there must be a better way*/
#define U64_MAX_VALUE 18446744073709551615ULL

/** FS-specific identifier value */
#define CFS_MAGIC_IDENT 0x20140106U

/** Maximum links to a file */
#define CFS_MAX_LINKS 100U
/** Maximum size of a file, set to max of what VFS handles */
#define CFS_MAX_FILESIZE MAX_LFS_FILESIZE

/** Size of a single block */
#define CFS_BLOCKSIZE           PAGE_CACHE_SIZE
/** Shift to get size of a single block (e.g. 13 for 4096 bytes)*/
#define CFS_BLOCKSIZE_SHIFT     PAGE_CACHE_SHIFT

/*wrap errors*/
#define CLYDE_ERR(fmt, a...) printk(KERN_ERR "clydefs: " fmt, ##a)

#define CLYDE_STUB printk(KERN_ERR "clydefs: %s<%d> is a STUB\n", __FUNCTION__, __LINE__)

/**Which inode number to hand out to the inode representing 
 * root */ 
#define CFS_INO_ROOT 1
/**Minimum inode number to hand out to an inode*/ 
#define CFS_INO_MIN 2
/**Maximum inode number to hand out to an inode*/ 
#define CFS_INO_MAX U64_MAX_VALUE

/**Number of bytes reserved for filename length*/ 
#define CFS_NAME_LEN 256

struct cfs_node_addr {
    u64 tid;
    u64 nid;
};

/** 
 * clydefs-specific addition information. 
 * Can be pointed to by s_fs_info of the superblock. 
 */ 
struct cfs_sb {
    /*Persisted values*/
    /**FS magic identifier, as read from the persisted superblock */ 
    u32 magic_ident;
    /**Generation value of superblock */ 
    u32 generation;
    /**inode parent table for '/' (and JUST '/')   */
    struct cfs_node_addr fs_inode_tbl;
    /**ID of file tree */ 
    u64 file_tree_tid;

    /*transient values*/
    /**Location of the superblock table */ 
    struct cfs_node_addr superblock_tbl;
};

/** 
 * clydefs inode structure 
 */
struct cfs_inode {
    struct inode vfs_inode;

    /**Address of the node holding this inode entry */ 
    struct cfs_node_addr parent_tbl;
    /**Address to inode's own inode table, only set if inode is a 
     * directory. */ 
    struct cfs_node_addr inode_tbl;
};

/*========= INLINE FUNCTIONS */
/** 
 *  Return the cfs-specific superblock info.
 *  @param sb the FS superblock
 *  @return the CFS-specfic superblock info
 */ 
static __always_inline struct cfs_sb *CFS_SB(struct super_block *sb)
{
    return (struct cfs_sb *)sb->s_fs_info;
}

/** 
 * Lock inode. 
 * @description locks the inode, should be done at least when 
 *              setting i_blocks, i_bytes, i_size.
 * @param ci the cfs inode to lock 
 * @post ci is locked 
 */ 
static __always_inline void CFSI_LOCK(struct cfs_inode *ci)
{
    spin_lock(&ci->vfs_inode.i_lock);
}

/** 
 * Unlock inode. 
 * @description unlocks the inode. Do so immediately after 
 *              finishing operations requiring a lock
 * @param ci the cfs inode to unlock 
 * @post ci is unlocked 
 */ 
static __always_inline void CFSI_UNLOCK(struct cfs_inode *ci)
{
    spin_unlock(&ci->vfs_inode.i_lock);
}

/*require asserts only if in debug mode*/
#ifdef CONFIG_CLYDEFS_DEBUG
#define CLYDE_ASSERT(x)                                                 \
do {    if (x) break;                                                   \
        printk(KERN_EMERG "### ASSERTION FAILED %s: %s: %d: %s\n",      \
               __FILE__, __func__, __LINE__, #x); dump_stack(); BUG();  \
} while (0);

#define CLYDE_DBG(fmt, a...) printk(KERN_ERR "cfs<%s>,%d -- " fmt, __FUNCTION__, __LINE__, ##a)
#else
#define CLYDE_ASSERT(x)
#define CLYDE_DBG(fmt, a...) 
#endif

#endif //__CLYDEFS_GLOBAL_H
