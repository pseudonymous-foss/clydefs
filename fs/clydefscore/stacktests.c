#include <linux/kernel.h>
#include <embUnit/embUnit.h>
#include "stack.h"

static struct stack ts;
unsigned long arr[5] = {1ul,2ul,3ul,4ul,5ul};
#define TEST_STACK_SIZE 4u

#define TEST(tst_fnc) new_TestFixture(#tst_fnc, tst_fnc)

/* 
    calculates the address of where the ptr value is stored: s.elems + sizeof(void*) * ndx
    converts to a pointer to the pointer type (pt), e.g. pt=int*  => int**
    dereferences the outermost ptr level, in order to read the actual ptr contents stored in the stack
*/ 
#define GET_STACK_ELEM(s,pt,ndx) (*(pt*)(s.elems+(sizeof(void*)*(ndx))))

static void set_up(void)
{
    clydefscore_stack_init(&ts, TEST_STACK_SIZE);
}

static void tear_down(void)
{
    clydefscore_stack_free(&ts);
}

/* 
 * Initialise a stack and test the initial
 * stack state.
 */
static void test_init(void)
{
    TEST_ASSERT_MESSAGE(ts.capacity == TEST_STACK_SIZE, "stack init did not set capacity properly");
    TEST_ASSERT_MESSAGE(ts.num_elems == 0, "stack initialization did not num_elems to 0");
    TEST_ASSERT_MESSAGE(ts.elems != NULL, "elems (stack) field wrongfully set to NULL value");
    TEST_ASSERT_MESSAGE(ts.head == ts.elems, "head ptr not pointing to start of elems stack");
}

/* 
 * Free an initialised stack and check the 
 * resulting state (namely that the data 
 * structure reflects it) 
 */
static void test_free(void)
{
    clydefscore_stack_push(&ts, &arr[0]);
    clydefscore_stack_push(&ts, &arr[1]);
    clydefscore_stack_free(&ts);
    TEST_ASSERT_MESSAGE(ts.elems == NULL, "elems is a stale ptr after free");
    TEST_ASSERT_MESSAGE(ts.head == NULL, "head is a stale ptr after free");
    TEST_ASSERT_MESSAGE(ts.num_elems == 0, "num_elems not reset after free");
    TEST_ASSERT_MESSAGE(ts.capacity == 0, "capacity not reset after free");
}

/* 
 * Push an argument to the stack 
 * and tests the peek function 
 */
static void test_peek_single(void)
{
    clydefscore_stack_push(&ts, &arr[0]);
    TEST_ASSERT_MESSAGE( 
        arr[0] == ((unsigned long*)clydefscore_stack_peek(&ts))[0], 
        "did not properly push value onto element stack or peek is improperly implemented"
    );
}

/* 
 * Push a single value onto the stack, 
 * ensures  
 */
static void test_pushpop_single(void)
{
    printk("(b4 push) ts.head addr : %p\n", ts.head);
    clydefscore_stack_push(&ts, arr);
    printk("sizeof(char): %ldu\n", sizeof(char));
    printk("sizeof(void): %ldu\n", sizeof(void));
    printk("sizeof(char*): %ldu\n", sizeof(char*));
    printk("sizeof(void*): %ldu\n", sizeof(void*));
    printk("ts.elems start addr: %p\n",ts.elems);
    printk("ts.elems[0] addr: %p\n", (ts.elems+sizeof(void*)*(0)));
    printk("ts.elems[1] addr (head): %p\n", (ts.elems+sizeof(void*)*(1)));
    printk("ts.head addr: %p\n", ts.head);
    
    TEST_ASSERT_EQUAL_ULONG_VAL(arr[0], *GET_STACK_ELEM(ts,unsigned long*, 0), "valued popped is not what I pushed onto the stack\n");
    
    TEST_ASSERT_EQUAL_U8(1, ts.num_elems, "expected a value pushed onto the stack\n");
    clydefscore_stack_pop(&ts);
    TEST_ASSERT_EQUAL_U8(0, ts.num_elems, "expected the value to be popped\n");
}


