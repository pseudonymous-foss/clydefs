#include <linux/kernel.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include "clydefs.h"
#include "mkfs.h"

#define nelem(A) (sizeof (A) / sizeof (A)[0])

static struct kobject cfs_kobj;
static struct kmem_cache *cfs_obj_pool = NULL;

struct cfs_obj {
    struct kobject kobj;
};

struct cfs_sysfs_entry {
    struct attribute attr;
    ssize_t (*show)(struct cfs_obj *, char *);
	ssize_t (*store)(struct cfs_obj *, const char *, size_t);
};

/** 
 * Parse sysfs arguments. 
 * @param p the raw argument string 
 * @param argv an array of character pointers. To hold the start 
 *             of a word each (word: a series of characters
 * @param argv_max maximum number of individual arguments (the 
 *                 number of character pointers in argv)
 * @return the number of individual arguments parsed from the 
 *         string, stored in argv[0] and up
*/ 
static ssize_t __parse_sysfs_args(char *p, char *argv[], int argv_max)
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

/** 
 * split arg string into words, storing each pointer in argv. 
 * @param page the input string to be parsed and split into 
 *             words
 * @param len the length of the input string 
 * @param p will hold the memory allocation made by this
 *          function.
 * @param argv an array of char*, will have a non-null entry for 
 *             each word found in the input string given by
 *             'page'
 * @param argv_max maximum entries that argv can hold. 
 * @pre '*p' is NULL 
 * @pre 'page' points to the start of a c-string 
 * @pre 'argv' is an allocated array of char* of at least as 
 *      great a size as the number of arguments expected
 * @note you are responsible for releasing the memory pointed to 
 *       by *p using kfree, provided the function returns
 *       successfully
 * @return -ENOMEM if allocation fails, otherwise the number of 
 *         words found.
 */ 
static ssize_t cfs_parse_sysfs_args(const char *page, size_t len, char **p, char *argv[], int argv_max)
{
	*p = kmalloc(len+1, GFP_ATOMIC);
    if (*p == NULL) {
        printk(KERN_ERR "aoedev_store_add: could not allocate memory for string buffer\n");
        return -ENOMEM;
    }
	memcpy(*p, page, len);
	(*p)[len] = '\0';
	
    return __parse_sysfs_args(*p, argv, argv_max);
}

static ssize_t cfs_fs_add(struct cfs_obj *g, const char *page, size_t len)
{
    int error = 0;
	char *argv[16];
	char *p = NULL;
    struct cfs_node_addr sb_tbl_addr;
    int numargs = cfs_parse_sysfs_args(page,len,&p,argv,nelem(argv));

    if (unlikely(numargs < 0)) {
        CLYDE_ERR("%s - failed to parse arguments (numargs:%d)\n", __FUNCTION__, numargs);
        goto parse_args_err;
    } else if (numargs != 1) {
        CLYDE_ERR("expected exactly one argument, the device path");
        error = -EINVAL;
    } else {
        
        error = cfsfs_create(&sb_tbl_addr,argv[0]); /*argv[0] => dev_path*/
        printk(
            "ClydeFS new FS instance created! Superblock table located at (tid:%llu,nid:%llu)\n", 
            sb_tbl_addr.tid, sb_tbl_addr.nid
        );
    }

	kfree(p);
	return error ? error : len;
parse_args_err:
    return -EINVAL;
}
static struct cfs_sysfs_entry fs_sysfs_add = __ATTR(add, 0644, NULL, cfs_fs_add);


static ssize_t cfs_fs_del(struct cfs_obj *g, const char *page, size_t len)
{
    struct cfs_node_addr superblock_addr;
    int error = 0;
	char *argv[16];
	char *p = NULL;
    int numargs = cfs_parse_sysfs_args(page,len,&p,argv,nelem(argv));

    if (unlikely(numargs < 0)) {
        CLYDE_ERR("%s - failed to parse arguments (numargs:%d)\n", __FUNCTION__, numargs);
        goto err;
    } else if (numargs != 3) {
        CLYDE_ERR("expected 3 arguments: device path, superblock tid, superblock nid\n");
        error = -EINVAL;
        goto err;
    }

    if (kstrtou64(argv[1],10,&superblock_addr.tid)){
        error = -EINVAL;
        goto err_numparse;
    }
    if (kstrtou64(argv[2],10,&superblock_addr.nid)){
        error = -EINVAL;
        goto err_numparse;
    }
    error = cfsfs_destroy(argv[0], &superblock_addr); /*argv[0] => dev_path*/


	kfree(p);
	return error ? error : len;
err_numparse:
   kfree(p); 
err:
    return -EINVAL;
}
static struct cfs_sysfs_entry fs_sysfs_del = __ATTR(del, 0644, NULL, cfs_fs_del);

static void cfs_release(struct kobject *kobj)
{} /*NO-OP*/

/*module-level attrs*/
static struct attribute *cfssys_ktype_attrs[] = {
	&fs_sysfs_add.attr,
	&fs_sysfs_del.attr,
	NULL,
};

static ssize_t cfs_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct cfs_sysfs_entry *entry;
	struct cfs_obj *g;

	entry = container_of(attr, struct cfs_sysfs_entry, attr);
	g = container_of(kobj, struct cfs_obj, kobj);

	if (!entry->show)
		return -EIO;

	return entry->show(g, page);
}

static ssize_t cfs_attr_store(struct kobject *kobj, struct attribute *attr,
			const char *page, size_t length)
{
	struct cfs_sysfs_entry *entry;

	entry = container_of(attr, struct cfs_sysfs_entry, attr);
    
    if (kobj != &cfs_kobj) {
        CLYDE_ERR("cfs sysfs code doesn't support non-module level functions\n");
        return -EIO;
    }

    return entry->store(NULL, page, length);
}

/*show(read), store(write) functions*/
static const struct sysfs_ops cfs_sysfs_ops = {
	.show		= cfs_attr_show,
	.store		= cfs_attr_store,
};

static struct kobj_type cfssys_ktype = {
    .default_attrs = cfssys_ktype_attrs,
    .sysfs_ops = &cfs_sysfs_ops,
    .release = cfs_release,
};

int cfssys_init(void)
{
    int retval;

    cfs_obj_pool = kmem_cache_create("cfs_obj", sizeof (struct cfs_obj), 0, 0, NULL);
    if (!cfs_obj_pool) {
        retval = -ENOMEM;
        goto err_kobject_init;
    }

    if ( (retval = kobject_init_and_add(&cfs_kobj, &cfssys_ktype, NULL, "clydefs")) ) {
        goto err_kobject_add;
    }
    
    return 0; /*success*/

err_kobject_add:
    kmem_cache_destroy(cfs_obj_pool);
err_kobject_init:
    return retval;
}

void cfssys_exit(void)
{
    kmem_cache_destroy(cfs_obj_pool);
}
