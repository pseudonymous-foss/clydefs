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

extern const struct inode_operations clydefs_dir_inode_ops;
extern struct file_operations clydefs_dir_file_ops;

struct dentry *clydefs_mount(struct file_system_type *fs_type, int flags,
                             const char *device_path, void *data);
static int clydefs_fill_super(struct super_block *sb, void *data, int silent);

/*Caches*/
static struct kmem_cache *cfssup_inode_pool = NULL;

/*Structures*/
static struct file_system_type clydefs_fs_type;
static const struct super_operations clydefs_super_operations;

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
static struct inode *cfssup_alloc_inode(struct super_block *sb)
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
	kmem_cache_free(cfssup_inode_pool, cfs_i(inode));
}

/** 
 * Schedule deallocation of inode. 
 * @param inode the inode to deallocate 
 * @note cfssup_cb_free_inode will be called once no more 
 *       readers reference the inode.
 */ 
static void cfssup_destroy_inode(struct inode *inode)
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

/** 
 * Get a pointer to the newest of the superblocks 
 * @param sb_arr a pointer to an array of superblock disk 
 *               representations.
 * @pre sb_arr is a pointer to an array of CLYDE_NUM_SB_ENTRIES 
 *      superblocks
 * @return a pointer to the newest of the superblocks or an 
 *         ERR_PTR in case of parsing invalid superblocks
 */ 
static __always_inline struct cfs_disk_sb *__get_newest_sb(struct cfs_disk_sb **sb_arr)
{
    unsigned int i;
    unsigned int newest_ndx;
    u32 newest_gen = 0;
    CLYDE_ASSERT(sb_arr != NULL);
    for (i = 0; i < CLYDE_NUM_SB_ENTRIES; i++) {
        if ( __le32_to_cpu(sb_arr[i]->generation) > newest_gen ) {
            newest_ndx = i;
            newest_gen = __le32_to_cpu(sb_arr[i]->generation);
        }
    }

    if ( unlikely(newest_ndx == 0 || newest_gen == 0) ) {
        /*no sb found with newer gen, should NEVER happen 
          as start-generation should be 1*/
        CLYDE_ERR("None of the superblocks had a generation number past 0 - aborting mount\n");
        return ERR_PTR(-EINVAL);
    } else {
        return sb_arr[i];
    }
}

/** 
 * Initialise the superblock structure 
 * @param sb the superblock structure 
 * @param data -- the parsed mount arguments
 * @param silent whether to report or suppress error messages
 * @note based on 'simple_fill_super' 
 */ 
static int clydefs_fill_super(struct super_block *sb, void *data, int silent)
{
    /* 
        FIXME:
            - set magic identifier in superblock (from what we read in)
            - fix inode retrieval etc
     */ 
    struct inode *inode;
    struct dentry *root;
    struct cfs_sb *cfs_sb = NULL;

    struct cfs_disk_sb **cfs_disk_sbs = NULL;
    struct cfs_disk_sb *cfs_disk_sb_newest = NULL;
    struct cfs_mnt_args *mnt_args = data;
    struct block_device *bd = NULL;
    fmode_t bd_mode = FMODE_READ|FMODE_WRITE;
    int retval;

    CLYDE_ASSERT(data != NULL); /*ensure we were supplied with mount arguments (device, tid&nid of sb)*/

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

    cfs_disk_sbs = kzalloc(sizeof(struct cfs_disk_sb)*CLYDE_NUM_SB_ENTRIES, GFP_KERNEL);
    if (!cfs_disk_sbs) {
        CLYDE_ERR("Failed to mount FS, could not allocate memory for reading in persisted superblocks\n");
        retval = -ENOMEM;
        goto err_alloc_disk_sb;
    }

    retval = cfsio_read_node_sync(
        sb->s_bdev, 
        NULL, NULL, 
        mnt_args->tid, mnt_args->nid,
        0, sizeof(struct cfs_disk_sb)*CLYDE_NUM_SB_ENTRIES, cfs_disk_sbs
    );
    if (retval) {
        CLYDE_ERR("Failed to mount FS, could not read superblock table\n");
        goto err_read_sb;
    }

    cfs_disk_sb_newest = __get_newest_sb(cfs_disk_sbs);
    if ( IS_ERR(cfs_disk_sb_newest) ) {
        CLYDE_ERR("Failed to mount FS, could not find a superblock with a valid generation number\n");
        retval = -EINVAL;
        goto err_read_sb;
    }

    cfs_sb->superblock_tbl.tid = mnt_args->tid;
    cfs_sb->superblock_tbl.nid = mnt_args->nid;

    

    sb->s_op = &clydefs_super_operations;

    /*sb->s_magic = CFS_MAGIC_IDENT;        FIXME set magic ident*/

    /*c/m/a time stamps work on second-granulatity*/
    sb->s_time_gran = 1000000000;
    sb->s_blocksize = CFS_BLOCKSIZE;
    sb->s_blocksize_bits = CFS_BLOCKSIZE_SHIFT;

    /*max filesize, set to maximum supported by VFS*/
    sb->s_maxbytes = CFS_MAX_FILESIZE;
    sb->s_max_links = CFS_MAX_LINKS;

    /*link superblock to fs-specific superblock info*/
    sb->s_fs_info = cfs_sb;

    inode = new_inode(sb);
    if (!inode)
        return -ENOMEM;

    inode->i_ino = 1; /*Ensure inode number '1' is reserved!*/
    inode->i_mode = S_IFDIR | 755;
    inode->i_atime = inode->i_mtime = inode->i_ctime = CURRENT_TIME; /*TODO */
    inode->i_op = &clydefs_dir_inode_ops;
    inode->i_fop = &clydefs_dir_file_ops;
    set_nlink(inode, 2);
    root = d_make_root(inode);
    if (!root)
        return -ENOMEM;
    
    sb->s_root = root;
    return 0; /*success*/

err_read_sb:    
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
struct dentry *clydefs_mount(struct file_system_type *fs_type, int flags,
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
    return mount_nodev(fs_type, flags, &mnt_args, clydefs_fill_super);
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

void clydefs_kill_super(struct super_block *sb)
{
    kill_litter_super(sb);
}

static struct file_system_type clydefs_fs_type = {
    .owner = THIS_MODULE,
    .name = "clydefs",
    .mount = clydefs_mount,
    .kill_sb = clydefs_kill_super,
    /*.fs_flags = FS_REQUIRES_DEV*/
};

static const struct super_operations clydefs_super_operations = {
    .alloc_inode = cfssup_alloc_inode,
    .destroy_inode = cfssup_destroy_inode,
    .statfs = simple_statfs,
};

