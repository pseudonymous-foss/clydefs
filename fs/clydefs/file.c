#include "file.h"
#include "inode.h"
#include "chunk.h"

const struct file_operations cfs_file_ops;
const struct file_operations cfs_dir_file_ops;

/**
 * Given a file pointer, find and return the parent inode.
 * @note not sure this method of getting the inode of another FS 
 *       works or is advisable, for that matter.
 * @return ino of parent directory
 */
static u64 get_parent_ino(struct file *filp)
{
    struct dentry *d = NULL;
    struct dentry *d_parent = NULL;
    u64 ino;

    CLYDE_ASSERT(filp != NULL);

    d = dget(filp->f_path.dentry);
    CLYDE_ASSERT(d != NULL);
    d_parent = dget(d->d_parent);
    CLYDE_ASSERT(d_parent != NULL);
    dput(d);

    ino = d_parent->d_inode->i_ino; /*FIXME: not getting a reference to the inode, could disappear*/
    
    dput(d_parent);
    return ino;
}

/** 
 * Called when the last reference to an open file is closed. 
 */ 
static int cfs_file_release(struct inode *inode, struct file *filp)
{
    /*
        As we rely on the generic file functions (which in turn rely on our
        page cache address operations) we needn't do anything further when
        a file is to be released.
    */
	return 0;
}

/**
 * Flush a range of a file's contents to disk and update inode 
 * to reflect any changes. 
 */
static int cfs_file_fsync(struct file *filp, loff_t start, loff_t end,
			    int datasync)
{
	struct inode *inode = filp->f_mapping->host;
	int ret;

	ret = filemap_write_and_wait_range(inode->i_mapping, start, end);
	if (ret)
		return ret;

	mutex_lock(&inode->i_mutex);
	ret = sync_inode_metadata(filp->f_mapping->host, 1);
	mutex_unlock(&inode->i_mutex);
	return ret;
}

/**
 * Called by 'fsync' system call to flush a file's contents to 
 * disk. 
 */
static int cfs_file_flush(struct file *file, fl_owner_t id)
{
    /*flush file data to disk, along with inode data*/
	return vfs_fsync(file, 0);
}

/* Relationship between i_mode and the DT_xxx types */
static inline unsigned char ientry_dt_type(struct cfsd_ientry *ientry)
{ /*rewritten from libfs.c dt_type*/
    return (le16_to_cpu(ientry->mode) >> 12) & 0b1111;
}

/** 
 * Return next directory in a directory listing. 
 * @param filp the directory file pointer 
 * @param dirent structure to hold information on next directory 
 *               entry
 * @param filldir callback function for populating 'dirent' with 
 *                the supplied information.
 * @return zero on success; On failure, a negative errorvalue 
 *         (such as ENOMEM)
 */
