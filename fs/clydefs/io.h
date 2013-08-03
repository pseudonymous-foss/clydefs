#ifndef __CLYDEFS_IO_H
#define __CLYDEFS_IO_H
#include <linux/types.h>

/*presently block size set corresponds to 512bytes*/
#define BLOCK_SIZE_SHIFT 9
#define BLOCK_SIZE_BYTES 1ul << BLOCK_SIZE_SHIFT

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

int cfsio_init(void);

void cfsio_exit(void);

int cfsio_create_tree_sync(u64 *ret_tid);

int cfsio_remove_tree_sync(u64 tid);

int cfsio_insert_node_sync(u64 *ret_nid, u64 tid);

int cfsio_remove_node(u64 tid, u64 nid);

int cfsio_update_node(bio_end_io_t on_complete, u64 tid, u64 nid, u64 offset, u64 len, void *data);

int cfsio_read_node(u64 tid, u64 nid, u64 offset, u64 len, void *data);

#endif //__CLYDEFS_IO_H
