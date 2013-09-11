#include <linux/fs.h>

#ifndef __CLYDEFS_INODE_H
#define __CLYDEFS_INODE_H
#include "clydefs.h"
#include "clydefs_disk.h"

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
    return (ci->vfs_inode.i_mode & S_IFDIR) ? csb->fs_inode_tbl.tid : csb->file_tree_tid;
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

/** 
 * Populate cfs inode with values from a disk inode entry. 
 * @param dst the inode to populate 
 * @param src the inode entry read from disk 
 * @pre the i_lock associated the inode is locked 
 * @post dst will be populated with the values from the disk 
 *       inode entry, converted to the native CPU format.
 * @note does not read nlen & name. 
 * @note the tid of 'data' still need to be set 
 *       with the superblock values.
 */ 
static __always_inline void __copy2c_inode(struct cfs_inode *dst, struct cfsd_ientry const * const src)
{
    struct inode *vfs_i = NULL;
    CLYDE_ASSERT( dst != NULL );
    CLYDE_ASSERT( src != NULL );
    CLYDE_ASSERT( spin_is_locked(&dst->vfs_inode.i_lock) );

    vfs_i = &dst->vfs_inode;

    /*set regular inode fields*/
    vfs_i->i_ino = le64_to_cpu(src->ino);
    vfs_i->i_uid = le32_to_cpu(src->uid);
    vfs_i->i_gid = le32_to_cpu(src->gid);
    __copy2c_timespec(&vfs_i->i_ctime, &src->ctime);
    __copy2c_timespec(&vfs_i->i_mtime, &src->mtime);
    __copy2c_timespec(&vfs_i->i_atime, &src->mtime); /*we don't record access time*/
    vfs_i->i_size = le64_to_cpu(src->size_bytes);
    dst->data.nid = le64_to_cpu(src->data_nid);
    atomic_set(&vfs_i->i_count,le32_to_cpu(src->icount));
    /*nlen omitted, not represented in inode*/
    vfs_i->i_mode = le16_to_cpu(src->mode);
    /*name omitted, not represented in inode*/
}

/** 
 * Copies in-memory values into on-disk inode representation.
 * @param dst the on-disk representation to populate 
 * @param src the in-memory representation from which the values 
 *            are drawn.
 */ 
static __always_inline void __copy2d_inode(struct cfsd_ientry *dst, struct cfs_inode const * const src)
{
    struct inode const *i = NULL;

    CLYDE_ASSERT(dst != NULL);
    CLYDE_ASSERT(src != NULL);
    i = &src->vfs_inode;

    /*NOTE: does not set nlen & name -- if persisting to an ientry, use */
    dst->ino = cpu_to_le64(i->i_ino);
    dst->uid = cpu_to_le32(i->i_uid);
    dst->gid = cpu_to_le32(i->i_gid);
    __copy2d_timespec(&dst->mtime, &i->i_mtime);
    __copy2d_timespec(&dst->ctime, &i->i_ctime);
    dst->size_bytes = cpu_to_le64(i->i_size);
    dst->data_nid = cpu_to_le64(src->data.nid);
    dst->icount = cpu_to_le32(atomic_read(&i->i_count));
    /*nlen  -- OMITTED*/
    dst->mode = cpu_to_le16(i->i_mode);
    /*name -- OMITTED*/
}

static __always_inline void cfsi_i_wlock(struct cfs_inode *ci)
{
    CLYDE_ASSERT(ci != NULL);
    mutex_lock(&ci->io_mutex);
    smp_mb();
}

static __always_inline void cfsi_i_wunlock(struct cfs_inode *ci)
{
    CLYDE_ASSERT(ci != NULL);
    smp_mb();
    mutex_unlock(&ci->io_mutex);
}


struct inode *cfsi_getroot(struct super_block *sb);

int cfsi_init(void);
void cfsi_exit(void);

#endif // __CLYDEFS_INODE_H
