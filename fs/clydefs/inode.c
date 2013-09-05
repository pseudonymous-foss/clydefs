#include <linux/fs.h>
#include <linux/sort.h>
#include <linux/time.h>
#include "clydefs.h"
#include "clydefs_disk.h"
#include "inode.h"
#include "io.h"
#include "super.h"

/*
    OUTSTANDING
    .create
        I'm marking the inode dirty yet I haven't yet made its ientry.
            (can branch on on_disk, which is 0 on a newly minted inode to
            then persist it to disk)
    - ERRORI markers, all point to an error relating to code in its immediate surroundings
 
    - I'm passing along a NULL parent (and a NULL ientry_loc) for cfs_inode_init in cfsi_getroot
        pass an entry_loc which fits where the inode ientry should be in fs itbl and NULL work
            (and **CHECK** for NULL when writing inode to disk) -- also guard code calling cfs_inode_init
                such that ONLY getroot will pass null)
*/

const struct inode_operations cfs_dir_inode_ops;
const struct inode_operations cfs_file_inode_ops;
extern const struct file_operations cfs_file_ops;
extern const struct file_operations cfs_dir_file_ops;
extern const struct address_space_operations cfs_aops;

static struct kmem_cache *chunk_pool = NULL;

enum CHUNK_LOOKUP_RES { FOUND = 0, NOT_FOUND = 1 };

/** 
 * Increment inode usage count.
 * @pre i->i_lock must be held 
 * @note fetched from fs/inode.c __iget, which isn't an exported 
 *       symbol
 */ 
static __always_inline void cfs_iref_get(struct cfs_inode *ci)
{
	atomic_inc(&ci->vfs_inode.i_count);
}

/** 
 * Decrement inode usage count. 
 */ 
static __always_inline void cfs_iref_put(struct cfs_inode *ci)
{
    iput(&ci->vfs_inode);
}

/** 
 * Return the inode number of an ientry formatted to CPU 
 * endianess. 
 * @return ientry inode number (ino) 
 */ 
static __always_inline u64 ientry_ino(struct cfsd_ientry const * const e)
{
    return le64_to_cpu(e->ino);
}

/** 
 * Advance the inode tbl offset. (handles wrap-arounds) 
 * @param off the offset to advance by one 
 * @return the offset, advanced by one 
 */ 
static __always_inline u64 ino_offset_adv(u64 off)
{
    u64 nxt = off + 1;
    if (unlikely(nxt == RECLAIM_INO_MAX))
        nxt = 0;
    return nxt;
}

/** 
 * Retreat the inode tbl offset. (handles wrap-arounds) 
 * @param off the offset to retreat by one 
 * @return the offset, retreated by one 
*/ 
static __always_inline u64 ino_offset_rtr(u64 off)
{
    if (unlikely(off == 0)) {
        return (RECLAIM_INO_MAX-1);
    } else {
        return off-1;
    }
}

/** 
 * Check if ino table is full.
 * @param cfs_sb the cfs-specific part of the filesystem's 
 *               superblock.
 * @return true if inode table is full, false otherwise 
 */ 
static __always_inline int ino_tbl_full(struct cfs_sb *cfs_sb)
{
    /* if advancing the end offset by one means pointing to the
       same place as the start offset, the table is full. */
    return (cfs_sb->ino_tbl_start == ino_offset_adv(cfs_sb->ino_tbl_end));
}

/** 
 * Check if ino table is empty.
 * @param cfs_sb the cfs-specific part of the filesystem's 
 *               superblock.
 * @return true if inode table is empty, false otherwise 
 */ 
static __always_inline int ino_tbl_empty(struct cfs_sb *cfs_sb)
{
    /*if the start offset and end offsets are the same, the 
      table is empty*/
    return (cfs_sb->ino_tbl_start == cfs_sb->ino_tbl_end);
}

/** 
 * Releases a previously reserved ino, allowing new inodes to 
 * take the value instead. 
 * @param sb the filesystem superblock 
 * @param ino the inode number (ino) to release 
 * @return 0 on success, 
 *  -ENOSPC on failure in case the fs can store no further 
 *  reclaimable ino's
 *  -EIO should writing to the inode table fail.
 */ 
