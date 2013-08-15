#include <linux/fs.h>
#include "clydefs.h"
#include "inode.h"
#include "io.h"

/** 
 * Read inode information from backing device. 
 * @pre cfs_i  
 */ 
static __read_cfs_inode(struct super_block *sb, struct cfs_inode *cfs_i)
{
    int retval;
    retval = cfsio_read_node_sync(
        sb->s_bdev, 
        NULL, NULL, 
        tid, nid, off, len, buf
    );

    if (retval) {
        CLYDE_ERR("Failed to read inode ", cfs_i->vfs_inode.i_ino)
        goto err;
    }

    cfs_i->vfs_inode.i_s
}




int clydefs_inode_create(struct inode *dir, struct dentry *d, umode_t mode, bool excl)
{
    printk(KERN_INFO "clydefs_inode_create called\n");
    return -1;
}

#if 0
//Silly idea, would have as many lookups as dentries here
/** 
 * Get inode identified by number 'ino'.
 * @param sb file system superblock 
 * @param ino desired inode number 
 * @note may incur disk activity if the inode is not already in 
 *       the inode cache.
 * @return inode reference or an ERR_PTR, -ENOMEM for failure to 
 *         allocate inode.
 */
struct inode *cfsi_iget(struct super_block *sb, unsigned long ino)
{
    /* 
        FIXME
            - unlock inode before returning 
     */ 
    struct cfs_sb *cfs_sb = cfs_sb_get(sb);
    struct inode *i = NULL;
    struct cfs_inode *cfs_i = NULL;

    /*verify inode number in allowed range*/
    CLYDE_ASSERT(ino >= CFS_INO_MIN && ino <= CFS_INO_MAX);

    i = iget_locked(sb, ino);
    if (!i) { /*no inode in cache, couldn't allocate one*/
        return ERR_PTR(-ENOMEM);
    } else if (!(inode->i_state & I_NEW)) {
        return i; /*inode found in cache */
    }

    /*inode not in cache, new inode returned, freshly allocated*/
    cfs_i = get_cfs_inode(i);

} 
#endif

/*
 * Lookup the data. This is trivial - if the dentry didn't already
 * exist, we know it is negative.  Set d_op to delete negative dentries.
 */
struct dentry *cfsi_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    #if 0
	static const struct dentry_operations simple_dentry_operations = {
		.d_delete = simple_delete_dentry,
	};

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);
	d_set_d_op(dentry, &simple_dentry_operations);
	d_add(dentry, NULL);
	return NULL;
    #endif
    CLYDE_ASSERT(dir->i_sb);

    struct super_block *sb = NULL;
    struct cfs_sb *cfs_sb = NULL;
    struct inode *i = NULL;
    struct cfs_inode *cfs_i = NULL;

    sb = dir->i_sb;

}

const struct inode_operations clydefs_dir_inode_ops = {
    .create = clydefs_inode_create,
    .lookup = cfsi_lookup,
};

