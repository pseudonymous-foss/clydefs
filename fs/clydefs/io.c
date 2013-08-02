#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include "io.h"

struct bio_page_data {
    struct page *page_addr;
    ulong bcnt, vec_off;
};

struct submit_syncbio_data {
	struct completion event;
	int error;
};

enum AOE_CMD {
    /*Support our vendor-specific codes*/
    AOECMD_CREATETREE = 0xF0,   /*create a new tree*/
    AOECMD_REMOVETREE,          /*remove a tree and all its child nodes*/
    AOECMD_READNODE,            /*read data from an node*/
    AOECMD_INSERTNODE,          /*create a new node with some initial data*/
    AOECMD_UPDATENODE,          /*update the data of an existing node*/
    AOECMD_REMOVENODE,          /*remove the node and associated data*/
};

static struct kmem_cache *tree_iface_pool = NULL;
static struct bio_set *bio_pool = NULL;
static u8 _nilbuf_data = 255;
struct bio_page_data nilbuf;

//static void __prep_bio_page_data(bio_page_data *bpd, void * data)

/** 
 * Allocate bios for both ATA and TREE commands. 
 * @param bt the type of BIO desired, ATA_BIO or TREE_BIO
 * @return NULL on allocation error, otherwise a bio. If a TREE 
 *         bio, the bi_treecmd field points to a struct
 *         tree_iface_data.
 * @note it is your responsibility to call bio_put() once the 
 *       bio is submitted and you're otherwise done with it.
 * @note to issue a valid tree cmd, b->bi_treecmd needs 
 *       additional data.
 * @note you're still required to specify a function to call 
 *       once the IO completes.
 */ 
static struct bio *__alloc_bio(enum BIO_TYPE bt)
{
    struct bio *b = NULL;
    b = bio_alloc_bioset(GFP_ATOMIC, 1, bio_pool);
    if (!b)
        goto err_alloc_bio;

    bio_get(b); /*ensure bio won't disappear on us*/

    if (bt == TREE_BIO) {
        struct tree_iface_data *td = NULL;
        td = kmem_cache_alloc(tree_iface_pool, GFP_ATOMIC);
        if (!td)
            goto err_alloc_tree_iface;
        memset(td, 0, sizeof *td);

        b->bi_treecmd = td;

        b->bi_sector = 0; /*using td->off to mark offsets for read/write*/
        
    } else {
        b->bi_treecmd = NULL;
    }

    return b;

err_alloc_tree_iface:
    bio_put(b);
err_alloc_bio:
    return NULL;
}

/**
 * Helper method for deallocating bio's.
 * @param b the bio to release
 * @note only call this if you're sure releasing one more
 *       reference will actually dealloc the bio.
 */ 
static void __dealloc_bio(struct bio *b)
{
    BUG_ON( atomic_read(&b->bi_cnt) != 1 );

    if (b->bi_treecmd) { /*assume tree bio*/
        kmem_cache_free(tree_iface_pool, b->bi_treecmd);
        b->bi_treecmd = NULL;
    }

    bio_put(b);
}

/**
 * Called when a sync bio is finished.
 * @description a function which populates some fields in
 *              preparation for the end of a synchronous bio.
 * @param b the finished bio which was intended to be
 *          synchronous
 * @param error error code of bio, 0 if no error occurred.
 */
static void __submit_bio_syncio(struct bio *b, int error)
{
	struct submit_syncbio_data *ret = b->bi_private;

	ret->error = error;
	complete(&ret->event);
    #if 0
    /*FIXME this is kind of stupid, just let the user manually clean up 
      by first issuing get_bio(b), then calling cleanup_if_treebio himself*/
    cleanup_if_treebio(b); 
    #endif
}

/**
 * Submit a bio and wait for its completion. 
 * @description wraps submit_bio into a synchronous call, using only the bi_private field. 
 * @param bio the bio to wait for 
 * @param rw read/write flag, accepts REQ_* values & WRITE
 * @return the error code of the completed bio                 
 */ 
static int __submit_bio_sync(struct bio *bio, int rw)
{
    struct submit_syncbio_data ret;

	rw |= REQ_SYNC;
	/*initialise queue*/
    ret.event.done = 0;
    init_waitqueue_head(&ret.event.wait);

	bio->bi_private = &ret;
	bio->bi_end_io = __submit_bio_syncio;
	submit_bio(rw, bio);
	wait_for_completion(&ret.event);

	return ret.error;
}

/** 
 * create a new tree on backing device.
 * @description creates a new tree on the backing device and
 *              returns the resulting id.
 */
int clydefs_io_create_tree(u64 *ret_tid) {
    struct bio *b = __alloc_bio(TREE_BIO);
    struct tree_iface_data *td;
    int error;

    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        error = -ENOMEM;
        goto err_alloc_bio;
    }
    td = (struct tree_iface_data*)b->bi_treecmd;

    td->cmd = AOECMD_CREATETREE;

    if (bio_add_page(b,nilbuf.page_addr,nilbuf.bcnt,nilbuf.vec_off) < nilbuf.bcnt) {
        error = -ENOMEM;
        goto err_page_add;
    }

    if ((error=__submit_bio_sync(b, READ)) != 0) {
        /*FIXME bio_end_io fnc retval here - what can it be ?*/
        goto err_bio_return;
    }

    return 0; /*success*/
err_bio_return:
err_page_add:
    __dealloc_bio(b);
err_alloc_bio:
    return error;
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
    #if 0
    struct bio *insert_bio = NULL;
    int nr_iovecs = 10; /*TODO: figure out how big these are, figure out what to throw in here*/
    /*TODO figure this GFP stuff out, linux/gfp.h*/
    insert_bio = bio_alloc_bioset(GFP_NOWAIT, nr_iovecs, bio_pool);
    if (insert_bio == NULL) {
        return BIO_ALLOC_FAIL;
    }
    submit_bio(WRITE, insert_bio);
    #endif
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

/** 
 * initialize clydefs io subsystem. 
 * @description allocates a bioset to guarantee quick 
 *              allocations of bios.
 * @return -ENOMEM on error, 0 on success
 */
int clydefs_io_init(void) {
    bio_pool = bioset_create(100, 0); /*pool_size, front_padding (if using larger structure than a bio)*/
    if (bio_pool == NULL) {
        pr_err("Failed to allocate a bioset\n");
        goto err_alloc_biopool;
    }

    tree_iface_pool = kmem_cache_create("tree_iface_data", sizeof (struct tree_iface_data), 0, 0, NULL);
    if (tree_iface_pool == NULL) {
        pr_err("Failed to allocate tree_iface_data pool\n");
        goto err_alloc_treepool;
    }

    /*several bio's do not send data apart from the tree 
      interface values, they will use this bogus buffer.
    */
    nilbuf.page_addr = virt_to_page(&_nilbuf_data);
    nilbuf.bcnt = sizeof(_nilbuf_data);
    nilbuf.vec_off = offset_in_page(&_nilbuf_data);

    return 0;
err_alloc_treepool:
    bioset_free(bio_pool);
err_alloc_biopool:
    return -ENOMEM;
}

/** 
 * release allocated memory. 
 * @description releases bioset and other resources acquired in 
 *              preparation for shutdown.
 */
void clydefs_io_exit(void) {
    if (tree_iface_pool)
        kmem_cache_destroy(tree_iface_pool);
    if (bio_pool)
        bioset_free(bio_pool);
}

