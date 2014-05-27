#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/pagemap.h>
#include "clydefs.h"
#include "inode.h"
#include "pagecache.h"
#include "io.h"

//#define CFS_DBGMSG(...) do {} while (0)
#define CFS_DBGMSG(fmt, a...) printk("cfs<%s>,%d DBG -- " fmt, __FUNCTION__, __LINE__, ##a)
//#define CFS_DBGMSG(fmt, a...) 
#define PAGE_NDX_UNSET -1

typedef enum {
    RT_PAGE_READ,
    RT_PAGE_RWU,
} read_type_t;

/*Caches*/
static struct kmem_cache *cfspc_pgseg_pool = NULL;

const struct address_space_operations cfs_aops;

/** 
 * Initialises page collection structure which itself will 
 * describe a range of pages within the inode 'host' to be 
 * written. 
 * @param pgseg the page segment structure itself 
 * @param expected_pages the number of pages covered by the 
 *                       range which in principle can be part of
 *                       this collection. (WB_SYNC_NONE may
 *                       reduce the actual set)
 * @param host the inode/file which owns these pages
 */
static void pgseg_init(struct page_segment *pgseg, 
                      unsigned int expected_pages, struct inode *host){
    pgseg->host = host;
    pgseg->expected_pages = expected_pages;
    
    pgseg->pages = NULL;
    pgseg->pages_len = 0;
    pgseg->pages_capacity = 0;

    pgseg->first_page_ndx = PAGE_NDX_UNSET;
}

static int pgseg_page_alloc(struct page_segment *pgseg)
{
	unsigned int pages = pgseg->expected_pages;
    CFS_DBGMSG("allocating pages array for page segment (expected_pages: %u)\n", pgseg->expected_pages);
    /*try halving allocation requests at each step 
      until success or complete failure*/
	for (; pages; pages >>= 1) {
		pgseg->pages = kmalloc(pages * sizeof(struct page *), GFP_KERNEL);
		if (likely(pgseg->pages)) {
            pgseg->pages_capacity = pages;
			return 0;
		}
	}
    CFS_ERR(
        "Failed to allocate *any* pages for page collection (ino: 0x%lx)\n", 
        pgseg->host->i_ino
    );
	return -ENOMEM;
}

static __always_inline int pgseg_addpage(
    struct page_segment *pgseg, struct page *page, unsigned int len)
{
    CFS_DBGMSG(
        "adding page.. pgseg{pages_len: '%u', pages_capacity:'%u'}\n", 
        pgseg->pages_len, pgseg->pages_capacity
    );
    if (unlikely(pgseg->pages_len == pgseg->pages_capacity)) {
        return -ENOMEM;
    }

    pgseg->pages[pgseg->pages_len++] = page;
    pgseg->length += len;
    return 0;
}

static struct page_segment *pgseg_adopt_segment(struct page_segment *pgseg)
{
    struct page_segment *npgseg = NULL;

    CLYDE_ASSERT(pgseg != NULL);
    CLYDE_ASSERT(cfspc_pgseg_pool != NULL);

    npgseg = kmem_cache_alloc(cfspc_pgseg_pool, GFP_ATOMIC);
	if (!npgseg) {
        CFS_DBG("Failed to allocate new page segment struct.\n");
		return NULL;
    }
    *npgseg = *pgseg;

    pgseg->pages = NULL;
    pgseg->pages_capacity = pgseg->pages_len = 0;

    return npgseg;
}

static __always_inline void pgseg_clean(struct page_segment *pgseg)
{
	kfree(pgseg->pages);
	pgseg->pages = NULL;
}

/** 
 * Calculates number of bytes to read/write based on the 
 * requested page in relation to the possible number of pages 
 * for the associated inode. 
 * @param p the requested page 
 * @return number of bytes to read, if not reading the last 
 *         page, a full page is to be read, otherwise reading as
 *         much of the last page as possible or nothing if the
 *         request is outside the inode's page range
 */
