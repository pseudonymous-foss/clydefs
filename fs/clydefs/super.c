#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/parser.h>
#include <linux/tree.h>

#include "clydefs.h"
#include "clydefs_disk.h"
#include "inode.h"
#include "io.h"

extern const struct inode_operations cfs_dir_inode_ops;
extern struct file_operations cfs_dir_file_ops;

static struct dentry *cfs_mount(struct file_system_type *fs_type, int flags,
                             const char *device_path, void *data);
static int cfs_fill_super(struct super_block *sb, void *data, int silent);

/*Caches*/
static struct kmem_cache *cfssup_inode_pool = NULL;

/*Structures*/
static struct file_system_type clydefs_fs_type;
static const struct super_operations cfs_super_operations;

/*arg-parse related*/
#define CLYDEFS_MAX_ARGS 2
enum MOUNT_ARG{ MNTARG_TID, MNTARG_NID, MNTARG_ERR };

/*FIXME kernel argparser doesn't allow 64bit values yet, 
  so only the lower 32bits are eligible TID's & NID's
  for pointing to the superblock table.*/
static match_table_t mnt_tokens = {
    /*tree id of super tree*/
	{MNTARG_TID, "tid=%u"},
    /*node id of entry superblock table*/
	{MNTARG_NID, "nid=%u"},
	{MNTARG_ERR, NULL}
};

enum SB_ENTRY {
    SB_OLDEST_ENTRY, SB_NEWEST_ENTRY
};

struct cfs_mnt_args {
    /** path to storage device holding the trees */
    char const *dev_path;
    /** tree holding the superblock table node   */
    u64 tid;
    /** node holding the superblock table  */
    u64 nid;
};

/** 
 *  Parse mount option string.
 *  @param ret_args the parsed mount options
 *  @param mnt_arg_string the raw argument string to parse for
 *                        mount options.
 *  @pre ret_args is zero'ed out
 *  @return 0 on success, -EINVAL on invalid input (such as
 *          missing mount options)
 */ 
static int __parse_mnt_args(struct cfs_mnt_args *ret_args, char *mnt_arg_string)
{
    char *p = NULL;
    substring_t args[CLYDEFS_MAX_ARGS]; 

    /*split on ','*/
    while ((p = strsep(&mnt_arg_string, ",")) != NULL) {
		int token;
		char str[32];

		if (!*p) /*ignore empty tokens*/
			continue;

		token = match_token(p, mnt_tokens, args);
		switch (token) {
        case MNTARG_TID:
            if (0 == match_strlcpy(str, &args[0], sizeof(str)))
				return -EINVAL;
			ret_args->tid = simple_strtoull(str, NULL, 10);
            if (ret_args->tid < TREE_MIN_TID) {
                CLYDE_ERR("Tree identifier must be >= %ull (got: %llu)\n", TREE_MIN_TID, ret_args->tid);
                return -EINVAL;
            }
            break;
        case MNTARG_NID:
            if (0 == match_strlcpy(str, &args[0], sizeof(str)))
				return -EINVAL;
			ret_args->nid = simple_strtoull(str, NULL, 10);
			if (ret_args->nid < TREE_MIN_NID) {
                CLYDE_ERR("Node identifier must be >= %ull, (got: %llu)\n", TREE_MIN_NID, ret_args->nid);
                return -EINVAL;
            }
            break;
		}
    }

    if (!ret_args->tid) {
        CLYDE_ERR("Missing tree identifier in mount options\n");
        return -EINVAL;
    }
    if (!ret_args->nid) {
        CLYDE_ERR("Missing node identifier in mount options\n");
        return -EINVAL;
    }
    return 0; /*success*/
}

/*INODE Cache Code*/
/** 
 * Allocate a inode structure. 
 * @param sb the file system superblock 
 * @note this function is called by iget_locked 
 */ 
static struct inode *cfs_alloc_inode(struct super_block *sb)
{
    /*we actually allocate a larger structure than just an inode 
      which we can later get a hold of via container_of */
    struct cfs_inode *inode;

	inode = kmem_cache_alloc(cfssup_inode_pool, GFP_KERNEL);
	if (!inode) {
		return NULL;
    }
    inode->vfs_inode.i_version = 1;
	return &inode->vfs_inode;
}

/** 
 * deallocate inode. 
 * @param head rcu structure where 'next' points to inode's rcu 
 *             field.
 * @pre no more readers accessing inode 
 * @post inode is freed 
 */ 
