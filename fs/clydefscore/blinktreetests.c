#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/atomic.h>

#include <embUnit/embUnit.h>
#include "blinktree.h"

#define BLINKTEST_INSERT_6_TEST_SIZE 8000
#define BLINKTEST_REMOVE_3_TEST_SIZE 2000

#ifdef CONFIG_CLYDEFS_CORE_DEBUG

extern struct tree *tree_list_head;

struct thread_entry {
    struct task_struct *thread;
    atomic_t val;
    u8 tree_id;
};

/*
 * Initialises the entries array, creates the threads and waits for both 
 * threads to finish before resuming work. 
 * --- 
 *   precondition:
 *      - entries is an array of three.
 *      - t1/t2 knows when to wake up this thread again and does so.
 *   postcondition:
 *      - entries holds three initialised thread_entry structs
 *          0 being this thread, 1 assigned to 't_fnc1' and 2
 *          assigned to 't_fnc2'.
 *      - the threads driving the tasks given by the functions
 *        have completed.
 */
static __always_inline void init_multithread_test(
    struct thread_entry *entries, 
    int (*t_fnc1)(void *data), 
    int (*t_fnc2)(void *data))
{
    u8 tid;
    tid = blinktree_create(2);
    TEST_ASSERT_TRUE(tid < 255, "there cannot have been made 254 existing trees since the start of this test.\n");

    atomic_set(&entries[0].val, 0);
    atomic_set(&entries[1].val, 0);
    atomic_set(&entries[2].val, 0);

    entries[0].tree_id = entries[1].tree_id = entries[2].tree_id = tid;

    entries[0].thread = current;
    entries[1].thread = kthread_create(
        t_fnc1,
        entries,
        "thread1"
    );
    entries[2].thread = kthread_create(
        t_fnc2,
        entries,
        "thread2"
    );

    TEST_ASSERT_TRUE(entries[1].thread && entries[2].thread, "failed to initialise threads for test\n");
    set_current_state(TASK_INTERRUPTIBLE);
    wake_up_process(entries[1].thread);
    wake_up_process(entries[2].thread);

    schedule(); /*sleep until woken - or resume straight away if signalled before this*/
}

static void set_up(void)
{
    /*nop*/
}

static void tear_down(void)
{
    /*nop*/
}

static __always_inline void ensure_all_locks_released(u8 tid, struct stack *s)
{
    int i = 0;
    struct btn *n;
    dbg_blinktree_getnodes(tid, s);
    while (clydefscore_stack_size(s)) {
        n = clydefscore_stack_pop(s);
        TEST_ASSERT_TRUE(spin_is_locked(n->lock) == 0, "Lock was not released!\n");
        i++;
    }
    printk("/////////////////////////////// %d locks checked\n",i);
}

static void test_blinktree_create(void)
{
    u8 id, counter=0;
    struct tree *p;
    printk("test_blinktree_create\n");

    id = blinktree_create(2);
    TEST_ASSERT_EQUAL_U8(1, id, "expected first tree to be of id 1\n");
    id = blinktree_create(2);
    TEST_ASSERT_EQUAL_U8(2, id, "expected second tree to be of id 2\n");

    p = tree_list_head;
    while(p != NULL){
        counter++;
        p = p->nxt;
    }

    TEST_ASSERT_EQUAL_U8(2, counter, "created 2 trees, so expected to find 2 trees\n");
}

/*
 * Tests insertion with the following properties: 
 *  - inorder
 *  - inserts too few elements to cause a node split
 *  - single-threaded
 */