static __always_inline u64 page_ndx_to_bytes(struct page const * const p)
{
    u64 i_size = i_size_read(p->mapping->host);
    u64 end_ndx = i_size >> PAGE_CACHE_SHIFT;
    if (p->index < end_ndx) {
        /*not last page, read it all*/
        return PAGE_CACHE_SIZE;
    } else if (p->index == end_ndx) {
        /*last page, figure out how many remaining 
          bytes there is to read*/
        return (i_size & ~PAGE_CACHE_MASK);
    } else {
        /*out of bounds*/
        return 0;
    }
}

/** 
 * @pre i->i_mutex held 
 */ 
static __always_inline void __write_failed(struct inode *i, loff_t off)
{
    if (off > i->i_size) {
        /*were writing past end, truncate page to reflect current inode size*/
        truncate_pagecache(i, off, i->i_size);
    }
}

static __always_inline void __dbg_page_status(struct page *p)
{
    CFS_DBG(" PAGE [UptoDate:%s] [Dirty:%s] [Writeback:%s] [Locked:%s]\n",
            PageUptodate(p) ? "Y" : "N",
            PageDirty(p) ? "Y" : "N",
            PageWriteback(p) ? "Y" : "N",
            PageLocked(p) ? "Y" : "N");
}

/** 
 *  Read (at most) a page's worth of data into the supplied page
 *  as specified by the p->index offset.
 *  @param p page to read data into
 *  @param rwu reflects whether this is part of a
 *             read-write-update operation (i.e. called in
 *             preparation of a write smaller than a full page
 *             on an otherwise not updated page) -- if so, page
 *             will not be unlocked afterwards.
 *  @pre PageLocked(p) => true
 *  @post if not rwu; page is unlocked and marked 'uptodate'
 */ 
static int cfsp_readpage(struct page *p, read_type_t rwu)
{
    /* 
        REQUIRED TO:
        - unlock and mark page uptodate after read completes
            [DONE]
        - see file "Locking" for more details!?
    */ 
    void *p_addr = NULL;
    struct inode *i = NULL;
    struct cfs_inode *ci = NULL;
    struct block_device *bd = NULL;
    struct cfs_sb *csb = NULL;
    u64 len;
    u64 off;
    int retval = 0;

    CFS_DBG("called");
    CLYDE_ASSERT(p != NULL);

    i = p->mapping->host;
    ci = CFS_INODE(i);
    bd = i->i_sb->s_bdev;
    csb = CFS_SB(i->i_sb);
    CFS_DBG(" ino(%lu)\n", i->i_ino); /*finish 'called ' msg*/
    atomic_inc(&csb->pending_io_ops);

    /*get offset of request in bytes*/
    off = p->index << PAGE_CACHE_SHIFT;
    
    __dbg_page_status(p);

    /*require page to be locked and containing stale data*/
    BUG_ON(!PageLocked(p));
    if (PageUptodate(p)){
        CLYDE_ERR("PageUptodate true for (ino: 0x%lx, p->index: 0x%lx)\n", i->i_ino, p->index);
        BUG();
    }
    len = page_ndx_to_bytes(p);

    if (!len) {
        /*out-of bounds*/
        CFS_WARN("attempted to read an out-of-bounds page\n");
        clear_highpage(p);
        SetPageUptodate(p);
        if (PageError(p)) {
            ClearPageError(p);
        }
    }
    
    p_addr = kmap(p);
    if (p_addr && len != 0) {
        retval = cfsio_read_node_sync(bd, NULL, NULL, ci->data.tid, ci->data.nid, off, len, p_addr);
        if (retval) {
            /*FIXME -- assume returning non-zero indicates an error and assume 
              not setting SetPageUptodate motivates whoever made the read
              request to try again*/
            CFS_WARN("failed to read a page in from node\n");
            retval = -1;
            goto out;
        }
        SetPageUptodate(p);
    } else {
        CFS_DBG("failed to translate supplied page into an actual address (using kmap(p) )\n");
    }
    if (rwu == RT_PAGE_READ) {
        /*just a regulard read which expects the page to be unlocked once done*/
        unlock_page(p);
    }
out:
    /*FIXME - when should I unmap a page ?*/
    kunmap(p);
    atomic_dec(&csb->pending_io_ops);
    return retval;
}

