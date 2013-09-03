#ifndef __CLYDEFS_GLOBAL_H
#define __CLYDEFS_GLOBAL_H

#include <linux/fs.h>
#include <linux/spinlock.h>

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
#define CLYDE_ERR(fmt, a...) printk(KERN_ERR "cfs<ERR, %s, %d>: " fmt, __FUNCTION__, __LINE__, ##a)

#define CFS_WARN(fmt, a...) printk(KERN_WARNING "cfs<WARN>: " fmt, ##a)

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

/**Number of reclaimable INO entries in the INO tbl (1mb, 
 * 8b/entry) */ 
#define RECLAIM_INO_MAX 131072ULL

#define CHUNK_NUMENTRIES 105U
#define CHUNK_LEAD_SLACK_BYTES 6U

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
    /**Generation value of superblock*/ 
    u32 generation;
    /**inode parent table for '/' (and JUST '/')   */
    struct cfs_node_addr fs_inode_tbl;
    /**ID of file tree */ 
    u64 file_tree_tid;

    /**INO reclamation table, for reusing inodes, can store XXX 
     * reclaimed id's before we leak. */
    struct cfs_node_addr fs_ino_tbl;

    /**Next free INO to give, only use iff ino_tbl_entries == 0 */ 
    u64 ino_nxt_free;
    /***Start offset into ino tbl containing reclaimable ino's */
    u64 ino_tbl_start;
    /**End offset of area containing reclaimable ino's*/
    u64 ino_tbl_end;
    /**Buffer used for ino transfers */ 
    u64 *ino_buf;

    /*transient values*/
    /**protect 'ino_*' fields */ 
    spinlock_t lock_fs_ino_tbl;
    /**protect 'generation' field */ 
    spinlock_t lock_generation;
    
    /**Location of the superblock table */ 
    struct cfs_node_addr superblock_tbl;
};

/** 
 * Describes the location of an ientry within 
 * a directory inode. 
 */
struct ientry_loc
{
    /**in which chunk was the entry located*/ 
    u64 chunk_ndx;
    /**offset within chunk */ 
    u64 chunk_off;
};

/** 
 * clydefs inode structure 
 */
struct cfs_inode {
    struct inode vfs_inode;

    /**true if the inode has been persisted or was loaded from 
     * disk, only look at dsk_* vars if this is set   */
    u8 on_disk;

    /**Location of where the ientry of this inode is located 
     * within the inode table of the parent directory. */ 
    struct ientry_loc dsk_ientry_loc;

    /**reference to parent inode whose itbl contains this inode's 
     * entry. , modify via i_lock*/
    struct cfs_inode *parent;

    /**Dentry holding the name under which this inode is 
     * persisted */ 
    struct dentry *ientry_dentry;

    /**Points to the inode's data, in case of a directory inode, 
     * this points to the directory's inode table in the inode 
     * tree. For files this points to the node in the file tree 
     * containing its data */ 
    struct cfs_node_addr data;
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

/*require asserts only if in debug mode*/
#ifdef CONFIG_CLYDEFS_DEBUG
#define CLYDE_ASSERT(x)                                                 \
do {    if (x) break;                                                   \
        printk(KERN_EMERG "### ASSERTION FAILED %s: %s: %d: %s\n",      \
               __FILE__, __func__, __LINE__, #x); dump_stack(); BUG();  \
} while (0);

#define CFS_DBG(fmt, a...) printk(KERN_ERR "cfs<%s>,%d -- " fmt, __FUNCTION__, __LINE__, ##a)
#else
#define CLYDE_ASSERT(x)
#define CFS_DBG(fmt, a...) 
#endif

#endif //__CLYDEFS_GLOBAL_H
