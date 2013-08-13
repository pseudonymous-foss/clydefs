#include <linux/kernel.h>
#include <embUnit/embUnit.h>
#include <linux/blkdev.h>
#include <linux/tree.h>
#include <linux/completion.h>
#include "io.h"

#define TEST(tst_fnc) new_TestFixture(#tst_fnc, tst_fnc)
#define TST_HDR printk("== %s called\n", __FUNCTION__)
#define U64_MAX_VALUE 18446744073709551615ull
#define TID_LEGAL_BOGUS_VAL (U64_MAX_VALUE-1)
#define NID_LEGAL_BOGUS_VAL (U64_MAX_VALUE-1)

extern char *dbg_dev;
static struct block_device *dbg_dev_bd = NULL;
static u64 tid = U64_MAX_VALUE;
#define LARGE_BUFFER_LEN 1536

u8 first_run = 1;
u64 *large_snd_buffer, *large_rcv_buffer; /*1536 * 8b => 12288b => 12kb buffer*/
DECLARE_COMPLETION(test_request_done);


static __always_inline void buffer_erase(u64 *buffer)
{
    int i;
    for (i=0; i<LARGE_BUFFER_LEN; i++)
        buffer[i] = 0;
}

static __always_inline void buffer_write_natural_numbers(u64 *buffer)
{
    int i;
    for (i=0; i<LARGE_BUFFER_LEN; i++) {
        buffer[i] = i+1;
    }
}

static __always_inline void buffer_fill_with(u64 *buffer, u64 fill_val)
{
    int i;
    for(i=0; i<LARGE_BUFFER_LEN; i++)
        buffer[i] = fill_val;
}

static __always_inline void mktree(int *retval, u64 *tid)
{
    *retval = cfsio_create_tree_sync(dbg_dev_bd, tid);
    TEST_ASSERT_TRUE(*retval == 0, "mktree: error while attempting to create tree: %d\n", *retval);
    TEST_ASSERT_TRUE(*tid != U64_MAX_VALUE, "mktree: tid value did not get set as a result of creating the tree\n");
}

static __always_inline void mknode(int *retval, u64 tid, u64 *nid)
{
    *retval = cfsio_insert_node_sync(dbg_dev_bd, nid, tid);
    TEST_ASSERT_TRUE(*retval == 0, "mknode: did not expect an error inserting a node into tree (%llu), error: %d\n", tid, *retval);
    TEST_ASSERT_TRUE(*nid != U64_MAX_VALUE, "nid wasn't set as a result of inserting a new node\n");
}

static void set_up(void)
{
    if (unlikely(first_run)) {
        first_run = 0;
        large_rcv_buffer = large_snd_buffer = NULL;

        large_rcv_buffer = (u64*)kmalloc(LARGE_BUFFER_LEN * sizeof(u64), GFP_NOIO | GFP_DMA);
        if(!large_rcv_buffer) {
            printk("Failed to allocate large_rcv_buffer\n");
            BUG();
        }
        large_snd_buffer = (u64*)kmalloc(LARGE_BUFFER_LEN * sizeof(u64), GFP_NOIO | GFP_DMA);
        if(!large_snd_buffer) {
            printk("Failed to allocate large_snd_buffer\n");
            BUG();
        }
    }

    if (cfsio_init()){
        pr_debug("%s:%d - %s() cfsio_init failed\n", __FILE__, __LINE__, __FUNCTION__);
    }
    /*reset completion between tests*/
    INIT_COMPLETION(test_request_done);
    buffer_write_natural_numbers(large_snd_buffer);
    buffer_erase(large_rcv_buffer);

    tid = U64_MAX_VALUE;
    return;
}

static void tear_down(void)
{
    if(tid != U64_MAX_VALUE) {
        int ret;
        if ((ret=cfsio_remove_tree_sync(dbg_dev_bd, tid)) != 0) {
            printk("ERR: %s - io_tests teardown failed while attempting to remove the tree\n", __FUNCTION__);
        }
    }
    cfsio_exit();
    return;
}

/* ========== TESTS ========== */
/*=============================*/

/** 
 *  test the creation of a new tree.
 */