/**
 *  @pre PageLocked(p) => true
 *  @post page is unlocked and marked 'uptodate'
 */ 
static int cfsp_aopi_readpage(struct file *f, struct page *p)
{
    /*isolated read page request, unlock page afterwards*/
    return cfsp_readpage(p, RT_PAGE_READ);
}

/**
 * 
 * @param f the file for which a range is to be written
 * @param mapping address space associated the file (and by 
 *                extension, the inode)
 * @param off offset into file's address space, in bytes
 * @param len length of write
 * @param flags a field for AOP_FLAG_XXX values, described in 
 *              include/linux/fs.h
 * @param pagep should be assigned the page chosen for the 
 *              coming write.
 * @param fsdata optional, additional data to be passed along 
 *               to .write_end
 * 
 * @return 0 on success; error otherwise (write won't proceed 
 *         then)
 * @post *pagep points to the page selected for the write (must 
 *       be locked)
 */
static int cfs_write_begin(struct file *f, struct address_space *mapping, loff_t off, unsigned len, unsigned flags, struct page **pagep, void **fsdata)
{
    /* 
        REQUIRED TO:
        - check that the write can complete
            [DONE] (can't check)
        - allocate space if necessary
            [DONE] (not our problem)
        - [write updates parts of basic blocks]
            read in these blocks so writeouts work as intended
        - return the locked page for the specified offset, in pagep
            [DONE] - relying on simple_write_begin
        - must be able to cope with short-writes
            (short-write:: len passed to write begin exceeds number of bytes copied into the page)
        - return 0 on success, < 0 on failure, which ensures write_end isn't called
     
     
        - may return a void* in 'fsdata', gets passed to write_end
        - flags is a field for AOP_FLAG_XXX values, described in include/linux/fs.h
    */

    struct page *p = NULL;
    int retval = 0;
    u64 read_bytes = 0;
    
    CFS_DBG("called\n");

    p = *pagep;
    if (p == NULL) {
        CFS_DBG("getting a page\n");
        retval = simple_write_begin(f,mapping,off,len,flags,pagep,fsdata);
        if (retval) {
            CFS_DBG("simple_write_begin call returned with an error\n");
            goto out;
        }
        p = *pagep; /*reassigning is necesary as simple_write_begin is expected to set *pagep */
    }
    CFS_DBG("simple_write_begin done..\n");

    
    if (PageUptodate(p) || len == PAGE_CACHE_SIZE) {
        /*not doing an RWU operation, simply finding a page 
          will suffice, then, all done!*/
        goto out;
    }

    CFS_DBG("page not up-to-date or doing a partial write\n");
    /*
        page is not up to date or we're writing less
        than the base unit of transfer which corresponds to
        a page - 
    */
    read_bytes = page_ndx_to_bytes(p);

    if (!read_bytes) {
        /*out of range*/
        CFS_DBG("out of range\n");
        clear_highpage(p);
        SetPageUptodate(p);
        goto out;
    }
        
    /*read data in as part of an rwu operation*/
    CFS_DBG("b4 cfsp_readpage\n");
    retval = cfsp_readpage(p, RT_PAGE_RWU);
    if (retval) {
        unlock_page(p);
        CFS_DBG("failed to read page");
    }
    CFS_DBG("b4 out\n");
out:
    if (unlikely(retval)) {
        __write_failed(mapping->host, off+len);
    }
    CFS_DBG("done\n");
    return retval;
}

static int cfsp_aopi_write_begin(
    struct file *f, struct address_space *mapping, 
    loff_t off, unsigned len, unsigned flags, 
    struct page **pagep, void **fsdata)
{
    *pagep = NULL;
    return cfs_write_begin(f,mapping,off,len,flags,pagep,fsdata);
}

