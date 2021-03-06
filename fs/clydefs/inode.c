#include <linux/fs.h>
#include <linux/sort.h>
#include <linux/time.h>
#include "clydefs.h"
#include "clydefs_disk.h"
#include "inode.h"
#include "io.h"
#include "super.h"
#include "chunk.h"

/*
    OUTSTANDING ISSUES
 
    - I'm passing along a NULL parent (and a NULL ientry_loc) for cfs_inode_init in cfsi_getroot
        pass an entry_loc which fits where the inode ientry should be in fs itbl and NULL work
            (and **CHECK** for NULL when writing inode to disk) -- also guard code calling cfs_inode_init
                such that ONLY getroot will pass null)
    - look @ cfsi_getroot
*/

const struct inode_operations cfs_dir_inode_ops;
const struct inode_operations cfs_file_inode_ops;
extern const struct file_operations cfs_file_ops;
extern const struct file_operations cfs_dir_file_ops;
extern const struct address_space_operations cfs_aops;

/* 
========================================================================================== 
Helper functions
*/ 
static void dbg_ientry_print(struct cfsd_ientry const * const e)
{
    CLYDE_ASSERT(e != NULL);
    CFS_DBG("\t{\n");
    if (e->name) {
        CFS_DBG("\t\tname: %s\n", e->name);
    } else {
        CFS_DBG("\t\tname: NULL\n");
    }
    
    CFS_DBG("\t\tino: %llu\n", le64_to_cpu(e->ino));
    CFS_DBG("\t\tsize_bytes: %llu\n", le64_to_cpu(e->size_bytes));
    CFS_DBG("\t}\n");
}

static void dbg_inode_print(struct inode const * const i) 
{
    CLYDE_ASSERT(i != NULL);
    CFS_DBG("\t{\n");
    
    CFS_DBG("\t\tino: %lu\n", i->i_ino);
    CFS_DBG("\t\tsize_bytes: %lld\n", i->i_size);
    CFS_DBG("\t}\n");
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
 * Associate 'parent' with 'ci' as its parent, increasing the 
 * reference count of the parent inode in the process -- without 
 * taking any locks. 
 * @pre already holds parent lock 
 * @post parent i_count increased, ci->parent points to parent. 
 */
static __always_inline void cfs_i_set_parent_nolock(struct cfs_inode *parent, struct cfs_inode *ci)
{
    preempt_disable();
    ci->parent = parent;
    atomic_inc(&parent->vfs_inode.i_count);
    preempt_enable();
}

/** 
 * handles the initialisation aspects common to all newly minted 
 * inodes. 
 * @param parent parent inode or NULL if initialising the root 
 *               inode.
 * @param ci the inode to initialise. 
 * @pre i_lock is held 
 */ 
static __always_inline void __cfs_i_common_init(struct cfs_inode *parent, struct cfs_inode *ci)
{ /*shared initialisation aspects*/
    umode_t i_mode;
    struct inode *i;
    CLYDE_ASSERT(ci != NULL);
    i_mode = ci->vfs_inode.i_mode;
    i = &ci->vfs_inode;
    CLYDE_ASSERT(i_mode != 0); /*MODE must have been set, otherwise can't set data.tid*/

    /*FIXME this function requires a lock on our parent, yet it doesn't follow our global 
      lockdep avoidance strategy (to always acquire inodes in increasing ino order)*/

    /*set parent reference*/
    if (likely(parent)) {
        /*assign parent to new inode and increment parent's usage count*/
        cfs_i_set_parent_nolock(parent, ci);
    } else {
        /*only root node can excuse parent==NULL 
          and that we only allow if the sb root isn't set yet*/
        CLYDE_ASSERT(ci->vfs_inode.i_sb->s_root == NULL);
        ci->parent = NULL;
    }

    /*set tid, actual value depends on whether this is a directory or not*/
    ci->data.tid = CFS_DATA_TID(ci);

    mutex_init(&ci->io_mutex);
    ci->sort_on_update = 0;

    /*override these after init if inode is persisted*/
    ci->on_disk = 0;
    ci->dsk_ientry_loc.chunk_ndx = ci->dsk_ientry_loc.ientry_ndx = 0;

    /*set various constants*/
    ci->vfs_inode.i_blkbits = CFS_BLOCKSIZE_SHIFT;
    CLYDE_ASSERT(ci->vfs_inode.i_sb != NULL);
    CLYDE_ASSERT(ci->vfs_inode.i_sb->s_bdi != NULL);
    
    /*set inode operations*/
    if (likely(i_mode & S_IFREG)) { /*FILE*/
        ci->status = IS_FILE;
        i->i_op = &cfs_file_inode_ops;
        i->i_fop = &cfs_file_ops;
        /*files are handled in the page cache*/
        i->i_mapping->backing_dev_info = ci->vfs_inode.i_sb->s_bdi;
        i->i_mapping->a_ops = &cfs_aops;
        i->i_mapping->host = i;
    } else if (i_mode & S_IFDIR) { /*DIR*/
        ci->status = IS_DIR;
        i->i_op = &cfs_dir_inode_ops;
        i->i_fop = &cfs_dir_file_ops;
        i->i_mapping->host = i;
        /*inodes are handled outside the page cache => no address operations*/
        i->i_mapping->backing_dev_info = &default_backing_dev_info;
        i->i_mapping->a_ops = &empty_aops;
        i->i_mapping->writeback_index = 0;
    } else {
        CFS_WARN("could not determine file type - setting regular file ops\n");
        ci->status = IS_FILE;
        i->i_op = &cfs_file_inode_ops;
        i->i_fop = &cfs_file_ops;
    }
}

/* 
========================================================================================== 
INO management
*/ 

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
    /*FIXME review*/
    #if 0
    /*If this is still uncommented, it provoked a deadlock, silly as that seems*/
    CLYDE_ASSERT( spin_is_locked(&ci->vfs_inode.i_lock) );
    #endif
    CLYDE_ASSERT(src != NULL);
    CLYDE_ASSERT(loc != NULL);
    i = &ci->vfs_inode;

    /*handle the metadata stored in the ientry*/
    __copy2c_inode(ci, src);
    __cfs_i_common_init(parent, ci);
    ci->dsk_ientry_loc = *loc;
    ci->on_disk = 1;

    CFS_DBG("REMOVE: initialised ino:%lu from disk, loc(param){chunk:%llu, entry:%llu) => ci->dsk_ientry_loc{chunk:%llu, entry:%llu)}\n",
            i->i_ino, loc->chunk_ndx, loc->ientry_ndx, ci->dsk_ientry_loc.chunk_ndx, ci->dsk_ientry_loc.ientry_ndx);
}

