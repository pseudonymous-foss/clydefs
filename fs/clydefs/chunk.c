#include "clydefs.h"
#include "chunk.h"

static struct kmem_cache *chunk_pool = NULL;

/** 
 * Initialise chunk's freelist by setting every entry to free. 
 * @param c the chunk under initialisation 
 */  
static __always_inline void __flist_init(struct cfsd_inode_chunk *c)
{
    int i;
    u8 *freelist; 
    CLYDE_ASSERT(c != NULL);

    freelist = c->hdr.freelist;
    for (i=0; i<CHUNK_FREELIST_BYTES; i++){
        freelist[i] = 0b11111111U; /*all entries available*/
    }
}

/** 
 * Allocate an entry in the chunk for insertion of a new ientry. 
 * @param ret_ndx on success(FOUND), will hold the index of the 
 *                allocated ientry within the chunk.
 * @param c the chunk in which to make the allocation. 
 * @return 0 if an entry was found and its index set in 
 *         'ret_ndx'; -1 if the chunk is filled
 * @pre inode itbl write lock held 
 */
static int __flist_entry_alloc(u64 *ret_ndx, struct cfsd_inode_chunk *c)
{
    u8 i, j, sv, v;
    u8 *freelist;
    CLYDE_ASSERT(ret_ndx != NULL);
    CLYDE_ASSERT(*ret_ndx == 0);
    CLYDE_ASSERT(c != NULL);
    /*FIXME write an assert to check write lock ?*/

    if (!c->hdr.entries_free) { /*no free entries*/
        CFS_DBG("c->hdr.entries_free: %u\n", c->hdr.entries_free);
        /*cannot be last chunk, whoever inserts the last ientry must make a new chunk for the itbl*/
        CLYDE_ASSERT(!c->hdr.last_chunk);
        return -1;
    }

    /*guaranteed an entry, now to find it...*/
    freelist = c->hdr.freelist;
    for (i=0; i<CHUNK_FREELIST_BYTES; i++) {
        if (freelist[i]) {
            /*a free entry somewhere in this byte*/
            v = freelist[i];
            sv = 1U;
            j = 0U;
            while(j < 8) { /*j [0;7]*/
                if (v & sv) {
                    /*found a free entry!*/
                    freelist[i] |= sv; /*reserve it*/
                    /*each b holds 8 entries, hence i*8, j is the index of the entry in the byte*/
                    *ret_ndx = (i*8)+j;
                    return 0;
                }
                /*try next index instead*/
                sv = 1 << ++j;
            }
            CFS_DBG("Determined a free entry was in a byte, yet couldn't find it!\n");
            BUG();
        } /*nothing found, next byte*/
    }
    CFS_DBG("entries_free indicated that there should at least be an empty entry, yet we didn't find it!\n");
    BUG();
}

/**
 * Allocate a new chunk 
 * @return NULL on failure, otherwise a zero'ed chunk. 
 */
void *cfsc_chunk_alloc()
{
    return kmem_cache_zalloc(chunk_pool, GFP_KERNEL);
}

/**
 * Free chunk.
 * @param c the chunk to free
 */
void cfsc_chunk_free(struct cfsd_inode_chunk *c)
{
    kmem_cache_free(chunk_pool, c);
}

/**
 * Finds an empty slot in the chunk and inserts the entry.
 * @param c the chunk into which the entry is inserted
 * @param e the entry to insert 
 * @return 0 on success; -1 if the chunk is full 
 * @note you may have to sort the chunk entries 
 */
int cfsc_chunk_insert_entry(u64 *ret_ndx, struct cfsd_inode_chunk *c, struct cfsd_ientry const *e)
{
    int retval = 0;
    if ((retval=__flist_entry_alloc(ret_ndx, c))) {
        CFS_DBG("__flist_entry_alloc err, retval: %d\n", retval);
        goto out;
    }

    c->entries[*ret_ndx] = *e;
    c->hdr.entries_free--;
out:
    return retval;
}

/** 
 * Marks ientry as being free. 
 * @param c the chunk in which the entry resides 
 * @param ndx the entry to free 
 * @pre inode itbl write lock held 
 */