static void cfssup_cb_free_inode(struct rcu_head *head)
{
	struct inode *inode = container_of(head->next, struct inode, i_rcu);
	kmem_cache_free(cfssup_inode_pool, CFS_INODE(inode));
}

/** 
 * Schedule deallocation of inode. 
 * @param inode the inode to deallocate 
 * @note cfssup_cb_free_inode will be called once no more 
 *       readers reference the inode.
 */ 
static void cfs_destroy_inode(struct inode *inode)
{
	call_rcu(&inode->i_rcu, cfssup_cb_free_inode);
}
 
/** 
 * Initialise the inode structure 
 * @param data a reference to the cfs_inode to initialise
 * @note wrapping standard fs inode_init_once function 
 * @note called when new pages are added to the inode cache 
 * @note see Linux Kernel Development, 3rd ed.  
 */ 
static void cfs_inode_init_once(void *data)
{
	struct cfs_inode *i = data;
	inode_init_once(&i->vfs_inode);
}

/** 
 * Initialise the inode cache. 
 */ 
static int __inodecache_init(void)
{
	cfssup_inode_pool = kmem_cache_create(
        "cfssup_inode_pool",
		sizeof(struct cfs_inode),
        0,
        /*objects are reclaimable*/
		SLAB_RECLAIM_ACCOUNT 
        /*spread allocation across memory rather than favouring memory local to current cpu*/
         | SLAB_MEM_SPREAD,  
        /*called whenever new pages are added to the cache*/
		cfs_inode_init_once
    );

	if (!cfssup_inode_pool)
		return -ENOMEM;
	return 0;
}

/** 
 * Destroy the inode cache. 
 * @note will wait until all rcu operations are finished. 
 */ 
static void __inodecache_destroy(void)
{
	rcu_barrier();
	kmem_cache_destroy(cfssup_inode_pool);
}


static int __always_inline __get_newest_sb_ndx(struct cfsd_sb *sb_arr)
{
    u32 newest_gen = 0;
    unsigned int newest_ndx = 0;
    unsigned i;
    for (i = 0; i < CLYDE_NUM_SB_ENTRIES; i++) {
        if ( le32_to_cpu(sb_arr[i].generation) > newest_gen ) {
            newest_ndx = i;
            newest_gen = le32_to_cpu(sb_arr[i].generation);
        }
    }
    if ( unlikely(newest_gen == 0) ) {
        /*no sb found with newer gen, should NEVER happen 
          as start-generation should be 1*/
        CLYDE_ERR("None of the superblocks had a generation number past 0 - aborting mount\n");
        return -EINVAL;
    } else {
        return newest_ndx; /*return index of newest entry*/
    }
}

static int __always_inline __get_oldest_sb_ndx(struct cfsd_sb *sb_arr)
{
    u32 oldest_gen = le32_to_cpu(sb_arr[0].generation);
    unsigned int oldest_ndx = 0;
    unsigned i;
    for (i = 1; i < CLYDE_NUM_SB_ENTRIES; i++) {
        if ( le32_to_cpu(sb_arr[i].generation) < oldest_gen ) {
            oldest_ndx = i;
            oldest_gen = le32_to_cpu(sb_arr[i].generation);
        }
    }
    return oldest_ndx; /*return index of oldest entry*/
}

/** 
 * Get a pointer to the newest/oldest of the superblocks 
 * @param sb_arr a pointer to an array of superblock disk 
 *               representations.
 * @param entry whether to find the newest or oldest superblock.
 * @pre sb_arr is a pointer to an array of CLYDE_NUM_SB_ENTRIES 
 *      superblocks
 * @return index of the oldest/newest superblock entry in 
 *         'sb_arr'. Or negative in case of error
 */ 
static int cfs_get_sb_ndx(struct cfsd_sb *sb_arr, enum SB_ENTRY entry)
{
    CLYDE_ASSERT(sb_arr != NULL);
    if (entry == SB_OLDEST_ENTRY)
        return __get_oldest_sb_ndx(sb_arr);
    else
        return __get_newest_sb_ndx(sb_arr);
}

/** 
 * Write FS metadata to device (notably the superblock) 
 * @param sb filesystem superblock 
 * @param wait 0=>synchronous, 1=>synchronous
 * @return 0 on success, otherwise an error
 */ 
