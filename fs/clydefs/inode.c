#include <linux/fs.h>
#include <linux/sort.h>
#include <linux/time.h>
#include "clydefs.h"
#include "clydefs_disk.h"
#include "inode.h"
#include "io.h"
#include "super.h"

const struct inode_operations cfs_dir_inode_ops;
const struct inode_operations cfs_file_inode_ops;
extern const struct file_operations cfs_file_ops;
extern const struct file_operations cfs_dir_file_ops;

static struct kmem_cache *chunk_pool = NULL;

enum CHUNK_LOOKUP_RES { FOUND = 0, NOT_FOUND = 1 };

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

#if 0
/** 
 * Populate cfs inode with values from a disk inode entry. 
 * @param dst the inode to populate 
 * @param src the inode entry read from disk 
 * @post dst will be populated with the values from the disk 
 *       inode entry, converted to the native CPU format.
 */ 
static __always_inline void set_cfs_i(struct cfs_inode *dst, struct cfsd_ientry const * const src)
{
    struct inode *vfs_i = NULL;
    CLYDE_ASSERT( dst != NULL );
    CLYDE_ASSERT( src != NULL );
    CLYDE_ASSERT( spin_is_locked(&dst->vfs_inode.i_lock) );

    vfs_i = &dst->vfs_inode;

    /*set regular inode fields*/
    vfs_i->i_uid = __le32_to_cpu(src->uid_t);
    vfs_i->i_gid = __le32_to_cpu(src->gid_t);
    __copy2c_timespec(&vfs_i->i_ctime, &src->ctime);
    __copy2c_timespec(&vfs_i->i_mtime, &src->mtime);
    __copy2c_timespec(&vfs_i->i_atime, &src->mtime); /*we don't record access time*/
    vfs_i->i_ino = __le64_to_cpu(src->ino);
    vfs_i->i_mode = __le16_to_cpu(src->mode);

    /*set cfs-specific fields*/
    __copy2c_nodeaddr(&dst->inode_tbl, &src->inode_tbl);
}
#endif

/** 
 * Populate cfs inode with values from a disk inode entry. 
 * @param dst the inode to populate 
 * @param src the inode entry read from disk 
 * @post dst will be populated with the values from the disk 
 *       inode entry, converted to the native CPU format.
 * @note does NOT copy the name&nlen as this is related to the 
 *       dentry, not the inode.
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
    vfs_i->i_uid = le32_to_cpu(src->uid_t);
    vfs_i->i_gid = le32_to_cpu(src->gid_t);
    __copy2c_timespec(&vfs_i->i_ctime, &src->ctime);
    __copy2c_timespec(&vfs_i->i_mtime, &src->mtime);
    __copy2c_timespec(&vfs_i->i_atime, &src->mtime); /*we don't record access time*/
    vfs_i->i_size = le64_to_cpu(src->size_bytes);
    vfs_i->i_mode = __le16_to_cpu(src->mode);

    /*set cfs-specific fields*/
    __copy2c_nodeaddr(&dst->inode_tbl, &src->inode_tbl);
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
    struct inode const * const i = &src->vfs_inode;

    dst->ino = cpu_to_le64(i->i_ino);
    dst->uid_t = cpu_to_le32(i->i_uid);
    dst->gid_t = cpu_to_le32(i->i_gid);
    __copy2d_timespec(&dst->mtime, &i->i_mtime);
    __copy2d_timespec(&dst->ctime, &i->i_ctime);
    dst->size_bytes = cpu_to_le64(i->i_size);
    dst->mode = cpu_to_le16(i->i_mode);
    __copy2d_nodeaddr(&dst->inode_tbl, &src->inode_tbl);
}

/** 
 * Copy dentry-related fields into given on-disk inode 
 * representation 
 * @param dst the on-disk representation of a given inode 
 * @param ientry_d the dentry of the inode described by 'dst' 
 */ 