static __always_inline void __flist_entry_free(struct cfsd_inode_chunk *c, u8 ndx)
{ /*free the entry by zeroing out the bit @ the ndx'th entry in the freelist */ 
    u8 bm;
    CLYDE_ASSERT(c != NULL);
    CLYDE_ASSERT(ndx < CHUNK_NUMENTRIES);
    bm = ~(1U << (ndx % 8)); /*byte mask*/
    c->hdr.freelist[ndx/8] &= bm; /*apply mask to clear bit*/
}

/** 
 * Sets chunk values that of a completely empty tail-end chunk 
 * @param c chunk to modify 
 * @param entire chunk has been zero'ed out 
 */ 
void cfsc_chunk_init_common(struct cfsd_inode_chunk *c)
{
    CLYDE_ASSERT(c != NULL);
    c->hdr.entries_free = CHUNK_NUMENTRIES;
    c->hdr.last_chunk = 1;
    __flist_init(c); /*set each bit to 1 to mark empty slots*/
}

static int ientry_cmp(struct cfsd_ientry const *v1, struct cfsd_ientry const *v2)
{
    /* comparator for inode entry bsearch & sort
        - neg iff e1 precedes e2
        - pos iff e2 precedes e1
        - 0 iff e1 == e2
    */
    return strcmp(v1->name, v2->name);
}

static __always_inline void chunk_mk_key(struct cfsd_ientry *search_key, struct dentry const * const d)
{
    search_key->nlen = d->d_name.len;
    strncpy(search_key->name, d->d_name.name, search_key->nlen); /*FIXME - */
}

#if 0
int bsearch(u64 *ret_ndx, const void *key, const void *base, u64 num, u64 size,
	      int (*cmp)(const void *key, const void *elt))
{
	u64 start = 0, end = num, mid = 0;
	int result;

	while (start < end) {
		mid = start + (end - start) / 2;

		result = cmp(key, base + mid * size);
		if (result < 0)
			end = mid;
		else if (result > 0)
			start = mid + 1;
		else {
            *ret_ndx = mid; /*index of entry*/
			return FOUND;
        }
	}
    /*search aborted, the element should have been either 
      immediately before or immediately after this one*/
    *ret_ndx = mid;
	return NOT_FOUND;
}
 
static __always_inline int chunk_lookup(
    u64 *ret_ndx, 
    struct cfsd_inode_chunk const * const c,
    u64 num_elems,
    struct cfsd_ientry const * const search_key)
{
    /*wraps the actual search and provides some type-checking*/
    return bsearch(
        ret_ndx, search_key, c, 
        num_elems, sizeof(struct cfsd_ientry), 
        ientry_cmp
    );
}
#endif

static int chunk_lookup(
    /*inspired by kernel's bsearch @ linux/bsearch.h */
    u64 *ret_ndx, 
    struct cfsd_inode_chunk const * const c, 
    struct cfsd_ientry const * const search_key)
{
    u64 start = 0, mid = 0; 
    u64 end = CHUNK_NUMENTRIES - c->hdr.entries_free;
    int result;

    while (start < end) {
        mid = start + (end-start) / 2;

        result = ientry_cmp(search_key, &c->entries[c->hdr.freelist[mid]]);
        if (result < 0) {
            end = mid;
        } else if (result > 0) {
            start = mid + 1;
        } else {
            *ret_ndx = c->hdr.freelist[mid]; /*index of entry*/
            return FOUND;
        }
    }
    /*search aborted*/
	return NOT_FOUND;
}
                                           

/** 
 *  Find entry with name given by 'ientry_d' inside parent
 *  directory given by inode table 'parent_tbl'
 *  @param ret_buf an allocated buffer capable of holding a
 *                 single chunk.
 *  @param ret_loc on success; holds the location within the
 *                 parent's inode table where the ientry
 *                 matching 'search_dentry' was found.
 *  @param parent the parent inode in which to search for the
 *                entry.
 *  @param search_dentry holds the name of the entry to find
 *  @return 0(FOUND) on success; error otherwise.
 *      -ENOMEM => allocations failed
 *      -EIO => error while reading a chunk
 *      -1(NOT_FOUND) => element not present in parent directory
 *  @pre ref_buf points to an allocated buffer large enough to
 *       hold one chunk
 *  @post on success; *ret_buf holds the chunk in which the
 *        entry was found, *ret_entry points to the entry inside
 *        the chunk which matched the search key.
 */ 
