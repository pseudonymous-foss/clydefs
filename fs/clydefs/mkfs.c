#include <linux/fs.h>
#include <linux/time.h>
#include "mkfs.h"
#include "clydefs.h"
#include "clydefs_disk.h"
#include "io.h"
#include "chunk.h"

extern void __print_disk_sb(struct cfsd_sb *dsb);
/** 
 * Open specified block device for writing 
 * @param ret_bd will hold a reference to the block device, if 
 *               successful
 * @param dev_path path of block device to open 
 * @return 0 on success, -ENOENT if opening the device failed. 
 */ 
static __always_inline __must_check int open_dev(struct block_device **ret_bd, char const * const dev_path)
{
    CLYDE_ASSERT( ret_bd != NULL );
    CLYDE_ASSERT( *ret_bd == NULL );
    
    (*ret_bd) = blkdev_get_by_path(dev_path, FMODE_READ|FMODE_WRITE, NULL);
	if (! (*ret_bd) || IS_ERR( (*ret_bd) )) {
		printk(KERN_ERR "add failed: can't open block device %s: %ld\n", dev_path, PTR_ERR( (*ret_bd) ));
		return -ENOENT;
	}

    return 0; /*success*/
}


/** 
 * Make file system trees. 
 * @param file_tree_tid the file tree identifier 
 * @param inode_tree_tid the inode tree identifier 
 * @param fs_tree_tid the file system tree identifier 
 * @post on success, file- inode- and fs_tree_tid all hold the 
 *       tids of the created trees.
 * @return 0 on success, error otherwise 
 */ 
static __always_inline int cfs_mk_trees(struct block_device *bd, u64 *file_tree_tid, u64 *inode_tree_tid, u64 *fs_tree_tid)
{
    int retval;
    printk("%s called...\n", __FUNCTION__);

    retval = cfsio_create_tree_sync(bd, file_tree_tid);
    if (retval) {
        CLYDE_ERR("Failed to create new file-tree\n");
        goto err_mk_file_tree;
    }

    retval = cfsio_create_tree_sync(bd, inode_tree_tid);
    if (retval) {
        CLYDE_ERR("Failed to create new inode-tree\n");
        goto err_mk_inode_tree;
    }

    retval = cfsio_create_tree_sync(bd, fs_tree_tid);
    if (retval) {
        CLYDE_ERR("Failed to create new fs-tree\n");
        goto err_mk_fs_tree;
    }

    return 0; /*success*/
err_mk_fs_tree:
    if (cfsio_remove_tree_sync(bd, *inode_tree_tid))
        CLYDE_ERR("WARN - fs creation failed, and failed to remove inode_tree(tid:%llu) while recovering\n", *inode_tree_tid);
err_mk_inode_tree:
    if (cfsio_remove_tree_sync(bd, *file_tree_tid))
        CLYDE_ERR("WARN - fs creation failed, and failed to remove file_tree(tid:%llu) while recovering\n", *file_tree_tid);
err_mk_file_tree:
    *file_tree_tid = *inode_tree_tid = *fs_tree_tid = 0;
    return retval;
}

/** 
 * @param ret_sb_tbl_nid will hold the nid of the superblock 
 *                       table in the fs tree.
 * @param bd the block device to write to 
 * @param fs_tree_tid the tree which will hold the superblock 
 *                    table.
 * @param tmp_sb holds the settings to write to all superblock 
 *               entries of the superblock table.
 * @return 0 on success, error otherwise. 
 * @pre fs tree exists and its tid is given by fs_tree_tid. 
 */ 
static __always_inline int cfs_mk_sb_tbl(
    u64 *ret_sb_tbl_nid, struct block_device *bd, u64 fs_tree_tid, 
    struct cfs_sb const * const tmp_sb)
{
    int retval;
    struct cfsd_sb *sb_arr = NULL;
    int i;
    struct cfsd_sb *sb_curr = NULL;

    printk("%s called\n", __FUNCTION__);
    CLYDE_ASSERT(ret_sb_tbl_nid != NULL);
    CLYDE_ASSERT(*ret_sb_tbl_nid == 0);
    CLYDE_ASSERT(bd != NULL);
    CLYDE_ASSERT(fs_tree_tid != 0);
    CLYDE_ASSERT(tmp_sb != NULL);

