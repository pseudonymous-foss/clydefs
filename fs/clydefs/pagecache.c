#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include "clydefs.h"
#include "inode.h"
#include "pagecache.h"
#include "io.h"
/* FIXME :: inode's must point to these page cache functions */

#if 0
struct page_req
{

    struct inode *inode;
    unsigned expected_pages;

};

static void __page_req_init(struct page_req *r, unsigned expected_pages, struct inode *i)
{
    r->inode = i;
    r->expected_pages = expected_pages;
}

static int readpage_strip(void *data, struct page *p)
{
    /*struct page_collect *pcol = data;*/
    struct page_req *r = data;
    struct inode *i = r->inode;
    loff_t i_size = i_size_read(i);
    struct cfs_inode *ci = CFS_INODE(i);
    /*last page that could be mapped to this file*/
    pgoff_t end_ndx = i_size >> PAGE_CACHE_SHIFT;
    size_t len;
    int retval;

    /*require page to be locked and containing stale data*/
    BUG_ON(!PageLocked(p));
    if (PageUptodate(p)){
        CLYDE_ERR("PageUptodate true for (ino: 0x%lx, p->index: 0x%lx)\n", r->inode->i_ino, p->index);
        BUG();
    }
    if (p->index < end_ndx) {
        /*not last page, read it all*/
        len = PAGE_CACHE_SIZE;
    } else if (p->index == end_ndx) {
        /*last page, figure out how many remaining 
          bytes there is to read*/
        len = i_size & ~PAGE_CACHE_MASK;
    } else {
        /*out of bounds*/
        len = 0;
    }

    if (!len) {
        /*out-of bounds*/
        clear_highpage(p);
        SetPageUptodate(p);
        if (PageError(p)) {
            ClearPageError(p);
        }
    }
}


static int cfs_readpages(struct file *f, struct address_space *mapping, 
                         struct list_head *pages, unsigned nr_pages)
{
    return -1;
}
#endif

static __always_inline __dbg_page_status(struct page *p)
{
    CFS_DBG(" PAGE [UptoDate:%s] [Dirty:%s] [Writeback:%s] [Locked:%s]\n",
            PageUptodate(p) ? "Y" : "N",
            PageDirty(p) ? "Y" : "N",
            PageWriteback(p) ? "Y" : "N",
            PageLocked(p) ? "Y" : "N");
}

/**
 *  @pre PageLocked(p) => true
 *  @post page is unlocked and marked 'uptodate'
 */ 
static int cfs_readpage(struct file *f, struct page *p)
{
    /* 
        REQUIRED TO:
        - unlock and mark page uptodate after read completes
        - see file "Locking" for more details!?
    */ 
    void *p_addr = NULL;
    struct inode *i = NULL;
    struct cfs_inode *ci = NULL;
    struct block_device *bd = NULL;
    loff_t i_size; /*read & store inode size, in bytes*/
    pgoff_t end_ndx;
    u64 len;
    u64 off;
    int retval;

    CLYDE_ASSERT(f != NULL);
    CLYDE_ASSERT(p != NULL);
    CLYDE_ASSERT(f->f_inode != NULL);

    i = f->f_inode;
    ci = CFS_INODE(i);
    bd = i->i_bdev;

    i_size = i_size_read(i);
    /*last page that could be mapped to this file*/
    end_ndx = i_size >> PAGE_CACHE_SHIFT;

    /*get offset of request in bytes*/
    off = p->index >> PAGE_CACHE_SHIFT;
    
    __dbg_page_status(p);

    /*require page to be locked and containing stale data*/
    BUG_ON(!PageLocked(p));
    if (PageUptodate(p)){
        CLYDE_ERR("PageUptodate true for (ino: 0x%lx, p->index: 0x%lx)\n", i->i_ino, p->index);
        BUG();
    }
    if (p->index < end_ndx) {
        /*not last page, read it all*/
        len = PAGE_CACHE_SIZE;
    } else if (p->index == end_ndx) {
        /*last page, figure out how many remaining 
          bytes there is to read*/
        len = i_size & ~PAGE_CACHE_MASK;
    } else {
        /*out of bounds*/
        len = 0;
    }

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
    if (p_addr) {
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

        if (PageLocked(p)) {
            unlock_page(p); /*FIXME is this always a good idea ?*/
        }
    } else {
        CFS_WARN("failed to translate supplied page into an actual address (using kmap(p) )\n");
    }
out:
    /*FIXME - when should I unmap a page ?*/
    kunmap(p);
    return retval;
}