int __must_check cfsc_ientry_find(
    struct cfsd_inode_chunk *ret_buf, struct ientry_loc *ret_loc, 
    struct cfs_inode *parent, struct dentry *search_dentry)
{
    struct cfsd_ientry search_key;          /*populated to function as the search key*/
    struct cfs_node_addr *itbl = NULL;      /*address of parent directory's inode table*/
    struct block_device *bd = NULL;         /*device holding the parent directory's inode table*/
    u64 off;
    int retval;
    
    CLYDE_ASSERT(ret_buf != NULL);
    CLYDE_ASSERT(ret_loc != NULL);
    CLYDE_ASSERT(parent != NULL);
    CLYDE_ASSERT(search_dentry != NULL);

    ret_loc->chunk_ndx = 0;
    ret_loc->chunk_off = 0;

    itbl = &parent->data;
    bd = parent->vfs_inode.i_sb->s_bdev;
    off = 0; 

    /*populate 'search_key' to make it searchable*/
    chunk_mk_key(&search_key, search_dentry);

read_chunk:
    spin_lock(&parent->io_lock);
    retval = cfsio_read_node_sync(
        bd, NULL, NULL,
        itbl->tid, itbl->nid, 
        off, 
        sizeof(struct cfsd_inode_chunk), 
        ret_buf
    );
    spin_unlock(&parent->io_lock);
    if (retval) {
        retval = -EIO;
        goto err_io;
    }

    ret_loc->chunk_off = 0;
    retval = chunk_lookup(&ret_loc->chunk_off, ret_buf, &search_key);
    if (retval == NOT_FOUND) {
        /*did not find the entry*/
        if(ret_buf->hdr.last_chunk) {
            goto out; /*will return NOT_FOUND*/
        } else { /*advance to next*/
            ret_loc->chunk_ndx++;
            off += sizeof(struct cfsd_ientry) + CHUNK_TAIL_SLACK_BYTES;
            goto read_chunk;
        }
    } else { /*found an entry!*/
        goto out;
    }

err_io: /*couldn't read chunk*/
    CFS_WARN("Failed while reading an inode table (trying to chunk[%llu], in node (%llu,%llu))\n", 
             off / (sizeof(struct cfsd_ientry)+CHUNK_TAIL_SLACK_BYTES), itbl->tid, itbl->nid);
out:
    return retval;
}

/** 
 * Make a new chunk in the inode table 
 * @param bd block device to write on 
 * @param itbl address of inode table node 
 * @param off offset within inode table 
 * @return 0 on success; error otherwise 
 * @post on success; a new, empty chunk with last_chunk set to 
 *       true is written from the beginning of 'off'
 * @pre has exclusive access to append a new chunk to inode's 
 *      itbl
 */
static int cfs_mk_chunk(struct block_device *bd, struct cfs_node_addr *itbl, u64 off)
{
    struct cfsd_inode_chunk *c = NULL;
    int retval;

    c = kmem_cache_zalloc(chunk_pool, GFP_KERNEL);
    if (!c) {
        retval = -ENOMEM;
        goto out;
    }
    cfsc_chunk_init_common(c);

    retval = cfsio_update_node_sync(
        bd, NULL, NULL, 
        itbl->tid, itbl->nid, off, 
        sizeof(struct cfsd_inode_chunk), c
    );
    if (retval) {
        retval = -EIO;
        goto err_write;
    }

    goto out; /*success*/
err_write:
    kmem_cache_free(chunk_pool, c);
out:
    return retval;
}

#if 0
/** 
 * Insert a new inode entry (corresponding to a file or 
 * directory) into the inode table of the parent directory. 
 * @param parent inode of parent directory, whose inode table 
 *               will receive the new entry defined by 'inode'
 *               and 'inode_d'
 * @param inode the metadata of the new inode entry 
 * @param inode_d the name of the new inode entry 
 * @pre parent's i_lock is locked 
 * @return 0 on sucess; error otherwise 
 *  -ENOMEM -- allocations failed
 *  -EIO -- error while reading chunks in parent inode table or
 *   persisting a chunk, existing or newly appended.
 */ 