    retval = cfsio_insert_node_sync(bd, ret_sb_tbl_nid, fs_tree_tid, sizeof(struct cfsd_sb)*CLYDE_NUM_SB_ENTRIES);
    if (retval) {
        CLYDE_ERR("FAILED to create new superblock table node inside fs tree\n");
        goto err_mk_sb_tbl;
    }

    sb_arr = kzalloc(sizeof(struct cfsd_sb)*CLYDE_NUM_SB_ENTRIES, GFP_KERNEL);
    if (!sb_arr) {
        retval = -ENOMEM;
        goto err_alloc_sb_arr;
    }

    sb_curr = sb_arr;
    for (i=0; i<CLYDE_NUM_SB_ENTRIES; i++,sb_curr++) {
        __copy2d_sb(sb_curr, tmp_sb);
        __print_disk_sb(sb_curr);
    }

    retval = cfsio_update_node_sync(
        bd, NULL, NULL, 
        fs_tree_tid, *ret_sb_tbl_nid, 
        0, sizeof(struct cfsd_sb)*CLYDE_NUM_SB_ENTRIES, 
        sb_arr
    );
    if (retval) {
        CLYDE_ERR(
            "%s - failed to write superblock table to fs-tree (tid: %llu, nid:%llu)\n", 
            __FUNCTION__, fs_tree_tid, *ret_sb_tbl_nid
        );
        goto err_write_sb_tbl;
    }

    kfree(sb_arr);
    return 0; /*success*/

err_write_sb_tbl:
    kfree(sb_arr);
err_alloc_sb_arr:
    if (cfsio_remove_node_sync(bd, fs_tree_tid, *ret_sb_tbl_nid)) {
        CLYDE_ERR(
            "%s - failed to unwind changes - could not remove intended sb tbl node from fs tree (tid:%llu, nid:%llu)\n", 
            __FUNCTION__, fs_tree_tid, *ret_sb_tbl_nid
        );
    } else {
        *ret_sb_tbl_nid = 0;
    }
err_mk_sb_tbl:
    return retval;
}

/** 
 *  Make the fs inode table containing just the root directory
 *  entry. In the process, populate and initialise the root
 *  inode entry.
 *  @param inode_tree_tid the tid of the tree in which to place
 *                        the root and fs inode tables.
 *  @return 0 on success, error otherwise
 *  @post on success; the root inode table is initialised and
 *        the root entry is initialised along with its own inode
 *        table.
 *  @post on success; ret_fs_itbl_nid is set to the nid of the
 *        fs inode table.
 */ 