static int cfs_write_begin(struct file *f, struct page *p, unsigned from, unsigned to)
{
    /* 
        REQUIRED TO:
        - check that the write can complete
        - allocate space if necessary
        - [write updates parts of basic blocks]
            read in these blocks to writeouts work as intended
        - return the locked page for the specified offset, in pagep
        - must be able to cope with short-writes
            (short-write:: len passed to write begin exceeds number of bytes copied into the page)
        - return 0 on success, < 0 on failure, which ensures write_end isn't called
     
     
        - may return a void* in 'fsdata', gets passed to write_end
        - flags is a field for AOP_FLAG_XXX values, described in include/linux/fs.h
    */ 
    return 0;
}

/** 
 * @pre PG_Dirty has been cleared, PageLocked(p) => true 
 */ 
static int cfs_writepage(struct page *p, struct writeback_control *wbc)
{
    /* 
        Required to:
        - set PG_Writeback
        - unlock page, either synchronously or asynchronously when the operation completes
        - [wbc->sync_mode == WB_SYNC_NONE -- ok to err out in case of errors]
            if aborting the writeout, return AOP_WRITEPAGE_ACTIVATE
        - see "Locking" file for more details !?
    */ 
    struct inode *i = NULL;
    struct cfs_inode *ci = NULL;
    struct block_device *bd = NULL;
    void *p_addr = NULL;
    loff_t i_size; /*read & store inode size, in bytes*/
    pgoff_t end_ndx;
    u64 len;
    u64 off;
    int retval;

    CLYDE_ASSERT(p != NULL);
    CLYDE_ASSERT(wbc != NULL);
    __dbg_page_status(p);

    i = p->mapping->host;
    ci = CFS_INODE(i);
    bd = i->i_bdev;
    i_size = i_size_read(i);

    /*last page that could be mapped to this file*/
    end_ndx = i_size >> PAGE_CACHE_SHIFT;
    /*get offset of request in bytes*/
    off = p->index >> PAGE_CACHE_SHIFT;

    BUG_ON(!PageLocked(p));
    if (PageUptodate(p)){
        CLYDE_ERR("PageUptodate true for (ino: 0x%lx, p->index: 0x%lx)\n", i->i_ino, p->index);
        BUG();
    }
    if (p->index < end_ndx) {
        /*not last page, read it all*/
        len = PAGE_CACHE_SIZE;
    } else if (p->index == end_ndx) {
        /*last page, figure out how many remaining 
          bytes there is to read*/
        len = i_size & ~PAGE_CACHE_MASK;
    } else {
        /*out of bounds*/
        len = 0;
    }

     p_addr = kmap(p);
    if (p_addr) {
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

        if (PageLocked(p)) {
            unlock_page(p); /*FIXME is this always a good idea ?*/
        }
    } else {
        CFS_WARN("failed to translate supplied page into an actual address (using kmap(p) )\n");
    }
out:
    /*FIXME - when should I unmap a page ?*/
    kunmap(p);
}

static int cfs_write_end(struct file *f, struct address_space *mapping, 
                         loff_t pos, unsigned len, unsigned copied, 
                         struct page *p, void *fsdata)
{
    /*
        REQUIRED TO
        - unlock the page, release its refcount
        - update i_size
        - return < 0 on failure, otherwise no of bytes (<= 'copied')
            that were able to be copied into pagecache
     
        - 'len' is the original 'len' passed to write_begin
        - 'copied' => amount that was able to be copied
        - ONLY called after a SUCCESSFUL write_begin
    */
    return -1;
}

static int cfs_releasepage(struct page *p, gfp_t gfp)
{ /*don't implement*/
    CFS_WARN("page 0x%lx released, STUB!\n", p->index);
    return 0;
}

static void cfs_invalidatepage(struct page *p, unsigned long off)
{ /*don't implement*/
    CFS_WARN("page 0x%lx offset 0x%lx invalidated, STUB!\n", p->index, off);
    WARN_ON(1);
}

const struct address_space_operations cfs_aops = {
    .readpage = cfs_readpage,
    
    .write_begin = cfs_write_begin,
    .writepage = cfs_writepage,
    .writepages = NULL, /*cfs_writepages,*/
    #if 0
    .write_end = cfs_write_end,
    #endif
    .releasepage = cfs_releasepage,
    .set_page_dirty = __set_page_dirty_nobuffers,
    .invalidatepage = cfs_invalidatepage,
    .bmap = NULL,
    .direct_IO = NULL,
    .get_xip_mem = NULL,
    .migratepage = NULL,
    .launder_page = NULL,
    .is_partially_uptodate = NULL,
    .error_remove_page = NULL,
};