static void test_tree_create(void)
{
    int retval;

    TST_HDR;

    retval = cfsio_create_tree_sync(dbg_dev_bd, &tid);
    TEST_ASSERT_TRUE(retval == 0, "error message from cfsio_create_tree_sync: %d\n", retval);
    TEST_ASSERT_TRUE(tid != U64_MAX_VALUE, "tid value did not get updated\n");

    /*rely on tear_down to clean up the tree*/
}

/** 
 * create a new tree only to request its deletion immediately 
 * afterwards. 
*/
static void test_tree_create_remove(void)
{
    u64 tid = U64_MAX_VALUE;
    int retval;

    TST_HDR;

    retval = cfsio_create_tree_sync(dbg_dev_bd, &tid);
    TEST_ASSERT_TRUE(retval == 0, "error message from cfsio_create_tree_sync: %d\n", retval);
    TEST_ASSERT_TRUE(tid != U64_MAX_VALUE, "tid value did not get updated\n");

    retval = cfsio_remove_tree_sync(dbg_dev_bd, tid);
    TEST_ASSERT_TRUE(retval == 0, "error message from cfsio_remove_tree_sync: %d\n", retval);
}

/** 
 * Try removing a non-existing tree. -ENOENT expected as retval 
 */ 
static void test_tree_remove_nonexisting_tree(void)
{
    int retval;

    TST_HDR;
    
    retval = cfsio_remove_tree_sync(dbg_dev_bd, TID_LEGAL_BOGUS_VAL);
    TEST_ASSERT_TRUE(
        retval == TERR_NO_SUCH_TREE, 
        "expected 'TERR_NO_SUCH_TREE(%d)' as the result of attempting to remove a non-existing tree, got (%d)\n",
        TERR_NO_SUCH_TREE, retval
    );
}

/** 
 *  Attempt inserting a node into the tree
 */
static void test_tree_insert(void)
{
    int retval;
    u64 nid = U64_MAX_VALUE;

    TST_HDR;

    mktree(&retval,&tid);
    retval = cfsio_insert_node_sync(dbg_dev_bd, &nid, tid);
    TEST_ASSERT_TRUE(retval == 0, "did not expect an error inserting a node into tree (%llu), error: %d\n", tid, retval);
    TEST_ASSERT_TRUE(nid != U64_MAX_VALUE, "nid wasn't set as a result of inserting a new node\n");
}

static void test_tree_insert_into_nonexisting_tree(void)
{
    
    int retval;
    u64 nid = U64_MAX_VALUE;

    TST_HDR;
    retval = cfsio_insert_node_sync(dbg_dev_bd, &nid, TID_LEGAL_BOGUS_VAL);
    TEST_ASSERT_TRUE(
        retval == TERR_NO_SUCH_TREE, 
        "expected TERR_NO_SUCH_TREE(%d) from inserting a node into a non-existing tree, got: %d\n",
        TERR_NO_SUCH_TREE, retval
    );
}

static void test_tree_remove_node(void)
{
    int retval;
    u64 nid = U64_MAX_VALUE;

    TST_HDR;
    mktree(&retval, &tid);
    mknode(&retval, tid, &nid);

    retval = cfsio_remove_node_sync(dbg_dev_bd, tid, nid);
    TEST_ASSERT_TRUE(retval == 0, "did not expect an error removing recently inserted node\n");
}

static void test_tree_remove_node_from_nonexisting_tree(void)
{
    int retval;
    u64 nid = NID_LEGAL_BOGUS_VAL;
    tid = TID_LEGAL_BOGUS_VAL;
    
    TST_HDR;
    retval = cfsio_remove_node_sync(dbg_dev_bd, tid, nid);
    TEST_ASSERT_TRUE(retval & TERR_NO_SUCH_TREE, "expected to get TERR_NO_SUCH_TREE among the errors...\n");
}

static void test_tree_remove_nonexisting_node(void)
{
    int retval;
    u64 nid = NID_LEGAL_BOGUS_VAL;

    TST_HDR;
    mktree(&retval, &tid);

    retval = cfsio_remove_node_sync(dbg_dev_bd, tid, nid);
    TEST_ASSERT_TRUE(retval & TERR_NO_SUCH_NODE, "expected TERR_NO_SUCH_NODE(%d) but got (%d)\n", TERR_NO_SUCH_NODE, retval);
}