/** 
 * Get or initialise inode, whichever comes first. 
 * @param dir the parent directory inode 
 * @param ientry the inode entry for which we'd like an inode 
 * @param loc location of the inode entry within the parent 
 *            inode table.
 * @post on failure: ERR_PTR(-ENOMEM) - otherwise a fully 
 *       initialised inode.
 * @post inode has its reference count increased. 
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

    CFS_DBG(" read inode ino:%lu, loc(param){chunk:%llu, entry:%llu}, ci->dsk_ientry_loc{chunk:%llu, entry:%llu}\n", 
            i->i_ino, loc->chunk_ndx, loc->ientry_ndx, ci->dsk_ientry_loc.chunk_ndx, ci->dsk_ientry_loc.ientry_ndx);
    return i;
}

/** 
 * Read root node from disk.
 * @param sb the file system superblock 
 * @return the root inode, or ERR_PTR of type -ENOMEM / -EIO 
 * @note if you intend to use this outside the initial 
 *       initialisation of the root inode, rewrite function to
 *       use ilookup before trying to read the root ientry.
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
    int retval;

    CLYDE_ASSERT(sb != NULL);
    cfs_sb = CFS_SB(sb);
    fs_inode_tbl = &cfs_sb->fs_inode_tbl;

    chunk_curr = cfsc_chunk_alloc();
    if (!chunk_curr) {
        retval = -ENOMEM;
        goto err_alloc;
    }

    /*read the single inode entry in*/
    retval = cfsio_read_node_sync(
        sb->s_bdev, NULL, NULL,
        fs_inode_tbl->tid, fs_inode_tbl->nid, 
        0, 
        sizeof(struct cfsd_ientry), 
        chunk_curr
    );

    if (retval) {
        retval = -EIO;
        goto err_io;
    }

    root_ientry = &chunk_curr->entries[0];
    CLYDE_ASSERT(root_ientry->ino == CFS_INO_ROOT);
    root_inode = iget_locked(sb, CFS_INO_ROOT);
    if (!root_inode) { /*no inode in cache, couldn't allocate one*/
        retval = -ENOMEM;
        goto err_io;
    } else if (!(root_inode->i_state & I_NEW)) {
        goto success;
    }

    /*we just created this inode (=> loading the FS)*/
    root_cfs_i = CFS_INODE(root_inode);
    CFSI_LOCK(root_cfs_i);
    cfs_inode_init(NULL, root_cfs_i, root_ientry, &entry_loc);
    CFSI_UNLOCK(root_cfs_i);

    /*purges 'I_NEW' flag from the inode state, making any 
      iget_locked calls for this inode proceed*/
    unlock_new_inode(root_inode);
    
    goto success;
