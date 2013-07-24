#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/ctype.h>
#include <linux/string.h>
#include <linux/fs.h>
#include <linux/blkdev.h>

#define VERSION "10"
#define nelem(A) (sizeof (A) / sizeof (A)[0])
#define DEV_PATH_LEN 256
#define TAG_LEN 32

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Jesper Madsen <jmad@itu.dk>");
MODULE_DESCRIPTION("AoE Test Driver, exposing a SysFS interface to test modified AoE driver");
MODULE_VERSION(VERSION);

static struct kmem_cache *tree_iface_pool = NULL;
static struct bio_set *bio_pool = NULL;

/** 
 * extended bio data for the tree-based
 * interface.
 */ 
struct tree_iface_data {
    u8 cmd;         /*one of the vendor-specific AOECMD_* codes*/
    u64 tid;
    u64 nid;
    u64 off;
    u64 len;
};

static void aoetest_release(struct kobject *kobj);
static struct kobj_type aoetest_ktype_device;

struct aoedev {
    struct kobject kobj;
    /*ref to blockdevice*/
    struct block_device *blkdev;
    /*path to device*/
    char dev_path[DEV_PATH_LEN];
    /*tag to identify device by*/
    char tag[TAG_LEN];
    /*next device*/
    struct aoedev *next;
};

/*kobject for the module itself*/
static struct kobject aoetest_kobj;
/*points to head the aoedev list, one entry per aoedev added*/
static struct aoedev *devlist = NULL;

/*to enforce order when adding/modifying aoedev refs*/
static spinlock_t lock;

/*shorthand container for sysfs entries*/
struct aoetest_sysfs_entry {
    struct attribute attr;
    ssize_t (*show)(struct aoedev *, char *);
	ssize_t (*store)(struct aoedev *, const char *, size_t);
};

enum BIO_TYPE{
    ATA_BIO,
    TREE_BIO,
};

enum AOE_CMD {
    /*Support our vendor-specific codes*/
    AOECMD_CREATETREE = 0xF0,   /*create a new tree*/
    AOECMD_REMOVETREE,          /*remove a tree and all its child nodes*/
    AOECMD_READNODE,            /*read data from an node*/
    AOECMD_INSERTNODE,          /*create a new node with some initial data*/
    AOECMD_UPDATENODE,          /*update the data of an existing node*/
    AOECMD_REMOVENODE,          /*remove the node and associated data*/
};

struct submit_syncbio_data {
	struct completion event;
	int error;
};



static __always_inline int is_tree_bio(struct bio *b)
{
    return (b->bi_treecmd != NULL);
}

static __always_inline void cleanup_if_treebio(struct bio *b)
{
    /*all done, cleanup time*/
    if (is_tree_bio(b)) { 
        kmem_cache_free(tree_iface_pool, b->bi_private);
        b->bi_private = NULL;
    }
}

/**
 * Called when a sync bio is finished.
 * @description a function which populates some fields in
 *              preparation for the end of a synchronous bio.
 * @param b the finished bio which was intended to be
 *          synchronous
 * @param error error code of bio, 0 if no error occurred.
 */
void submit_bio_syncio(struct bio *b, int error)
{
	struct submit_syncbio_data *ret = b->bi_private;

	ret->error = error;
	complete(&ret->event);

    /*FIXME this is kind of stupid, just let the user manually clean up 
      by first issuing get_bio(b), then calling cleanup_if_treebio himself*/
    cleanup_if_treebio(b); 
}

/**
 * Called when a bio is finished.
 * @description bio's allocated via the alloc_bio helper method 
 *              will have this function called when they are
 *              finished.
 * @param b the finished bio 
 * @param error error code of bio, 0 if no error occurred.
 */
void alloc_bio_end_fnc(struct bio *b, int error)
{
    /*FIXME this is kind of stupid, just let the user manually clean up 
      by first issuing get_bio(b), then calling cleanup_if_treebio himself*/
    cleanup_if_treebio(b); 
}

