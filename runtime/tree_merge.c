#include <runtime.h>

static inline boolean istuple(value v)
{
    return (tagof(v) == tag_tuple) || (tagof(v) == tag_tuple_handler);
}

static CLOSURE_5_1(get_existing_dest, void, heap, merge, tuple, symbol, value, value);
static void get_existing_dest(heap h, merge m, tuple parent, symbol n, value v, value existing_child)
{
    if (istuple(existing_child)) {
        tree_merge(h, existing_child, v, apply(m));
    }
    set(parent, n, v, apply(m));
}

static CLOSURE_3_2(source_each, void, heap, merge, tuple, symbol, value);
static void source_each(heap h, merge m, tuple parent, symbol n, value v)
{
    if (istuple(v)){
        get(parent, n, closure(h, get_existing_dest, h, m, parent, n, v));
    } else {
        set(parent, n, v, apply(m));
    }
}

value tree_merge_internal(heap h, tuple sd, tuple s, merge m)
{
    iterate(s, closure(h, source_each, h, m, sd));
}


value tree_merge(heap h, tuple sd, tuple s, status_handler complete)
{
    tree_merge_internal(h, sd, s, allocate_merge(h, complete));
}