success:
    cfsc_chunk_free(chunk_curr);
    return root_inode;
err_io:
    cfsc_chunk_free(chunk_curr);
err_alloc:
    return ERR_PTR(retval);
}

/** 
 * Determine if supplied inode is the root inode. 
 * @param i inode to test 
 * @return 1 iff 'i' corresponds to the root inode, 0 
 *         otherwise.
 */ 
int cfsi_is_root(struct inode *i)
{
    int retval;
    struct dentry *d = NULL;
    CLYDE_ASSERT(i != NULL);
    d = dget(i->i_sb->s_root);
    retval = d->d_inode == i;
    dput(d);
    return retval;
}

#if 0
static void __cfsi_inode_update_done(struct cfsio_rq_cb_data *req_data, void *endio_cb_data, int error)
{
    if (error) {
        CFS_DBG("cfsi_inode_update complete handler -- error detected\n");
    }
    /*endio_cb_data => chunk buffer*/
    cfsc_chunk_free(endio_cb_data);
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
    bd = sb->s_bdev;
    ci = CFS_INODE(i);
    /*to persist an inode entry we need the name it was saved under*/
    CLYDE_ASSERT(ci->ientry_dentry != NULL);
    
    chunk = cfsc_chunk_alloc();
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

#if 0
/** 
 * Add an inode entry for 'i' to directory 'dir' identified by 
 * name in 'i_d'. 
 * @pre i inode with a usage count of 1 or more. 
 */ 
static __always_inline int cfs_vfs_ientry_add(struct inode *dir, struct inode *i, struct dentry *i_d)
{
    int retval;
    retval = cfsc_ientry_insert(CFS_INODE(dir), CFS_INODE(i), i_d);
    if (retval) {
        CFS_DBG("Failed to insert inode (ino(%lu), name(%s)) into itbl of parent ino(%lu) \n", i->i_ino, i_d->d_name.name, dir->i_ino);
        inode_dec_link_count(i);
        /*if i is a newly created inode, this render it eligible for reclamation*/
        iput(i);
        goto out;
    }
    d_instantiate(i_d, i); /*FIXME : ensure inode count has been incremented*/
out:
    return retval;
}
#endif 

/**
 * @param ci inode entry to update 
 * @param i_dentry (potentially new) dentry associated with 
 *                 inode entry.
 * @pre ci on_disk & dsk_ientry_loc is set 
 * @note 'i_dentry' can be null, need only be set if 
 *       sort_on_update is true
 * @return 0 on success, negative on errors 
 */
static __always_inline int __write_inode_update(struct cfs_inode *ci, struct dentry *i_dentry)
{ /*update the ientry of an already persisted inode*/
    CFS_DBG("called ci{ino:%lu}", ci->vfs_inode.i_ino);
    CLYDE_ASSERT(ci->on_disk);
    return cfsc_ientry_update(ci->parent, ci, i_dentry);
}

/** 
 * @param ci inode to insert 
 * @param i_dentry dentry associated inode
 * @pre ci->parent points to parent directory inode 
 * @post On success; on_disk = 1 && dsk_ientry_loc set with the 
 *       location of the ientry on disk.
 */
static int __write_inode_insert(struct cfs_inode *ci, struct dentry *i_dentry)
{ /*Called on new inodes, or inodes which have just been moved to another directory*/
    int retval;
    struct cfs_inode *cdir = NULL;
    struct super_block *sb = NULL;
    CLYDE_ASSERT(ci != NULL);
    CLYDE_ASSERT(ci->parent != NULL);
    cdir = ci->parent;

    CFS_DBG("called dir{ino:%lu} i{ino:%lu}  i_dentry:%s\n", 
            cdir->vfs_inode.i_ino, 
            ci->vfs_inode.i_ino, i_dentry->d_name.name);

    sb = cdir->vfs_inode.i_sb;
    CLYDE_ASSERT(sb != NULL);
    CLYDE_ASSERT(ci->parent != NULL);

    retval = cfsc_ientry_insert(cdir, ci, i_dentry);
    if (retval) {
        CFS_DBG("\t Failed to write ientry!\n");
        goto err_write_ientry;
    }

    CFS_DBG("ientry written, ino:%lu, dsk_ientry_loc{chunk:%llu, entry:%llu}\n", 
            ci->vfs_inode.i_ino, ci->dsk_ientry_loc.chunk_ndx, ci->dsk_ientry_loc.ientry_ndx);

    return 0; /*success*/

err_write_ientry:
    if (ci->vfs_inode.i_mode | S_IFDIR) {
        /*directory*/
        CFS_DBG("Failed to write inode entry to disk, releasing ino, inode and inode tbl\n");
        CLYDE_ASSERT(ci->data.tid == CFS_DATA_TID(ci));
        CLYDE_ASSERT(ci->data.nid != 0);
        if (cfsio_remove_node_sync(sb->s_bdev, ci->data.tid, ci->data.nid)) {
            CFS_DBG(
                "Failed to remove itbl node when trying to recover from being unable to write a directory inode entry (tid:%llu, nid:%llu)\n",
                ci->data.tid, ci->data.nid
            );
        }

    } else {
        CFS_DBG("Failed to write inode entry to disk, releasing ino and inode\n");
    }
    cfs_ino_release(cdir->vfs_inode.i_sb, ci->vfs_inode.i_ino);
    iput(&ci->vfs_inode);

    return retval;
}

/**
 * Persist an inode to disk.
 *  
 * @note called by cfs_write_inode on dirty inodes 
 */
int cfsi_write_inode(struct cfs_inode *ci, struct dentry *i_dentry)
{
    int retval = 0;
    CLYDE_ASSERT(ci != NULL);

    if (ci->on_disk) {
        /*only require an i_dentry iff we're renaming*/
        if (ci->sort_on_update) {
            CLYDE_ASSERT(i_dentry != NULL);
        }
        retval = __write_inode_update(ci, i_dentry);
        if (ci->sort_on_update && retval == 0) {
            /*write was a success - whatever change required re-sorting has been written*/
            ci->sort_on_update = 0;
        }
    } else {
        CLYDE_ASSERT(i_dentry != NULL); /*need a name to identify the entry*/
        retval = __write_inode_insert(ci, i_dentry);
    }

    return retval;
}

/** 
 * Make a new inode and write it to disk 
 * @param dir parent directory 
 * @param d the dentry identifying the file name 
 * @param mode the inode's initial mode 
 * @return 0 on success; error otherwise 
 * @note MUST take care to write the inode onto the disk 
 */ 
static struct inode *cfs_inode_init_new(struct inode *dir, struct dentry *d, umode_t mode)
{
    struct super_block *sb = NULL;
    struct cfs_sb *csb = NULL;
    struct inode *i = NULL;
    struct cfs_inode *ci = NULL;
    struct cfs_inode *cdir = NULL;

    CLYDE_ASSERT(dir != NULL);
    sb = dir->i_sb;
    csb = CFS_SB(sb);
    cdir = CFS_INODE(dir);

    i = new_inode(sb);
    if (!i) {
        CLYDE_ERR("Failed to allocate new inode\n");
        i = ERR_PTR(-ENOMEM);
        goto out;
    }

    inode_init_owner(i, dir , mode);
    i->i_ino = cfs_ino_nxt(sb);
    i->i_ctime = i->i_mtime = i->i_atime = CURRENT_TIME;
    i->i_size = 0;

    ci = CFS_INODE(i);
    __cfs_i_common_init(cdir, ci);

    /*assign parent (and increment parent's usage to reflect its use)*/
    insert_inode_hash(i); /*hash inode for lookup, based on sb and ino*/

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

    c = cfsc_chunk_alloc();
    if (!c) {
        retval = -ENOMEM;
        goto out;
    }
    ret_itbl->tid = CFS_INODE_TID(csb);
    retval = cfsc_mk_itbl_node(&ret_itbl->nid, bd, ret_itbl->tid);
    if (retval) {
        goto err_node_ins;
    }
    cfsc_chunk_init(c);

    retval = cfsc_write_chunk_sync(bd, ret_itbl->tid, ret_itbl->nid, c, 0);
    if (retval) {
        goto err_node_write;
    }

    goto out; /*success*/

err_node_write:
    if (cfsio_remove_node_sync(bd, ret_itbl->tid, ret_itbl->nid))
        CFS_WARN("Failed to undo changes, tried to remove node (tid:%llu,nid:%llu)\n", 
                 ret_itbl->tid, ret_itbl->nid);
err_node_ins:
    cfsc_chunk_free(c);
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
    struct cfs_inode *ci = NULL;
    struct block_device *bd = NULL;
    struct cfs_sb *csb = NULL;
    int retval = 0;
    CFS_DBG("called dir{ino:%lu} dentry{%s}, mode{%u}\n", dir->i_ino, d->d_name.name, mode);
    CLYDE_ASSERT(dir != NULL);
    CLYDE_ASSERT(d != NULL);
    bd = dir->i_sb->s_bdev;
    csb = CFS_SB(dir->i_sb);

    i = cfs_inode_init_new(dir, d, mode | S_IFREG); /*handles i_count increase*/
    if (IS_ERR(i)) {
        retval = PTR_ERR(i);
        CFS_DBG("failed to create new inode for file\n");
        goto out;
    }
    ci = CFS_INODE(i);

    atomic_inc(&csb->pending_io_ops);
    ci->data.tid = CFS_DATA_TID(ci);
    retval = cfsio_insert_node_sync(bd, &ci->data.nid, ci->data.tid, 4096);
    if (retval) {
        CFS_DBG("failed to create file node for new file.\n");
        atomic_dec(&csb->pending_io_ops);
        goto err_data_node_ins;
    }

    /*do not mark the inode dirty, the function below will take care of 
      writing the inode to disk, obviating the need to mark it*/
    retval = cfsi_write_inode(CFS_INODE(i), d);
    if (retval) {
        CFS_DBG("Failed to write data node for new file\n");
        atomic_dec(&csb->pending_io_ops);
        goto err_persist_inode;
    }

    CFS_DBG("success, instantiating dentry with inode\n");
    d_instantiate(d, i);
    atomic_dec(&csb->pending_io_ops);
out:
    return retval;
err_persist_inode:
    cfsio_remove_node_sync(bd, ci->data.tid, ci->data.nid);
err_data_node_ins:
    iput(i);
    return retval;
}

/** 
 *  Called to lookup an inode in parent directory 'dir'
 *  identified by dentry 'd'.
 *  @post dentry now refers to the bound inode
 *  @post if found; the found inode has its count incremented by
 *        one. Otherwise; dentry is associated with a NULL inode
 *  @pre called with the directory inode semaphore held.
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
    struct cfs_sb *csb = NULL;
    struct cfsd_inode_chunk *c = NULL;
    
    struct cfsd_ientry *entry = NULL;
    struct ientry_loc ientry_loc;
    int retval;

    CLYDE_ASSERT(dir != NULL);
    CLYDE_ASSERT(d != NULL);
    CFS_DBG("called dir{ino:%lu}, dentry{name:%s}\n", dir->i_ino, d->d_name.name);

    csb = CFS_SB(dir->i_sb);
    atomic_inc(&csb->pending_io_ops);

    if(d->d_name.len > CFS_NAME_LEN) {
        retval = -ENAMETOOLONG;
        goto err;
    }

    c = cfsc_chunk_alloc();
    if (!c) {
        CFS_DBG("failed to allocate a chunk for lookup purposes\n");
        retval = -ENOMEM;
        goto err;
    }

    retval = cfsc_ientry_find(c, &ientry_loc, CFS_INODE(dir), d);
    if (retval) {
        if (retval != NOT_FOUND)
            CFS_DBG("failed to lookup entry, but the error wasn't (-1=>NOT_FOUND), something ELSE happened\n");
        goto out; /*if retval == NOT_FOUND, splice a NULL inode to the dentry*/
    }

    /*found the ientry; now get either the existing inode or intialise it*/
    CFS_DBG("found the ientry");
    entry = &c->entries[ientry_loc.ientry_ndx];
    dbg_ientry_print(entry);
    i = cfs_iget(CFS_INODE(dir), entry, &ientry_loc);
    if ( IS_ERR(i) ) {
        CFS_DBG("failed to get/allocate inode from ientry, ERR_PTR: %ld\n", PTR_ERR(i));
        /*not having the actual inode, we MUST splice with a NULL inode 
          to indicate the entry wasn't found*/
        i = NULL;
        goto out;
    }
    CFS_DBG("ientry found and converted to inode\n");
    dbg_inode_print(i);