static void test_blinktree_insert_1(void)
{
    struct stack node_stack;
    u8 i,tid;
    char data = '.';
    u64 key_order[] = {1, 2, 3};
    u8 num_keys = 3;

    clydefscore_stack_init(&node_stack,5);

    tid = blinktree_create(2);
    TEST_ASSERT_TRUE(tid < 255, "there cannot have been made 254 existing trees since the start of this test.\n");

    for(i=0; i<num_keys; i++)
        blinktree_insert(tid, key_order[i], &data);

    dbg_blinktree_getkeys(tid,&node_stack);
    TEST_ASSERT_EQUAL_U8(num_keys, clydefscore_stack_size(&node_stack), "tree did not return as many node keys as were inserted\n");
    
    for (i=0; i<clydefscore_stack_size(&node_stack); i++) {
        TEST_ASSERT_EQUAL_U64(key_order[i], *((u64*)clydefscore_stack_pop(&node_stack)), "iter(%u):node order wrong\n", i);
    }

    clydefscore_stack_free(&node_stack);
}

/*
 * Tests insertion with the following properties: 
 *  - non-ordered insert
 *  - inserts too few elements to cause a node split
 *  - single-threaded
 */
static void test_blinktree_insert_2(void)
{
    struct stack node_stack;
    u8 i,tid;
    char data = '.';
    /*keys, in the order of insertion*/
    u64 key_order[] = {3, 1, 2};
    /*getting every key in-order out of the tree, we expect this*/
    u64 key_iter_order[] = {1,2,3};
    u8 num_keys = 3;

    clydefscore_stack_init(&node_stack,5);

    tid = blinktree_create(2);
    TEST_ASSERT_TRUE(tid < 255, "there cannot have been made 254 existing trees since the start of this test.\n");

    for(i=0; i<num_keys; i++) {
        printk("-\\-\\-\\");
        dbg_blinktree_print_inorder(tid);
        blinktree_insert(tid, key_order[i], &data);
    }

    dbg_blinktree_getkeys(tid,&node_stack);
    TEST_ASSERT_EQUAL_U8(num_keys, clydefscore_stack_size(&node_stack), "tree did not return as many node keys as were inserted\n");
    
    dbg_blinktree_print_inorder(tid);
    for (i=0; i<clydefscore_stack_size(&node_stack); i++) {
        TEST_ASSERT_EQUAL_U64(key_iter_order[i], *((u64*)clydefscore_stack_pop(&node_stack)), "iter(%u):node order wrong\n", i);
    }

    clydefscore_stack_free(&node_stack);
}

/*
 * Tests insertion with the following properties: 
 *  - non-ordered insert
 *  - inserts too many elements for one node, causing a root node split
 *  - single-threaded
 */
static void test_blinktree_insert_3(void)
{
    struct stack node_stack;
    u8 i,tid;

    char data = '.';
    /*keys, in the order of insertion*/
    u64 key_order[] = {1, 4, 2, 3, 5};
    /*getting every key in-order out of the tree, we expect this*/
    u64 key_iter_order[] = {1,2,3,4,5};
    u8 num_keys = 5;

    clydefscore_stack_init(&node_stack, num_keys);

    tid = blinktree_create(2);
    TEST_ASSERT_TRUE(tid < 255, "there cannot have been made 254 existing trees since the start of this test.\n");

    /*insert all keys but the last - ensure the split hasn't happened yet*/
    for(i=0; i<num_keys-1; i++){
        printk("\tinserting '%u'\n", i);
        dbg_blinktree_print_inorder(tid);
        blinktree_insert(tid, key_order[i], &data);
    }
    dbg_blinktree_getnodes(tid, &node_stack);
    TEST_ASSERT_EQUAL_U8(1, 
        clydefscore_stack_size(&node_stack), 
        "expected only one node in tree having inserted exactly 2k elements\n"
    );

    clydefscore_stack_clear(&node_stack);

    /*insert last key, ensure split happened*/
    blinktree_insert(tid,key_order[num_keys-1], &data); /*insert last key*/
    dbg_blinktree_getnodes(tid, &node_stack);
    TEST_ASSERT_EQUAL_U8(2, 
        clydefscore_stack_size(&node_stack), 
        "expected exactly 2 nodes as 2k+1 entries have been inserted into the tree, necessitating a split\n"
    );

    clydefscore_stack_clear(&node_stack);

    dbg_blinktree_print_inorder(tid);
    dbg_blinktree_getkeys(tid,&node_stack);
    TEST_ASSERT_EQUAL_U8(num_keys, clydefscore_stack_size(&node_stack), "tree did not return as many node keys as were inserted\n");

    for (i=0; i<clydefscore_stack_size(&node_stack); i++) {
        TEST_ASSERT_EQUAL_U64(key_iter_order[i], *((u64*)clydefscore_stack_pop(&node_stack)), "iter(%u):node order wrong\n", i);
    }

    clydefscore_stack_free(&node_stack);
}

