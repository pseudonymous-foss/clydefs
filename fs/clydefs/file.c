#include "file.h"

const struct file_operations clydefs_file_ops;
const struct file_operations clydefs_dir_file_ops;

int clydefs_file_open(struct inode *inode, struct file *file)
{
    return -1;
}

ssize_t clydefs_file_read(struct file *file, char __user *buf, 
                     size_t count, loff_t *offset)
{
    return 0;
}

ssize_t clydefs_file_write(struct file *file, const char __user *buf, 
                      size_t count, loff_t *offset)
{
    return 0;
}

const struct file_operations clydefs_file_ops = {
    .open = clydefs_file_open,
    .read = clydefs_file_read,
    .write = clydefs_file_write,
};

const struct file_operations clydefs_dir_file_ops = {
    .open		= dcache_dir_open,
	.release	= dcache_dir_close,
	.llseek		= dcache_dir_lseek,
	.read		= generic_read_dir,
	.readdir	= dcache_readdir,
	.fsync		= noop_fsync,
};
