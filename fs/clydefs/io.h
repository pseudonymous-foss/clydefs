#ifndef __CLYDEFS_IO_H
#define __CLYDEFS_IO_H
#include <linux/types.h>

enum BIO_TYPE{
    ATA_BIO,
    TREE_BIO,
};

/** 
 * extended bio data for the tree-based
 * interface.
 */ 
struct tree_iface_data {
    u8 cmd;         /*one of the vendor-specific AOECMD_* codes*/
    u64 tid;
    u64 nid;
    u64 off;
    u64 len;
    u64 err;
};

int clydefs_io_init(void);

void clydefs_io_exit(void);

int clydefs_io_create_tree(u64 *ret_tid);

void clydefs_io_remove_tree(u64 tid);

u64 clydefs_io_insert(u64 tid, u64 len, void *data);

void clydefs_io_remove(u64 tid, u64 nid);

void clydefs_io_update(u64 tid, u64 nid, u64 offset, u64 len, void *data);

void clydefs_io_read(u64 tid, u64 nid, u64 offset, u64 len, void *data);

#endif //__CLYDEFS_IO_H