static void test_pushpop_multiple(void)
{
    /*Pushes on to stack in the order a,b,c*/
    unsigned long a = 10;
    unsigned long c = 9;
    unsigned long b = 13;
    clydefscore_stack_push(&ts, &a);
    clydefscore_stack_push(&ts, &b);
    clydefscore_stack_push(&ts, &c);

    TEST_ASSERT_EQUAL_U8(3, ts.num_elems, "wrong number of elements in stack\n");
    TEST_ASSERT_EQUAL_ULONG_VAL(10, *GET_STACK_ELEM(ts,unsigned long*,0), "stack order is wrong\n");
    TEST_ASSERT_EQUAL_ULONG_VAL(13, *GET_STACK_ELEM(ts,unsigned long*,1), "stack order is wrong\n");
    TEST_ASSERT_EQUAL_ULONG_VAL(9, *GET_STACK_ELEM(ts,unsigned long*,2), "stack order is wrong\n");

    TEST_ASSERT_EQUAL_ULONG_VAL(9, *((unsigned long*)clydefscore_stack_pop(&ts)), "stack order was right yet popping order is wrong\n");
    TEST_ASSERT_EQUAL_ULONG_VAL(13, *((unsigned long*)clydefscore_stack_pop(&ts)), "stack order was right yet popping order is wrong\n");
    TEST_ASSERT_EQUAL_ULONG_VAL(10, *((unsigned long*)clydefscore_stack_pop(&ts)), "stack order was right yet popping order is wrong\n");
}

/* 
 * pushes exactly as many elements as the stack 
 * capacity advertises. 
 * Do some sanity checks and then ensure the stack 
 * ptr hasn't changed (ie. we haven't reallocated 
 * and grown the stack to accomodate the elems) 
 */
static void test_pushtocapacity(void)
{
    int i;
    int data[] = {10,11,12,13};
    void *old_addr = ts.elems;

    TEST_ASSERT_EQUAL_U8(4u, ts.capacity, "test stack capacity changed from what test was written against, rewrite test");

    for(i=0;i<4;i++)
        clydefscore_stack_push(&ts, &data[i]);

    TEST_ASSERT_EQUAL_INT(13, *((int*)clydefscore_stack_peek(&ts)), "unexpected top element\n");
    TEST_ASSERT_EQUAL_U8(4u, ts.num_elems, "expected 4 elements in stack\n")
    TEST_ASSERT_TRUE(old_addr == ts.elems, "did not expect stack to reallocate when pushing 4 elements into a stack of initial size 4\n");
}

/* 
 * push more elements onto stack than there is room for. 
 * This forces a reallocation and copy of data to the new 
 * allocation. Ensure the stack  
 */
static void test_pushpastcapacity(void)
{
    void *old_addr;
    int i = 0;
    int data[] = { 10,11,12,13,14,15 };
    old_addr = ts.elems;
    for(i=0; i<6; i++)
        clydefscore_stack_push(&ts, &data[i]);

    TEST_ASSERT_MESSAGE(6 == ts.num_elems, "wrong numer of elements in stack");
    TEST_ASSERT_MESSAGE(old_addr != ts.elems, "expected ts.elems to point to a new address following the reallocation needed from pushing too many elements onto the stack");

    TEST_ASSERT_MESSAGE(15 == *((int*)clydefscore_stack_pop(&ts)), "expected top element to be 15");
    TEST_ASSERT_MESSAGE(14 == *((int*)clydefscore_stack_pop(&ts)), "expected top element to be 14");
    /*the remaining elements should've fit in the old allocation*/
    TEST_ASSERT_MESSAGE(13 == *((int*)clydefscore_stack_pop(&ts)), "expected top element to be 13 (first element from old stack, check realloc/grow code)");
    TEST_ASSERT_MESSAGE(12 == *((int*)clydefscore_stack_pop(&ts)), "expected top element to be 12");
    TEST_ASSERT_MESSAGE(11 == *((int*)clydefscore_stack_pop(&ts)), "expected top element to be 11");
    TEST_ASSERT_MESSAGE(10 == *((int*)clydefscore_stack_pop(&ts)), "expected top element to be 10");
}

