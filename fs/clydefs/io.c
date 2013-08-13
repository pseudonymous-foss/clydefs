#include <linux/fs.h>
#include <linux/bio.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/blkdev.h>
#include <linux/tree.h>
#include "global.h"
#include "io.h"

/* 
    Status
 
    * __alloc_bio currently preallocates just one io vector, we'll be needing one per page.
        NEED to make this an argument to supply allocation.
 
    * update_node currenly does all sorts of funky calculations which may presuppose the
        block size being less than the page size itself, go through it and work out if
        block_size==page_size would work - change otherwise
 
    * FIXME -- still need to consider how to include the block-dev/context into the request struct.
        Give people a chance to reissue failed fragments
 
    * FIXME -- add TERR_IO_ERR to account for partially completed requests or single-bio requests
        where the bio fails.
 
    * FIXME -- fix sync commands so that the correct error is returned, not just the
        bio end_io retval
 
    * FIXME -- bad API, on_complete provides the error value via the req cb struct
        AND as a separate parameter, 'error'.
*/

struct bio_page_data {
    struct page *page_addr;
    ulong bcnt, vec_off;
};

struct submit_syncbio_data {
	struct completion event;
	int error;
};



/** 
 * Contains data related to the request. 
 */ 
struct cfsio_rq {
    /** number of bios registered as completed */
    atomic_t bio_completed;
    /** The fields of the request directly available to the
     *  end_io functions */
    struct cfsio_rq_cb_data cb_data;
    /** is set only when all requests have been counted. To
     *  prevent premature completion before the last requests
     *  have been issued. */
    atomic_t initialised;
    /** function to call once the request is fully completed */
    cfsio_on_endio_t endio_cb;
};

static struct kmem_cache *cfsio_rqfrag_pool = NULL;
static struct kmem_cache *cfsio_rq_pool = NULL;
static struct bio_set *bio_pool = NULL;

/*maximum number of pages*/
#define BIO_MAX_PAGES_PER_CHUNK (BIO_MAX_SECTORS >> (PAGE_SHIFT - BLOCK_SIZE_SHIFT))

#define tbio_get_frag(b) container_of((b)->bi_treecmd, struct cfsio_rq_frag, td);

/* 
  several bio's do not send data apart from the tree 
  interface values, they will use this bogus buffer.
*/
static u8 _nilbuf_data = 255;
struct bio_page_data nilbuf;

static void __free_tbio_fragments(struct cfsio_rq *rq);

/** 
 * Bring request structure into a valid state. 
 * @description this function sets relevant values in cfsio_rq 
 *              itself and its callback structure to ensure a
 *              valid state.
 */ 
static void __cfsio_rq_init(struct cfsio_rq *rq)
{
    INIT_LIST_HEAD(&rq->cb_data.lst);
    spin_lock_init(&rq->cb_data.lst_lock);
}

/** 
 * called whenever a single bio part of a cfsio request
 * completed.
 */
static void fragment_end_io(struct bio *b, int error) {
    struct tree_iface_data *td;
    struct cfsio_rq *req;
    struct cfsio_rq_frag *frag;

    BUG_ON(b->bi_treecmd == NULL);
    BUG_ON(b->bi_private == NULL);

    td = (struct tree_iface_data*)b->bi_treecmd;
    req = (struct cfsio_rq *)b->bi_private;

    printk(
        "%s called\n\t\t(bio_num:%d) (bio_completed:%d) initialised(%d)\n",
        __FUNCTION__, atomic_read(&req->cb_data.bio_num), 
        atomic_read(&req->bio_completed), atomic_read(&req->initialised)
    );
    
    /*Set the fields required for a valid object state*/
    req->cb_data.error |= error;

    /*add fragment to the list of fragments making up the request*/
    frag = tbio_get_frag(b);
    spin_lock(&req->cb_data.lst_lock);
    list_add(&frag->lst, &req->cb_data.lst);
    spin_unlock(&req->cb_data.lst_lock);

    /*finally, mark this fragment as completed*/
    atomic_inc(&req->bio_completed);
           
    if (atomic_read(&req->initialised) == 0) {
        /*this bio completed before all bios of the 
          request were dispatched, nothing further to do*/
        printk("\t\tbio fragment_end_io called before request initialised, don't process further\n");
        return;
    }
    
    if (atomic_read(&req->bio_completed) == atomic_read(&req->cb_data.bio_num)) {
        printk("\t\tALL BIO'S COMPLETED!!!\n\t\t(time to fire usr cb)\n");
        /*all bio's completed*/
        if (atomic_add_return(1, &req->initialised) != 2) {
            printk("\t\t someone beat us to handling the completion code\n");
            return; /*someone else beat us to it*/
        }
        /*holding the last bio of the request*/

        /*invoke user callback*/
        req->endio_cb(&req->cb_data,req->cb_data.error);

        /*request finished, free fragments and request*/
        __free_tbio_fragments(req);
        kmem_cache_free(cfsio_rq_pool, req);
    }
}

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
 * @post returned bio reference with increased count 
 * @post bio->treecmd points to the treecmd structure, which is 
 *       contained in a request fragment structure as member
 *       'td'
 * @post request fragment structure's bio ptr points to the 
 *       allocated bio.
 */ 
