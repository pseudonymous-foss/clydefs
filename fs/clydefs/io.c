#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include "io.h"



/* 
    Status
 
    * __alloc_bio currently preallocates just one io vector, we'll be needing one per page.
        NEED to make this an argument to supply allocation.
 
    * update_node currenly does all sorts of funky calculations which may presuppose the
        block size being less than the page size itself, go through it and work out if
        block_size==page_size would work - change otherwise
 
    * need to follow xfs / ext4 example and have clydefs-specific end_io callbacks. Until then
        direct finished bios to an internal bio_end_io_t callback and call OUR callback when
        all bio's making up a command/operation has been finished.
*/

struct bio_page_data {
    struct page *page_addr;
    ulong bcnt, vec_off;
};

struct submit_syncbio_data {
	struct completion event;
	int error;
};

struct cfs_io_end {
    /**number of bios associated with this operation*/
    atomic_t nbio;
    /**completed bios so far*/ 
    atomic_t cbio;

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

/*maximum number of pages*/
#define BIO_MAX_PAGES_PER_CHUNK (BIO_MAX_SECTORS >> (PAGE_SHIFT - BLOCK_SIZE_SHIFT))

/* 
  several bio's do not send data apart from the tree 
  interface values, they will use this bogus buffer.
*/
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
    
    b = bio_alloc_bioset(GFP_ATOMIC, 1, bio_pool); /*FIXME nr_iovec here is going to be dangerous!*/
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
int cfsio_create_tree_sync(u64 *ret_tid) {
    struct bio *b = __alloc_bio(TREE_BIO);
    struct tree_iface_data *td;
    int retval;

    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        retval = -ENOMEM;
        goto err_alloc_bio;
    }
    td = (struct tree_iface_data*)b->bi_treecmd;

    td->cmd = AOECMD_CREATETREE;

    if (bio_add_page(b,nilbuf.page_addr,nilbuf.bcnt,nilbuf.vec_off) < nilbuf.bcnt) {
        retval = -ENOMEM;
        goto out;
    }

    if ((retval=__submit_bio_sync(b, READ)) != 0) {
        /*FIXME bio_end_io fnc retval here - what can it be ?*/
        goto out;
    }

    /*success, fall through to dealloc bio*/
    *ret_tid = td->tid;

out:
    __dealloc_bio(b);
err_alloc_bio:
    return retval;
}

/** 
 * remove tree on backing device. 
 * @description removes a tree on the backing device, freeing 
 *              both the tree itself and any node(s) and their
 *              data in the process.
 */ 
int cfsio_remove_tree_sync(u64 tid) {
    struct bio *b = __alloc_bio(TREE_BIO);
    struct tree_iface_data *td;
    int retval;

    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        retval = -ENOMEM;
        goto err_alloc_bio;
    }
    td = (struct tree_iface_data*)b->bi_treecmd;

    td->cmd = AOECMD_REMOVETREE;
    td->tid = tid;

    if (bio_add_page(b,nilbuf.page_addr,nilbuf.bcnt,nilbuf.vec_off) < nilbuf.bcnt) {
        retval = -ENOMEM;
        goto out;
    }

    if ((retval=__submit_bio_sync(b, READ)) != 0) {
        /*FIXME bio_end_io fnc retval here - what can it be ?*/
        goto out;
    }

    /*success, fall through to dealloc bio*/

out:
    __dealloc_bio(b);
err_alloc_bio:
    return retval;
}

/** 
 * insert a new node with data into the tree.
 * @description creates a new node initially populated with
 *              'data' in the tree identified by tid.
 * @param ret_nid where to store the assigned node id 
 * @param tid the tree id
 * @return 0 on success, negative on errors
 * @post on success, *ret_nid will hold the assigned node 
 *       identifier.
 */
int cfsio_insert_node_sync(u64 *ret_nid, u64 tid) {
    struct bio *b = __alloc_bio(TREE_BIO);
    struct tree_iface_data *td;
    int retval;

    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        retval = -ENOMEM;
        goto err_alloc_bio;
    }
    td = (struct tree_iface_data*)b->bi_treecmd;

    td->cmd = AOECMD_INSERTNODE;
    td->tid = tid;

    if (bio_add_page(b,nilbuf.page_addr,nilbuf.bcnt,nilbuf.vec_off) < nilbuf.bcnt) {
        retval = -ENOMEM;
        goto out;
    }

    if ((retval=__submit_bio_sync(b, READ)) != 0) {
        /*FIXME bio_end_io fnc retval here - what can it be ?*/
        goto out;
    }

    /*success, fall through to dealloc bio*/
    *ret_nid = td->nid;