/*
 * Tests insertion with the following properties: 
 *  - non-ordered insert
 *  - inserts too many elements for one node, causing a root node split
 *  - single-threaded
 */
static void test_blinktree_insert_4(void)
{
    struct stack node_stack;
    u8 i,tid;
    char data = '.';
    u64 key_order[] = {1, 4, 2, 3, 5, 8, 7, 6};
    u64 key_iter_order[] = {1,2,3,4,5,6,7,8};
    u8 num_keys = 8;

    clydefscore_stack_init(&node_stack,num_keys);

    tid = blinktree_create(2);
    TEST_ASSERT_TRUE(tid < 255, "there cannot have been made 254 existing trees since the start of this test.\n");

    for(i=0; i<num_keys; i++){
        printk("--------Insert iter: %u\n",i);
        dbg_blinktree_print_inorder(tid);
        blinktree_insert(tid, key_order[i], &data);
    }

    dbg_blinktree_print_inorder(tid);
    dbg_blinktree_getkeys(tid,&node_stack);
    TEST_ASSERT_EQUAL_U8(num_keys, clydefscore_stack_size(&node_stack), "tree did not return as many node keys as were inserted\n");

    for (i=0; i<clydefscore_stack_size(&node_stack); i++) {
        TEST_ASSERT_EQUAL_U64(key_iter_order[i], *((u64*)clydefscore_stack_pop(&node_stack)), "iter(%u):node order wrong\n", i);
    }

    clydefscore_stack_free(&node_stack);
}

/*
 * Tests insertion with the following properties:
 *  -inorder insert
 *  - inserts too many elements forcing many node splits
 *  - single-threaded
 */
static void test_blinktree_insert_5(void)
{
    struct stack node_stack;
    u8 i,tid;
    char data = '.';
    u8 num_keys = 240;

    clydefscore_stack_init(&node_stack,num_keys);

    tid = blinktree_create(2);
    TEST_ASSERT_TRUE(tid < 255, "there cannot have been made 254 existing trees since the start of this test.\n");

    for(i=0; i<num_keys; i++){
        printk("--------Insert iter: %u\n",i);
        dbg_blinktree_print_inorder(tid);
        blinktree_insert(tid, i+1, &data);
    }

    dbg_blinktree_print_inorder(tid);
    dbg_blinktree_getkeys(tid,&node_stack);
    TEST_ASSERT_EQUAL_U8(num_keys, clydefscore_stack_size(&node_stack), "tree did not return as many node keys as were inserted\n");

    for (i=0; i<clydefscore_stack_size(&node_stack); i++) {
        TEST_ASSERT_EQUAL_U64(i+1, *((u64*)clydefscore_stack_pop(&node_stack)), "iter(%u):node order wrong\n", i);
    }

    clydefscore_stack_free(&node_stack);
}




static int test_blinktree_insert6_t1(void *data)
{
    struct thread_entry *entries = (struct thread_entry*)data;
    u64 i;
    u8 first = 1;
    atomic_set(&entries[1].val,1); /*mark ourselves as ready*/

    printk("test_blinktree_insert6 t1 ready\n");
    while (!atomic_read(&entries[2].val)) {} /*wait for other thread*/
    
    for(i=1; i<=BLINKTEST_INSERT_6_TEST_SIZE; i += 2){
        /*&i will be invalid, but it's irrelevant for the test*/
        blinktree_insert(entries[1].tree_id, i, &i);
        if(first) {
            pr_warn("t1 inserted first element\n");
            first=0;
        }
    }
    dbg_blinktree_print_inorder(entries[1].tree_id);
    printk("t1 tree printed\n");

    /*done, increment thread 0 counter (job's done) and wake if necessary*/
    atomic_inc(&entries[0].val);
    printk("t1 is done\n");
    if (atomic_read(&entries[0].val) >= 2) {
        wake_up_process(entries[0].thread);
    } else {
        printk("waiting on t2\n");
    }
    return 0;
}