static int cfs_ino_release(struct super_block *sb, u64 ino)
{
    struct cfs_sb *cfs_sb = NULL;
    struct cfs_node_addr *ino_tbl = NULL;
    int retval = 0;

    CLYDE_ASSERT(sb != NULL);
    CLYDE_ASSERT(ino >= CFS_INO_MIN);
    CLYDE_ASSERT(ino <= CFS_INO_MAX);

    cfs_sb = CFS_SB(sb);
    ino_tbl = &cfs_sb->fs_ino_tbl;
    
    spin_lock(&cfs_sb->lock_fs_ino_tbl);
    if (unlikely(ino_tbl_full(cfs_sb))) {
        CFS_WARN("Cannot reclaim inode number (%llu) - ino tbl is full - ino leak!\n", ino);
        retval = -ENOSPC;
        goto err_tbl_full;
    }
    /*obvious speed-up here is to batch up releases in a wait queue 
      and acquire ino's from this queue as well*/
    retval = cfsio_update_node_sync(
        sb->s_bdev, NULL, NULL, 
        ino_tbl->tid, ino_tbl->nid, 
        sizeof(u64)*cfs_sb->ino_tbl_end, sizeof(u64), 
        cfs_sb->ino_buf
    );

    if (retval) {
        retval = -EIO;
        goto err_io;
    }

    cfs_sb->ino_tbl_end = ino_offset_adv(cfs_sb->ino_tbl_end);
    goto out;

err_io:
err_tbl_full:
out:
    spin_unlock(&cfs_sb->lock_fs_ino_tbl);
    return retval;
}

/** 
 * Retrieve a free inode number. 
 * @param sb the file system superblock 
 * @return a new, reserved ino
 * @note if the inode number is not needed, either retain it for 
 *       later or release it properly.
 */ 
static u64 cfs_ino_nxt(struct super_block *sb)
{
    /* 
        First check if any reclaimable inode numbers exist,
        if so, get one of those, if not, get a new one by
        incrementing the counter 
    */
    struct cfs_sb *cfs_sb = NULL;
    struct cfs_node_addr *ino_tbl = NULL;
    int error = 0;
    u64 ret_ino;

    CLYDE_ASSERT(sb != NULL);
    cfs_sb = CFS_SB(sb);
    ino_tbl = &cfs_sb->fs_ino_tbl;

    spin_lock(&cfs_sb->lock_fs_ino_tbl);
    if (!ino_tbl_empty(cfs_sb)) {
        error = cfsio_read_node_sync(
            sb->s_bdev, NULL, NULL, 
            ino_tbl->tid, ino_tbl->nid, 
            cfs_sb->ino_tbl_start*sizeof(u64), sizeof(u64), 
            cfs_sb->ino_buf
        );
        if (unlikely(error)) {
            CFS_WARN("Failed to read ino tbl entry!\n");
            goto fresh_ino;
        }
        ret_ino = *((u64 *)cfs_sb->ino_buf);
        goto out;
    }
fresh_ino:
    /*get a new ino*/
    ret_ino = cfs_sb->ino_nxt_free++;

out:
    spin_unlock(&cfs_sb->lock_fs_ino_tbl);
    return ret_ino;
}

/** 
 * Populate cfs inode with values from a disk inode entry. 
 * @param dst the inode to populate 
 * @param src the inode entry read from disk 
 * @pre the i_lock associated the inode is locked 
 * @post dst will be populated with the values from the disk 
 *       inode entry, converted to the native CPU format.
 * @note does not read nlen & name. 
 * @note the tid of 'data' still need to be set 
 *       with the superblock values.
 */ 
