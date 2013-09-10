#include <linux/kernel.h>
#include <embUnit/embUnit.h>
#include "io.h"
#include "chunk.h"

#define TEST(tst_fnc) new_TestFixture(#tst_fnc, tst_fnc)
#define TST_HDR printk("== %s called\n", __FUNCTION__)

static struct cfsd_inode_chunk *c = NULL;

static void set_up(void)
{
    if (cfsio_init()){
        pr_debug("%s:%d - %s() cfsio_init failed\n", __FILE__, __LINE__, __FUNCTION__);
    }

    if (cfsc_init()){
        pr_debug("%s:%d - %s() cfsc_init failed\n", __FILE__, __LINE__, __FUNCTION__);
    }

    c = cfsc_chunk_alloc();
    if (c == NULL) {
        pr_debug("%s failed to allocate chunk!\n", __FUNCTION__);
    }
    return;
}

static void tear_down(void)
{
    if (c != NULL) {
        cfsc_chunk_free(c);
    }
    smp_mb();
    cfsc_exit();
    cfsio_exit();
    return;
}

/* ========== TESTS ========== */
/*=============================*/

/**
Chunk allocations are meant to be zeroed out to ensure they only 
contain the values explicitly set. 
*/ 
static void test_chunk_alloc_blank(void)
{
    size_t i, chunk_size;
    u8 *m = (u8*)c;
    
    
    TST_HDR;
    TEST_ASSERT_TRUE(c != NULL, "allocation of a chunk failed\n");
    i = 0;
    chunk_size = sizeof(struct cfsd_inode_chunk);

    while(i < chunk_size) {
        TEST_ASSERT_TRUE(m[i] == 0b00000000, "chunk is not zeroed out properly! byte %zu was (%u)!\n", i, m[i]);
        i++;
    }
}

/**
 * Ensure the initialisation function sets all the header fields
 * to values reflecting an empty chunk
 */
static void test_chunk_init(void)
{
    int i;

    TST_HDR;

    cfsc_chunk_init(c);

    TEST_ASSERT_TRUE(
        c->hdr.entries_free == CHUNK_NUMENTRIES, 
        "newly initialised chunk reported %d entries free, but CHUNK_NUMENTRIES is %d\n", 
        c->hdr.entries_free, CHUNK_NUMENTRIES
    );

    TEST_ASSERT_TRUE(c->hdr.last_chunk == 1, "expect all newly initialised chunks to reflect being the tail chunk\n");

    for(i = 0; i<CHUNK_FREELIST_BYTES; i++){
        TEST_ASSERT_TRUE(
            c->hdr.freelist[i] == 0b11111111, 
            "Expected freelist to report only free slots - block(%d) failed, expected (0b11111111 => 255), got (%u)\n", 
            i, c->hdr.freelist[i]
        );
    }

    for(i = 0; i<CHUNK_NUMENTRIES; i++) {
        TEST_ASSERT_TRUE(
            c->hdr.off_list[i] == OFFSET_UNUSED, 
            "Expected all offset values to be unused(%u). Val %d gave %u\n", 
            OFFSET_UNUSED, i, c->hdr.off_list[i]
        );
    }
}

/** 
 * Inserting a single entry into the chunk. 
 * Check that the bookkeeping variables of the chunk header 
 * (freeist, offlist & entries_free) are all kept up to date 
 *  
 * Also, we expect the insertion algorithm to always insert a 
 * new entry into the first free slot that can be found. 
 */
static void test_chunk_insert_single(void)
{
    u64 entry_ndx = 0;
    struct cfsd_ientry ientry;
    int retval = 0;

    ientry.ino = 1;
    ientry.gid = ientry.uid = 1000;
    ientry.mode = 0755;

    TST_HDR;

    cfsc_chunk_init(c);

    retval = cfsc_chunk_entry_insert(&entry_ndx, c, &ientry);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");
    
    TEST_ASSERT_EQUAL_U8(CHUNK_NUMENTRIES-1, c->hdr.entries_free, "insertion did not decrement number of free entries\n")
    TEST_ASSERT_EQUAL_U64(0, entry_ndx, "expected insertion to pick the first available slot\n");

    TEST_ASSERT_TRUE(c->hdr.off_list[entry_ndx] != OFFSET_UNUSED, "expected the first offset entry to be used now");
    TEST_ASSERT_EQUAL_U8(254, c->hdr.freelist[0], "expected one bit in the freelist to be flipped signalling the slot had been taken\n");

    TEST_ASSERT_TRUE(memcmp(&ientry, &c->entries[0], sizeof(struct cfsd_ientry)) == 0, "the entry contents wasn't copied over right\n");
}

