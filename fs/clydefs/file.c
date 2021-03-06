#include "file.h"
#include "inode.h"
#include "chunk.h"

const struct file_operations cfs_file_ops;
const struct file_operations cfs_dir_file_ops;

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
    CFS_DBG("called\n");
	return 0;
}

/** 
 * Called when file usage is decremented, up to the FS to decide
 * what to do, if anything. 
 */
static int cfs_file_flush(struct file *file, fl_owner_t id)
{
    /*flush file data to disk, along with inode data*/
    CFS_DBG("called\n");
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
        i->i_ino, filp->f_path.dentry->d_name.name, entry_num, chunk_ndx, entry_ndx
    );
    c = cfsc_chunk_alloc();
    if (!c) {
        retval = -ENOMEM;
        goto err_alloc;
    }

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
            CFS_DBG("calling filldir entry{name:%s, nlen:%u, off:%llu, ino:%llu} (mode: %u)\n", 
                    c_entry->name, le16_to_cpu(c_entry->nlen), chunk_ndx + entry_ndx, 
                    le64_to_cpu(c_entry->ino), le16_to_cpu(c_entry->mode));
            retval = filldir(
                dirent,                                     /*directory structure to write into*/
                c_entry->name, le16_to_cpu(c_entry->nlen),  /*name and length of name*/
                chunk_ndx + entry_ndx,                      /*offset of entry (FIXME: can I do this ? just treat offset as an index into a list of entries?)*/
                le64_to_cpu(c_entry->ino),                  /*ino of directory entry*/
                ientry_dt_type(c_entry)                     /*file type of directory entry, fs.h ca. 1408 DT_{REG,BLK,CHR,LNK,DIR} etc*/
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
    .fsync = generic_file_fsync,
    .flush = cfs_file_flush,
};

const struct file_operations cfs_dir_file_ops = {
    .llseek = generic_file_llseek,  /*OK, does not rely on page table*/
    .read = generic_read_dir,       /*OK, does not rely on page table*/
    .readdir = cfs_readdir,
};