#if 0
/** 
 * @pre PG_Dirty has been cleared, PageLocked(p) => true 
 */ 
static int cfsp_aopi_writepage(struct page *p, struct writeback_control *wbc)
{
    /* 
        Required to:
        - set PG_writeback (enum pageflags, page_flags)
            [DONE]
        - unlock page, either synchronously or asynchronously when the operation completes
            [DONE]
        - [wbc->sync_mode == WB_SYNC_NONE -- ok to err out in case of errors]
            if aborting the writeout, return AOP_WRITEPAGE_ACTIVATE
        - see "Locking" file for more details !?
    */ 
    struct inode *i = NULL;
    struct cfs_inode *ci = NULL;
    struct cfs_sb *csb = NULL;
    struct block_device *bd = NULL;
    void *p_addr = NULL;
    u64 len;
    u64 off;
    int retval = 0;

    CFS_DBG("called\n");
    CLYDE_ASSERT(p != NULL);
    CLYDE_ASSERT(wbc != NULL);
    __dbg_page_status(p);
    i = p->mapping->host;
    ci = CFS_INODE(i);
    csb = CFS_SB(i->i_sb);
    bd = i->i_sb->s_bdev;

    atomic_inc(&csb->pending_io_ops);

    /*get offset of request in bytes*/
    off = p->index << PAGE_CACHE_SHIFT;

    BUG_ON(!PageLocked(p));
    len = page_ndx_to_bytes(p);

    p_addr = kmap(p);
    if (p_addr && len != 0) {
        set_page_writeback(p);
        retval = cfsio_update_node_sync(bd, NULL, NULL, ci->data.tid, ci->data.nid, off, len, p_addr);
        if (retval) {
            /*FIXME -- assume returning non-zero indicates an error and assume 
              not setting SetPageUptodate motivates whoever made the read
              request to try again*/
            CFS_WARN("failed to write page to node\n");
            retval = -1;
            goto out;
        }
        SetPageUptodate(p);
        end_page_writeback(p); /*mark that we are done writing the data back to storage*/

        if (PageLocked(p)) {
            CFS_DBG("page locked, write op over, unlock page\n");
            unlock_page(p);
        } else {
            CFS_DBG("PAGE WASN'T LOCKED !? ERROR!");
            BUG();
        }

    } else {
        CFS_DBG("failed to translate supplied page into an actual address (using kmap(p) )\n");
    }

out:
    if (p_addr) 
        kunmap(p);
    CFS_DBG("done\n");
    atomic_dec(&csb->pending_io_ops);
    return retval;
}
#endif

/** 
 * See 'cfsio_on_endio_t' for details 
 */ 
static void write_segment_done(struct cfsio_rq_cb_data *req_data, void *data, int error)
{
    struct cfs_sb *csb;
    struct page_segment *pgseg = data;
    int i;
    /*TODO: should be able to handle failure on a per-bio level 
      (and actually know which pages participated in a bio) to
      sort out errors.
      Mark each page in a failed bio with SetPageError*/

    CLYDE_ASSERT(pgseg != NULL);

    csb = CFS_SB(pgseg->host->i_sb);
    atomic_dec(&csb->pending_io_ops);

    printk("write_segment_done called\n");

    if(error) {
        CFS_ERR("Cannot handle I/O errors at this level, presently\n");
        BUG();
    }

    for (i = 0; i < pgseg->pages_len; i++) { /*Happy-path scenario*/
        struct page *p = pgseg->pages[i];
        SetPageUptodate(p);
        end_page_writeback(p);
        unlock_page(p);
    }

    /*release page segment*/
    pgseg_clean(pgseg);
    kmem_cache_free(cfspc_pgseg_pool, pgseg);
}

/** 
 * instigate the writing of the supplied collection 
 * @return 0 on success, error otherwise 
 */ 
