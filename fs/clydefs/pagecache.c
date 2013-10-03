#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include "clydefs.h"
#include "inode.h"
#include "pagecache.h"
#include "io.h"
/* FIXME :: inode's must point to these page cache functions */

typedef enum {
    RT_PAGE_READ,
    RT_PAGE_RWU,
} read_type_t;



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
            read in these blocks to writeouts work as intended
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
    .writepage = cfsp_aopi_writepage,
    .writepages = generic_writepages, /*relies on .writepage*/

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