int __ientry_insert(struct cfs_inode *parent, struct cfs_inode *inode, struct dentry *inode_d)
{
    struct cfsd_inode_chunk *chunk_curr = NULL;         /*ptr to chunk memory*/
    struct cfs_node_addr *itbl = NULL;                  /*reference to parent's inode table*/
    struct block_device *bd = NULL;                     /*block device to operate on*/
    u64 off;
    struct cfsd_ientry tmp_ientry;
    int retval;

    CLYDE_ASSERT(parent != NULL);
    CLYDE_ASSERT(inode != NULL);
    CLYDE_ASSERT(inode_d != NULL);
    CLYDE_ASSERT( spin_is_locked(&parent->vfs_inode.i_lock) );

    chunk_curr = kmem_cache_alloc(chunk_pool, GFP_KERNEL);
    if (!chunk_curr) {
        retval = -ENOMEM;
        goto err;
    }

    itbl = &parent->data;
    bd = parent->vfs_inode.i_sb->s_bdev;
    CLYDE_ASSERT(bd != NULL);
    off = CHUNK_LEAD_SLACK_BYTES;

    /*populate structure for writing*/
    cfs_ientry_init(&tmp_ientry, inode, inode_d);

read_chunk:
    retval = cfsio_read_node_sync(
        bd, NULL, NULL,
        itbl->tid, itbl->nid, 
        off, 
        sizeof(struct cfsd_inode_chunk), 
        chunk_curr
    );
    if (retval) {
        CLYDE_ERR("failed to read inode entry!\n");
        retval = -EIO;
        goto out;
    }

    if (chunk_curr->entries_used < CHUNK_NUMENTRIES) {
        /*space available in chunk*/
        chunk_curr->entries[chunk_curr->entries_used++] = tmp_ientry;
        
        sort(
            chunk_curr->entries, 
            chunk_curr->entries_used, 
            sizeof(struct cfsd_ientry), 
            ientry_cmp, NULL /*swap fnc => NULL, generic byte-by-byte swap used*/
        );

        if (chunk_curr->entries_used == CHUNK_NUMENTRIES) {
            /*last entry in chunk, append inode table with a new 
              chunk and mark this chunk as no longer being the last one*/
            chunk_curr->last_chunk = 0;
            retval = cfs_mk_chunk(bd,itbl,off);
            if (retval) {
                /*failed to append a new chunk to the now filled chunk, abort*/
                goto out;
            }
        }

        retval = cfsio_update_node_sync(
            bd,NULL,NULL,
            itbl->tid,itbl->nid,
            off, sizeof(struct cfsd_inode_chunk), 
            chunk_curr
        );
        if (retval) {
            CLYDE_ERR("Failed to write chunk after adding new inode entry\n");
            retval = -EIO;
            goto out;
        }
    } else {
        if (!chunk_curr->last_chunk) {
            goto read_chunk;
        }
    }

    retval = 0; /*fall through*/
out:
    kmem_cache_free(chunk_pool, chunk_curr);
err:
    return retval;
}
#endif

/** 
 * Wrapper for __ientry_insert 
 */
int cfsc_ientry_insert(struct cfs_inode *parent, struct cfs_inode *inode, struct dentry *inode_d)
{
    CLYDE_ASSERT( 1 == 2 ); //force bug
    #if 0
    int retval;
    CLYDE_ASSERT( !spin_is_locked(&parent->vfs_inode.i_lock) );

    spin_lock( &parent->vfs_inode.i_lock );
    retval = __ientry_insert(parent, inode, inode_d);
    spin_unlock( &parent->vfs_inode.i_lock );
    return retval;
    #endif
}

int cfsc_init(void)
{
    chunk_pool = kmem_cache_create(
        "chunk_pool",
		sizeof(struct cfsd_ientry),
        0,
        /*objects are reclaimable*/
		SLAB_RECLAIM_ACCOUNT 
        /*spread allocation across memory rather than favouring memory local to current cpu*/
         | SLAB_MEM_SPREAD,  
        NULL
    );

	if (!chunk_pool)
		return -ENOMEM;
	return 0;
}

void cfsc_exit(void)
{
    kmem_cache_destroy(chunk_pool);
}
