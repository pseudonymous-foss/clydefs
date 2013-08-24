#include <linux/fs.h>
#include <linux/sort.h>
#include <linux/bsearch.h>
#include <linux/time.h>
#include "clydefs.h"
#include "clydefs_disk.h"
#include "inode.h"
#include "io.h"

#if 0

const struct inode_operations clydefs_dir_inode_ops = {
    .create = clydefs_inode_create,
    .lookup = cfsi_lookup,
};
#endif

static struct kmem_cache *chunk_pool = NULL;

static __always_inline void set_timespec(struct timespec *dst, __le64 time_sec)
{ /*set inode timespec value from disk value*/
    dst->tv_sec = time_sec;
}

static __always_inline void set_itbl_addr(struct cfs_node_addr *dst, struct cfsd_node_addr const * const src)
{
    dst->tid = __le64_to_cpu(src->tid);
    dst->nid = __le64_to_cpu(src->nid);
}

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
    set_timespec(&vfs_i->i_ctime, src->ctime);
    set_timespec(&vfs_i->i_mtime, src->mtime);
    set_timespec(&vfs_i->i_atime, src->mtime); /*we don't record access time*/
    vfs_i->i_ino = __le64_to_cpu(src->ino);
    vfs_i->i_mode = __le16_to_cpu(src->mode);

    /*set cfs-specific fields*/
    set_itbl_addr(&dst->inode_tbl, &src->inode_tbl);
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
 * @param inode the inode to populate with the result, if found.
 * @param parent the parent inode
 * @param search_dentry a dentry with d_name populated with the 
 *                      filename to search for.
 * @return 0 on success, errors otherwise. 
 * @pre inode points to an already allocated inode object 
 * @pre search_dentry's d_name{len,name} are populated 
 */ 
int read_inode_entry(
    struct cfs_inode *inode, struct cfs_inode *parent, struct dentry *search_dentry)
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
    off = CHUNK_BEGIN_OFF;

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
        set_cfs_i(inode, cfsd_entry);
        retval = 0;
        /*fall out*/
    } else {
        off += sizeof(struct cfsd_ientry);
        goto read_chunk;
    }

err_io: /*couldn't read chunk*/
    kmem_cache_free(chunk_pool, chunk_curr);
err:
    return retval;
}



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

    return NULL; /*FIXME - STUB*/
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
        0, 
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
    set_cfs_i(root_cfs_i, root_ientry);
    CFSI_UNLOCK(root_cfs_i);

    /*This inode has no parents, hence no parent table*/
    root_cfs_i->parent_tbl.tid = 0;
    root_cfs_i->parent_tbl.nid = 0;

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


const struct inode_operations cfs_dir_inode_ops = {

};