out:
    __dealloc_bio(b);
err_alloc_bio:
    return retval;
}

/** 
 * remove node from tree
 * 
 * @param tid the tree id
 * @param nid the node id
 * @description removes the specified node from the specified
 *              tree.
 */
int cfsio_remove_node_sync(u64 tid, u64 nid) {
    struct bio *b = __alloc_bio(TREE_BIO);
    struct tree_iface_data *td;
    int retval;

    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        retval = -ENOMEM;
        goto err_alloc_bio;
    }
    td = (struct tree_iface_data*)b->bi_treecmd;

    td->cmd = AOECMD_REMOVENODE;
    td->tid = tid;
    td->nid = nid;

    if (bio_add_page(b,nilbuf.page_addr,nilbuf.bcnt,nilbuf.vec_off) < nilbuf.bcnt) {
        retval = -ENOMEM;
        goto out;
    }

    if ((retval=__submit_bio_sync(b, READ)) != 0) {
        /*FIXME bio_end_io fnc retval here - what can it be ?*/
        goto out;
    }

    /*success, fall through to dealloc bio*/
out:
    __dealloc_bio(b);
err_alloc_bio:
    return retval;
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
int cfsio_update_node(bio_end_io_t on_complete, u64 tid, u64 nid, u64 offset, u64 len, void *data) {

    /* 
      Issues:
      * should bio allocation fail halfway in I'm in trouble.
      * investigate the BLOCK_SIZE / PAGE_SIZE issue (read comments @ top)
    */
    int retval;
    struct bio *b;
    u64 chunk_pages;
    u8 *data_cur = data; /*ptr to what next page should contain*/
    /*total number of pages to transfer*/
    u64 pages_left = len >> PAGE_SHIFT; /*how many times len divides by PAGE_SIZE*/
    int trailing_bytes = len & ((1 << PAGE_SHIFT) - 1); /*would there have been a remainder of that division?*/
    if  (trailing_bytes) { 
        /*need a trailing page which will contain less than a full page of data*/
        pages_left++;
    }

next_chunk:
    /*how many pages to transfer with this bio*/
    chunk_pages = pages_left;
    if (chunk_pages > BIO_MAX_PAGES_PER_CHUNK) { /*FIXME; check this*/
        chunk_pages = BIO_MAX_PAGES_PER_CHUNK;
    }
    pages_left -= chunk_pages;

    b = __alloc_bio(TREE_BIO);
    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        retval = -ENOMEM;
        goto err_alloc_bio; /*FIXME that will not work here, we don't know how many bio's we'll issue*/
    }    

    while(chunk_pages)
    { /*add all pages for this chunk, page-by-page*/
        int written, bio_page_size;
        
        /*if not the last page of the chunk, or not the last chunk at all, we are definitely writing a full page*/
        if (likely(chunk_pages > 1 || (pages_left > chunk_pages)))
            bio_page_size = PAGE_SIZE;
        else {
            /*last page of last chunk being written*/
            bio_page_size = trailing_bytes ? trailing_bytes : PAGE_SIZE;
        }
        
        written = bio_add_page(
                b,virt_to_page(data_cur),
                offset_in_page(data_cur),
                bio_page_size
        );
        if ( unlikely(written == 0) ) { /*add_page either succeeds or returns 0*/
            /*can be due to device limitations, fire off bio now*/
            break;
        }

        data_cur += PAGE_SIZE;
        chunk_pages--;
    }

    submit_bio(WRITE, b);

    if(unlikely(chunk_pages)) {
        pages_left += chunk_pages; /*didn't finish our chunk, return leftovers*/
    }
    if(pages_left)
        goto next_chunk;

err_alloc_bio:
out:
    return retval;
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
int cfsio_read_node(u64 tid, u64 nid, u64 offset, u64 len, void *data) {
    return;
}

/** 
 * initialize clydefs io subsystem. 
 * @description allocates a bioset to guarantee quick 
 *              allocations of bios.
 * @return -ENOMEM on error, 0 on success
 */
int cfsio_init(void) {
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
    
    /*bogus values for bio's which send no additional data than 
      the tree command values themselves.*/
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
void cfsio_exit(void) {
    if (tree_iface_pool)
        kmem_cache_destroy(tree_iface_pool);
    if (bio_pool)
        bioset_free(bio_pool);
}