static __always_inline int cfs_mk_fs_itbl(u64 *ret_fs_itbl_nid, struct block_device *bd, u64 inode_tree_tid)
{
    struct cfsd_inode_chunk *chunk = NULL;
    struct cfsd_ientry root_entry;
    u64 root_itbl_nid, fs_itbl_nid, root_entry_ndx = 0;
    int retval;

    printk("%s called\n", __FUNCTION__);
    CLYDE_ASSERT(ret_fs_itbl_nid != NULL);
    CLYDE_ASSERT(*ret_fs_itbl_nid == 0);
    CLYDE_ASSERT(inode_tree_tid != 0);

    chunk = kzalloc(sizeof(struct cfsd_inode_chunk), GFP_KERNEL);
    if (!chunk) {
        retval = -ENOMEM;
        goto err_alloc_chunk;
    }
    cfsc_chunk_init(chunk);

    /*create and write the empty root inode table chunk*/
    retval = cfsio_insert_node_sync(
        bd, &root_itbl_nid, inode_tree_tid, CHUNK_SIZE_DISK_BYTES
    );
    if (retval) {
        CLYDE_ERR("Failed to create node for root's inode table in inode tree (tid:%llu)\n", inode_tree_tid);
        goto err_mk_root_itbl;
    }
    cfsc_chunk_init(chunk);
    retval = cfsc_write_chunk_sync(bd, inode_tree_tid, root_itbl_nid, chunk, 0);
    if (retval) {
        CLYDE_ERR("Failed to write contents of root's inode table in inode tree (tid:%llu,nid:%llu)\n", inode_tree_tid, root_itbl_nid);
        goto err_mk_root_itbl_write;
    }

    /*create the FS inode table*/
    retval = cfsio_insert_node_sync(
        bd, &fs_itbl_nid, inode_tree_tid, sizeof(struct cfsd_inode_chunk)
    );
    if (retval) {
        CLYDE_ERR("Failed to create node for fs inode table in inode tree (tid:%llu)\n", inode_tree_tid);
        goto err_mk_fs_itbl;
    }

    /*set up root inode entry and write it to the fs inode table*/
    root_entry.ino = cpu_to_le64(CFS_INO_ROOT);
    root_entry.uid = 0;
    root_entry.gid = 0;
    root_entry.ctime = root_entry.mtime = cpu_to_le64( get_seconds() );
    /*all inodes start with just one chunk*/
    root_entry.size_bytes = cpu_to_le64(CHUNK_SIZE_DISK_BYTES);
    root_entry.data_nid = cpu_to_le64(inode_tree_tid);
    root_entry.icount = 0;
    root_entry.mode = cpu_to_le16(S_IFDIR | 0755);
    strcpy(root_entry.name, "/");
    root_entry.nlen = strlen(root_entry.name);

    if ( cfsc_chunk_entry_insert(&root_entry_ndx, chunk, &root_entry) ){
        /*we cannot fail inserting into an empty chunk, must be a programming error*/
        CFS_DBG("Failed inserting root_entry into newly allocated, empty chunk\n");
        BUG();
    }
    if (root_entry_ndx != 0) {
        CFS_DBG("root entry must be written to the first entry, by convention - programming error!\n");
        BUG();
    }

    smp_mb();
    retval = cfsc_write_chunk_sync(bd, inode_tree_tid, fs_itbl_nid, chunk, 0);
    smp_mb();
    if (retval) {
        CLYDE_ERR("Failed to write contents to fs inode table (tid:%llu, nid:%llu) (retval:%d)\n", inode_tree_tid, fs_itbl_nid, retval);
        goto err_mk_fs_itbl_write;
    }

    *ret_fs_itbl_nid = fs_itbl_nid; /*return nid of the fs inode table*/
    kfree(chunk);
    return 0; /*success*/

err_mk_fs_itbl_write:
    if (cfsio_remove_node_sync(bd, inode_tree_tid, fs_itbl_nid)) {
        CLYDE_ERR(
            "%s - failure to unwind changes, could not remove created fs inode table node (tid:%llu,nid:%llu)\n", 
            __FUNCTION__, inode_tree_tid, fs_itbl_nid
        );
    }
err_mk_fs_itbl:
err_mk_root_itbl_write:
    if (cfsio_remove_node_sync(bd, inode_tree_tid, root_itbl_nid)) {
        CLYDE_ERR(
            "%s - failure to unwind changes, could not remove created root inode table node (tid:%llu,nid:%llu)\n", 
            __FUNCTION__, inode_tree_tid, root_itbl_nid
        );
    }
err_mk_root_itbl:
    kfree(chunk);
err_alloc_chunk:
    *ret_fs_itbl_nid = 0;
    return retval;
}

/** 
 * Create a new ClydeFS filesystem instance on the block device 
 * located at 'dev_path' 
 * @param ret_fs_sb_tbl where to store the address of the 
 *                      superblock table node.
 * @param dev_path path to TREE-CMD supporting block device 
 * @return 0 on success, error otherwise. 
 * @post on success; ret_fs_sb_tbl points to the newly created 
 *       file system's superblock node.
 */ 
