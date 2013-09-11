#ifndef __CLYDEFS_CHUNK_H
#define __CLYDEFS_CHUNK_H
#include "clydefs.h"
#include "clydefs_disk.h"

/**Actual size of a chunk */ 
#define CHUNK_SIZE_BYTES sizeof(struct cfsd_inode_chunk)

/**Total size of a single chunk, slack included,. This is the 
 * distance, in bytes, from the head of one chunk to the 
 * next. */ 
#define CHUNK_SIZE_DISK_BYTES (CHUNK_SIZE_BYTES + CHUNK_TAIL_SLACK_BYTES)

/**Value indicating an unused offset, must be higher than the 
 * number of entries possible to have in a chunk */ 
#define OFFSET_UNUSED 0b11111111U

/**Number of items in chunk */ 
#define CHUNK_NUM_ITEMS(c) (CHUNK_NUMENTRIES - (c)->hdr.entries_free)

enum CHUNK_LOOKUP_RES { FOUND = 0, NOT_FOUND = 1 };


void *cfsc_chunk_alloc(void);
void cfsc_chunk_init(struct cfsd_inode_chunk *c);
void cfsc_chunk_free(struct cfsd_inode_chunk *c);


int cfsc_chunk_entry_insert(u64 *ret_ndx, struct cfsd_inode_chunk *c, struct cfsd_ientry const *e);
void cfsc_chunk_entry_delete(struct cfsd_inode_chunk *c, u8 entry_ndx);

int cfsc_mk_itbl_node(u64 *ret_itbl_nid, struct block_device *bd, u64 tid);

int cfsc_read_chunk_sync(struct block_device *bd, u64 tid, u64 nid, struct cfsd_inode_chunk *c, int chunk_off);
int cfsc_write_chunk_sync(struct block_device *bd, u64 tid, u64 nid, struct cfsd_inode_chunk *c, int chunk_off);

int cfsc_ientry_insert(struct cfs_inode *parent, struct cfs_inode *inode, struct dentry *inode_d);
int cfsc_ientry_update(struct cfs_inode *parent, struct cfs_inode *ci);
int cfsc_ientry_delete(struct cfs_inode *parent, struct cfs_inode *ci);
int __must_check cfsc_ientry_find(
    struct cfsd_inode_chunk *ret_buf, struct ientry_loc *ret_loc, 
    struct cfs_inode *parent, struct dentry *search_dentry);



int cfsc_init(void);
void cfsc_exit(void);
#endif //__CLYDEFS_CHUNK_H