out:
    cfsc_chunk_free(c);
    /*splicing i==NULL here means getting a negative 
      dentry, which is ok if the entry didn't exist.*/
    atomic_dec(&csb->pending_io_ops);
    return d_splice_alias(i, d);
err:
    atomic_dec(&csb->pending_io_ops);
    return ERR_PTR(retval);
}

static int cfs_vfsi_mkdir(struct inode *dir, struct dentry *d, umode_t mode)
{
    struct inode *i = NULL;
    struct cfs_inode *ci = NULL;
    struct block_device *bd;
    struct cfs_sb *csb = NULL;
    int retval;

    CLYDE_ASSERT(dir != NULL);
    CLYDE_ASSERT(d != NULL);

    inode_inc_link_count(dir);

    bd = dir->i_sb->s_bdev;
    csb = CFS_SB(dir->i_sb);
    CLYDE_ASSERT(bd != NULL);
    
    atomic_inc(&csb->pending_io_ops);
    i = cfs_inode_init_new(dir, d, mode|S_IFDIR);
    if (IS_ERR(i)) {
        retval = PTR_ERR(i);
        goto out;
    }
    ci = CFS_INODE(i);

    retval = __mkdir_mkitbl(&ci->data, dir->i_sb);
    if (retval) {
        goto err_mk_itbl;
    }

    retval = 0;
    if ((retval = cfsi_write_inode(ci, d))) {
        goto err_write_inode;
    }

    inode_inc_link_count(i);
    CFS_DBG("success, instantiating dentry with inode\n");
    d_instantiate(d, i);
    goto out; /*success*/
err_write_inode:
err_mk_itbl:
    cfs_ino_release(dir->i_sb, i->i_ino);
    inode_dec_link_count(dir);
    iput(i);
out:
    atomic_dec(&csb->pending_io_ops);
    return retval;
}