static int write_segment(struct page_segment *pgseg_src)
{
    //copy off segment and adopt the page array                     [DONE]
    //increment pending_io var                                      [DONE]

    //set some form of async handler on my cfsio_data_request to:
    //  - free page array (and such, use pgseg_ fnc)
    //  - unlock pages, clear the page writeback mark
    // decrement pending_io var, 

    struct page_segment *pgseg = NULL;
    struct inode *i = NULL;
    struct cfs_inode *ci = NULL;
    struct cfs_sb *csb = NULL;
    struct block_device *bd = NULL;
    u64 offset;

    if (pgseg_src->pages_len == 0){
        CFS_DBGMSG("requested to write empty page segment - ignoring.\n");
        return 0;
    }

    CFS_DBGMSG("Adopting page segment\n");
    pgseg = pgseg_adopt_segment(pgseg_src);
    if (!pgseg) {
        CFS_DBGMSG("\tAdopting the page segment failed!\n");
        goto err_pgseg_alloc;
    }

    i = pgseg->host;
    ci = CFS_INODE(i);
    csb = CFS_SB(i->i_sb);
    bd = i->i_sb->s_bdev;

    atomic_inc(&csb->pending_io_ops);
    
    CFS_DBGMSG(
        "Determining size of last page, pgseg{pages_len: '%u', pages_capacity: '%u'}\n", 
        pgseg->pages_len, pgseg->pages_capacity
    );
    /*must determine the size of the last page, all others are assumed PAGE_SIZE*/
    pgseg->page_last_size = (u32)(page_ndx_to_bytes(
        pgseg->pages[pgseg->pages_len-1]) & 0xFFFFFFFF);

    offset = pgseg->pages[0]->index << PAGE_CACHE_SHIFT;
    CFS_DBGMSG("before cfsio_update_node_ps\n");
    cfsio_update_node_ps(
        bd, pgseg, 
        write_segment_done, pgseg, 
        ci->data.tid, ci->data.nid, offset
    );

    return 0;
    err_pgseg_alloc:
        return -ENOMEM;
}

/** 
 *  
 * @param data the page collection struct - Resides on stack, 
 *             MUST be copied off when a write is issued.
 * @return 0 on success, error otherwise 
 */ 
static int bundle_page(struct page *page,
			   struct writeback_control *wbc_unused, void *data)
{
    struct page_segment *pgseg = data;
    struct inode *i = pgseg->host;
    loff_t i_size = i_size_read(i);
    pgoff_t end_index = i_size >> PAGE_CACHE_SHIFT;
    u32 len;
    int ret;

    BUG_ON(!PageLocked(page));

    /*FIXME: do I have some form of initialization wait-for-inode-creation stuff ?*/

    /*unless it's the last page, we can safely write the entire page*/
    if (page->index < end_index) {
        len = PAGE_CACHE_SIZE;
    } else {
        len = i_size & ~PAGE_CACHE_MASK;

        /*TODO: TRUNCATION CODE -- address later*/
    }

add_page:
    if (unlikely(pgseg->first_page_ndx == PAGE_NDX_UNSET)) {
        pgseg->first_page_ndx = page->index;
    } else if (unlikely((pgseg->first_page_ndx + pgseg->pages_len) != page->index)) {
        /*contiguity broken, exec request as is and queue up this page as the first 
          of a new segment.*/
        CFS_DBGMSG(
            "bundle_page(0x%lx, 0x%lx) contiguity broken, issuing write\n",
            i->i_ino, page->index
        );
        ret = write_segment(pgseg);
        if (unlikely(ret)){
            goto err;
        }

        /*segment write issued, pgseg can model a new segment now.*/
        goto add_page;
    }

    if (!pgseg->pages) {
        /*allocate entries for page array*/
        ret = pgseg_page_alloc(pgseg);
        if (unlikely(ret)) {
            goto err;
        }
    }

    CFS_DBGMSG(
        "bundle_page(0x%lx, 0x%lx) len=0x%x\n",
        i->i_ino, page->index, len
    );

