#ifndef __CLYDEFS_PAGECACHE_H
#define __CLYDEFS_PAGECACHE_H

struct page_segment {
    struct inode *host;
    // -- expected in that unless WB_SYNC_ALL, not all pages
    // in the range are guaranteed to be written
    unsigned int expected_pages;

    /*the array of pages this collection encompasses*/
    struct page **pages;            
    u32 pages_len;         /*number of pages in arr*/
    u32 pages_capacity;    /*size of array*/

    loff_t first_page_ndx;          /*index of first page in segment*/
    u32 page_last_size;             /*size of last page, all others are 
                                    assumed to be 'PAGE_SIZE', set right
                                    before issuing the write*/

    u64 length;                     /*length of this entire segment*/
};

int cfspc_init(void);
void cfspc_exit(void);
#endif //__CLYDEFS_PAGECACHE_H