/** 
 * Allocate bios for both ATA and TREE commands. 
 * @param bt the type of BIO desired, ATA_BIO or TREE_BIO
 * @return NULL on allocation error, otherwise a bio. If a TREE 
 *         bio, the bi_treecmd field points to a struct
 *         tree_iface_data.
 * @note it is your responsibility to call bio_put() once the 
 *       bio is submitted and you're otherwise done with it.
 * @note to issue a valid tree cmd, b->bi_treecmd needs 
 *       additional data.
 */ 
struct bio *alloc_bio(enum BIO_TYPE bt)
{
    struct bio *b = NULL;
    b = bio_alloc_bioset(GFP_KERNEL, 1, bio_pool);
    if (!b)
        goto err_alloc_bio;

    bio_get(b); /*ensure bio won't disappear on us*/

    if (bt == TREE_BIO) {
        struct tree_iface_data *td = NULL;
        td = kmem_cache_alloc(tree_iface_pool, GFP_KERNEL);
        if (!td)
            goto err_alloc_tree_iface;
        memset(td, 0, sizeof *td);

        b->bi_treecmd = td;

        b->bi_sector = 0; /*using td->off to mark offsets for read/write*/
        
    } else {
        b->bi_treecmd = NULL;
    }

    b->bi_end_io = &alloc_bio_end_fnc;

    return b;

err_alloc_tree_iface:
    bio_put(b);
err_alloc_bio:
    return NULL;
}

/**
 * Submit a bio and wait for its completion. 
 * @description wraps submit_bio into a synchronous call, using only the bi_private field. 
 * @param bio the bio to wait for 
 * @param rw read/write flag, accepts REQ_* values & WRITE
 * @return the error code of the completed bio                 
 */ 
int submit_bio_sync(struct bio *bio, int rw)
{
    struct submit_syncbio_data ret;

	rw |= REQ_SYNC;
	/*initialise queue*/
    ret.event.done = 0;
    init_waitqueue_head(&ret.event.wait);

	bio->bi_private = &ret;
	bio->bi_end_io = submit_bio_syncio;
	submit_bio(rw, bio);
	wait_for_completion(&ret.event);

	return ret.error;
}



/** 
 * @param p the raw argument string 
 * @param argv an array of character pointers. To hold the start 
 *             of a word each (word: a series of characters
 * @param argv_max maximum number of individual arguments (the 
 *                 number of character pointers in argv)
 * @return the number of individual arguments parsed from the 
 *         string, stored in argv[0] and up
*/ 
static ssize_t aoetest_sysfs_args(char *p, char *argv[], int argv_max)
{
	int argc = 0;

	while (*p) {
		while (*p && isspace(*p))
			++p;
		if (*p == '\0')
			break;
		if (argc < argv_max)
			argv[argc++] = p;
		else {
			printk(KERN_ERR "too many args!\n");
			return -1;
		}
		while (*p && !isspace(*p))
			++p;
		if (*p)
			*p++ = '\0';
	}
	return argc;
}

static ssize_t __aodev_add_dev(char *dev_path , char *tag)
{
	struct block_device *bd;
	struct aoedev *d, *curr_d;
    int ret = 0;

	printk("__aodev_add_dev\n");

	bd = blkdev_get_by_path(dev_path, FMODE_READ|FMODE_WRITE, NULL);
	if (!bd || IS_ERR(bd)) {
		printk(KERN_ERR "add failed: can't open block device %s: %ld\n", dev_path, PTR_ERR(bd));
		return -ENOENT;
	}

	if (get_capacity(bd->bd_disk) == 0) {
		printk(KERN_ERR "add failed: zero sized block device.\n");
		ret = -ENOENT;
		goto err;
	}

	spin_lock(&lock);
    /*Guard against adding the same device multiple times*/
    for (curr_d = devlist; curr_d; curr_d = curr_d->next) {
        if( strncmp(dev_path, curr_d->dev_path, DEV_PATH_LEN) == 0) {
            spin_unlock(&lock);
            printk(KERN_ERR "device already added to AoE Test module (%s)\n", dev_path);
            ret = -EEXIST;
            goto err;
        }
    }

	d = kmalloc(sizeof(struct aoedev), GFP_KERNEL);
	if (!d) {
		printk(KERN_ERR "add failed: kmalloc error for '%s'\n", dev_path);
		ret = -ENOMEM;
		goto err;
	}

	memset(d, 0, sizeof(struct aoedev));
	d->blkdev = bd;
	strncpy(d->dev_path, dev_path, nelem(d->dev_path)-1);
    strncpy(d->tag, tag, nelem(d->tag)-1);
	
	kobject_init_and_add(&d->kobj, &aoetest_ktype_device, &aoetest_kobj, "%s", tag);
    
    /*prepend dev to devlist*/
	d->next = devlist;
	devlist = d;
	spin_unlock(&lock);

	printk("Exposed TREE/ATA interface of device '%s', tagged: '%s'\n", d->dev_path, d->tag);
	return 0;
err:
	blkdev_put(bd, FMODE_READ|FMODE_WRITE);
	return ret;
}

