#include "clydefs.h"
#include "chunk.h"
#include "io.h"
#include "inode.h"

static struct kmem_cache *chunk_pool = NULL;

static __always_inline int __write_chunk_sync(struct block_device *bd, u64 tid, u64 nid, struct cfsd_inode_chunk *c, int chunk_off) 
{
    return cfsio_update_node_sync(
        bd, NULL, NULL, tid, nid, 
        chunk_off * CHUNK_SIZE_DISK_BYTES, CHUNK_SIZE_BYTES, c
    );
}

static __always_inline int __read_chunk_sync(struct block_device *bd, u64 tid, u64 nid, struct cfsd_inode_chunk *c, int chunk_off)
{
    return cfsio_read_node_sync(
        bd, NULL, NULL, 
        tid, nid, chunk_off * CHUNK_SIZE_DISK_BYTES, 
        CHUNK_SIZE_BYTES, c
    );
}

static __always_inline u64 __entry_offset(u64 entry_ndx)
{
    return offsetof(struct cfsd_inode_chunk, entries) + sizeof(struct cfsd_ientry) * entry_ndx;
}

/** 
 * Write chunk header 
 * @param c the chunk whose header is to be written to disk 
 * @param itbl address of the node in which the inode table is 
 *             located.
 * @param chunk_off chunk offset of the chunk whose header is to 
 *                  be overridden. E.g. 0 => lead chunk
*/ 
static __always_inline int __write_chunk_hdr_sync(
    struct block_device *bd,
    struct cfsd_inode_chunk *c,
    struct cfs_node_addr *itbl,
    u64 chunk_off)
{
    return cfsio_update_node_sync(
        bd, NULL, NULL, 
        itbl->tid, itbl->nid, 
        /*calculate the offset within the chunk marking the beginning of the cfsd_chunk_hdr*/
        (chunk_off * CHUNK_SIZE_DISK_BYTES) + (CHUNK_SIZE_BYTES - sizeof(struct cfsd_chunk_hdr)), 
        sizeof(struct cfsd_chunk_hdr),
        &c->hdr
    );
}

static __always_inline void __offlist_init(struct cfsd_inode_chunk *c)
{
    int i;
    u8 *off_list;
    CLYDE_ASSERT(c != NULL);
    off_list = c->hdr.off_list;
    for (i=0; i<CHUNK_NUMENTRIES; i++) {
        off_list[i] = OFFSET_UNUSED;
    }
}

static __always_inline void __offlist_entry_free(struct cfsd_inode_chunk *c, u8 ndx)
{
    c->hdr.off_list[ndx] = OFFSET_UNUSED;
}

/* 
CHUNK FREELIST CODE 
====================================================================================== 
*/ 
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
                    freelist[i] &= ~sv; /*reserve it (swap pattern to ensure we write a zero @ the spot to reserve)*/
                    /*each b holds 8 entries, hence i*8, j is the index of the entry in the byte*/
                    *ret_ndx = (i*8)+j;
                    return 0;
                }
                /*try next index instead*/
                sv = 1U << ++j;
            }
            CFS_DBG("Determined a free entry was in a byte, yet couldn't find it!\n");
            BUG();
        } /*nothing found, next byte*/
    }
    CFS_DBG("entries_free indicated that there should at least be an empty entry, yet we didn't find it!\n");
    BUG();
}

/** 
 * Marks ientry as being free. 
 * @param c the chunk in which the entry resides 
 * @param ndx the entry to free 
 * @pre inode itbl write lock held 
 */
static __always_inline void __flist_entry_free(struct cfsd_inode_chunk *c, u8 ndx)
{ /*free the entry by zeroing out the bit @ the ndx'th entry in the freelist */ 
    u8 tmp = ndx % 8;
    CLYDE_ASSERT(c != NULL);
    CLYDE_ASSERT(ndx < CHUNK_NUMENTRIES);
    CFS_DBG("ndx(%u) => tmp(%u)\n", ndx, tmp);
    tmp = (1U << (tmp)); /*byte mask*/
    CFS_DBG("tmp bm: %u -- applying to byte(%u)\n",tmp, ndx/8);
    c->hdr.freelist[ndx/8] |= tmp; /*apply mask to clear bit*/
}