static int test_blinktree_insert6_t2(void *data)
{
    struct thread_entry *entries = (struct thread_entry*)data;
    u64 i;
    u8 first = 1;
    atomic_set(&entries[2].val,1); /*mark ourselves as ready*/
    printk("test_blinktree_insert6 t2 ready\n");
    while (!atomic_read(&entries[1].val)) {} /*wait for other thread*/
    
    for(i=2; i<=BLINKTEST_INSERT_6_TEST_SIZE; i += 2){
        /*&i will be invalid, but it's irrelevant for the test*/
        blinktree_insert(entries[2].tree_id, i, &i);
        if(first) {
            pr_warn("t2 inserted first element\n");
            first=0;
        }
    }
    
    dbg_blinktree_print_inorder(entries[2].tree_id);
    printk("t2 tree printed\n");

    /*done, increment thread 0 counter (job's done) and wake if necessary*/
    atomic_inc(&entries[0].val);
    printk("t2 is done\n");
    if (atomic_read(&entries[0].val) >= 2) {
        wake_up_process(entries[0].thread);
    } else {
        printk("waiting on t1\n");
    }
    return 0;
}

/*
 * Tests insertion with the following properties: 
 *  - multi-threaded
 *  - (individually) in-order insert
 *  - inserts 100 elements per thread => 200 elements total
 *     => tree splits root and subnodes multiple times
 */
static void test_blinktree_insert_6(void)
{
    struct stack node_stack;
    
    int i;
    struct thread_entry entries[3];
    u8 tid;

    /*init 2 threads, init thread_entry 'entries', wait for threads to finish*/
    init_multithread_test(entries, test_blinktree_insert6_t1, test_blinktree_insert6_t2);

    tid = entries[0].tree_id;

    if (clydefscore_stack_init(&node_stack,BLINKTEST_INSERT_6_TEST_SIZE)){
        printk("Failed to allocated stack for analysis of multi-threaded outcome\n");
        BUG();
    }
    dbg_blinktree_getkeys(tid,&node_stack);
    for(i=1; i<=BLINKTEST_INSERT_6_TEST_SIZE; i++) {
        TEST_ASSERT_EQUAL_U64(i, 
            *((u64*)clydefscore_stack_pop(&node_stack)), 
            "iter(%u):node order wrong (multi-threading insertion issue?)\n", i
        );
    }
    dbg_blinktree_print_inorder(tid);
    TEST_ASSERT_TRUE(clydefscore_stack_size(&node_stack) == 0, "TEST ERR: reusing non-empty stack\n");
    ensure_all_locks_released(tid, &node_stack);
    clydefscore_stack_free(&node_stack);
}

/* Tests removal with the following properties:
 *  - removes an entry from a tree with just one element
 */
static void test_blinktree_remove_1(void)
{
    u8 tid;
    struct stack node_stack;
    char data = '.';
    u64 node_key = 1;

    tid = blinktree_create(2);
    TEST_ASSERT_TRUE(tid < 255, "there cannot have been made 254 existing trees since the start of this test.\n");

    clydefscore_stack_init(&node_stack, 2);

    blinktree_insert(tid,node_key,&data);
    dbg_blinktree_getkeys(tid, &node_stack);
    TEST_ASSERT_EQUAL_U8(1, clydefscore_stack_size(&node_stack), "inserted one entry, expected one entry\n");
    clydefscore_stack_clear(&node_stack);

    blinktree_remove(tid,node_key);
    dbg_blinktree_getkeys(tid, &node_stack);
    TEST_ASSERT_EQUAL_U8(0, clydefscore_stack_size(&node_stack), "expected to have removed the only entry in the tree\n");

    clydefscore_stack_free(&node_stack);
}