static int cfs_sync_fs(struct super_block *sb, int wait)
{
    struct cfsd_sb *cfsd_arr = NULL;        /*buffer to hold sequence of cfsd_sb entries*/
    struct cfs_node_addr *super_tbl = NULL; /*hold address of superblock table node*/
    struct cfs_sb *cfs_sb = NULL;           /*hold reference to FS-specific sb data*/
    int oldest_sb_ndx;                      /*index of oldest sb entry*/
    struct cfsd_sb *oldest_sb = NULL;       /*hold reference to oldest sb entry*/
    u64 oldest_sb_offset;                   /*offset of oldest sb entry in supertable node(& buffer)*/
    int retval;

    CLYDE_DBG("called\n");
    cfsd_arr = kzalloc(sizeof(struct cfsd_sb)*CLYDE_NUM_SB_ENTRIES, GFP_KERNEL);
    if (!cfsd_arr) {
        CLYDE_ERR("%s - could not allocate memory for reading in persisted superblocks\n", __FUNCTION__);
        retval = -ENOMEM;
        goto err_alloc;
    }

    cfs_sb = CFS_SB(sb);
    super_tbl = &cfs_sb->superblock_tbl;

    /*read superblock table*/
    retval = cfsio_read_node_sync(
        sb->s_bdev, 
        NULL, NULL, 
        super_tbl->tid, super_tbl->nid,
        0, sizeof(struct cfsd_sb)*CLYDE_NUM_SB_ENTRIES, cfsd_arr
    );
    if (retval) {
        CLYDE_ERR("%s - Failed to read superblock entries\n", __FUNCTION__);
        goto err_sb_read;
    }

    /*find oldest entry and compute its offset within the table*/
    oldest_sb_ndx = cfs_get_sb_ndx(cfsd_arr, SB_OLDEST_ENTRY);
    if (oldest_sb_ndx < 0) { /*error reading entry*/
        CLYDE_ERR("%s - failed to get index of oldest sb entry\n", __FUNCTION__);
        retval = -EIO;
        goto err_sb_read;
    }
    oldest_sb = &cfsd_arr[oldest_sb_ndx];
    oldest_sb_offset = sizeof(struct cfsd_sb)*oldest_sb_ndx;

    /*update disk sb struct to current sb values*/
    __copy2d_sb(oldest_sb, cfs_sb);

    /*overwrite the oldest sb entry in the supertable node (@ offset 'oldest_sb_offset')*/
    retval = cfsio_update_node_sync(
        sb->s_bdev, NULL, NULL,
        super_tbl->tid, super_tbl->nid,
        oldest_sb_offset,
        sizeof(struct cfsd_sb),
        oldest_sb
    );
    if (retval) {
        CLYDE_ERR("%s - failed to write new superblock contents to disk\n", __FUNCTION__);
        retval = -EIO;
        goto err_sb_write;
    }

    kfree(cfsd_arr);
    return 0; /*success*/
err_sb_write:
err_sb_read:
    kfree(cfsd_arr);
err_alloc:
    return retval;
}

/** 
 * Free clydefs-specific structures of the superblock. 
 * @param sb superblock of filesystem instance about to be 
 *           unmounted.
 */ 
static void cfs_put_super(struct super_block *sb)
{
    struct cfs_sb *cfs_sb = NULL;
    printk("clydefs: unmounting fs...\n");
    cfs_sb = CFS_SB(sb);
    kfree(cfs_sb->ino_buf);
    kfree(sb->s_fs_info); /*free fs-specific superblock info*/
    sb->s_fs_info = NULL;
}

void __print_disk_sb(struct cfsd_sb *dsb)
{
    printk("printing disk superblock\n:");
    printk("\tfile_tree_tid : raw(%llx) cpu(%llx)\n", dsb->file_tree_tid, le64_to_cpu(dsb->file_tree_tid));
    printk("\tfs_inode_tbl.tid : raw(%llx) cpu(%llx)\n", dsb->fs_inode_tbl.tid, le64_to_cpu(dsb->fs_inode_tbl.tid));
    printk("\tfs_inode_tbl.nid : raw(%llx) cpu(%llx)\n", dsb->fs_inode_tbl.nid, le64_to_cpu(dsb->fs_inode_tbl.nid));
    printk("\tgeneration : raw(%x) cpu(%x)\n", dsb->generation, le32_to_cpu(dsb->generation));
    printk("\tmagic : raw(%x) cpu(%x)\n", dsb->magic_ident, le32_to_cpu(dsb->magic_ident));

    printk("\tfs_ino_tbl.tid : raw(%llx) cpu(%llx)\n", dsb->fs_ino_tbl.tid, le64_to_cpu(dsb->fs_ino_tbl.tid));
    printk("\tfs_ino_tbl.nid : raw(%llx) cpu(%llx)\n", dsb->fs_ino_tbl.nid, le64_to_cpu(dsb->fs_ino_tbl.nid));
    printk("\tino_nxt_free : raw(%llx) cpu(%llx)\n", dsb->ino_nxt_free, le64_to_cpu(dsb->ino_nxt_free));
    printk("\tino_tbl_start : raw(%llx) cpu(%llx)\n", dsb->ino_tbl_start, le64_to_cpu(dsb->ino_tbl_start));
    printk("\tino_tbl_end : raw(%llx) cpu(%llx)\n", dsb->ino_tbl_end, le64_to_cpu(dsb->ino_tbl_end));
}