static ssize_t aoedev_store_add(struct aoedev *dev, const char *page, size_t len)
{
	int error = 0;
	char *argv[16];
	char *p;

	p = kmalloc(len+1, GFP_KERNEL);
	memcpy(p, page, len);
	p[len] = '\0';
	
	if (aoetest_sysfs_args(p, argv, nelem(argv)) != 2) {
		printk(KERN_ERR "bad arg count for add\n");
		error = -EINVAL;
	} else
		error = __aodev_add_dev(argv[0], argv[1]); /*dev_path, tag*/

	kfree(p);
	return error ? error : len;
}
static struct aoetest_sysfs_entry aoedev_sysfs_add = __ATTR(add, 0644, NULL, aoedev_store_add);

static ssize_t __aodev_del_dev(char *tag)
{
	struct aoedev *d, **b;
	int ret;

	b = &devlist;
	d = devlist;
	spin_lock(&lock);
	
	for (; d; b = &d->next, d = *b) {
        if (strncmp(tag, d->tag, TAG_LEN) == 0) {
            break;
        }
    }

	if (d == NULL) {
		printk(KERN_ERR "del failed: no dekmem_tree_iface_cachevice by tag %s not found.\n", 
			tag);
		ret = -ENOENT;
		goto err;
	}

	*b = d->next;
	
	spin_unlock(&lock);
	
	blkdev_put(d->blkdev, FMODE_READ|FMODE_WRITE);
	
	kobject_del(&d->kobj);
	kobject_put(&d->kobj);
	
	return 0;
err:
	spin_unlock(&lock);
	return ret;
}

static ssize_t aoedev_store_del(struct aoedev *dev, const char *page, size_t len)
{
	int error = 0;
	char *argv[16];
	char *p;

	p = kmalloc(len+1, GFP_KERNEL);
	memcpy(p, page, len);
	p[len] = '\0';

	if (aoetest_sysfs_args(p, argv, nelem(argv)) != 1) {
		printk(KERN_ERR "bad arg count for del\n");
		error = -EINVAL;
	} else
		error = __aodev_del_dev(argv[0]);

	kfree(p);
	return error ? error : len;
}
static struct aoetest_sysfs_entry aoedev_sysfs_del = __ATTR(del, 0644, NULL, aoedev_store_del);

/*module-level attrs*/
static struct attribute *aoetest_ktype_module_attrs[] = {
	&aoedev_sysfs_add.attr,
	&aoedev_sysfs_del.attr,
	NULL,
};

static ssize_t show_devpath(struct aoedev *dev, char *page)
{
    return sprintf(page, "%s\n", dev->dev_path);
}

/*
static ssize_t store_model(struct aoedev *dev, const char *page, size_t len)
{
	spncpy(dev->model, page, nelem(dev->model));
	return 0;
} 
*/
static struct aoetest_sysfs_entry aoetest_sysfs_devpath = __ATTR(tag, 0644, show_devpath, NULL);
 
