#ifndef __CLYDEFS_CHUNK_H
#define __CLYDEFS_CHUNK_H
#include "clydefs.h"
#include "clydefs_disk.h"
#include "io.h" /*for cfsio_update_node_sync used in cfsc_write_chunk_sync macro*/

/**Actual size of a chunk */ 
#define CHUNK_SIZE_BYTES sizeof(struct cfsd_inode_chunk)

/**Total size of a single chunk, slack included,. This is the 
 * distance, in bytes, from the head of one chunk to the 
 * next. */ 
#define CHUNK_SIZE_DISK_BYTES (CHUNK_SIZE_BYTES + CHUNK_TAIL_SLACK_BYTES)

enum CHUNK_LOOKUP_RES { FOUND = 0, NOT_FOUND = 1 };


void *cfsc_chunk_alloc(void);
void cfsc_chunk_free(struct cfsd_inode_chunk *c);

int cfsc_ientry_insert(struct cfs_inode *parent, struct cfs_inode *inode, struct dentry *inode_d);
void cfsc_chunk_init_common(struct cfsd_inode_chunk *c);
int __must_check cfsc_ientry_find(
    struct cfsd_inode_chunk *ret_buf, struct ientry_loc *ret_loc, 
    struct cfs_inode *parent, struct dentry *search_dentry);
int cfsc_chunk_insert_entry(u64 *ret_ndx, struct cfsd_inode_chunk *c, struct cfsd_ientry const *e);

int cfsc_init(void);
void cfsc_exit(void);


static __always_inline int cfsc_write_chunk_sync(struct block_device *bd, u64 tid, u64 nid, struct cfsd_inode_chunk *c, int chunk_off) 
{
    return cfsio_update_node_sync(
        bd, NULL, NULL, tid, nid, 
        chunk_off * CHUNK_SIZE_DISK_BYTES, CHUNK_SIZE_BYTES, c
    );
}
#endif //__CLYDEFS_CHUNK_H