    ret = pgseg_addpage(pgseg, page, len); /*OOM, write segment now.*/
    if (unlikely(ret)) {
        CFS_DBGMSG(
            "bundle_page - pgseg_addpage failed, pages_len=%u segment_length(bytes)=%llu - issuing write_segment\n",
            pgseg->pages_len, pgseg->length
        );

        ret = write_segment(pgseg);
        if (unlikely(ret)) {
            CFS_DBGMSG("write_segment failed => %d\n", ret);
            goto err;
        }

        /*segment write issued, pgseg can model a new segment now.*/
        goto add_page; 
    }

    BUG_ON(PageWriteback(page));
    set_page_writeback(page);

    return 0;
err:
    CFS_DBGMSG(
        "Err: bundle_page(0x%lx, 0x%lx) => %d\n",
        i->i_ino, page->index, ret
    );
    set_bit(AS_EIO, &page->mapping->flags);
    unlock_page(page);
    return ret;
}

int cfsp_aopi_writepages(struct address_space *mapping,
		       struct writeback_control *wbc)
{
    /*FIXME -- should actually keep track of pages written via 
      wbc->nr_to_write (decrement as pages are written)*/
    struct page_segment pgseg;
    loff_t start, end, expected_pages;
    int ret;

    start = wbc->range_start >> PAGE_CACHE_SHIFT;
    end = (wbc->range_end == LLONG_MAX) ?
        start + mapping->nrpages :
        wbc->range_end >> PAGE_CACHE_SHIFT;

    if (start || end )
        expected_pages = end - start + 1;
    else
        expected_pages = mapping->nrpages;

    CFS_DBGMSG("inode(0x%lx) wbc->start=0x%llx wbc->end=0x%llx "
               "nrpages=%lu start=0x%llx end=0x%llx expected_pages=%lld\n",
               mapping->host->i_ino, wbc->range_start, wbc->range_end,
               mapping->nrpages, start, end, expected_pages);

    pgseg_init(&pgseg, expected_pages, mapping->host);

    ret = write_cache_pages(mapping, wbc, bundle_page, &pgseg);
	if (unlikely(ret)) {
		CFS_ERR("write_cache_pages returned => %d\n", ret);
		return ret;
	}

    CFS_DBGMSG("write_cache_pages finished bundling up pages"
               " into segment(s) - issuing write\n");
	ret = write_segment(&pgseg);
	if (unlikely(ret))
		return ret;

	if (wbc->sync_mode == WB_SYNC_ALL) {
        /*FS integrity write, ensure pages are written!*/
        CFS_DBGMSG("WB_SYNC_ALL (=>integrity write) -- issuing write_segment to write remaining\n");
		return write_segment(&pgseg);
	} else if (pgseg.pages_len) {
        /*non-integrity write. Let the leftover pages 
          parttake in some subsequent write*/
		unsigned i;

		for (i = 0; i < pgseg.pages_len; i++) {
			struct page *page = pgseg.pages[i];
            
            /*
            end_page_writeback(page);
            set_page_dirty(page); 
            */ 
            redirty_page_for_writepage(wbc,page);
			unlock_page(page);
		}
	}
	return 0;
}

/** 
 * @description after a write, this operation unlocks and 
 *              releases the page (decreasing its refcount) and
 *              updates the inode size to reflect the current
 *              size of the file, if changed. Finally 
 * @param f file we are writing to 
 * @param mapping address space associated file and inode. 
 * @param off ... 
 * @param len len of write request made 
 * @param copied amount that was able to be copied 
 * @param p reference to page cache page, used in the write 
 *          operation
 * @param fsdata (optional) additional data use by the FS during 
 *               the write operation.
 * @pre imutex of inode associated file pointer 'f' and address 
 *      space 'mapping' is held
 * @pre calling this function means the write itself was 
 *      successful.
 * @return on success; number of bytes actually written (copied)
 */
