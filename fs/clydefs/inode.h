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
static __always_inline struct cfs_inode *CFS_INODE(struct inode *i)
{
    return container_of(i, struct cfs_inode, vfs_inode);
}

struct inode *cfsi_getroot(struct super_block *sb);

int cfsi_init(void);
void cfsi_exit(void);

#endif // __CLYDEFS_INODE_H