/* 
SEARCH CODE 
====================================================================================== 
*/ 
static int ientry_cmp(struct cfsd_ientry const *v1, struct cfsd_ientry const *v2)
{
    /* comparator for inode entry bsearch & sort
        - neg iff e1 precedes e2
        - pos iff e2 precedes e1
        - 0 iff e1 == e2
    */
    return strcmp(v1->name, v2->name);
}

/**
 * @pre dentry name len is at most CFS_NAME_LEN long.
 */
static __always_inline void chunk_mk_key(struct cfsd_ientry *search_key, struct dentry const * const d)
{
    search_key->nlen = d->d_name.len;
    strcpy(search_key->name, d->d_name.name);
}

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
        
        result = ientry_cmp(search_key, &c->entries[c->hdr.off_list[mid]]);
        if (result < 0) {
            end = mid;
        } else if (result > 0) {
            start = mid + 1;
        } else {
            *ret_ndx = c->hdr.off_list[mid]; /*index of entry*/
            return FOUND;
        }
    }
    /*search aborted*/
	return NOT_FOUND;
}

/* 
SORT CODE 
====================================================================================== 
*/ 
static void offset_swap(void *a, void *b)
{ /*adopted from u32_swap in the kernel*/
	u8 t = *(u8 *)a;
	*(u8 *)a = *(u8 *)b;
	*(u8 *)b = t;
}
static int ientry_cmp_sort(struct cfsd_ientry *entry_base, void const *e1_off, void const *e2_off)
{
    struct cfsd_ientry const *v1 = entry_base + (*(u8*)e1_off);
    struct cfsd_ientry const *v2 = entry_base + (*(u8*)e2_off);

    return strcmp(v1->name, v2->name);
}

static void hsort(void *base, size_t num, size_t size,
	  struct cfsd_ientry *entry_base)
      /*essentially from include/sort.h - had to modify comparison function 
        to work off a different offset*/
{
	/* pre-scale counters for performance */
	int i = (num/2 - 1) * size, n = num * size, c, r;

	/* heapify */
	for ( ; i >= 0; i -= size) {
		for (r = i; r * 2 + size < n; r  = c) {
			c = r * 2 + size;
			if (c < n - size &&
					ientry_cmp_sort(entry_base, base + c, base + c + size) < 0)
				c += size;
			if (ientry_cmp_sort(entry_base, base + r, base + c) >= 0)
				break;
            offset_swap(base + r, base + c);
		}
	}

	/* sort */
	for (i = n - size; i > 0; i -= size) {
		offset_swap(base, base + i);
		for (r = 0; r * 2 + size < i; r = c) {
			c = r * 2 + size;
			if (c < i - size &&
					ientry_cmp_sort(entry_base, base + c, base + c + size) < 0)
				c += size;
			if (ientry_cmp_sort(entry_base, base + r, base + c) >= 0)
				break;
			offset_swap(base + r, base + c);
		}
	}
}

/* 
CHUNK INTERFACE CODE 
====================================================================================== 
*/ 
/**
 * Allocate a new chunk 
 * @return NULL on failure, otherwise a zero'ed chunk. 
 */
void *cfsc_chunk_alloc()
{
    return kmem_cache_zalloc(chunk_pool, GFP_ATOMIC);
}

/** 
 * Sets chunk values that of a completely empty tail-end chunk 
 * @param c chunk to modify 
 * @param entire chunk has been zero'ed out 
 */ 