static struct bio *__alloc_bio(enum BIO_TYPE bt)
{
    struct bio *b = NULL;
    
    b = bio_alloc_bioset(GFP_ATOMIC, 1, bio_pool); /*FIXME nr_iovec here is going to be dangerous!*/
    if (!b)
        goto err_alloc_bio;

    bio_get(b); /*ensure bio won't disappear on us*/

    if (bt == TREE_BIO) {
        struct cfsio_rq_frag *frag = NULL;
        frag = kmem_cache_zalloc(cfsio_rqfrag_pool, GFP_ATOMIC);
        if (!frag) {
            pr_debug("failed to allocate request fragment for bio\n");
            goto err_alloc_fragment;
        }
        frag->b = b;
        b->bi_treecmd = &frag->td;
        b->bi_sector = 0; /*using td->off to mark offsets for read/write*/
    } else {
        b->bi_treecmd = NULL;
    }
    b->bi_private = NULL;

    return b;

err_alloc_fragment:
    bio_put(b);
err_alloc_bio:
    return NULL;
}

/**
 * Helper method for deallocating bio's.
 * @param b the bio to release
 * @note only call this if no further work will be done on the 
 *       bio, regardless of other outstanding references.
 */ 
static void __dealloc_bio(struct bio *b)
{
    printk("%s called\n", __FUNCTION__);
    CLYDE_ASSERT(b != NULL);
    if (b->bi_treecmd) { /*assume tree bio*/
        struct cfsio_rq_frag *frag = tbio_get_frag(b);
        CLYDE_ASSERT(frag != NULL);
        kmem_cache_free(cfsio_rqfrag_pool, frag);
        b->bi_treecmd = NULL;
        b->bi_private = NULL;
    }
    printk("%s -- bio->bio_cnt (get/put var): %d\n", __FUNCTION__, atomic_read(&b->bi_cnt));
    CLYDE_ASSERT( atomic_read(&b->bi_cnt) == 1);
    bio_put(b);
    
}

/** 
 * free all fragments associated a particular request. 
 * @description used to free all memory used for the fragments 
 *              associated a particular IO request.
 * @param rq the request containing the fragments 
 * @post all memory associated the request fragments has been 
 *       freed.
 */
static void __free_tbio_fragments(struct cfsio_rq *rq)
{
    
    struct cfsio_rq_frag *frag;
    struct list_head *head, *pos, *nxt;
    printk("%s called\n", __FUNCTION__);
    head = &rq->cb_data.lst;
    printk("\t\tb4 list traversal\n");
    list_for_each_safe(pos,nxt, head) {
        printk("\t\t\tlist_entry(...)\n");
        frag = list_entry(pos, struct cfsio_rq_frag, lst);
        printk("\t\t\tlist_del(...)\n");
        list_del(pos);              /*unlink*/
        printk("\t\t\t__dealloc_bio(...)\n");
        __dealloc_bio(frag->b);     /*clean up bio & fragment memory use*/
    }
}