/* 
 * Tests order-preservation from removing entries
 * requiring the shifting of others  
 * */ 
static void test_blinktree_remove_2(void)
{
    struct stack node_stack;
    u8 i,tid;
    char data = '.';
    /*keys, in the order of insertion*/
    u64 key_order[] = {3, 1, 2, 7, 5, 6, 8, 4};
    /*getting every key in-order out of the tree, we expect this*/
    u64 key_iter_order[] = {1,2,3,4,5,6,7,8};
    /* 
     * Why remove 2, 4, and 8 ? 
     *  2: middle entry of first node (requires shifting of 3)
     *  4: first entry of second node (requires shifting 5,6)
     *  8: last entry of third node (no shifting required)
     */
    u64 key_iter_order_after_rem[] = {1,3,5,6,7};
    u8 num_keys = 8;
    u8 num_keys_after_rem = 5;

    clydefscore_stack_init(&node_stack,5);

    tid = blinktree_create(2);
    TEST_ASSERT_TRUE(tid < 255, "there cannot have been made 254 existing trees since the start of this test.\n");

    for(i=0; i<num_keys; i++) {
        blinktree_insert(tid, key_order[i], &data);
    }
    dbg_blinktree_getkeys(tid,&node_stack);
    TEST_ASSERT_EQUAL_U8(num_keys, clydefscore_stack_size(&node_stack), "tree did not return as many node keys as were inserted\n");
    for (i=0; i<clydefscore_stack_size(&node_stack); i++) {
        TEST_ASSERT_EQUAL_U64(key_iter_order[i], *((u64*)clydefscore_stack_pop(&node_stack)), "iter(%u):node order wrong\n", i);
    }
    clydefscore_stack_clear(&node_stack);
    dbg_blinktree_print_inorder(tid);


    /*--The actual test--*/
    blinktree_remove(tid,8ul);
    blinktree_remove(tid,4ul);
    blinktree_remove(tid,2ul);
    dbg_blinktree_print_inorder(tid);
    dbg_blinktree_getkeys(tid,&node_stack);
    TEST_ASSERT_EQUAL_U8(
        num_keys_after_rem, 
        clydefscore_stack_size(&node_stack), 
        "expected %u elements left after removing %u from a tree of %u elements\n", num_keys_after_rem, num_keys-num_keys_after_rem, num_keys
    );
    for (i=0; i<clydefscore_stack_size(&node_stack); i++) {
        TEST_ASSERT_EQUAL_U64(key_iter_order_after_rem[i], *((u64*)clydefscore_stack_pop(&node_stack)), "iter(%u): node order wrong\n", i);
    }

    clydefscore_stack_free(&node_stack);
}

int test_blinktree_remove_3_t1(void *data)
{
    struct thread_entry *entries = (struct thread_entry*)data;
    u64 i;
    u8 flip;
    atomic_set(&entries[1].val,1); /*mark ourselves as ready*/
    

    printk("test_blinktree_remove_3_t1 ready\n");
    while (!atomic_read(&entries[2].val)) {} /*wait for other thread*/
    
    /*Insert odd elements in range [1;$test_size]*/
    for(i=1; i<=BLINKTEST_REMOVE_3_TEST_SIZE; i += 2){
        /*&i will be invalid, but it's irrelevant for the test*/
        blinktree_insert(entries[1].tree_id, i, &i);
    }

    flip=1;
    /*remove every other odd element in range [1;$test_size], reverse order*/
    for(i=BLINKTEST_REMOVE_3_TEST_SIZE-1; i<1; i -= 2) {
        if (flip) {
            blinktree_remove(entries[1].tree_id, i);
        }
        flip = flip ? 0 : 1;
    }
    /*stopped early to avoid negative numbers in unsigned value, removing last key here*/
    if (flip) {
        blinktree_remove(entries[1].tree_id, 1);
    }

    dbg_blinktree_print_inorder(entries[1].tree_id);
    printk("test_blinktree_remove_3_t1 tree printed\n");

    /*done, increment thread 0 counter (job's done) and wake if necessary*/
    atomic_inc(&entries[0].val);
    printk("test_blinktree_remove_3_t1 is done\n");
    if (atomic_read(&entries[0].val) >= 2) {
        wake_up_process(entries[0].thread);
    } else {
        printk("waiting on test_blinktree_remove_3_t2\n");
    }
    return 0;
}

