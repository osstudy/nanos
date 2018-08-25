#include <runtime.h>

static inline boolean istuple(value v)
{
    return (tagof(v) == tag_tuple) || (tagof(v) == tag_tuple_handler);
}

CLOSURE_2_2(get_request, void, heap, tuple, symbol, value);
static void get_request(heap h, merge m, tuple parent, symbol n, value sv, value schild)
{
    if (istuple(schild)) {
        tree_merge(h, sv, schild, apply(m));
    }
    set(parent, n, v);
}

CLOSURE_3_2(source_each, void, heap, merge, tuple, symbol, value);
static void source_each(heap h, merge m, tuple parent, symbol n, value v)
{
    if (istuple(v)){
        get(parent, n, closure(h, get_parent, n, m, child));
    } else {
        set(parent, n, v);
    }
}

value tree_merge(heap h, tuple sd, tuple s, status handler complete)
{
    merge m = allocate_merge(h, complete);
    iterate(d, closure(h, source_each, sd, m));
}