/**
 * Called when a sync bio is finished.
 * @description a function which populates some fields in
 *              preparation for the end of a synchronous bio.
 * @param b the finished bio which was intended to be
 *          synchronous
 * @param error error code of bio, 0 if no error occurred.
 */
static void __submit_bio_sync_end_io(struct bio *b, int error)
{
	struct submit_syncbio_data *ret = b->bi_private;
        printk("%s called...\n", __FUNCTION__);
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
    printk("%s called...\n", __FUNCTION__);
    rw |= REQ_SYNC;
	/*initialise queue*/
    ret.event.done = 0;
    init_waitqueue_head(&ret.event.wait);

    printk("configuring bio...\n");
	bio->bi_private = &ret;
	bio->bi_end_io = __submit_bio_sync_end_io;
    printk("b4 actual submit\n");
	submit_bio(rw, bio);
	wait_for_completion(&ret.event);

	return ret.error;
}

/** 
 * create a new tree on backing device.
 * @description creates a new tree on the backing device and
 *              returns the resulting id.
 */
int cfsio_create_tree_sync(struct block_device *bd, u64 *ret_tid) {
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    int retval;

    printk("%s called...\n", __FUNCTION__);
    b = __alloc_bio(TREE_BIO);

    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        retval = -ENOMEM;
        goto err_alloc_bio;
    }
    printk("bio allocated\n");
    td = (struct tree_iface_data*)b->bi_treecmd;

    td->cmd = AOECMD_CREATETREE;
    printk("b4 bio_add_page (nilbuf)\n");
    
    b->bi_bdev = bd;
    if (bio_add_page(b,nilbuf.page_addr,nilbuf.bcnt,nilbuf.vec_off) < nilbuf.bcnt) {
        retval = -ENOMEM;
        goto out;
    }
    printk("empty page added to bio\n");

    printk("b4 sumitting bio\n");
    if ((retval=__submit_bio_sync(b, READ)) != 0) {
        retval = TERR_IO_ERR;
        goto out;
    }
    /*io successful, return TREE err val*/
    retval = td->err;
    bio_put(b);

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
int cfsio_remove_tree_sync(struct block_device *bd, u64 tid) {
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    int retval;

    b = __alloc_bio(TREE_BIO);
    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        retval = -ENOMEM;
        goto err_alloc_bio;
    }
    td = (struct tree_iface_data*)b->bi_treecmd;

    td->cmd = AOECMD_REMOVETREE;
    td->tid = tid;

    b->bi_bdev = bd;
    if (bio_add_page(b,nilbuf.page_addr,nilbuf.bcnt,nilbuf.vec_off) < nilbuf.bcnt) {
        retval = -ENOMEM;
        goto out;
    }

    if ((retval=__submit_bio_sync(b, READ)) != 0) {
        retval = TERR_IO_ERR;
        goto out;
    }
    /*io successful, return TREE err val*/
    retval = td->err;
    bio_put(b);

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
int cfsio_insert_node_sync(struct block_device *bd, u64 *ret_nid, u64 tid) {
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    int retval;

    b = __alloc_bio(TREE_BIO);
    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        retval = -ENOMEM;
        goto err_alloc_bio;
    }
    td = (struct tree_iface_data*)b->bi_treecmd;

    td->cmd = AOECMD_INSERTNODE;
    td->tid = tid;

    b->bi_bdev = bd;
    if (bio_add_page(b,nilbuf.page_addr,nilbuf.bcnt,nilbuf.vec_off) < nilbuf.bcnt) {
        retval = -ENOMEM;
        goto out;
    }
    
    if ((retval=__submit_bio_sync(b, READ)) != 0) {
        retval = TERR_IO_ERR;
        goto out;
    }
    /*io successful, return TREE err val*/
    retval = td->err;
    bio_put(b);

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
int cfsio_remove_node_sync(struct block_device *bd, u64 tid, u64 nid) {
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    int retval;

    b = __alloc_bio(TREE_BIO);
    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        retval = -ENOMEM;
        goto err_alloc_bio;
    }
    td = (struct tree_iface_data*)b->bi_treecmd;

    td->cmd = AOECMD_REMOVENODE;
    td->tid = tid;
    td->nid = nid;

    b->bi_bdev = bd;
    if (bio_add_page(b,nilbuf.page_addr,nilbuf.bcnt,nilbuf.vec_off) < nilbuf.bcnt) {
        retval = -ENOMEM;
        goto out;
    }

    if ((retval=__submit_bio_sync(b, READ)) != 0) {
        retval = TERR_IO_ERR;
        goto out;
    }
    /*io successful, return TREE err val*/
    retval = td->err;
    bio_put(b);

    /*success, fall through to dealloc bio*/