int cfsfs_create(struct cfs_node_addr *ret_fs_sb_tbl, char const * const dev_path)
{
    struct block_device *bd = NULL;
    u64 file_tree_tid;                  /*the tree tids*/
    u64 inode_tree_tid;
    u64 fs_tree_tid;     
    u64 fs_itbl_nid = 0;                /*filesystem inode table nid*/
    u64 ino_tbl_nid = 0;                /*node for storing reclaimed INO's from deleted inodes*/
    struct cfs_sb tmp_sb;               /*initial superblock settings*/
    u64 sb_tbl_nid = 0;                 /*nid of created superblock table in fs tree*/
    int retval;

    if ((retval = open_dev(&bd, dev_path))) {
        goto err_bdev;
    }

    printk("Making file-, inode- and super-trees.\n");
    retval = cfs_mk_trees(bd, &file_tree_tid, &inode_tree_tid, &fs_tree_tid);
    if (retval) {
        CLYDE_ERR("%s - failed make file system trees\n", __FUNCTION__);
        goto err_mk_trees;
    }
    CLYDE_ASSERT(file_tree_tid != 0);
    CLYDE_ASSERT(inode_tree_tid != 0);
    CLYDE_ASSERT(fs_tree_tid != 0);

    retval = cfs_mk_fs_itbl(&fs_itbl_nid, bd, inode_tree_tid);
    if (retval) {
        CLYDE_ERR("%s - failed inode tables for fs and root dir\n", __FUNCTION__);
        goto err_mk_itbls;
    }
    CLYDE_ASSERT(fs_itbl_nid != 0);

    retval = cfsio_insert_node_sync(bd,&ino_tbl_nid, fs_tree_tid, 1024*1024);
    if (retval) {
        CLYDE_ERR("%s - failed to create inode reclamation tbl\n", __FUNCTION__);
        goto err_mk_ino_tbl;
    }
    CLYDE_ASSERT(ino_tbl_nid != 0);

    /*Populate the superblock tbl with settings*/
    tmp_sb.file_tree_tid = file_tree_tid;
    tmp_sb.fs_inode_tbl.tid = inode_tree_tid;
    tmp_sb.fs_inode_tbl.nid = fs_itbl_nid;
    tmp_sb.generation = 1;
    tmp_sb.magic_ident = CFS_MAGIC_IDENT;

    tmp_sb.fs_ino_tbl.tid = fs_tree_tid;
    tmp_sb.fs_ino_tbl.nid = ino_tbl_nid;
    tmp_sb.ino_nxt_free = CFS_INO_MIN;
    tmp_sb.ino_tbl_start = tmp_sb.ino_tbl_end = 0; /*empty*/
    

    retval = cfs_mk_sb_tbl(&sb_tbl_nid, bd, fs_tree_tid, &tmp_sb);
    if (retval) {
        CLYDE_ERR("%s - failed to create superblock table\n", __FUNCTION__);
        goto err_mk_sb_tbl;
    }

    CLYDE_ASSERT(sb_tbl_nid != 0);

    /*done*/
    ret_fs_sb_tbl->tid = fs_tree_tid;
    ret_fs_sb_tbl->nid = sb_tbl_nid;
    goto success;

err_mk_sb_tbl: /*only nodes are created by cfs_mk_trees -- we expect deleting the trees to handle this*/
    if (cfsio_remove_node_sync(bd, fs_tree_tid, ino_tbl_nid))
        CLYDE_ERR(
            "WARN- fs creation failed, and failed to remove inode reclamation table node while recovering (tid:%llu,nid:%llu)\n",
            fs_tree_tid, ino_tbl_nid
        );
err_mk_ino_tbl:
err_mk_itbls:
    if (cfsio_remove_tree_sync(bd, file_tree_tid))
        CLYDE_ERR("WARN - fs creation failed, and failed to remove file_tree(tid:%llu) while recovering\n", file_tree_tid);
    if (cfsio_remove_tree_sync(bd, inode_tree_tid))
        CLYDE_ERR("WARN - fs creation failed, and failed to remove inode_tree(tid:%llu) while recovering\n", inode_tree_tid);
    if (cfsio_remove_tree_sync(bd, fs_tree_tid))
        CLYDE_ERR("WARN - fs creation failed, and failed to remove fs_tree(tid:%llu) while recovering\n", fs_tree_tid);
err_mk_trees:
success:
    blkdev_put(bd, FMODE_READ|FMODE_WRITE);
err_bdev:
    return retval;
}

/** 
 * Delete filesystem identified by superblock address. 
 * @param dev_path path to the TREE-CMD supporting block device 
 *                 containing the file system.
 * @param superblock_addr the tid and nid identifying the 
 *                        location of the filesystem superblock.
 * @return 0 on success, error otherwise. 
 */ 
int cfsfs_destroy(
    char const * const dev_path, 
    struct cfs_node_addr const * const superblock_addr)
{
    printk("STUB!! %s is not implemented!\n", __FUNCTION__);
    return -ENOSYS;
}