/* FILE INODE OPERATIONS */
int cfs_vfsi_unlink(struct inode *dir, struct dentry *d)
{
    printk(KERN_EMERG "deletion of inodes not supported!!\n");
    return -1;
}

int cfsi_init(void)
{
    /*NO-OP*/
	return 0;
}

void cfsi_exit(void)
{ /*NO-OP */}

const struct inode_operations cfs_file_inode_ops = {
};

const struct inode_operations cfs_dir_inode_ops = {
    .create = cfs_vfsi_create,
    .lookup = cfs_vfsi_lookup,
    .mkdir = cfs_vfsi_mkdir,
    //.unlink =  FNC_HERE, /*remove inode specified by directory entry*/

    /*basic file & dir support*/
    /* .create =  FNC_HERE,*/
    /* .lookup =  FNC_HERE,*/
    /* .mkdir =  FNC_HERE,*/
    /* .rmdir =  FNC_HERE,*/
    /* .rename =  FNC_HERE,*/
    /* .truncate =  FNC_HERE,*/


    /*hard-link support*/
    /* .link =  FNC_HERE,*/
    

    /*symlink support*/
    /* .symlink =  FNC_HERE,*/
    /* .readlink =  FNC_HERE,*/
    /* .follow_link =  FNC_HERE,*/
    /* .put_link =  FNC_HERE,*/

    /*device/pipe support*/
    /* .mknod =  FNC_HERE,*/
};
