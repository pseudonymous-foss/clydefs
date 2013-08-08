#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include "io.h"

#define CLYDEFS_MAGIC_IDENT 0x20140106

extern const struct inode_operations clydefs_dir_inode_ops;
extern struct file_operations clydefs_dir_file_ops;

struct dentry *clydefs_mount(struct file_system_type *fs_type, int flags,
                             const char *device_name, void *data);
static int clydefs_fill_super(struct super_block *sb, void *data, int silent);

/*Structures*/
static struct file_system_type clydefs_fs_type;
static const struct super_operations clydefs_super_operations;

struct dentry *clydefs_mount(struct file_system_type *fs_type, int flags,
                             const char *device_name, void *data)
{

    return mount_single(fs_type, flags, data, clydefs_fill_super);
}

/* 
 * Initialises superblock structure. 
 * Based on 'simple_fill_super' 
 */ 
static int clydefs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct inode *inode;
    struct dentry *root;

    sb->s_blocksize = PAGE_CACHE_SIZE;
    sb->s_blocksize_bits = PAGE_CACHE_SHIFT;
    sb->s_magic = CLYDEFS_MAGIC_IDENT;
    sb->s_op = &clydefs_super_operations;
    sb->s_time_gran = 1;

    inode = new_inode(sb);
    if (!inode)
        return -ENOMEM;

    inode->i_ino = 1; /*Ensure inode number '1' is reserved!*/
    inode->i_mode = S_IFDIR | 755;
    inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME; /*TODO */
    inode->i_op = &clydefs_dir_inode_ops;
    inode->i_fop = &clydefs_dir_file_ops;
    set_nlink(inode, 2);
    root = d_make_root(inode);
    if (!root)
        return -ENOMEM;
    
    sb->s_root = root;
    return 0;
}

int super_init(void)
{
    return register_filesystem(&clydefs_fs_type);
}

void super_exit(void)
{
    unregister_filesystem(&clydefs_fs_type);
}

void clydefs_kill_super(struct super_block *sb)
{
    kill_litter_super(sb);
}

static struct file_system_type clydefs_fs_type = {
    .owner = THIS_MODULE,
    .name = "clydefs",
    .mount = clydefs_mount,
    .kill_sb = clydefs_kill_super,
    /*.fs_flags = FS_REQUIRES_DEV*/
};

static const struct super_operations clydefs_super_operations = {
    .statfs = simple_statfs,
};