/** 
 * Initialise the superblock structure.
 * @param sb the superblock structure 
 * @param data -- the parsed mount arguments
 * @param silent whether to report or suppress error messages
 * @note based on 'simple_fill_super' 
 */ 
static int cfs_fill_super(struct super_block *sb, void *data, int silent)
{
    /* 
        FIXME:
            - set magic identifier in superblock (from what we read in)
            - fix inode retrieval etc
     */ 
    struct inode *root;
    struct cfs_sb *cfs_sb = NULL;

    struct cfsd_sb *cfsd_sb_arr = NULL;
    struct cfsd_sb *cfs_disk_sb_newest = NULL;
    struct cfs_mnt_args *mnt_args = data;
    struct block_device *bd = NULL;
    fmode_t bd_mode = FMODE_READ|FMODE_WRITE;
    u64 *ino_buf = NULL;
    int retval;

    CLYDE_ASSERT(data != NULL); /*ensure we were supplied with mount arguments (device, tid&nid of sb)*/
    CLYDE_DBG("mounting superblock tbl @ (%llu,%llu)\n", mnt_args->tid, mnt_args->nid);

    bd = blkdev_get_by_path(mnt_args->dev_path, bd_mode, NULL);
	if (!bd || IS_ERR(bd)) {
        CLYDE_ERR("Failed to mount FS, could not open block device '%s': err(%ld)\n", mnt_args->dev_path, PTR_ERR(bd));
        retval = -ENOENT;
        goto err;
	}
    sb->s_bdev = bd; /*set block device for sb*/

    cfs_sb = kzalloc(sizeof(struct cfs_sb), GFP_KERNEL);
    if (!cfs_sb) {
        CLYDE_ERR("Failed to mount FS, could not allocate extended superblock information\n");
        retval = -ENOMEM;
        goto err_alloc_sb;
    }

    cfsd_sb_arr = kzalloc(sizeof(struct cfsd_sb)*CLYDE_NUM_SB_ENTRIES, GFP_KERNEL);
    if (!cfsd_sb_arr) {
        CLYDE_ERR("Failed to mount FS, could not allocate memory for reading in persisted superblocks\n");
        retval = -ENOMEM;
        goto err_alloc_disk_sb;
    }

    ino_buf = kzalloc(sizeof(u64), GFP_KERNEL);
    if (!ino_buf) {
        CLYDE_ERR("Failed to mount fs, could not allocate ino tbl buffer\n");
        retval = -ENOMEM;
        goto err_alloc_ino_buf;
    }

    retval = cfsio_read_node_sync(
        sb->s_bdev, 
        NULL, NULL, 
        mnt_args->tid, mnt_args->nid,
        0, sizeof(struct cfsd_sb)*CLYDE_NUM_SB_ENTRIES, cfsd_sb_arr
    );
    if (retval) {
        CLYDE_ERR("Failed to mount FS, could not read superblock table\n");
        goto err_read_sb;
    }

    retval = cfs_get_sb_ndx(cfsd_sb_arr, SB_NEWEST_ENTRY);
    if (retval < 0) { /*error*/
        CLYDE_ERR("Failed to mount FS, could not find a superblock with a valid generation number\n");
        retval = -EINVAL;
        goto err_read_sb;
    } else {
        cfs_disk_sb_newest = &cfsd_sb_arr[retval]; /*get the newest sb entry*/
        retval = 0; /*reset for error-detection*/
    }

    CLYDE_DBG("found sb entry\n");

    cfs_sb->superblock_tbl.tid = mnt_args->tid;
    cfs_sb->superblock_tbl.nid = mnt_args->nid;
    cfs_sb->ino_buf = ino_buf;

    sb->s_op = &cfs_super_operations;

    if (le32_to_cpu(cfs_disk_sb_newest->magic_ident) != CFS_MAGIC_IDENT) {
        CLYDE_ERR(
            "superblock magic identifier doesn't match the expected! (got: %x, expected: %x)\n", 
            le32_to_cpu(cfs_disk_sb_newest->magic_ident), CFS_MAGIC_IDENT
        );
        retval = -EINVAL;
        goto err_read_sb;
    }
    sb->s_magic = CFS_MAGIC_IDENT;
    __copy2c_sb(cfs_sb, cfs_disk_sb_newest);

    /*initialise sb locks*/
    spin_lock_init(&(cfs_sb->lock_fs_ino_tbl));

    /*c/m/a time stamps work on second-granulatity*/
    sb->s_time_gran = 1000000000;
    sb->s_blocksize = CFS_BLOCKSIZE;
    sb->s_blocksize_bits = CFS_BLOCKSIZE_SHIFT;

    /*max filesize, set to maximum supported by VFS*/
    sb->s_maxbytes = CFS_MAX_FILESIZE;
    sb->s_max_links = CFS_MAX_LINKS;

    /*link superblock to fs-specific superblock info*/
    sb->s_fs_info = cfs_sb;

    root = cfsi_getroot(sb);
    if (IS_ERR(root)) {
        CLYDE_ERR("Failed to retrieve/read root inode!\n");
        goto err_read_root_inode;
    }

    sb->s_root = d_make_root(root);
    if (!sb->s_root) {
        CLYDE_ERR("Failed to create root dentry\n");
        retval = -ENOMEM;
        goto err_root_dentry;
    }

    if (!S_ISDIR(root->i_mode)) {
		dput(sb->s_root);
		sb->s_root = NULL;
		CLYDE_ERR("Root inode did not set as directory!?\n");
		retval = -EINVAL;
		goto err_root_dirmode;
	}

    /*FIXME -- add bdi info for writeback support if wanted*/
    kfree(cfsd_sb_arr);
    return 0; /*success*/

err_root_dirmode:
err_root_dentry:
    iput(root);
err_read_root_inode:
err_read_sb:
    kfree(ino_buf);
err_alloc_ino_buf:
    kfree(cfsd_sb_arr);
err_alloc_disk_sb:
    kfree(cfs_sb);
err_alloc_sb:
    blkdev_put(bd, bd_mode);
err:
    return retval;
}

