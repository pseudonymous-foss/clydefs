#include <linux/fs.h>
#include <linux/bio.h>
#include "io.h"

static struct bio_set *bio_pool = NULL;
/*IDEA: mem-manage headers here in their entirety ? */
/** 
 * initialize clydefs io subsystem. 
 * @description allocates a bioset to guarantee quick 
 *              allocations of bios.
 * @return -ENOMEM on error, 0 on success
 */
int clydefs_io_init(void) {
    bio_pool = bioset_create(100, 0); /*pool_size, front_padding (if using larger structure than a bio)*/
    if (bio_pool == NULL) {
        return -ENOMEM;
    }

    return 0;
}

/** 
 * release allocated memory. 
 * @description releases bioset and other resources acquired in 
 *              preparation for shutdown.
 */
void clydefs_io_exit(void) {
    bioset_free(bio_pool);
}


/** 
 * create a new tree on backing device.
 * @description creates a new tree on the backing device and
 *              returns the resulting id.
 */
u64 clydefs_io_create_tree(void) {
    return 0;
}

/** 
 * remove tree on backing device. 
 * @description removes a tree on the backing device, freeing 
 *              both the tree itself and any node(s) and their
 *              data in the process.
 */ 
void clydefs_io_remove_tree(u64 tid) {
    return;
}

/** 
 * insert a new node with data into the tree.
 * @description creates a new node initially populated with
 *              'data' in the tree identified by tid.
 * @param tid the tree id
 * @param len the length, in bytes, of the data array.
 * @param data the address of the data array. 
 * @return the id of the new node or BIO_ALLOC_FAIL on failure
 */
u64 clydefs_io_insert(u64 tid, u64 len, void *data) {
    struct bio *insert_bio = NULL;
    int nr_iovecs = 10; /*TODO: figure out how big these are, figure out what to throw in here*/
    /*TODO figure this GFP stuff out, linux/gfp.h*/
    insert_bio = bio_alloc_bioset(GFP_NOWAIT, nr_iovecs, bio_pool);
    if (insert_bio == NULL) {
        return BIO_ALLOC_FAIL;
    }
    submit_bio(WRITE, insert_bio);
    return 0;
}

/** 
 * remove node from tree
 * 
 * @param tid the tree id
 * @param nid the node id
 * @description removes the specified node from the specified
 *              tree.
 */
void clydefs_io_remove(u64 tid, u64 nid) {
    return;
}

/** 
 *  update data in node
 * 
 *  @param tid the id of the tree containing the node
 *  @param nid the id of the node to update
 *  @param offset the offset, in bytes, to write the data
 *  @param len the length, in bytes, of the data to write
 *  @param data the data to write
 *  @description updates the node identified by nid in the tree
 *               identified by tid by writing the supplied data
 *               at the supplied offset in the node.
 */
void clydefs_io_update(u64 tid, u64 nid, u64 offset, u64 len, void *data) {
    return;
}

/**
 * read node data
 * 
 * @param tid the id of the tree containing the node
 * @param nid the id of the node to read from
 * @param offset the offset within the node from which to begin reading
 * @param len the number of bytes to read 
 * @description reads the specified sequence of bytes from the 
 *              node.
 * @todo what about reading past the end, what attempting reads 
 *       to non-existing sequences entirely.
 */
void clydefs_io_read(u64 tid, u64 nid, u64 offset, u64 len, void *data) {
    return;
}
