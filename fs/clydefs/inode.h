#include <linux/fs.h>

#ifndef __CLYDEFS_INODE_H
#define __CLYDEFS_INODE_H
#include "clydefs.h"
//const struct inode_operations clydefs_dir_inode_ops;

/** 
 * Get the fs-specific parent structure containing the inode. 
 * @param i the inode reference 
 * @return the clydefs-specific parent structure containing the inode 
 */
static __always_inline struct cfs_inode *CFS_INODE(struct inode const * const i)
{
    return container_of(i, struct cfs_inode, vfs_inode);
}

/** 
 *  Return the tid of the inode tree
 *  @param csb the cfs-specific superblock information.
 *  @return tid of the inode tree
 */ 
static __always_inline u64 CFS_INODE_TID(struct cfs_sb *csb)
{
    return csb->fs_inode_tbl.tid;
}

/** 
 * return the identifier for the tree containing the data of 
 * this inode. 
 * @description inodes either model a directory or a file of 
 *              some sort. If the inode models a file, its data
 *              is stored in the file tree, thus the tid of the
 *              file tree is returned, otherwise, the inode
 *              models a directory and the inode tree tid is
 *              returned.
 * @param ci the inode whose data tid we want 
 * @return the tid of the tree in which the inode's node can be 
 *         found.
 */
static __always_inline u64 CFS_DATA_TID(struct cfs_inode const * const ci)
{
    struct cfs_sb const * const csb = CFS_SB(ci->vfs_inode.i_sb);
    return (ci->vfs_inode.i_mode | S_IFDIR) ? csb->fs_ino_tbl.tid : csb->file_tree_tid;
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

struct inode *cfsi_getroot(struct super_block *sb);

int cfsi_init(void);
void cfsi_exit(void);

#endif // __CLYDEFS_INODE_H