/** 
 * Mount clydefs file system. 
 * @param fs_type the file_system_type struct, holding 
 *                references to the mount and umount functions.
 * @param flags supplied mount options 
 * @param device_path the path identifying the backing device 
 * @param data further mount options to be parsed - expect a 
 *             null-terminated string.
 */ 
static struct dentry *cfs_mount(struct file_system_type *fs_type, int flags,
                             char const *device_path, void *data)
{
    struct cfs_mnt_args mnt_args;
    int retval;

    mnt_args.dev_path = device_path;
    retval = __parse_mnt_args(&mnt_args, data);
    if (retval) {
        return ERR_PTR(retval);
    }
    
    /*FIXME look into preventing double-mounting ?*/
    return mount_nodev(fs_type, flags, &mnt_args, cfs_fill_super);
}

/** 
 * Initialise file system. 
 * @description initialises the structures shared between 
 *              individual instances of the file system.
 */ 
int super_init(void)
{
    int retval;

    retval = __inodecache_init();
    if (retval)
        goto err;

    retval = register_filesystem(&clydefs_fs_type);
    if (retval)
        goto err_register_fs;

    return 0; /*success*/

err_register_fs:
    __inodecache_destroy();
err:
    return retval;
}

void super_exit(void)
{
    /*reverse order of super_init*/

    unregister_filesystem(&clydefs_fs_type);
    __inodecache_destroy();
}

void cfs_kill_super(struct super_block *sb)
{
    kill_litter_super(sb);
}

static struct file_system_type clydefs_fs_type = {
    .owner = THIS_MODULE,
    .name = "clydefs",
    .mount = cfs_mount,
    .kill_sb = cfs_kill_super,
    /*.fs_flags = FS_REQUIRES_DEV*/
};

static const struct super_operations cfs_super_operations = {
    .alloc_inode = cfs_alloc_inode,
    .destroy_inode = cfs_destroy_inode,
    .statfs = simple_statfs,
    .put_super = cfs_put_super,
    .sync_fs = cfs_sync_fs,
};