static __always_inline void __copy2d_inode_dentry(struct cfsd_ientry *dst, struct dentry const * const ientry_d)
{
    dst->nlen = cpu_to_le32(ientry_d->d_name.len);
    strncpy(dst->name, ientry_d->d_name.name, dst->nlen);
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

#if 0
static __always_inline struct cfsd_ientry *chunk_find_entry(
    struct cfsd_inode_chunk *c, struct cfsd_ientry *search_key)
{
    /*use bsearch module to carry out search 
      assume fully sorted*/
    return (struct cfsd_ientry*) bsearch(
        search_key,                     /*item to search for*/
        c,                              /*ptr to first elem*/
        CHUNK_NUMENTRIES,               /*number of elems*/
        sizeof(struct cfsd_ientry),     /*size of each elem*/
        ientry_cmp
    );
}

/** 
 * Read inode entry specified by its parent directory ('parent') 
 * inode and its filename ('search_dentry') specified by dentry. 
 * @param ret_inode the inode to populate with the result, if 
 *               found.
 * @param parent the parent inode
 * @param search_dentry a dentry with d_name populated with the 
 *                      filename to search for.
 * @return 0 on success, errors otherwise. 
 * @pre inode points to an already allocated inode object 
 * @pre search_dentry's d_name{len,name} are populated 
 */ 
int read_inode_entry(
    struct cfs_inode *ret_inode, struct cfs_inode *parent, struct dentry *search_dentry)
{
    struct cfsd_inode_chunk *chunk_curr = NULL;       /*ptr to chunk memory*/
    struct cfsd_ientry *cfsd_entry = NULL;  /*hold ptr to inode entry in chunk*/
    struct cfsd_ientry search_key;          /*struct used as a search key*/
    struct cfs_node_addr *itbl = NULL;      /*reference to parent's inode table*/
    int off;                                /*offset of current chunk to read into the parent inode table*/
    int retval;

    chunk_curr = kmem_cache_alloc(chunk_pool, GFP_KERNEL);
    if (!chunk_curr) {
        retval = -ENOMEM;
        goto err;
    }
    itbl = &parent->inode_tbl;
    off = CHUNK_LEAD_SLACK_BYTES;

    /*populate 'search_key' to make it searchable*/
    chunk_mk_key(&search_key, search_dentry);

read_chunk:
    retval = cfsio_read_node_sync(
        parent->vfs_inode.i_sb->s_bdev, NULL, NULL,
        itbl->tid, itbl->nid, 
        off, 
        sizeof(struct cfsd_inode_chunk), 
        chunk_curr
    );

    if (retval) {
        retval = -EIO;
        goto err_io;
    }

    cfsd_entry = chunk_find_entry(chunk_curr, &search_key);
    if( cfsd_entry ) {
        /*key found, write disk entry values to inode*/
        __copy2c_inode(ret_inode, cfsd_entry);
        retval = 0;
        /*fall out*/
    } else {
        off += CHUNK_LEAD_SLACK_BYTES + sizeof(struct cfsd_ientry);
        goto read_chunk;
    }

err_io: /*couldn't read chunk*/
    kmem_cache_free(chunk_pool, chunk_curr);
err:
    return retval;
}
#endif 

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
 *  @param ret_ndx on success; holds the index within the chunk
 *                 in 'ret_buf' where the entry was found.
 *  @param parent the parent inode in which to search for the
 *                entry.
 *  @param search_dentry holds the name of the entry to find
 *  @return 0(FOUND) on success; error otherwise.
 *      -ENOMEM => allocations failed
 *      -EIO => error while reading a chunk
 *      -1(NOT_FOUND) => element not present in parent directory
 *  @post on success; *ret_buf holds the chunk in which the
 *        entry was found, *ret_entry points to the entry inside
 *        the chunk which matched the search key.
 */ 
static int __must_check cfsi_ientry_find(struct cfsd_inode_chunk *ret_buf, u64* ret_ndx, struct cfs_inode *parent, struct dentry *search_dentry)
{
    struct cfsd_ientry search_key;          /*populated to function as the search key*/
    struct cfs_node_addr *itbl = NULL;      /*address of parent directory's inode table*/
    struct block_device *bd = NULL;         /*device holding the parent directory's inode table*/
    u64 off;
    int retval;

    CLYDE_ASSERT(ret_buf != NULL);
    CLYDE_ASSERT(ret_ndx != NULL);
    CLYDE_ASSERT(parent != NULL);
    CLYDE_ASSERT(search_dentry != NULL);

    itbl = &parent->inode_tbl;
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
    retval = chunk_lookup(ret_ndx, ret_buf, ret_buf->entries_used, &search_key);
    if (retval == NOT_FOUND) {
        /*did not find the entry*/
        if(ret_buf->last_chunk) {
            goto out; /*will return NOT_FOUND*/
        } else { /*advance to next*/
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

    itbl = &parent->inode_tbl;
    bd = parent->vfs_inode.i_sb->s_bdev;
    CLYDE_ASSERT(bd != NULL);
    off = CHUNK_LEAD_SLACK_BYTES;

    /*populate structure for writing*/
    __copy2d_inode(&tmp_ientry, inode);
    __copy2d_inode_dentry(&tmp_ientry, inode_d);

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

#if 0
/** 
 *  Retrieve inode by inode number
 *  @deprecated 
 */ 
static __always_inline struct inode *__get_inode(struct super_block *sb, unsigned long ino)
{
    /* 
        FIXME
            - unlock inode before returning
     */ 
    struct inode *i = NULL;
    struct cfs_inode *cfs_i = NULL;
    struct cfs_sb *cfs_sb = NULL;

    cfs_sb = CFS_SB(sb);

    i = iget_locked(sb, ino);
    if (!i) { /*no inode in cache, couldn't allocate one*/
        return ERR_PTR(-ENOMEM);
    } else if (!(i->i_state & I_NEW)) {
        return i; /*inode found in cache */
    }

    /*inode not in cache, new inode returned, freshly allocated*/
    /*---------------------------------------------------------*/
    cfs_i = CFS_INODE(i);

    CLYDE_STUB;
    return NULL; /*FIXME - STUB*/
}
#endif

/** 
 * Get inode from inode entry - will return an existing inode 
 * object if the inode is already referenced. 
 * @param sb the file system superblock. 
 * @param ientry the inode disk entry. 
 * @return on success; an inode matching the supplied inode 
 *         entry. Check IS_ERR(ret) to determine if an error
 *         occurred.
 * @post whether new or existing, inode object's reference count 
 *       is increased.
 */ 
static __always_inline struct inode *cfs_get_inode(struct super_block *sb, struct cfsd_ientry *ientry)
{
    struct inode *i = NULL;

    CLYDE_ASSERT(sb != NULL);
    CLYDE_ASSERT(ientry != NULL);

    i = iget_locked(sb, le64_to_cpu(ientry->ino));
    if (!i) { /*no inode found, failed to allocate a new*/
        return ERR_PTR(-ENOMEM);
    } else if (!(i->i_state & I_NEW)) {
        /*existing inode object found, not newly allocated*/
        return i;
    }

    /*not found in cache, freshly allocated object*/
    __copy2c_inode(CFS_INODE(i), ientry); /*set persisted values*/
    i->i_blkbits = CFS_BLOCKSIZE_SHIFT;

    /*FIXME Am I done ?*/
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
    int errval;
    struct cfsd_ientry *root_ientry;
    struct inode *root_inode = NULL;
    struct cfs_inode *root_cfs_i = NULL;
    struct cfs_node_addr *fs_inode_tbl = NULL;

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
    __copy2c_inode(root_cfs_i, root_ientry);
    CFSI_UNLOCK(root_cfs_i);

    /*link to the parent table containing the root entry (fs inode tbl)*/
    root_cfs_i->parent_tbl.tid = fs_inode_tbl->tid;
    root_cfs_i->parent_tbl.nid = fs_inode_tbl->nid;

    root_inode->i_op = &cfs_dir_inode_ops;
    root_inode->i_fop = &cfs_dir_file_ops;

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

/* 
========================================================================================== 
VFS operations and helpers 
*/ 

/** 
 * Add an inode entry for 'i' to directory 'dir' identified by 
 * name in 'i_d'. 
 */ 
static __always_inline int cfs_vfs_ientry_add(struct inode *dir, struct inode *i, struct dentry *i_d)
{
    int retval;
    retval = cfs_ientry_insert(CFS_INODE(dir), CFS_INODE(i), i_d);
    if (retval) {
        inode_dec_link_count(i);
        iput(i);
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
    ci->parent_tbl = CFS_INODE(dir)->inode_tbl;
    insert_inode_hash(i); /*hash ino based on sb and ino*/
    
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
    u64 ientry_ndx;
    int retval;

    if(d->d_name.len > CFS_NAME_LEN)
        return ERR_PTR(-ENAMETOOLONG);

    c = kmem_cache_alloc(chunk_pool, GFP_KERNEL);
    if (!c) {
        CFS_WARN("failed to allocate a chunk for lookup purposes\n");
        return ERR_PTR(-ENOMEM);
    }

    retval = cfsi_ientry_find(c, &ientry_ndx, CFS_INODE(dir), d);
    if (retval) {
        if (retval != NOT_FOUND)
            CFS_WARN("failed to lookup entry, but the error wasn't (-1=>NOT_FOUND), something ELSE happened\n");
        goto out; /*FIXME: if retval == NOT_FOUND I'd need to set a null inode to dentry*/
    }

    i = cfs_get_inode(dir->i_sb, &c->entries[ientry_ndx]);
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

    retval = __mkdir_mkitbl(&ci->inode_tbl, dir->i_sb);
    if (retval) {
        goto err_mk_itbl;
    }

    retval = cfs_ientry_insert(CFS_INODE(dir), ci, dentry);
    if (retval) {
        goto err_add_ientry;
    }

    i->i_op = &cfs_dir_inode_ops;
    i->i_fop = &cfs_dir_file_ops;
    inode_inc_link_count(i);

    goto out; /*success*/

err_add_ientry:
    if (cfsio_remove_node_sync(bd, ci->inode_tbl.tid, ci->inode_tbl.nid))
        CFS_WARN("Failed to undo changes, could not remove unused inode table (tid:%llu,nid:%llu)\n",
                 ci->inode_tbl.tid, ci->inode_tbl.nid);
err_mk_itbl:
    cfs_ino_release(dir->i_sb, i->i_ino);
    inode_dec_link_count(dir);
    iput(i);
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