int test_blinktree_remove_3_t2(void *data)
{
    struct thread_entry *entries = (struct thread_entry*)data;
    u64 i;
    u8 flip;
    atomic_set(&entries[2].val,1); /*mark ourselves as ready*/

    printk("test_blinktree_remove_3_t2 ready\n");
    while (!atomic_read(&entries[1].val)) {} /*wait for other thread*/
    
    /*Insert even elements in range [0;$test_size], reverse order*/
    for (i=BLINKTEST_REMOVE_3_TEST_SIZE; i>=2; i-=2) {
        /*&i will be invalid, but it's irrelevant for the test*/
        blinktree_insert(entries[2].tree_id, i, &i);
    }
    /*terminate loop early to avoid assigning neg value to unsigned variable*/
    blinktree_insert(entries[2].tree_id, 0, &i);

    flip=1;
    /*remove every other even element in range [0;$test_size]*/
    for (i=0; i<BLINKTEST_REMOVE_3_TEST_SIZE; i+=2) {
        if (flip) {
            blinktree_remove(entries[2].tree_id,i);
        }
        flip = flip ? 0 : 1;
    }

    dbg_blinktree_print_inorder(entries[2].tree_id);
    printk("test_blinktree_remove_3_t2 tree printed\n");

    /*done, increment thread 0 counter (job's done) and wake if necessary*/
    atomic_inc(&entries[0].val);
    printk("test_blinktree_remove_3_t2 is done\n");
    if (atomic_read(&entries[0].val) >= 2) {
        wake_up_process(entries[0].thread);
    } else {
        printk("waiting on test_blinktree_remove_3_t1\n");
    }
    return 0;
}

void test_blinktree_remove_3(void) 
{
    struct thread_entry entries[3];

    /*init 2 threads, init thread_entry 'entries', wait for threads to finish*/
    init_multithread_test(entries,test_blinktree_remove_3_t1,test_blinktree_remove_3_t2);

    dbg_blinktree_print_inorder(entries[0].tree_id);
}

TestRef blinktree_tests(void)
{
    EMB_UNIT_TESTFIXTURES(fixtures)
    {
        /*TEST(test_blinktree_create),
        TEST(test_blinktree_insert_1),
        TEST(test_blinktree_insert_2),
        TEST(test_blinktree_insert_3),
        TEST(test_blinktree_insert_4),
        TEST(test_blinktree_insert_5),*/
        /*TEST(test_blinktree_insert_6),*/
        /*TEST(test_blinktree_remove_1),
        TEST(test_blinktree_remove_2),*/
        TEST(test_blinktree_remove_3),
    };
    EMB_UNIT_TESTCALLER(blinktreetest,"blinktreetest",set_up,tear_down, fixtures);

    return (TestRef)&blinktreetest;
}

#else //CONFIG_CLYDEFS_CORE_DEBUG -not- set
TestRef blinktree_tests(void)
{
    printk("blinktree_tests disabled as debug mode is not enabled\n");

    EMB_UNIT_TESTFIXTURES(fixtures)
    {
    };
    EMB_UNIT_TESTCALLER(blinktreetest,"blinktreetest",set_up,tear_down, fixtures);

    return (TestRef)&blinktreetest;
}
#endif