static __always_inline void __copy2c_inode(struct cfs_inode *dst, struct cfsd_ientry const * const src)
{
    struct inode *vfs_i = NULL;
    CLYDE_ASSERT( dst != NULL );
    CLYDE_ASSERT( src != NULL );
    CLYDE_ASSERT( spin_is_locked(&dst->vfs_inode.i_lock) );

    vfs_i = &dst->vfs_inode;

    /*set regular inode fields*/
    vfs_i->i_ino = le64_to_cpu(src->ino);
    vfs_i->i_uid = le32_to_cpu(src->uid);
    vfs_i->i_gid = le32_to_cpu(src->gid);
    __copy2c_timespec(&vfs_i->i_ctime, &src->ctime);
    __copy2c_timespec(&vfs_i->i_mtime, &src->mtime);
    __copy2c_timespec(&vfs_i->i_atime, &src->mtime); /*we don't record access time*/
    vfs_i->i_size = le64_to_cpu(src->size_bytes);
    dst->data.nid = le64_to_cpu(src->data_nid);
    atomic_set(&vfs_i->i_count,le32_to_cpu(src->icount));
    /*nlen omitted, not represented in inode*/
    vfs_i->i_mode = le16_to_cpu(src->mode);
    /*name omitted, not represented in inode*/
}

/** 
 * Copies in-memory values into on-disk inode representation.
 * @param dst the on-disk representation to populate 
 * @param src the in-memory representation from which the values 
 *            are drawn.
 * @note cfs_inode parent addr should be set afterwards by 
 *       initiialization code
 */ 
static __always_inline void __copy2d_inode(struct cfsd_ientry *dst, struct cfs_inode const * const src)
{
    struct inode const *i = NULL;

    CLYDE_ASSERT(dst != NULL);
    CLYDE_ASSERT(src != NULL);
    i = &src->vfs_inode;

    /*NOTE: does not set nlen & name -- if persisting to an ientry, use */
    dst->ino = cpu_to_le64(i->i_ino);
    dst->uid = cpu_to_le32(i->i_uid);
    dst->gid = cpu_to_le32(i->i_gid);
    __copy2d_timespec(&dst->mtime, &i->i_mtime);
    __copy2d_timespec(&dst->ctime, &i->i_ctime);
    dst->size_bytes = cpu_to_le64(i->i_size);
    dst->data_nid = cpu_to_le64(src->data.nid);
    dst->icount = cpu_to_le32(atomic_read(&i->i_count));
    /*nlen  -- OMITTED*/
    dst->mode = cpu_to_le16(i->i_mode);
    /*name -- OMITTED*/
}

/** 
 *  Initialise inode with values from the inode entry disk
 *  representation and the file system superblock.
 *  @pre i_lock of inode 'ci' is held
 */
static __always_inline void cfs_inode_init(
    struct cfs_inode *parent, struct cfs_inode *ci, 
    struct cfsd_ientry const * const src,
    struct ientry_loc const * const loc
    )
{
    struct inode *i = NULL;

    CLYDE_ASSERT(ci != NULL);
    CLYDE_ASSERT( spin_is_locked(&ci->vfs_inode.i_lock) );
    CLYDE_ASSERT(src != NULL);
    CLYDE_ASSERT(loc != NULL);
    i = &ci->vfs_inode;

    /*handle the metadata stored in the ientry*/
    __copy2c_inode(ci, src);

    /*set tid, actual value depends on whether this is a directory or not*/
    ci->data.tid = CFS_DATA_TID(ci);

    /*set parent reference, and increment its usage count accordingly*/
    if(unlikely(parent == NULL)){
        /*only root node can excuse parent==NULL 
          and that we only allow if the sb root isn't set yet*/
        CLYDE_ASSERT(ci->vfs_inode.i_sb->s_root == NULL);
    } else {
        /*Associate with parent*/
        spin_lock(&parent->vfs_inode.i_lock);
        cfs_iref_get(parent);
        ci->parent = parent;
        spin_unlock(&parent->vfs_inode.i_lock);        
    }
    ci->on_disk = 1;
    ci->dsk_ientry_loc = *loc;
    
    /*set various constants*/
    i->i_blkbits = CFS_BLOCKSIZE_SHIFT;
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
    strncpy(dst->name, src_d->d_name.name, src_d->d_name.len);
    dst->nlen = cpu_to_le16((u16)src_d->d_name.len);

    return 0; /*success*/
}

