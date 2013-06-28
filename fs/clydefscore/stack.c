#include <linux/types.h>
#include <linux/slab.h>
#include "utils.h"
#include "stack.h"

#define GET_VAL(addr) (*(void**)(addr))
#define SET_HEAD_VAL(val) do {*((void**)(s->head)) = (val); } while(0);


static __always_inline void grow_bounds_check(struct stack *self)
{
    if (unlikely(self->capacity > 9223372936854780000ull)) {
        pr_warn("attempted to grow stack with a capacity of %llu, this would result in overflowing the num_elements and capacity variables\n", self->capacity);
        BUG();
    }
}

/* 
 * initialises the stack structure. 
 * --- 
 * s: the stack itself 
 * elem_size: size o 
 */
int clydefscore_stack_init(struct stack *s, u64 initial_capacity)
{
    CLYDE_ASSERT( s != NULL );
    s->capacity = initial_capacity;
    s->elems = (char*)kmalloc(sizeof(char*) * s->capacity, GFP_KERNEL);
    s->num_elems = 0;
    if( ! s->elems )
        return -ENOMEM;
    s->head = s->elems;
    return 0;
}

int clydefscore_stack_push(struct stack *s, void *elem)
{
    CLYDE_ASSERT(s != NULL);

    if (unlikely(s->num_elems == s->capacity)) {
        char *ns;
        grow_bounds_check(s);
        ns = (char*)kmalloc(sizeof(void*) * s->capacity * 2, GFP_KERNEL);
        if (!ns)
            return -ENOMEM;
        memcpy(ns, s->elems, sizeof(void*) * s->num_elems);
        kfree(s->elems);
        s->elems = ns;
        s->head = ns + (sizeof(void*)*s->num_elems);
        s->capacity *= 2;
    }
    
    SET_HEAD_VAL(elem);
    s->num_elems++;
    s->head += sizeof(void*);
    return 0;
}

__always_inline void *clydefscore_stack_pop(struct stack *s)
{
    CLYDE_ASSERT(s != NULL);
    CLYDE_ASSERT(s->num_elems > 0);

    s->num_elems--;
    s->head -= sizeof(void*);
    return GET_VAL(s->head);
}

__always_inline void *clydefscore_stack_peek(struct stack *s)
{
    CLYDE_ASSERT(s != NULL);
    CLYDE_ASSERT(s->num_elems > 0);

    return GET_VAL(s->head - sizeof(void*));
}

__always_inline u8 clydefscore_stack_size(struct stack *s)
{
    return s->num_elems;
}

/* 
 * frees the internally managed aspects of the stack 
 *  
 * --- 
 * s: the stack 
 * --- 
 *  postcondition:
 *   - the stack object itself is collectable
 *   - the stack structure can be reinitialised.
 */
__always_inline void clydefscore_stack_free(struct stack *s)
{
    if(s->elems != NULL )
        kfree(s->elems);
    s->elems = s->head = NULL;
    s->num_elems = s->capacity = 0;
}

/*
 * clears the stack of all elements
 * resetting the stack to pristine condition
 * ---
 * s: the stack
 * ---
 *  postcondition:
 *    - the stack can be used as if it was just initialised
 */
__always_inline void clydefscore_stack_clear(struct stack *s)
{
    s->head = s->elems;
    s->num_elems = 0;
}
