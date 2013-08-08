#include <linux/kernel.h>
#include <embUnit/embUnit.h>
#include <linux/blkdev.h>
#include <linux/tree.h>
#include "io.h"

#define TEST(tst_fnc) new_TestFixture(#tst_fnc, tst_fnc)
#define TST_HDR printk("== %s called\n", __FUNCTION__)
#define U64_MAX_VALUE 18446744073709551615ull
#define TID_LEGAL_BOGUS_VAL (U64_MAX_VALUE-1)
#define NID_LEGAL_BOGUS_VAL (U64_MAX_VALUE-1)

extern char *dbg_dev;
static struct block_device *dbg_dev_bd = NULL;

static u64 tid = U64_MAX_VALUE;

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
    if (cfsio_init()){
        pr_debug("%s:%d - %s() cfsio_init failed\n", __FILE__, __LINE__, __FUNCTION__);
    }

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

TestRef io_tests(void)
{
    dbg_dev_bd = blkdev_get_by_path(dbg_dev, FMODE_READ|FMODE_WRITE, NULL);
	if (!dbg_dev_bd || IS_ERR(dbg_dev_bd)) {
		printk(KERN_ERR "add failed: can't open block device %s: %ld\n", dbg_dev, PTR_ERR(dbg_dev_bd));
		return NULL;
	} else {
        printk("device %s added.. \n", dbg_dev);
    }

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
    };
    EMB_UNIT_TESTCALLER(iotest,"iotest",set_up,tear_down, fixtures);

    return (TestRef)&iotest;
}


