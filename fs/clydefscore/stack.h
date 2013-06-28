#ifndef __CLYDEFSCORE_STACK_H
#define __CLYDEFSCORE_STACK_H
/*FIXME: don't grow the stack past 255*/
struct stack {
    u64 num_elems;
    u64 capacity;
    char *elems;
    char *head;
};

int clydefscore_stack_init(struct stack *s, u64 initial_capacity);
int clydefscore_stack_push(struct stack *s, void *elem);
void *clydefscore_stack_pop(struct stack *s);
void *clydefscore_stack_peek(struct stack *s);
void clydefscore_stack_free(struct stack *s);
u8 clydefscore_stack_size(struct stack *s);
void clydefscore_stack_clear(struct stack *s);
#endif //__CLYDEFSCORE_STACK_H