out:
    __dealloc_bio(b);
err_alloc_bio:
    return retval;
}

static int cfsio_data_request(struct block_device *bd, enum AOE_CMD cmd, int rw, cfsio_on_endio_t on_complete, 
                              u64 tid, u64 nid, u64 offset, u64 len, void *data)
{
    /* 
      Issues:
      * should bio allocation fail halfway in I'm in trouble.
      * investigate the BLOCK_SIZE / PAGE_SIZE issue (read comments @ top)
    */
    int retval;
    struct bio *b;
    struct cfsio_rq *req = NULL;
    u64 chunk_pages;
    struct tree_iface_data *b_td;
    u8 first_bio = 1;
    u8 *data_cur = data; /*ptr to what next page should contain*/
    /*total number of pages to transfer*/
    u64 pages_left = len >> PAGE_SHIFT; /*how many times len divides by PAGE_SIZE*/
    int trailing_bytes = len & ((1 << PAGE_SHIFT) - 1); /*would there have been a remainder of that division?*/
    if  (trailing_bytes) { 
        /*need a trailing page which will contain less than a full page of data*/
        pages_left++;
    }
    printk("data size %llu bytes, => %llu pages of %lu bytes (trailing_bytes: %d)\n",
           len, pages_left, PAGE_SIZE, trailing_bytes);

    if ( cmd == AOECMD_UPDATENODE )
        printk("data to write:\n");
    print_hex_dump(KERN_EMERG, "", DUMP_PREFIX_NONE, 16, 1, data, len, 0);

    req = kmem_cache_zalloc(cfsio_rq_pool, GFP_KERNEL);
    if (!req) {
        pr_err("Failed to allocate request structure\n");
        retval = -ENOMEM;
        goto err_alloc_req;
    }

next_chunk:
    b = NULL; b_td = NULL;
    chunk_pages = pages_left;
    if (chunk_pages > BIO_MAX_PAGES_PER_CHUNK) { /*FIXME; check this*/
        chunk_pages = BIO_MAX_PAGES_PER_CHUNK;
    }
    printk("chunk_pages: %llu\n", chunk_pages);
    pages_left -= chunk_pages;

    b = __alloc_bio(TREE_BIO);
    if (!b) {
        pr_warn("failed to allocate tree bio\n");
        retval = -ENOMEM;
        goto err_alloc_bio; /*FIXME that will not work here, we don't know how many bio's we'll issue*/
    }
    b->bi_bdev = bd;
    b->bi_end_io = fragment_end_io;
    b->bi_private = req;

    b_td = (struct tree_iface_data *)b->bi_treecmd;
    b_td->cmd = cmd;
    b_td->tid = tid;
    b_td->nid = nid;
    b_td->off = offset;
    b_td->len = len;

    if (first_bio) {
        first_bio = 0;

        /*initialise request structure and add this bio as the first element*/
        __cfsio_rq_init(req);
        req->cb_data.data = data;
        req->cb_data.data_len = len;
        req->endio_cb = on_complete;
    }

    while(chunk_pages)
    { /*add all pages for this chunk, page-by-page*/
        int written, bio_page_size;
        
        /*if not the last page of the chunk, or not the last chunk at all, we are definitely writing a full page*/
        if (likely(chunk_pages > 1 || (pages_left)))
            bio_page_size = PAGE_SIZE;
        else {
            /*last page of last chunk being written*/
            bio_page_size = trailing_bytes ? trailing_bytes : PAGE_SIZE;
        }
        
        written = bio_add_page(
                b,virt_to_page(data_cur),
                bio_page_size,
                offset_in_page(data_cur)
        );
        if ( unlikely(written == 0) ) { /*add_page either succeeds or returns 0*/
            /*can be due to device limitations, fire off bio now*/
            printk("%s - bio being broken up as last page add failed\n", __FUNCTION__);
            break;
        }
        printk("\t\tbio_add_page called successfully (bio_page_size: %d)\n", bio_page_size);

        data_cur += bio_page_size;
        chunk_pages--;
    }

    atomic_inc(&req->cb_data.bio_num);
    if(unlikely(chunk_pages)) {
        pages_left += chunk_pages; /*didn't finish our chunk, return leftovers*/
    }
    
    if(pages_left) {
        submit_bio(rw, b);
        bio_put(b);
        goto next_chunk;
    } else {
        atomic_set(&req->initialised, 1);
        smp_mb();
        submit_bio(rw, b);
        bio_put(b);
    }

err_alloc_bio:
err_alloc_req:
    return retval;
}

