#ifndef __CLYDEFS_IO_H
#define __CLYDEFS_IO_H
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/tree.h>

/*presently block size set corresponds to 512bytes*/
#define BLOCK_SIZE_SHIFT 9
#define BLOCK_SIZE_BYTES 1ul << BLOCK_SIZE_SHIFT

enum BIO_TYPE{
    ATA_BIO,
    TREE_BIO,
};

/** 
 *  represents one fragment of a potentially larger request.
 */
struct cfsio_rq_frag {
    struct tree_iface_data td;
    struct bio *b; /*the bio reference from bio_end_io_t*/
    int bio_err; /*the value of 'error' from bio_end_io_t*/
    struct list_head lst;
};

struct cfsio_rq_cb_data {
    /** number of bio's in this request */
    atomic_t bio_num;

    /** 
     * the beginning of a list of length 'bio_num', one entry per
     * fragment of the request, where each fragments corresponds to
     * a bio.
     */ 
    struct list_head lst;
    spinlock_t lst_lock;

    /** 
     * an error field which is non-zero provided any of the bios 
     * returned an error. Check the error field of each fragment to 
     * determine which failed. 
     */ 
    int error;

    /**supplied data pointer, if any*/ 
    void *data;
    /**length of supplied data buffer */ 
    u64 data_len;
};

/** function to call once an io request has completed */
typedef void (*cfsio_on_endio_t)(struct cfsio_rq_cb_data *req_data, int error);

int cfsio_init(void);

void cfsio_exit(void);

int cfsio_create_tree_sync(struct block_device *bd, u64 *ret_tid);

int cfsio_remove_tree_sync(struct block_device *bd, u64 tid);

int cfsio_insert_node_sync(struct block_device *bd, u64 *ret_nid, u64 tid);

int cfsio_remove_node_sync(struct block_device *bd, u64 tid, u64 nid);

int cfsio_update_node(struct block_device *bd, cfsio_on_endio_t on_complete, u64 tid, u64 nid, u64 offset, u64 len, void *data);

int cfsio_read_node(struct block_device *bd, cfsio_on_endio_t on_complete, u64 tid, u64 nid, u64 offset, u64 len, void *data);

#endif //__CLYDEFS_IO_H