/* 
 * chars(1 byte) are smaller than native 64bit pointers (8 bytes) 
 * hence it makes for a nice test to ensure offset calculations etc 
 * work as expected. 
 */
static void test_pushpop_multiple_chars(void)
{
    char a = 'j';
    
    char c = 'l';

    char b = 'k';
    struct stack cs;
    clydefscore_stack_init(&cs, TEST_STACK_SIZE);
    clydefscore_stack_push(&cs, &a);
    clydefscore_stack_push(&cs, &b);
    clydefscore_stack_push(&cs, &c);

    TEST_ASSERT_EQUAL_U8(3, cs.num_elems, "wrong number of elements in stack\n");
    TEST_ASSERT_EQUAL_CHAR('j', *GET_STACK_ELEM(cs,char*,0), "cs stack order is wrong\n");
    TEST_ASSERT_EQUAL_CHAR('k', *GET_STACK_ELEM(cs,char*,1), "cs stack order is wrong\n");
    TEST_ASSERT_EQUAL_CHAR('l', *GET_STACK_ELEM(cs,char*,2), "cs stack order is wrong\n");

    TEST_ASSERT_EQUAL_CHAR('l', *((char*)clydefscore_stack_pop(&cs)), "unexpected element at stack top\n");
    TEST_ASSERT_EQUAL_CHAR('k', *((char*)clydefscore_stack_pop(&cs)), "unexpected element in stack middle\n");
    TEST_ASSERT_EQUAL_CHAR('j', *((char*)clydefscore_stack_pop(&cs)), "unexpected element at stack bottom\n");
    clydefscore_stack_free(&cs);
}

/*
 * pushes enough char* elements into the stack that 
 * a relocation is necessary. Subsequently examines 
 * the stack and pops each in turn, checking that 
 * ordering is maintained. 
 * (only difference from prior test is that this one uses char* 
 * to ensure no lingering pointer arithmetic errors.)
 */
static void test_pushpastcapacity_chars(void)
{   
    void *old_addr;
    int i;
    char data[] = { 'a','b','c','d','e','f','g' };
    u8 stack_capacity = 4;
    struct stack cs;
    
    clydefscore_stack_init(&cs, stack_capacity);
    old_addr = cs.elems;

    for(i=0; i<7; i++)
        clydefscore_stack_push(&cs, &data[i]);

    TEST_ASSERT_EQUAL_U8(7, cs.num_elems, "wrong number of elements in stack\n");
    TEST_ASSERT_TRUE(old_addr != ts.elems, "pushed 7 elements into a stack of capacity %u, expected a reallocation (none happened)\n", stack_capacity);
    TEST_ASSERT_EQUAL_U8(8, cs.capacity, "pushed 7 elements onto a stack of size %u, expected a reallocation to double stack capacity\n", stack_capacity);

    TEST_ASSERT_EQUAL_CHAR('g', *((char*)clydefscore_stack_pop(&cs)), "unexpected top element\n");
    TEST_ASSERT_EQUAL_CHAR('f', *((char*)clydefscore_stack_pop(&cs)), "unexpected top element\n");
    TEST_ASSERT_EQUAL_CHAR('e', *((char*)clydefscore_stack_pop(&cs)), "unexpected top element\n");
    TEST_ASSERT_EQUAL_CHAR('d', *((char*)clydefscore_stack_pop(&cs)), "unexpected top element\n");
    TEST_ASSERT_EQUAL_CHAR('c', *((char*)clydefscore_stack_pop(&cs)), "unexpected top element\n");
    TEST_ASSERT_EQUAL_CHAR('b', *((char*)clydefscore_stack_pop(&cs)), "unexpected top element\n");
    TEST_ASSERT_EQUAL_CHAR('a', *((char*)clydefscore_stack_pop(&cs)), "unexpected top element\n");

    clydefscore_stack_free(&cs);

}