void test_chunk_insert_multiple(void)
{
    u64 entry_ndx[3] = {0,0,0};
    struct cfsd_ientry ientry[3];
    u8 entries_free;
    int retval = 0;

    TST_HDR;

    cfsc_chunk_init(c);

    ientry[0].ino = 1;
    ientry[0].gid = ientry[0].uid = 1000;
    ientry[0].mode = 0755;

    ientry[1].ino = 2;
    ientry[1].gid = ientry[1].uid = 1100;
    ientry[1].mode = 0744;

    ientry[2].ino = 3;
    ientry[2].gid = ientry[2].uid = 1200;
    ientry[2].mode = 0700;

    entries_free = c->hdr.entries_free;
    TEST_ASSERT_EQUAL_U8(
        CHUNK_NUMENTRIES, entries_free, 
        "newly initialised chunk reported %u free entries, expected %u\n", 
        entries_free, CHUNK_NUMENTRIES
    );

    /*test first ientry*/
    retval = cfsc_chunk_entry_insert(&entry_ndx[0], c, &ientry[0]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    TEST_ASSERT_EQUAL_U8(entries_free-1, c->hdr.entries_free, "insertion did not decrement number of free entries\n")
    TEST_ASSERT_EQUAL_U64(0, entry_ndx[0], "expected insertion to pick the first available slot\n");
    TEST_ASSERT_TRUE(c->hdr.off_list[entry_ndx[0]] != OFFSET_UNUSED, "expected the first offset entry to be used now");
    TEST_ASSERT_EQUAL_U8(254, c->hdr.freelist[0], "expected one bit in the freelist to be flipped signalling the slot had been taken\n");
    TEST_ASSERT_TRUE(memcmp(&ientry[0], &c->entries[0], sizeof(struct cfsd_ientry)) == 0, "the entry contents wasn't copied over right\n");

    /*Test second ientry*/
    entries_free = c->hdr.entries_free;
    retval = cfsc_chunk_entry_insert(&entry_ndx[1], c, &ientry[1]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    TEST_ASSERT_EQUAL_U8(entries_free-1, c->hdr.entries_free, "insertion did not decrement number of free entries\n")
    TEST_ASSERT_EQUAL_U64(1, entry_ndx[1], "expected insertion to pick the first available slot\n");
    TEST_ASSERT_TRUE(c->hdr.off_list[entry_ndx[1]] != OFFSET_UNUSED, "expected the first offset entry to be used now");
    TEST_ASSERT_EQUAL_U8(252, c->hdr.freelist[0], "expected one bit in the freelist to be flipped signalling the slot had been taken\n");
    TEST_ASSERT_TRUE(memcmp(&ientry[1], &c->entries[1], sizeof(struct cfsd_ientry)) == 0, "the entry contents wasn't copied over right\n");

    /*Test third and final ientry*/
    entries_free = c->hdr.entries_free;
    retval = cfsc_chunk_entry_insert(&entry_ndx[2], c, &ientry[2]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    TEST_ASSERT_EQUAL_U8(entries_free-1, c->hdr.entries_free, "insertion did not decrement number of free entries\n")
    TEST_ASSERT_EQUAL_U64(2, entry_ndx[2], "expected insertion to pick the first available slot\n");
    TEST_ASSERT_TRUE(c->hdr.off_list[entry_ndx[2]] != OFFSET_UNUSED, "expected the first offset entry to be used now");
    TEST_ASSERT_EQUAL_U8(248, c->hdr.freelist[0], "expected one bit in the freelist to be flipped signalling the slot had been taken\n");
    TEST_ASSERT_TRUE(memcmp(&ientry[2], &c->entries[2], sizeof(struct cfsd_ientry)) == 0, "the entry contents wasn't copied over right\n");
}

void test_chunk_delete(void)
{
    struct cfsd_ientry ientry;
    u64 ret_ndx = 0;
    int retval = 0;

    ientry.ino = 1;
    ientry.gid = ientry.uid = 1000;
    ientry.mode = 0755;

    TST_HDR;

    cfsc_chunk_init(c);

    retval = cfsc_chunk_entry_insert(&ret_ndx, c, &ientry);
    TEST_ASSERT_EQUAL_INT(0, retval, "chunk_entry_insert failed \n");
    TEST_ASSERT_TRUE(ret_ndx != U64_MAX_VALUE, "ret_ndx wasn't set in insertion function\n");

    TEST_ASSERT_EQUAL_U8(CHUNK_NUMENTRIES-1, c->hdr.entries_free, "insertion failed\n");
    
    cfsc_chunk_entry_delete(c, (u8)ret_ndx); /*void fnc*/
    TEST_ASSERT_EQUAL_U8(CHUNK_NUMENTRIES, c->hdr.entries_free, "deletion didn't update entries_free\n");
    TEST_ASSERT_EQUAL_U8(255U, c->hdr.freelist[0], "expected deletion to mark the slot free in the freelist again\n");
    TEST_ASSERT_EQUAL_U8(OFFSET_UNUSED, c->hdr.off_list[ret_ndx], "expected off_list[0] entry to be set to 'OFFSET_UNUSED'(%u) again\n", OFFSET_UNUSED);
}

void test_chunk_delete_middle(void)
{
        u64 entry_ndx[3] = {0,0,0};
    struct cfsd_ientry ientry[3];
    int retval = 0;

    TST_HDR;

    cfsc_chunk_init(c);

    ientry[0].ino = 1;
    ientry[0].gid = ientry[0].uid = 1000;
    ientry[0].mode = 0755;

    ientry[1].ino = 2;
    ientry[1].gid = ientry[1].uid = 1100;
    ientry[1].mode = 0744;

    ientry[2].ino = 3;
    ientry[2].gid = ientry[2].uid = 1200;
    ientry[2].mode = 0700;

    retval = cfsc_chunk_entry_insert(&entry_ndx[0], c, &ientry[0]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    retval = cfsc_chunk_entry_insert(&entry_ndx[1], c, &ientry[1]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    retval = cfsc_chunk_entry_insert(&entry_ndx[2], c, &ientry[2]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    TEST_ASSERT_EQUAL_U8(CHUNK_NUMENTRIES-3, c->hdr.entries_free, "tried inserting 3 entries - entries_free not reflecting this\n");

    cfsc_chunk_entry_delete(c, 1); /*delete middle element*/
    TEST_ASSERT_EQUAL_U8(CHUNK_NUMENTRIES-2, c->hdr.entries_free, "deleted middle element\n");
    TEST_ASSERT_EQUAL_U8(OFFSET_UNUSED, c->hdr.off_list[1], "offset list entry for middle ientry should've been set to unused\n");
    TEST_ASSERT_EQUAL_U8(250, c->hdr.freelist[0], "expected freelist[0] to reflect having entries 1 and 3 set (yielding 250)\n");
}

void test_chunk_delete_last(void)
{
        u64 entry_ndx[3] = {0,0,0};
    struct cfsd_ientry ientry[3];
    int retval = 0;

    TST_HDR;

    cfsc_chunk_init(c);

    ientry[0].ino = 1;
    ientry[0].gid = ientry[0].uid = 1000;
    ientry[0].mode = 0755;

    ientry[1].ino = 2;
    ientry[1].gid = ientry[1].uid = 1100;
    ientry[1].mode = 0744;

    ientry[2].ino = 3;
    ientry[2].gid = ientry[2].uid = 1200;
    ientry[2].mode = 0700;

    retval = cfsc_chunk_entry_insert(&entry_ndx[0], c, &ientry[0]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    retval = cfsc_chunk_entry_insert(&entry_ndx[1], c, &ientry[1]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    retval = cfsc_chunk_entry_insert(&entry_ndx[2], c, &ientry[2]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    TEST_ASSERT_EQUAL_U8(CHUNK_NUMENTRIES-3, c->hdr.entries_free, "tried inserting 3 entries - entries_free not reflecting this\n");

    cfsc_chunk_entry_delete(c, 2); /*delete last element*/
    TEST_ASSERT_EQUAL_U8(CHUNK_NUMENTRIES-2, c->hdr.entries_free, "deleted middle element\n");
    TEST_ASSERT_EQUAL_U8(OFFSET_UNUSED, c->hdr.off_list[2], "offset list entry for middle ientry should've been set to unused\n");
    TEST_ASSERT_EQUAL_U8(252, c->hdr.freelist[0], "expected freelist[0] to reflect having entries 1 and 3 set (yielding 250)\n");
}

/** 
 * insert 3 ientries, delete middle entry, insert two more. 
 * Ergo => expect 4 entries present, and expect the fourth 
 * ientry to be at the slot where the second entry was initially
 * inserted, while the fifth ientry is inserted at the fourth 
 * slot. 
 */ 
void test_chunk_insert_delete_mix(void)
{
    u64 entry_ndx[5] = {0,0,0,0,0};
    struct cfsd_ientry ientry[5];
    int retval = 0;

    TST_HDR;

    cfsc_chunk_init(c);

    ientry[0].ino = 1;
    ientry[0].gid = ientry[0].uid = 1000;
    ientry[0].mode = 0755;

    ientry[1].ino = 2;
    ientry[1].gid = ientry[1].uid = 1100;
    ientry[1].mode = 0744;

    ientry[2].ino = 3;
    ientry[2].gid = ientry[2].uid = 1200;
    ientry[2].mode = 0700;

    ientry[3].ino = 4;
    ientry[3].gid = ientry[3].uid = 1300;
    ientry[3].mode = 0655;

    ientry[4].ino = 5;
    ientry[4].gid = ientry[4].uid = 1400;
    ientry[4].mode = 0677;

    retval = cfsc_chunk_entry_insert(&entry_ndx[0], c, &ientry[0]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    retval = cfsc_chunk_entry_insert(&entry_ndx[1], c, &ientry[1]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    retval = cfsc_chunk_entry_insert(&entry_ndx[2], c, &ientry[2]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    cfsc_chunk_entry_delete(c, 2); /*delete middle*/

    retval = cfsc_chunk_entry_insert(&entry_ndx[3], c, &ientry[3]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");

    retval = cfsc_chunk_entry_insert(&entry_ndx[4], c, &ientry[4]);
    TEST_ASSERT_TRUE(retval == 0, "failed to insert ientry into chunk\n");


    TEST_ASSERT_EQUAL_U8(CHUNK_NUMENTRIES-4, c->hdr.entries_free, "inserted 3 entries, deleted one, inserted 2 more, should yield 4 used slots\n");

    TEST_ASSERT_TRUE(memcmp(&ientry[0], &c->entries[0], sizeof(struct cfsd_ientry)) == 0, "ientry[0] <=> c->entries[0] the entry contents wasn't copied over right\n");
    TEST_ASSERT_TRUE(memcmp(&ientry[1], &c->entries[1], sizeof(struct cfsd_ientry)) == 0, "ientry[1] <=> c->entries[1] the entry contents wasn't copied over right\n");
    TEST_ASSERT_TRUE(memcmp(&ientry[3], &c->entries[2], sizeof(struct cfsd_ientry)) == 0, "ientry[3] <=> c->entries[2] the entry contents wasn't copied over right\n");
    TEST_ASSERT_TRUE(memcmp(&ientry[4], &c->entries[3], sizeof(struct cfsd_ientry)) == 0, "ientry[4] <=> c->entries[3] the entry contents wasn't copied over right\n");
}

TestRef chunk_tests(void)
{
    EMB_UNIT_TESTFIXTURES(fixtures)
    {
        TEST(test_chunk_alloc_blank),
        TEST(test_chunk_init),
        TEST(test_chunk_insert_single),
        TEST(test_chunk_insert_multiple),
        TEST(test_chunk_delete),
        TEST(test_chunk_delete_middle),
        TEST(test_chunk_delete_last),
        TEST(test_chunk_insert_delete_mix),
    };
    EMB_UNIT_TESTCALLER(chunktest,"chunktest",set_up,tear_down, fixtures);
    return (TestRef)&chunktest;
}