static void __test_tree_node_write_small_on_complete(struct cfsio_rq_cb_data *req_data, int error)
{
    printk("request end_io fired, (%s)\n", __FUNCTION__);
    TEST_ASSERT_TRUE(req_data->error == 0, "unexpected bio errors, transient error?\n");

    TEST_ASSERT_TRUE(
        atomic_read(&req_data->bio_num) == 1, 
        "wrote less than a page, expected just ONE bio, got: %d\n", 
        atomic_read(&req_data->bio_num)
    );
    complete(&test_request_done); /*allow the test to continue*/
}

/* 
  write less than a page's worth (4kb) of data to the node, no offset
  --blind test-- I cannot verify the result before employing read.
*/
static void test_tree_node_write_small(void)
{
    int retval;
    u64 nid = NID_LEGAL_BOGUS_VAL;

    TST_HDR;

    mktree(&retval, &tid);
    mknode(&retval, tid, &nid);

    retval = cfsio_update_node(
        dbg_dev_bd,
        __test_tree_node_write_small_on_complete, 
        tid, nid, 0, sizeof(u64)*10, large_snd_buffer
    );
    printk("%s -- b4 waiting for completion event\n", __FUNCTION__);
    wait_for_completion(&test_request_done);
    printk("%s -- after waiting for completion event (done)\n", __FUNCTION__);
}

/*Writing less than a page's worth of data, this time at an offset*/
static void test_tree_node_write_small_offset(void)
{
    int retval;
    u64 nid = NID_LEGAL_BOGUS_VAL;

    TST_HDR;

    mktree(&retval, &tid);
    mknode(&retval, tid, &nid);

    retval = cfsio_update_node(
        dbg_dev_bd,
        __test_tree_node_write_small_on_complete, 
        tid, nid, 4050, sizeof(u64)*10, large_snd_buffer /*write data 4050 bytes into stream*/
    );

    wait_for_completion(&test_request_done);
}


static void __on_complete_io(struct cfsio_rq_cb_data *req_data, int error)
{
    TEST_ASSERT_TRUE(req_data->error == 0, "unexpected bio errors, transient error?\n");
    complete(&test_request_done); /*allow the test to continue*/
}

/* 
 * Write the entirety of 'large_snd_buffer' into a node, 
 * which should give 12kb of data to be written. 
 * Afterwards, we verify by reading the node 
 */
static void test_tree_node_write_larger_buffer_and_read(void)
{
    int retval, i;
    u64 nid = U64_MAX_VALUE;

    TST_HDR;
    mktree(&retval, &tid);
    mknode(&retval, tid, &nid);
    buffer_fill_with(large_rcv_buffer, U64_MAX_VALUE);

    retval = cfsio_update_node(
        dbg_dev_bd,
        __on_complete_io, 
        tid, nid, 0, sizeof(u64)*LARGE_BUFFER_LEN, large_snd_buffer
    );
    wait_for_completion(&test_request_done);

    INIT_COMPLETION(test_request_done); /*reset*/
    printk("================= READ TIME\n");
    retval = cfsio_read_node(
        dbg_dev_bd, __on_complete_io, 
        tid, nid, 0, sizeof(u64)*LARGE_BUFFER_LEN, large_rcv_buffer
    );
    wait_for_completion(&test_request_done);
    printk("large_snd_buffer: ");
    print_hex_dump(KERN_EMERG, "", DUMP_PREFIX_NONE, 16, 1, large_snd_buffer, sizeof(u64)*LARGE_BUFFER_LEN, 0);
    printk("large_rcv_buffer: ");
    print_hex_dump(KERN_EMERG, "", DUMP_PREFIX_NONE, 16, 1, large_rcv_buffer, sizeof(u64)*LARGE_BUFFER_LEN, 0);
    for(i=0; i<LARGE_BUFFER_LEN; i++) {
        TEST_ASSERT_TRUE(
            large_rcv_buffer[i] == large_snd_buffer[i], 
            "iter:%d - large_rcv_buffer[i](%llu) == large_snd_buffer[i](%llu) failed\n",
            i, large_rcv_buffer[i], large_snd_buffer[i]
        );
    }
    printk("test_tree_node_write_larger_buffer_and_read completed\n");
}