static ssize_t show_createtree(struct aoedev *dev, char *page)
{
    /*create a tree, output the result*/
    struct bio *b = NULL;
    struct tree_iface_data *td = NULL;
    //u64 tid = 0;

    b = alloc_bio(TREE_BIO);
    if (!b)
        goto err_alloc;

    td = (struct tree_iface_data *) b->bi_treecmd;
    td->cmd = AOECMD_CREATETREE;
    b->bi_bdev = dev->blkdev;
    
    if (submit_bio_sync(b, 0)) /*FIXME -- what do write for RW??*/
        goto err_bio;
    
    return sprintf(page, "create_tree :: hole\n"); /*FIXME : how to grab the response tid ? */

err_alloc:
    return sprintf(page, "-2\n");
err_bio:
    return sprintf(page, "-1\n");
}
static struct aoetest_sysfs_entry aoetest_sysfs_createtree = __ATTR(create_tree, 0644, show_createtree, NULL);

/*device-level attrs*/
static struct attribute *aoetest_ktype_device_attrs[] = {
    &aoetest_sysfs_devpath.attr,
    &aoetest_sysfs_createtree.attr,
    NULL,
};

static ssize_t aoetest_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct aoetest_sysfs_entry *entry;
	struct aoedev *dev;

	entry = container_of(attr, struct aoetest_sysfs_entry, attr);
	dev = container_of(kobj, struct aoedev, kobj);

	if (!entry->show)
		return -EIO;

	return entry->show(dev, page);
}

static ssize_t aoetest_attr_store(struct kobject *kobj, struct attribute *attr,
			const char *page, size_t length)
{
	ssize_t ret;
	struct aoetest_sysfs_entry *entry;

	entry = container_of(attr, struct aoetest_sysfs_entry, attr);

	if (kobj == &aoetest_kobj) /*module-level*/
		ret = entry->store(NULL, page, length);
	else { /*device-level*/
		struct aoedev *dev = container_of(kobj, struct aoedev, kobj);

		if (!entry->store)
			return -EIO;

		ret = entry->store(dev, page, length);
	}

	return ret;
}

/*show(read), store(write) functions for module-level and device-level settings alike*/
static const struct sysfs_ops aoetest_sysfs_ops = {
	.show		= aoetest_attr_show,
	.store		= aoetest_attr_store,
};

/*top-level => module-level */
static struct kobj_type aoetest_ktype_module = {
	.default_attrs	= aoetest_ktype_module_attrs,
	.sysfs_ops		= &aoetest_sysfs_ops, /*show/store for module-level interface files*/
	.release		= aoetest_release, /*No-OP*/
};

/*device-level */
static struct kobj_type aoetest_ktype_device = {
	.default_attrs	= aoetest_ktype_device_attrs,
	.sysfs_ops		= &aoetest_sysfs_ops, /*show/store for module-level interface files*/
	.release		= aoetest_release, /*No-OP*/
};

static void aoetest_release(struct kobject *kobj)
{} /*NO-OP*/


static __exit void
aoe_exit(void)
{
    kobject_del(&aoetest_kobj);
	kobject_put(&aoetest_kobj);

    if (tree_iface_pool)
        kmem_cache_destroy(tree_iface_pool);
    if (bio_pool)
        bioset_free(bio_pool);

	return;
}

static __init int
aoe_init(void)
{
    spin_lock_init(&lock);
    kobject_init_and_add(&aoetest_kobj, &aoetest_ktype_module, NULL, "aoetest");
    
    tree_iface_pool = kmem_cache_create("tree_iface_data", sizeof (struct tree_iface_data), 0, 0, NULL);
    if (tree_iface_pool == NULL)
        goto err_alloc_treepool;
    
    bio_pool = bioset_create(100, 0); /*pool_size, front_padding (if using larger structure than a bio)*/
    if (bio_pool == NULL) {
        goto err_alloc_biopool;
    }

    return 0;

err_alloc_biopool:
    kmem_cache_destroy(tree_iface_pool);
err_alloc_treepool:
    return -ENOMEM;
}

module_init(aoe_init);
module_exit(aoe_exit);