static void test_push_ptr(void)
{
    struct stack mystack;
    int mydatavar_1 = 2;
    int *mydataptr_1 = &mydatavar_1;
    int *mydataptr_2 = &mydatavar_1;
    clydefscore_stack_init(&mystack, 4);
    clydefscore_stack_push(&mystack, mydataptr_1);

    TEST_ASSERT_EQUAL_PTR(mydataptr_1, mydataptr_2, "two pointers to the same data should point the same place and thus be equal\n");
    TEST_ASSERT_EQUAL_PTR(mydataptr_1, ((int*)clydefscore_stack_peek(&mystack)), "ptrs do not contain same address value\n" );
    TEST_ASSERT_EQUAL_INT(mydatavar_1, *((int*)clydefscore_stack_peek(&mystack)), "dereferencing the ptrs give different values\n" );
    clydefscore_stack_free(&mystack);
}

/* 
 * test to ensure that we're pushing addr (value) 
 * of a pointer, rather than an address to the pointer 
 * itself. 
 */
static void test_pushing_addrs(void)
{
    struct stack mystack;
    int mydatavar_1 = 2;
    int mydatavar_2 = 4;
    int *mydataptr_1 = &mydatavar_1;

    clydefscore_stack_init(&mystack, 4);
    clydefscore_stack_push(&mystack, mydataptr_1);

    TEST_ASSERT_EQUAL_PTR(
        mydataptr_1, 
        (int*)clydefscore_stack_peek(&mystack), 
        "pointers didn't match\n"
    );
    mydataptr_1 = &mydatavar_2;
    TEST_ASSERT_EQUAL_PTR(
        &mydatavar_1, 
        (int*)clydefscore_stack_peek(&mystack), 
        "pointer in stack did not point to the data expected\n"
    );
    TEST_ASSERT_EQUAL_INT(
        mydatavar_1, 
        *(int*)clydefscore_stack_pop(&mystack), 
        "pointer in stack did not point the same place as a newly minted pointer\n"
    );
}

/* 
 * test to ensure that clearing the stack 
 * does nothing but set number of elements to zero 
 * and reset the head ptr 
 * (we try to guard against unwanted reallocations 
 * in the test, but dangling pointers cannot be detected) 
 */
static void test_stack_clear(void)
{
    int mydatavar_1 = 2;
    int mydatavar_2 = 4;
    int mydatavar_3 = 6;
    u64 old_capacity;
    void **old_elems;

    clydefscore_stack_push(&ts, &mydatavar_1);
    clydefscore_stack_push(&ts, &mydatavar_2);
    clydefscore_stack_push(&ts, &mydatavar_3);

    TEST_ASSERT_EQUAL_U8(3, clydefscore_stack_size(&ts), "expected to have pushed exactly 3 elements onto the stack\n");
    
    old_capacity = ts.capacity;
    old_elems = (void**)ts.elems;
    clydefscore_stack_clear(&ts);
    TEST_ASSERT_EQUAL_U64(old_capacity, ts.capacity, "clearing the stack should *not* affect stack capacity\n");
    TEST_ASSERT_EQUAL_PTR(old_elems, ts.elems, "clearing the stack should *not* affect where ts.elems points to, no reallocations/free should be made\n");
    TEST_ASSERT_EQUAL_PTR(ts.elems, ts.head, "head should point to the start of elems, signifying that the next element should be inserted at the bottom of the stack\n");
    TEST_ASSERT_EQUAL_U8(0, ts.num_elems, "clearing the stack should render the stack empty, thus num_elems should be 0\n");
}

TestRef stack_tests(void)
{
    EMB_UNIT_TESTFIXTURES(fixtures)
    {
        TEST(test_init),
        TEST(test_free),
        TEST(test_peek_single),
        TEST(test_pushpop_single),
        TEST(test_pushpop_multiple),
        TEST(test_pushtocapacity),
        TEST(test_pushpastcapacity),
        TEST(test_pushpop_multiple_chars),
        TEST(test_pushpastcapacity_chars),
        TEST(test_push_ptr),
        TEST(test_pushing_addrs),
        TEST(test_stack_clear),
    };
    EMB_UNIT_TESTCALLER(stacktest,"stacktest",set_up,tear_down, fixtures);

    return (TestRef)&stacktest;
}