static int cfs_readdir(struct file *filp, void *dirent, filldir_t filldir)
{   
    /*
        read directory entries into 'dirent' structure by supplying key information
        about the entry to 'filldir' callback.
     
        entry_num == filp->pos - 2 -- signifying that each directory has two
            entries by default, '.' and '..'
    */
    struct inode *i = file_inode(filp);
    struct cfs_inode *ci = CFS_INODE(i);
    struct cfs_node_addr *itbl = &ci->data;
    struct cfsd_inode_chunk *c = NULL;
    struct cfsd_ientry *c_entry = NULL;
    int retval = 0;
    /*I'm going to use f_pos as an index into the complete list of dir-entries*/

    //loff_t entry_num = filp->f_pos - 2; /*which entry number to read first*/
    loff_t entry_num = filp->f_pos;

    u64 chunk_ndx = entry_num / CHUNK_NUMENTRIES; /*which chunk to read*/
    u64 entry_ndx = entry_num % CHUNK_NUMENTRIES; /*index in chunk of first entry to read*/

    CFS_DBG(
        "called file{ino:%lu, name:%s, f_pos/entry_num:%lld} => ndx{chunk:%llu, entry:%llu}\n", 
        i->i_ino, ci->itbl_dentry->d_name.name, entry_num, chunk_ndx, entry_ndx
    );
    c = cfsc_chunk_alloc();
    if (!c) {
        retval = -ENOMEM;
        goto err_alloc;
    }

    #if 0
    if (entry_ndx == 0 /*< 0*/)
    { /*starting directory listing, fill entries for '.' and '..'*/
        struct inode *dir_inode = filp->f_inode;

        if (1 && entry_ndx == -2) {
            retval = filldir(dirent, ".", 1, -2, dir_inode->i_ino, DT_DIR);
            if (retval) {
                retval = 0;
                goto out;
            }
            entry_num++; /* => entry_num == -1*/
        }
        if (1 && entry_ndx == -1) {
            /*'..' => parent entry*/

            if (unlikely(cfsi_is_root(dir_inode))) {
                /*listing the root directory*/
                retval = filldir(dirent, "..", 2, -1, get_parent_ino(filp), DT_DIR);
            } else {
                /*a subdirectory within the filesystem*/
                retval = filldir(dirent, "..", 2, -1, CFS_INODE(dir_inode)->parent->vfs_inode.i_ino, DT_DIR);
                
            }

            if (retval) {
                    retval = 0;
                    goto out;
            }
            entry_num++; /*=> entry_num == 0, => read real entries*/
        }
    }
    #endif

    while(1) {
        u64 end_of_chunk;
        CFS_DBG("itbl{tid:%llu, nid:%llu} chunk_ndx{%llu}\n",
                itbl->tid, itbl->nid, chunk_ndx);
        cfsi_i_wlock(ci);
        retval = cfsc_read_chunk_sync(i->i_sb->s_bdev, itbl->tid, itbl->nid, c, chunk_ndx);
        CFS_DBG("c{entries_free:%u, last_chunk:%u}\n",
                c->hdr.entries_free, c->hdr.last_chunk);
        cfsi_i_wunlock(ci);
        if (retval) {
            CFS_DBG("Failed to read chunk '%llu'\n", chunk_ndx);
            goto out;
        }
        end_of_chunk = CHUNK_NUM_ITEMS(c);

        /*loop through chunk entries, copying them into dirent one at a time*/
        while(entry_ndx < end_of_chunk) {
            CFS_DBG("deref entry: %llu\n", entry_ndx);
            c_entry = &c->entries[entry_ndx];
            CFS_DBG("calling filldir\n");
            retval = filldir(
                dirent,                                     /*directory structure to write into*/
                c_entry->name, le16_to_cpu(c_entry->nlen),  /*name and length of name*/
                chunk_ndx + entry_ndx,                      /*offset of entry (FIXME: can I do this ? just treat offset as an index into a list of entries?)*/
                le64_to_cpu(c_entry->ino),                  /*ino of directory entry*/
                ientry_dt_type(c_entry)                     /*file type of directory entry, fs.h ca. 1408 DT_{REG,BLK,CHR,LNK,DIR} etc*/
            );
            CFS_DBG(
                "filldir called entry{name:%s, nlen:%d, off:%llu, ino:%llu}\n", 
                c_entry->name, le16_to_cpu(c_entry->nlen), chunk_ndx+entry_ndx, le64_to_cpu(c_entry->ino)
            );
            if (retval) {
                /*filldir is filled for this time*/
                CFS_DBG("dirent is overfull, exiting (success)\n");
                retval = 0; /*reset retval, we were successful*/
                goto out;
            }
            entry_num++; /*sucessfully told VFS about yet another directory entry*/
            entry_ndx++;
        }
        if (c->hdr.last_chunk) {
            /*no more entries*/
            CFS_DBG("No more entries to read\n");
            goto out;
        } else {
            CFS_DBG("read everything in chunk, advancing to next chunk\n");
            chunk_ndx++; /*read in next chunk*/
            entry_ndx = 0; /*start at first entry of next chunk*/
        }
    }
out:
    filp->f_pos = entry_num; /*advance index by as many entries as we've read*/
    cfsc_chunk_free(c);
err_alloc:
    return retval;
}

const struct file_operations cfs_file_ops = {
    .open = generic_file_open,
    .read = do_sync_read,
    .write = do_sync_write,
    .aio_read = generic_file_aio_read,
    .aio_write = generic_file_aio_write,
    .llseek = generic_file_llseek,
    .mmap = generic_file_mmap,

    /*splice data from file to pipe*/
    .splice_read = generic_file_splice_read,
    .splice_write = generic_file_splice_write,

    .release = cfs_file_release,
    .fsync = cfs_file_fsync,
    .flush = cfs_file_flush,
};

const struct file_operations cfs_dir_file_ops = {
    .llseek = generic_file_llseek,  /*OK, does not rely on page table*/
    .read = generic_read_dir,       /*OK, does not rely on page table*/
    .readdir = cfs_readdir,
};