static int cfsp_aopi_write_end(struct file *f, struct address_space *mapping, 
                         loff_t off, unsigned len, unsigned copied, 
                         struct page *p, void *fsdata)
{
    /*
        REQUIRED TO
        - unlock the page, release its refcount
            [DONE]
        - update i_size
            [DONE]
        - return < 0 on failure, otherwise no of bytes (<= 'copied')
            that were able to be copied into pagecache
            [DONE]
    */
    struct inode *i = mapping->host;
    loff_t i_size = i->i_size; /*we hold i_mutex, so reading directly is ok*/
    int retval;

    CFS_DBG("called\n");
    printk("cfsp_aopi_write_end (ino: 0x%lx, page ndx: 0x%lx)\n", mapping->host->i_ino,p->index);

    /*will unlock & release the page (release=>refcount put operation), & update i_size*/
    retval = simple_write_end(f,mapping,off,len,copied,p,fsdata);
    if (unlikely(retval)) {
        CFS_DBG("write failed!\n");
        __write_failed(i, off+len);
    }

    if (i_size != i->i_size) {
        /*size changed as a result of the write*/
        CFS_DBG("i{ino:%lu} size changed as a result of the write\n", i->i_ino);
        mark_inode_dirty(i);
    }

    CFS_DBG("done\n");
    return retval;
}

/*no need to define*/
#if 0 
static int cfsp_releasepage(struct page *p, gfp_t gfp)
{
    CFS_DBG("called\n");
    CFS_WARN("page 0x%lx released, STUB!\n", p->index);
    return 0;
}
#endif 

/*no need to define*/
#if 0
static void cfsp_invalidatepage(struct page *p, unsigned long off)
{ /*don't implement*/
    CFS_DBG("called\n");
    CFS_WARN("page 0x%lx offset 0x%lx invalidated, STUB!\n", p->index, off);
    WARN_ON(1);
}
#endif

/*no need to define*/
#if 0
int cfsp_set_page_dirty(struct page *page)
{
    CFS_DBG("called\n");
    return __set_page_dirty_nobuffers(page);
}
#endif 

const struct address_space_operations cfs_aops = {
    .readpage = cfsp_aopi_readpage,
    
    /*buffered writes*/
    .write_begin = cfsp_aopi_write_begin,
    .write_end = cfsp_aopi_write_end,

    /*mostly of interest to mmap'ed calls*/
    //.writepage = cfsp_aopi_writepage,
    .writepage = NULL,
    .writepages = cfsp_aopi_writepages,

    /*.releasepage = cfsp_releasepage,*/ /*OPTIONAL, but see documentation for responsibilities if defined*/
    /*.set_page_dirty = cfsp_set_page_dirty,*/ /*OPTIONAL, but see documentation for responsibilities if defined*/
    /*.invalidatepage = cfsp_invalidatepage,*/
    .bmap = NULL,
    .direct_IO = NULL,
    .get_xip_mem = NULL,
    .migratepage = NULL,
    .launder_page = NULL,
    .is_partially_uptodate = NULL,
    .error_remove_page = NULL,
};

/** 
 * Initialise the segment cache. 
 */ 
static int __segmentcache_init(void)
{
	cfspc_pgseg_pool = kmem_cache_create(
        "cfspc_pgseg_pool",
		sizeof(struct cfs_inode),
        0,
        /*objects are reclaimable*/
		SLAB_RECLAIM_ACCOUNT,
        NULL
    );

	if (!cfspc_pgseg_pool)
		return -ENOMEM;
	return 0;
}

/** 
 * Destroy the segment cache. 
 * @note will wait until all rcu operations are finished. 
 */ 
static void __segmentcache_destroy(void)
{
	rcu_barrier();
	kmem_cache_destroy(cfspc_pgseg_pool);
}


/** 
 * Initialise page cache structures.
 */ 
int cfspc_init(void)
{
    int retval;

    retval = __segmentcache_init();
    if (retval)
        goto err;

    return 0; /*success*/
err:
    return retval;
}

void cfspc_exit(void)
{
    /*reverse order of pagecache_init*/
    CFS_DBG("called\n");
    __segmentcache_destroy();
}
