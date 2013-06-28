#include "inode.h"

int clydefs_inode_create(struct inode *dir, struct dentry *d, umode_t mode, bool excl)
{
    printk(KERN_INFO "clydefs_inode_create called\n");
    return -1;
}

const struct inode_operations clydefs_dir_inode_ops = {
    .create = clydefs_inode_create,
    .lookup = simple_lookup,
};

