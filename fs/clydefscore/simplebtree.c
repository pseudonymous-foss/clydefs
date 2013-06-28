/*
    A very simple tree implementation.
 
    Concurrency: none - we have a global lock
 
    Improvements:
        * make concurrency work
        * abstract away from actual pointers ?
            (could use UIDs (tID, oID, bID) to make referencing blocks possible even after relocation)
        * variable-sized block allocations
        * abstract away from fixed data sizes ?
            We could make structs of functions to compare, analyze and work with our data, the question is
            if it's needed and
        * 
*/

#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>

/*FIXME, lazy alloc strategy, theoretically not necessary to allocate 
  one additional page iff the byte size request modulo page size is 0*/
#define DEVICE_SIZE_BYTES 1073741824ul
#define DEVICE_NUMPAGE_ALLOC (DEVICE_SIZE_BYTES / PAGE_SIZE + 1)


/* 
 * initialise the simple tree
 */
int simpletree_init(void)
{
    void *mem = vmalloc(DEVICE_NUMPAGE_ALLOC);   
    if(!mem)
        return -ENOMEM;
    return 0;
}

/* 
 * Tree node
 */
struct stn{
    u64 key;
    struct stn *parent;
    u8 is_leaf;

    u8 numkeys;
    u64 *child_keys;
    u64 *child_nodes;
};

/* 
 * Tree data, contains information 
 * about length(in bytes) and address of 
 * a contiguous section of memory 
 * --- 
 * num_bytes: number of bytes in this data element
 * data: ptr to start of data
 */
struct std{
    /**/
    u8 num_bytes;
    u8 *data;
};


/*
 * Allocates a new tree
 */
void simplebtree_create(void)
{
    //slide 12

}

/* 
 * Allocate a new tree node
 * --- 
 * node: will hold the pointer to the new node, if successful 
 * key: the key for the new node 
 * parent: the node's parent (NULL=>root) 
 * is_leaf: is this node a leaf node ? (non-null => yes!) 
 * --- 
 * return: 
 *   -ENOMEM if node allocation fails
 *   0 otherwise (on success)
 */
static noinline int make_node(struct stn *node, u64 key, struct stn *parent, u8 is_leaf){
    node = (struct stn*)kmalloc(sizeof(struct stn), GFP_KERNEL);
    if(!node)
        return -ENOMEM;

    node->key = key;
    node->parent = parent;
    node->is_leaf = is_leaf;
    node->numkeys = 0;
    node->child_keys = node->child_nodes = NULL;

    return 0;
}

/* 
 * Allocate a data block of a variable size of bytes 
 * --- 
 *   leaf
 * --- 
 * return: 
 *   -ENOMEM if allocation failed
 *   0 if allocation went well
 */
static noinline int make_data(struct std *block, u8 num_bytes){
    //alloc leaf
    block = (struct std*)kmalloc(sizeof(struct std), GFP_KERNEL);
    if(!block)
        goto err_alloc;

    block->data = (u8*) kmalloc(sizeof(u8)*num_bytes, GFP_KERNEL);
    if(!block->data)
        goto err_data_alloc;
    block->num_bytes = num_bytes;

    goto out;

err_data_alloc:
    kfree(block);
err_alloc:
    return -ENOMEM;
out:
    return 0;
}

/* 
 * Searches from root node toward a leaf. 
 * --- 
 * root: root node
 * key: key to search for
 * --- 
 * returns: a node to the leaf, NULL if no results
 */
static struct stn *find_leaf(struct stn *root, u64 key)
{
    u8 i;
    struct stn *c = root;
    if (c == NULL)
        return NULL;
    
    while (!c->is_leaf) {
        for(i=0; i < c->numkeys; i++) {
            /*Go through each child ptr, to find the next edge down*/
            if (key < c->child_keys[i]) {
                /* FIXME -- double-check literature, this doesn't seem right*/
                break;
            }
        }
        /*found the right index, continue down this way*/
        c = (struct stn*)c->child_nodes[i];
    }
    /*return the leaf node*/
    return c;
}

/*
 * x: pointer to root node
 * key: key to be searched for 
 * --- 
 * return: the matching leaf or NULL if nothing was found 
 */  
struct std *simplebtree_search(struct stn *root, u64 key)
{
    //slide 10
    
    u8 i = 0;

    struct stn *c = find_leaf(root, key);
    if ( c == NULL ) return NULL;

    for(; i < c->numkeys; i++) {
        if( c->child_keys[i] == key)
            return (struct std*)c->child_nodes[i];
    }
        
    return NULL; /*no child with matching key found*/
}

void simplebtree_insert(struct stn *root, u64 key, struct std *val)
{
    //slide 16

}

void simplebtree_delete(struct stn *root, u64 key)
{
    //slide 29

}
