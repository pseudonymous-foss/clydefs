#ifndef __CLYDEFS_IO_H
#define __CLYDEFS_IO_H
#include <linux/types.h>

/*TODO: set a better value, check that backend will never return 0 as a valid tree/node id*/
#define BIO_ALLOC_FAIL 0

int clydefs_io_init(void);

void clydefs_io_exit(void);

u64 clydefs_io_create_tree(void);

void clydefs_io_remove_tree(u64 tid);

u64 clydefs_io_insert(u64 tid, u64 len, void *data);

void clydefs_io_remove(u64 tid, u64 nid);

void clydefs_io_update(u64 tid, u64 nid, u64 offset, u64 len, void *data);

void clydefs_io_read(u64 tid, u64 nid, u64 offset, u64 len void *data);

#endif //__CLYDEFS_IO_H