/** 
 *  update data in node.
 *  
 *  @param on_complete function to call once all fragments of
 *                     the command have completed.
 *  @param tid the id of the tree containing the node
 *  @param nid the id of the node to update
 *  @param offset the offset, in bytes, to write the data
 *  @param len the length, in bytes, of the data to write
 *  @param data the data to write
 *  @description updates the node identified by nid in the tree
 *               identified by tid by writing the supplied data
 *               at the supplied offset in the node.
 */
int cfsio_update_node(struct block_device *bd, cfsio_on_endio_t on_complete, u64 tid, u64 nid, u64 offset, u64 len, void *data) {
    return cfsio_data_request(bd, AOECMD_UPDATENODE, WRITE, on_complete, tid, nid, offset, len, data);
}

/**
 * read node data.
 *  
 * @param on_complete function to call once all fragments of the 
 *                    command have completed.
 * @param tid the id of the tree containing the node
 * @param nid the id of the node to read from
 * @param offset the offset within the node from which to begin reading
 * @param len the number of bytes to read 
 * @description reads the specified sequence of bytes from the 
 *              node.
 * @todo what about reading past the end, what attempting reads 
 *       to non-existing sequences entirely.
 */
int cfsio_read_node(struct block_device *bd, cfsio_on_endio_t on_complete, u64 tid, u64 nid, u64 offset, u64 len, void *data) {
    return cfsio_data_request(bd, AOECMD_READNODE, WRITE, on_complete, tid, nid, offset, len, data); /*TODO : WRITE => READ*/
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

    cfsio_rqfrag_pool = kmem_cache_create("cfsio_rq_frag", sizeof (struct cfsio_rq_frag), 0, 0, NULL);
    if (cfsio_rqfrag_pool == NULL) {
        pr_err("Failed to allocate cfsio_rqfrag_pool pool\n");
        goto err_alloc_rqfragpool;
    }

    cfsio_rq_pool = kmem_cache_create("cfsio_rq", sizeof (struct cfsio_rq), 0, 0, NULL);
    if (cfsio_rq_pool == NULL) {
        pr_err("Failed to allocate request pool (cfsio_rq_pool)\n");
        goto err_alloc_rqpool;
    }
    
    /*bogus values for bio's which send no additional data than 
      the tree command values themselves.*/
    nilbuf.page_addr = virt_to_page(&_nilbuf_data);
    nilbuf.bcnt = sizeof(_nilbuf_data);
    nilbuf.vec_off = offset_in_page(&_nilbuf_data);

    pr_debug("cfsio_init successful...\n");
    return 0;
err_alloc_rqpool:
    kmem_cache_destroy(cfsio_rqfrag_pool);
err_alloc_rqfragpool:
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
    if (cfsio_rqfrag_pool)
        kmem_cache_destroy(cfsio_rqfrag_pool);
    if (cfsio_rq_pool)
        kmem_cache_destroy(cfsio_rq_pool);
    if (bio_pool)
        bioset_free(bio_pool);
}

