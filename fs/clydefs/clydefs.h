#ifndef __CLYDEFS_GLOBAL_H
#define __CLYDEFS_GLOBAL_H

#include <linux/fs.h>

#define CLYDEFS_MAGIC_IDENT 0x20140106u

/*wrap errors*/
#define CLYDE_ERR(fmt, a...) printk(KERN_ERR "clydefs: " fmt, ##a)

#define CLYDE_STUB printk(KERN_ERR "clydefs: %s<%d> is a STUB\n", __FUNCTION__, __LINE__)

struct cfs_node_addr {
    u64 tid;
    u64 nid;
};

/** 
 * clydefs-specific addition information. 
 * Can be pointed to by s_fs_info of the superblock. 
 */ 
struct cfs_sb_info {
    /**Location of the superblock table */ 
    struct cfs_node_addr superblock_tbl;
};

/** 
 * clydefs inode structure 
 */
struct cfs_inode {
    struct inode vfs_inode;
};

/*require asserts only if in debug mode*/
#ifdef CONFIG_CLYDEFS_DEBUG
#define CLYDE_ASSERT(x)                                                 \
do {    if (x) break;                                                   \
        printk(KERN_EMERG "### ASSERTION FAILED %s: %s: %d: %s\n",      \
               __FILE__, __func__, __LINE__, #x); dump_stack(); BUG();  \
} while (0);
#else
#define CLYDE_ASSERT(x)
#endif

#endif //__CLYDEFS_GLOBAL_H