static int ientry_cmp(void const *e1, void const *e2)
{
    /* comparator for inode entry bsearch & sort
        - neg iff e1 precedes e2
        - pos iff e2 precedes e1
        - 0 iff e1 == e2
    */
    struct cfsd_ientry const * const en1 = e1;
    struct cfsd_ientry const * const en2 = e2;
    return strcmp(en1->name, en2->name);
}

static __always_inline void chunk_mk_key(struct cfsd_ientry *search_key, struct dentry const * const d)
{
    search_key->nlen = d->d_name.len;
    strncpy(search_key->name, d->d_name.name, search_key->nlen); /*FIXME - */
}

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
static int __must_check cfs_ientry_find(
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

    itbl = &parent->data;
    bd = parent->vfs_inode.i_sb->s_bdev;
    /*each chunk has some slack space preceding it*/
    off = CHUNK_LEAD_SLACK_BYTES; 

    /*populate 'search_key' to make it searchable*/
    chunk_mk_key(&search_key, search_dentry);

read_chunk:
    retval = cfsio_read_node_sync(
        bd, NULL, NULL,
        itbl->tid, itbl->nid, 
        off, 
        sizeof(struct cfsd_inode_chunk), 
        ret_buf
    );

    if (retval) {
        retval = -EIO;
        goto err_io;
    }
    retval = chunk_lookup(&ret_loc->chunk_off, ret_buf, ret_buf->entries_used, &search_key);
    if (retval == NOT_FOUND) {
        /*did not find the entry*/
        if(ret_buf->last_chunk) {
            goto out; /*will return NOT_FOUND*/
        } else { /*advance to next*/
            ret_loc->chunk_ndx++;
            off += CHUNK_LEAD_SLACK_BYTES + sizeof(struct cfsd_ientry);
            goto read_chunk;
        }
    }

err_io: /*couldn't read chunk*/
    CFS_WARN("Failed while reading an inode table (trying to chunk[%llu], in node (%llu,%llu))\n", 
             off / (CHUNK_LEAD_SLACK_BYTES+sizeof(struct cfsd_ientry)), itbl->tid, itbl->nid);
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
    c->last_chunk = 1;

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

/** 
 * Wrapper for __ientry_insert 
 */ 
static __always_inline int cfs_ientry_insert(struct cfs_inode *parent, struct cfs_inode *inode, struct dentry *inode_d)
{
    int retval;
    CLYDE_ASSERT( !spin_is_locked(&parent->vfs_inode.i_lock) );

    spin_lock( &parent->vfs_inode.i_lock );
    retval = __ientry_insert(parent, inode, inode_d);
    spin_unlock( &parent->vfs_inode.i_lock );
    return retval;
}


/** 
 * Get inode from inode entry - will return an existing inode 
 * object if the inode is already referenced. 
 * @param sb the file system superblock. 
 * @param ientry the inode disk entry. 
 * @param parent_itbl address of parent inode's inode table 
 *                    wherein this inode's entry is.
 * @return on success; an inode matching the supplied inode 
 *         entry. Check IS_ERR(ret) to determine if an error
 *         occurred.
 * @post whether new or existing, inode object's reference count 
 *       is increased.
 */ 

/** 
 * Get or initialise inode, whichever comes first. 
 * @param dir the parent directory inode 
 * @param ientry the inode entry for which we'd like an inode 
 * @param loc location of the inode entry within the parent 
 *            inode table.
 * @post on failure: ERR_PTR(-ENOMEM) - otherwise a fully 
 *       initialised inode.
 */ 
static __always_inline struct inode *cfs_iget(
    struct cfs_inode *dir,
    struct cfsd_ientry const * const ientry,
    struct ientry_loc const * const loc
    )
{
    struct inode *i = NULL;
    struct cfs_inode *ci = NULL;
    u64 ino;

    CLYDE_ASSERT(dir != NULL);
    CLYDE_ASSERT(ientry != NULL);
    ino = ientry_ino(ientry);

    i = iget_locked(dir->vfs_inode.i_sb, le64_to_cpu(ientry->ino));
    if (!i) { /*no inode found, failed to allocate a new*/
        return ERR_PTR(-ENOMEM);
    } else if (!(i->i_state & I_NEW)) {
        /*existing inode object found, not newly allocated*/
        return i;
    }
    ci = CFS_INODE(i);

    /*not found in cache, freshly allocated object*/
    CFSI_LOCK(ci);
    cfs_inode_init(dir, ci, ientry, loc);
    CFSI_UNLOCK(ci);
    unlock_new_inode(i); /*all done, others may access the inode now*/
    return i;
}

/** 
 * Read root node from disk.
 * @param sb the file system superblock 
 * @return the root inode, or ERR_PTR of type -ENOMEM / -EIO
 */
struct inode *cfsi_getroot(struct super_block *sb)
{
    struct cfs_sb *cfs_sb = NULL;
    struct cfsd_inode_chunk *chunk_curr;
    struct cfsd_ientry *root_ientry;
    struct inode *root_inode = NULL;
    struct cfs_inode *root_cfs_i = NULL;
    struct cfs_node_addr *fs_inode_tbl = NULL;
    struct ientry_loc entry_loc;
    int errval;

    CLYDE_ASSERT(sb != NULL);
    cfs_sb = CFS_SB(sb);
    fs_inode_tbl = &cfs_sb->fs_inode_tbl;

    chunk_curr = kmem_cache_alloc(chunk_pool, GFP_KERNEL);
    if (!chunk_curr) {
        errval = -ENOMEM;
        goto err_alloc;
    }

    /*read the single inode entry in*/
    errval = cfsio_read_node_sync(
        sb->s_bdev, NULL, NULL,
        fs_inode_tbl->tid, fs_inode_tbl->nid, 
        CHUNK_LEAD_SLACK_BYTES, 
        sizeof(struct cfsd_ientry), 
        chunk_curr
    );

    if (errval) {
        errval = -EIO;
        goto err_io;
    }

    root_ientry = &chunk_curr->entries[0];
    CLYDE_ASSERT(root_ientry->ino == CFS_INO_ROOT);
    root_inode = iget_locked(sb, CFS_INO_ROOT);
    if (!root_inode) { /*no inode in cache, couldn't allocate one*/
        errval = -ENOMEM;
        goto err_io;
    } else if (!(root_inode->i_state & I_NEW)) {
        goto success;
    }

    /*we just created this inode (=> loading the FS)*/
    root_cfs_i = CFS_INODE(root_inode);
    CFSI_LOCK(root_cfs_i);
    cfs_inode_init(NULL, root_cfs_i, root_ientry, &entry_loc);
    CFSI_UNLOCK(root_cfs_i);

    root_inode->i_op = &cfs_dir_inode_ops;
    root_inode->i_fop = &cfs_dir_file_ops;
    root_inode->i_mapping->a_ops = &cfs_aops;

    /*doesn't actually release any locks, only purges 'I_NEW' flag from the inode state*/
    unlock_new_inode(root_inode);
    
    goto success;
success:
    kmem_cache_free(chunk_pool, chunk_curr);
    return root_inode;
err_io:
    kmem_cache_free(chunk_pool, chunk_curr);
err_alloc:
    return ERR_PTR(errval);
}

#if 0
static void __cfsi_inode_update_done(struct cfsio_rq_cb_data *req_data, void *endio_cb_data, int error)
{
    if (error) {
        CFS_DBG("cfsi_inode_update complete handler -- error detected\n");
    }
    /*endio_cb_data => chunk buffer*/
    kmem_cache_free(chunk_pool, endio_cb_data);
}

/** 
 * Update on-disk inode entry.
 * @param i inode to persist 
 * @pre i->parent_tbl initialised 
 * @pre entries represented in cfs_ientry are initialised
*/
int cfsi_inode_update(struct inode *i, int sync)
{
    /*todo have small variable to indicate if the inode is NEW, insert the inode if so ?*/
    struct super_block *sb = NULL;
    struct block_device *bd = NULL;
    struct cfsd_inode_chunk *chunk = NULL;
    struct dentry *search_dentry = NULL;
    struct cfs_inode *ci = NULL;

    int retval;

    CLYDE_ASSERT(i != NULL);
    sb = i->i_sb;
    bd = i->i_bdev;
    ci = CFS_INODE(i);
    /*to persist an inode entry we need the name it was saved under*/
    CLYDE_ASSERT(ci->ientry_dentry != NULL);

    chunk = kmem_cache_alloc(chunk_pool, GFP_KERNEL);
    if (!chunk) {
        CFS_DBG("failed to allocate chunk\n");
        retval = -ENOMEM;
        goto out;
    }
    cfs_ientry_find(chunk, &ret_ndx, cfsi_parent_ptr, &search_dentry);
    
    if (sync) {
        retval = cfsio_update_node_sync(bd,NULL,NULL, CFS_INODE_TID(CSB_SB(sb)), NID, off, len, chunk);
        
    } else {
        retval = cfsio_update_node(bd, cfsi_inode_update, chunk, CFS_INODE_TID(CSB_SB(sb)), NID, off, len, chunk);
    }

out:
    return retval;
}


/** 
 * Delete an inode from disk 
 * @param i inode to delete 
 * @param sync if set, handle deletion synchronously 
 */ 
int cfsi_inode_delete(struct inode *i, int sync)
{

}
#endif

/* 
========================================================================================== 
VFS operations and helpers 
*/ 

/** 
 * Add an inode entry for 'i' to directory 'dir' identified by 
 * name in 'i_d'. 
 * @pre i inode with a usage count of 1 or more. 
 */ 
static __always_inline int cfs_vfs_ientry_add(struct inode *dir, struct inode *i, struct dentry *i_d)
{
    int retval;
    retval = cfs_ientry_insert(CFS_INODE(dir), CFS_INODE(i), i_d);
    if (retval) {
        inode_dec_link_count(i);
        /*if i is a newly created inode, this render it eligible for reclamation*/
        cfs_iref_put(CFS_INODE(i));
        goto out;
    }
    d_instantiate(i_d, i); /*FIXME : ensure inode count has been incremented*/
out:
    return retval;
}

/** 
 * Make a new inode and write it to disk 
 * @param dir parent directory 
 * @param mode the inode's initial mode 
 * @return 0 on success; error otherwise 
 */ 
static struct inode *cfs_mk_inode(struct inode *dir, umode_t mode)
{
    struct super_block *sb = NULL;
    struct cfs_sb *csb = NULL;
    struct inode *i = NULL;
    struct cfs_inode *ci = NULL;

    CLYDE_ASSERT(dir != NULL);
    sb = dir->i_sb;
    csb = CFS_SB(sb);

    i = new_inode(sb);
    if (!i) {
        CLYDE_ERR("Failed to allocate new inode\n");
        i = ERR_PTR(-ENOMEM);
        goto out;
    }
    ci = CFS_INODE(i);

    inode_init_owner(i,dir,mode);
    i->i_ino = cfs_ino_nxt(sb);
    i->i_blkbits = CFS_BLOCKSIZE_SHIFT;
    i->i_ctime = i->i_mtime = i->i_atime = CURRENT_TIME;
    i->i_size = 0;
    /*assign parent (and increment parent's usage to reflect its use)*/
    spin_lock(&dir->i_lock);
    cfs_iref_get(CFS_INODE(dir));
    ci->parent = CFS_INODE(dir);
    spin_unlock(&dir->i_lock);
    insert_inode_hash(i); /*hash inode for lookup, based on sb and ino*/

    /*FIXME make the inode's entry on disk*/
    mark_inode_dirty(i); /*inode is (obviously) dirty, write it to disk*/
    cfssup_sb_inc_generation(csb); /*we've taken an ino, make sb generation reflect this*/
    
out:
    return i;
}

/* DIRECTORY INODE OPERATIONS */

/** 
 * Create empty inode table for newly initialised directory 
 * @pre 'ret_itbl' is a node address structure ready to be 
 *      overwritten
 * @post on success; 'ret_itbl' points to the new inode table 
 *       with the empty, initialised chunk
 * @return 0 on success; error otherwise 
 * @param sb the superblock 
 */ 
static int __mkdir_mkitbl(struct cfs_node_addr *ret_itbl, struct super_block *sb)
{
    struct cfs_sb *csb = NULL;
    struct cfsd_inode_chunk *c = NULL;
    struct block_device *bd;
    int retval;

    CLYDE_ASSERT(sb != NULL);
    csb = CFS_SB(sb);
    bd = sb->s_bdev;

    c = kmem_cache_zalloc(chunk_pool, GFP_KERNEL);
    if (!c) {
        retval = -ENOMEM;
        goto out;
    }
    ret_itbl->tid = CFS_INODE_TID(csb);
    retval = cfsio_insert_node_sync(
        bd, &ret_itbl->nid, ret_itbl->tid, 
        CHUNK_LEAD_SLACK_BYTES + sizeof(struct cfsd_inode_chunk)
    );
    if (retval) {
        goto err_node_ins;
    }

    c->last_chunk = 1;

    retval = cfsio_update_node_sync(
        bd, NULL, NULL, 
        ret_itbl->tid, ret_itbl->nid, 
        CHUNK_LEAD_SLACK_BYTES, 
        sizeof(struct cfsd_inode_chunk), c
    );
    if (retval) {
        goto err_node_write;
    }

    goto out; /*success*/

err_node_write:
    if (cfsio_remove_node_sync(bd, ret_itbl->tid, ret_itbl->nid))
        CFS_WARN("Failed to undo changes, tried to remove node (tid:%llu,nid:%llu)\n", 
                 ret_itbl->tid, ret_itbl->nid);
err_node_ins:
    kmem_cache_free(chunk_pool, c);
out:
    return retval;
}

/** 
 * Create a new inode entry for a regular file in parent 
 * directory 'dir', identified by dentry 'd'. 
 * @param dir parent directory of inode/dentry pair 
 * @param d a negative(unused) dentry object, to be bound to the 
 *          inode.
 * @param mode initial mode of inode 
 * @param excl ??? 
 * @note called by creat() and open() system calls 
 */ 
static int cfs_vfsi_create(struct inode *dir, struct dentry *d, umode_t mode, bool excl)
{
    struct inode *i = NULL;

    int retval;

    i = cfs_mk_inode(dir,mode); /*handles i_count increase*/
    if (IS_ERR(i)) {
        retval = PTR_ERR(i);
        goto out;
    }

    i->i_op = &cfs_file_inode_ops;
    i->i_fop = &cfs_file_ops;
    i->i_mapping->a_ops = &cfs_aops;
    mark_inode_dirty_sync(i);

    retval = cfs_vfs_ientry_add(dir,i,d);
out:
    return retval;
}

/** 
 *  Called to lookup an inode in parent directory 'dir'
 *  identified by dentry 'd'.
 *  @post dentry now refers to the bound inode
 *  @post if found; the found inode has its count incremented by
 *        one. Otherwise; dentry is associated with a NULL inode
 *  @return on success; an inode object matching the entry
 *          identified by dentry 'd' in directory 'dir'. on
 *          failure; dentry is linked to a NULL inode,
 *          indicating a disconnected dentry.
 */
static struct dentry *cfs_vfsi_lookup(struct inode *dir, struct dentry *d, unsigned int flags)
{
    /* 
        REQ's
            * must d_add / d_splice_alias dentry and inode to bind them together
            * i_count field of inode should be incremented (done by cfs_get_inode)
    */ 
    struct inode *i = NULL;
    struct cfsd_inode_chunk *c = NULL;
    
    struct cfsd_ientry *entry = NULL;
    struct ientry_loc ientry_loc;
    int retval;

    if(d->d_name.len > CFS_NAME_LEN)
        return ERR_PTR(-ENAMETOOLONG);

    c = kmem_cache_alloc(chunk_pool, GFP_KERNEL);
    if (!c) {
        CFS_WARN("failed to allocate a chunk for lookup purposes\n");
        return ERR_PTR(-ENOMEM);
    }

    retval = cfs_ientry_find(c, &ientry_loc, CFS_INODE(dir), d);
    if (retval) {
        if (retval != NOT_FOUND)
            CFS_WARN("failed to lookup entry, but the error wasn't (-1=>NOT_FOUND), something ELSE happened\n");
        goto out; /*FIXME: if retval == NOT_FOUND I'd need to set a null inode to dentry*/
    }

    entry = &c->entries[ientry_loc.chunk_off];
    i = cfs_iget(CFS_INODE(dir), entry, &ientry_loc);
    if ( IS_ERR(i) ) {
        CFS_WARN("failed to get/allocate inode from ientry, ERR_PTR: %ld\n", PTR_ERR(i));
        /*not having the actual inode, we MUST splice with a NULL inode 
          to indicate the entry wasn't found*/
        i = NULL;
        goto out;
    }
out:
    kmem_cache_free(chunk_pool, c);
    /*splicing i==NULL here means getting a negative 
      dentry, which is ok if the entry didn't exist.*/
    return d_splice_alias(i, d);
}

static int cfs_vfsi_mkdir(struct inode *dir, struct dentry *dentry, umode_t mode)
{
    struct inode *i = NULL;
    struct cfs_inode *ci = NULL;
    struct block_device *bd;
    int retval;

    CLYDE_ASSERT(dir != NULL);
    CLYDE_ASSERT(dentry != NULL);

    inode_inc_link_count(dir);

    bd = dir->i_bdev;
    CLYDE_ASSERT(bd != NULL);
    
    i = cfs_mk_inode(dir, mode|S_IFDIR);
    if (IS_ERR(i)) {
        retval = PTR_ERR(i);
        goto out;
    }
    ci = CFS_INODE(i);

    retval = __mkdir_mkitbl(&ci->data, dir->i_sb);
    if (retval) {
        goto err_mk_itbl;
    }

    retval = cfs_ientry_insert(CFS_INODE(dir), ci, dentry);
    if (retval) {
        goto err_add_ientry;
    }

    i->i_op = &cfs_dir_inode_ops;
    i->i_fop = &cfs_dir_file_ops;
    i->i_mapping->a_ops = &cfs_aops;
    inode_inc_link_count(i);

    goto out; /*success*/

err_add_ientry:
    if (cfsio_remove_node_sync(bd, ci->data.tid, ci->data.nid))
        CFS_WARN("Failed to undo changes, could not remove unused inode table (tid:%llu,nid:%llu)\n",
                 ci->data.tid, ci->data.nid);
err_mk_itbl:
    cfs_ino_release(dir->i_sb, i->i_ino);
    inode_dec_link_count(dir);
    cfs_iref_put(ci);
out:
    return retval;
}

/* FILE INODE OPERATIONS */

int cfsi_init(void)
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

void cfsi_exit(void)
{
    kmem_cache_destroy(chunk_pool);
}

const struct inode_operations cfs_file_inode_ops = {
};

const struct inode_operations cfs_dir_inode_ops = {
    .create = cfs_vfsi_create,
    .lookup = cfs_vfsi_lookup,
    .mkdir = cfs_vfsi_mkdir,
    /*basic file & dir support*/
    /* .create =  FNC_HERE,*/
    /* .lookup =  FNC_HERE,*/
    /* .mkdir =  FNC_HERE,*/
    /* .rmdir =  FNC_HERE,*/
    /* .rename =  FNC_HERE,*/
    /* .truncate =  FNC_HERE,*/


    /*hard-link support*/
    /* .link =  FNC_HERE,*/
    /* .unlink =  FNC_HERE,*/

    /*symlink support*/
    /* .symlink =  FNC_HERE,*/
    /* .readlink =  FNC_HERE,*/
    /* .follow_link =  FNC_HERE,*/
    /* .put_link =  FNC_HERE,*/

    /*device/pipe support*/
    /* .mknod =  FNC_HERE,*/
};