/* 
 * Start at an offset and write the remainder of the 12kb buffer 
 * 'large_snd_buffer' into a node, then read it back and ensure the 
 * data is the same. 
 *  
 * Test carried out with offsets from offset_start to offset_end with step 1 
 *  
 * This test exercises ability to both specify offsets which produces addresses 
 * not fitting machine's int alignment AND the ability to read any length, not 
 * just a multiple of sizeof(int) bytes. 
 */
static void test_tree_node_write_larger_buffer_and_read_various_offsets(void)
{
    int retval;
    u64 nid;
    u64 write_offset, len;
    u8 *rcv_buf_start = (u8*)large_rcv_buffer;
    u8 *snd_buf_start = (u8*)large_snd_buffer;
    u64 offset_start = 1ULL; /*offset to start at*/
    u64 offset_end = 10ULL; /*offset to end at*/
    u8 *snd_buf_ptr;

    TST_HDR;
    mktree(&retval, &tid);
    for(write_offset = offset_start, len = (LARGE_BUFFER_LEN*sizeof(u64)) - offset_start; write_offset <= offset_end; write_offset++, len--){
        nid = U64_MAX_VALUE;
        mknode(&retval, tid, &nid);
        buffer_erase(large_rcv_buffer);
        snd_buf_ptr = snd_buf_start + write_offset;

        retval = cfsio_update_node(
            dbg_dev_bd,
            __on_complete_io, 
            tid, nid, write_offset, len, snd_buf_ptr
        );
        wait_for_completion(&test_request_done);

        INIT_COMPLETION(test_request_done); /*reset*/
        retval = cfsio_read_node(
            dbg_dev_bd, __on_complete_io, 
            tid, nid, write_offset, len, large_rcv_buffer
        );
        wait_for_completion(&test_request_done);
        printk("Checking received data when writing len(%llu) offset(%llu)\n", len, write_offset);
        
        print_hex_dump(KERN_EMERG, "rcv: ", DUMP_PREFIX_NONE, 16, 1, rcv_buf_start, sizeof(u64)*8, 0);
        print_hex_dump(KERN_EMERG, "snd: ", DUMP_PREFIX_NONE, 16, 1, snd_buf_ptr, sizeof(u64)*8, 0);
        TEST_ASSERT_TRUE( memcmp(rcv_buf_start,snd_buf_ptr,len) == 0, "received buffer did not contain the same data as was sent\n" );
        INIT_COMPLETION(test_request_done); /*reset*/
    }
    
    printk("%s completed\n", __FUNCTION__);
}

TestRef io_tests(void)
{
    EMB_UNIT_TESTFIXTURES(fixtures)
    {
        TEST(test_tree_create),
        TEST(test_tree_create_remove),
        TEST(test_tree_remove_nonexisting_tree),
        TEST(test_tree_insert),
        TEST(test_tree_insert_into_nonexisting_tree),
        TEST(test_tree_remove_node),
        TEST(test_tree_remove_node_from_nonexisting_tree),
        TEST(test_tree_remove_nonexisting_node),
        TEST(test_tree_node_write_small),
        TEST(test_tree_node_write_small_offset),
        TEST(test_tree_node_write_larger_buffer_and_read),
        TEST(test_tree_node_write_larger_buffer_and_read_various_offsets),
    };
    EMB_UNIT_TESTCALLER(iotest,"iotest",set_up,tear_down, fixtures);

    dbg_dev_bd = blkdev_get_by_path(dbg_dev, FMODE_READ|FMODE_WRITE, NULL);
	if (!dbg_dev_bd || IS_ERR(dbg_dev_bd)) {
		printk(KERN_ERR "add failed: can't open block device %s: %ld\n", dbg_dev, PTR_ERR(dbg_dev_bd));
		return NULL;
	} else {
        printk("device %s added.. \n", dbg_dev);
    }

    return (TestRef)&iotest;
}