void cfsc_chunk_init(struct cfsd_inode_chunk *c)
{
    CLYDE_ASSERT(c != NULL);
    c->hdr.entries_free = CHUNK_NUMENTRIES;
    c->hdr.last_chunk = 1;
    __flist_init(c); /*set each bit to 1 to mark empty slots*/
    __offlist_init(c); /*set each offset value such that it is marked unused*/
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
 * Sort chunk inode entries.
 * @param c the chunk whose inode entries to sort. 
 */
void cfsc_chunk_sort(struct cfsd_inode_chunk *c)
{
    CLYDE_ASSERT(c != NULL);
    hsort(
        /*sort off_list, which is a list of u8 offsets into c->entries*/
        c->hdr.off_list, 
        CHUNK_NUM_ITEMS(c), 
        sizeof(u8),
        /*comparisons, the basis of the sort, are made by using the offset 
          values to compute the actual ientry location and doing a str
          comparison on their names*/ 
        c->entries
    );
}

/**
 * Finds an empty slot in the chunk and inserts the entry. 
 * @param ret_ndx on success; holds the offset within the chunk 
 *                into which the ientry was inserted.
 * @param c the chunk into which the entry is inserted
 * @param e the entry to insert 
 * @return 0 on success; -1 if the chunk is full 
 * @note you may have to sort the chunk entries 
 * @pre should have exclusive access to the chunk 
 */
int cfsc_chunk_entry_insert(u64 *ret_ndx, struct cfsd_inode_chunk *c, struct cfsd_ientry const *e)
{
    int retval = 0;
    if ((retval=__flist_entry_alloc(ret_ndx, c))) {
        CFS_DBG("__flist_entry_alloc err, retval: %d\n", retval);
        goto out;
    }

    c->entries[*ret_ndx] = *e;
    CLYDE_ASSERT(*ret_ndx <= 255U);
    c->hdr.off_list[CHUNK_NUM_ITEMS(c)] = (u8)*ret_ndx;
    c->hdr.entries_free--;
out:
    return retval;
}

/** 
 * Delete entry from chunk and update administrative fields to 
 * reflect it. 
 * @param c the chunk in which to remove the element 
 * @param entry_ndx the offset of the ientry to remove within 
 *                  the chunk.
 * @pre should have exclusive access to the chunk 
 * @note remember to sort after deletions 
 */ 
void cfsc_chunk_entry_delete(struct cfsd_inode_chunk *c, u8 entry_ndx)
{
    __flist_entry_free(c, entry_ndx);
    __offlist_entry_free(c, entry_ndx);
    c->hdr.entries_free++;
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
 *  @note acquires parent's write locks while reading chunks in.
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
    CFS_DBG(
        "called parent{ino:%lu, name:%s, itbl_nid:%llu} search_dentry{%s}\n", 
        parent->vfs_inode.i_ino, parent->itbl_dentry->d_name.name,
        parent->data.nid, search_dentry->d_name.name
    );
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
    cfsi_i_wlock(parent);
    retval = cfsio_read_node_sync(
        bd, NULL, NULL,
        itbl->tid, itbl->nid, 
        off, 
        sizeof(struct cfsd_inode_chunk), 
        ret_buf
    );
    cfsi_i_wunlock(parent);
    if (retval) {
        retval = -EIO;
        goto err_io;
    }
    if (itbl->nid == 1 && parent->vfs_inode.i_ino == CFS_INO_ROOT){
        CFS_DBG(
            "Read a chunk of the root itbl {tid:%llu,nid:%llu} - chunk_hdr{entries_free:%u, last_chunk:%u}\n", 
            itbl->tid, itbl->nid, ret_buf->hdr.entries_free, ret_buf->hdr.last_chunk
        );
    }

    ret_loc->chunk_off = 0; /*chunk_lookup requires this to be set to 0*/
    retval = chunk_lookup(&ret_loc->chunk_off, ret_buf, &search_key);
    if (retval == NOT_FOUND) {
        /*did not find the entry*/
        if(ret_buf->hdr.last_chunk) {
            CFS_DBG("could not find entry, searched %llu chunks\n", ret_loc->chunk_ndx);
            goto out; /*will return NOT_FOUND*/
        } else { /*advance to next*/
            CFS_DBG("entry not in this chunk, advancing to next\n");
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
{ /*FIXME unused*/
    struct cfsd_inode_chunk *c = NULL;
    int retval;

    c = cfsc_chunk_alloc();
    if (!c) {
        retval = -ENOMEM;
        goto out;
    }
    cfsc_chunk_init(c);

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
    cfsc_chunk_free(c);
out:
    return retval;
}

/**Fully populate an ientry for in preparation for writing it 
 * to disk based on the inode's own data and referenced dentry.
 * @param dst the ientry to populate 
 * @param src the inode from which to source metadata 
 * @param  
 * @return 0 on success, -ENAMETOOLONG if the name exceeds the 
 *         filesystem maximum name length.
 * @pre src is a fully initialised inode with an associated 
 *      dentry.
 */ 
static __always_inline int cfs_ientry_init(
    struct cfsd_ientry *dst, 
    struct cfs_inode const * const src,
    struct dentry const * const src_d)
{
    struct inode const *i = NULL;

    CLYDE_ASSERT(dst != NULL);
    CLYDE_ASSERT(src != NULL);
    CLYDE_ASSERT(src_d != NULL);

    i = &src->vfs_inode;
    CLYDE_ASSERT(i != NULL);

    /*handle metadata*/
    __copy2d_inode(dst,src);
    if (src_d->d_name.len > CFS_NAME_LEN) {
        return -ENAMETOOLONG;
    }
    strcpy(dst->name, src_d->d_name.name);
    dst->nlen = cpu_to_le16((u16)src_d->d_name.len);
    CFS_DBG("ientry namecopy: src{d_name.name:%s, len:%d} => dst{name:%s}\n", src_d->d_name.name, src_d->d_name.len, dst->name);
    return 0; /*success*/
}

/** 
 * Insert a new inode entry (corresponding to a file or 
 * directory) into the inode table of the parent directory. 
 * @param parent inode of parent directory, whose inode table 
 *               will receive the new entry defined by 'inode'
 *               and 'inode_d'
 * @param inode the metadata of the new inode entry 
 * @param inode_d the name of the new inode entry 
 * @pre NO LOCKS TAKEN 
 * @post on success; on_disk=1 and dsk_ientry_loc set 
 * @return 0 on sucess; error otherwise 
 *  -ENOMEM -- allocations failed
 *  -EIO -- error while reading chunks in parent inode table or
 *   persisting a chunk, existing or newly appended.
 */
int cfsc_ientry_insert(struct cfs_inode *parent, struct cfs_inode *inode, struct dentry *inode_d)
{
    struct cfsd_inode_chunk *chunk_curr = NULL;         /*ptr to chunk memory*/
    struct cfs_node_addr *itbl = NULL;                  /*reference to parent's inode table*/
    struct block_device *bd = NULL;                     /*block device to operate on*/
    u64 off, ientry_ndx=0;
    struct cfsd_ientry tmp_ientry;
    int retval;

    CFS_DBG("called...\n");
    CLYDE_ASSERT(parent != NULL);
    CLYDE_ASSERT(inode != NULL);
    CLYDE_ASSERT(inode_d != NULL);

    chunk_curr = cfsc_chunk_alloc();
    if (!chunk_curr) {
        retval = -ENOMEM;
        goto err;
    }
    
    itbl = &parent->data;
    bd = parent->vfs_inode.i_sb->s_bdev;
    CLYDE_ASSERT(bd != NULL);
    off = 0;

    /*populate structure for writing*/
    cfs_ientry_init(&tmp_ientry, inode, inode_d);
    cfsi_i_wlock(parent);

read_chunk:
    retval = cfsio_read_node_sync(
        bd, NULL, NULL,
        itbl->tid, itbl->nid, 
        off, 
        CHUNK_SIZE_BYTES, 
        chunk_curr
    );
    if (retval) {
        CLYDE_ERR("failed to read itbl chunk!\n");
        retval = -EIO;
        goto out;
    }

    if (chunk_curr->hdr.entries_free) {
        /*space available in chunk*/
        if (cfsc_chunk_entry_insert(&ientry_ndx, chunk_curr, &tmp_ientry)) {
            CFS_DBG("entries_free wasn't 0 at the time of checking, this code should be protected against race conditions\n");
            BUG();
        }
        cfsc_chunk_sort(chunk_curr);

        if (chunk_curr->hdr.entries_free == 0) {
            /*last entry in chunk, append inode table with a new 
              chunk and mark this chunk as no longer being the last one*/
            retval = cfs_mk_chunk(bd,itbl, off + CHUNK_SIZE_DISK_BYTES);
            if (retval) {
                /*failed to append a new chunk to the now filled chunk, abort*/
                goto out;
            }

            chunk_curr->hdr.last_chunk = 0;
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

        /*SUCCESS: update inode to reflect its disk location*/
        inode->dsk_ientry_loc.chunk_ndx = off;
        inode->dsk_ientry_loc.chunk_off = ientry_ndx;
        smp_mb();
        inode->on_disk = 1;
    } else {
        if (!chunk_curr->hdr.last_chunk) {
            off += CHUNK_SIZE_DISK_BYTES;
            goto read_chunk;
        }
    }

    retval = 0; /*fall through*/
out:
    cfsi_i_wunlock(parent);
    kmem_cache_free(chunk_pool, chunk_curr);
err:
    return retval;
}

/** 
 * Update an existing entry for inode 'ci' in the inode table of
 * 'parent'. 
 * @param parent the parent directory of ci 
 * @param ci the entry to be updated. 
 * @return 0 on success; 
 * -ENOMEM if allocations fail; 
 * -EIO if IO errors occur; 
 * -ENAMETOOLONG if ci's associated dentry name is too long 
 */ 
int cfsc_ientry_update(struct cfs_inode *parent, struct cfs_inode *ci)
{
    struct cfsd_inode_chunk *c = NULL;
    struct cfsd_ientry *entry = NULL;
    struct block_device *bd = NULL;

    int retval = 0;

    CLYDE_ASSERT(parent != NULL);
    /*parent must have an itbl*/
    CLYDE_ASSERT(parent->vfs_inode.i_mode & S_IFDIR);
    CLYDE_ASSERT(ci != NULL);
    /*require inode to be associated to its dentry*/
    CLYDE_ASSERT(ci->itbl_dentry != NULL);
    /*require inodes to be fully initialised*/
    CLYDE_ASSERT(ci->status != IS_UNINITIALISED);
    /*can only update an entry already on disk, after all*/
    CLYDE_ASSERT(ci->on_disk);

    bd = parent->vfs_inode.i_sb->s_bdev;

    c = cfsc_chunk_alloc();
    if (!c) {
        CFS_DBG("Failed to allocate chunk\n");
        retval = -ENOMEM;
        goto err_alloc;
    }

    cfsi_i_wlock(parent);
    retval = __read_chunk_sync(
        bd, ci->data.tid, ci->data.nid, 
        c, ci->dsk_ientry_loc.chunk_off
    );
    if (retval) {
        CFS_DBG("Failed to read the specified chunk in which the entry resides\n");
        retval = -EIO;
        goto err_io;
    }

    /*Update ientry*/
    entry = &c->entries[ci->dsk_ientry_loc.chunk_off];
    spin_lock(&ci->vfs_inode.i_lock);
    __copy2d_inode(entry, ci);
    if (ci->sort_on_update) {
        struct dentry *d = ci->itbl_dentry;

        /*changes requiring re-sorting elements can only be changing the inode name*/
        if (d->d_name.len > CFS_NAME_LEN) {
            return -ENAMETOOLONG;
        }
        strcpy(entry->name, d->d_name.name);
        entry->nlen = cpu_to_le16((u16)d->d_name.len);

        cfsc_chunk_sort(c);
    }
    spin_unlock(&ci->vfs_inode.i_lock);

    /*write the updated entry back to disk*/
    retval = cfsio_update_node(
        bd, NULL, NULL, 
        ci->data.tid, ci->data.nid, 
        __entry_offset(ci->dsk_ientry_loc.chunk_off), 
        sizeof(entry), 
        entry
    );
    if (retval) {
        CFS_DBG("Failed to write entry down to chunk\n");
        retval = -EIO;
        goto err_io;
    }
    /*write the chunk hdr back to disk*/
    retval = __write_chunk_hdr_sync(bd, c, &ci->data, ci->dsk_ientry_loc.chunk_off);
    if (retval) {
        CFS_DBG("Failed to chunk hdr after changing its contents\n");
        BUG();
    }
    /*success, fall through*/
err_io:
    cfsi_i_wunlock(parent);
err_alloc:
    return retval;
}

int cfsc_ientry_delete(struct cfs_inode *parent, struct cfs_inode *ci)
{
    struct cfsd_inode_chunk *c = NULL;
    struct block_device *bd = NULL;

    int retval = 0;

    CLYDE_ASSERT(parent != NULL);
    /*parent must have an itbl*/
    CLYDE_ASSERT(parent->vfs_inode.i_mode & S_IFDIR);
    CLYDE_ASSERT(ci != NULL);
    /*require inode to be associated to its dentry*/
    CLYDE_ASSERT(ci->itbl_dentry != NULL);
    /*require inodes to be fully initialised*/
    CLYDE_ASSERT(ci->status != IS_UNINITIALISED);
    /*can only update an entry already on disk, after all*/
    CLYDE_ASSERT(ci->on_disk);

    bd = parent->vfs_inode.i_sb->s_bdev;

    c = cfsc_chunk_alloc();
    if (!c) {
        CFS_DBG("Failed to allocate chunk\n");
        retval = -ENOMEM;
        goto err_alloc;
    }

    cfsi_i_wlock(parent);
    retval = __read_chunk_sync(
        bd, ci->data.tid, ci->data.nid, 
        c, ci->dsk_ientry_loc.chunk_off
    );
    if (retval) {
        CFS_DBG("Failed to read the specified chunk in which the entry resides\n");
        retval = -EIO;
        goto err_io;
    }

    /*remove ientry*/
    CLYDE_ASSERT( ((u8)255U) >= (u8)ci->dsk_ientry_loc.chunk_off); /*FIXME remove*/
    cfsc_chunk_entry_delete(c, ci->dsk_ientry_loc.chunk_off);
    cfsc_chunk_sort(c);

    /*write the chunk hdr back to disk*/
    retval = __write_chunk_hdr_sync(bd, c, &ci->data, ci->dsk_ientry_loc.chunk_off);
    if (retval) {
        CFS_DBG("Failed to chunk hdr after changing its contents\n");
        BUG();
    }
    /*success, fall through*/
err_io:
    cfsi_i_wunlock(parent);
err_alloc:
    return retval;
}

/* 
Wrappers & helpers
====================================================================================== 
*/
/**
 * Allocate node for an inode table.
 * @post on success; ret_itbl_nid will be set to the node id of the newly allocated node
 * @return 0 on success, negative on io errors 
 */
int cfsc_mk_itbl_node(u64 *ret_itbl_nid, struct block_device *bd, u64 tid)
{
    return cfsio_insert_node_sync(
        bd, ret_itbl_nid, tid, CHUNK_SIZE_DISK_BYTES
    );
}

int cfsc_write_chunk_sync(struct block_device *bd, u64 tid, u64 nid, struct cfsd_inode_chunk *c, int chunk_off) 
{
    return __write_chunk_sync(bd, tid, nid, c, chunk_off);
}

int cfsc_read_chunk_sync(struct block_device *bd, u64 tid, u64 nid, struct cfsd_inode_chunk *c, int chunk_off)
{
    return __read_chunk_sync(bd, tid, nid, c, chunk_off);
}

int cfsc_init(void)
{
    chunk_pool = kmem_cache_create(
        "chunk_pool",
		sizeof(struct cfsd_inode_chunk),
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
