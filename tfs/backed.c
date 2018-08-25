#include <tfs_internal.h>

typedef struct backed_thandler {
    struct tuple_handler th;
    // backing tracks the log for id correlation - fix
    tuple backing;
    void (*set)(fsfile f, symbol n, value v, status_handler sh);
    void (*get)(fsfile f, symbol, value_handler v);
    fsfile f;
} *backed_thandler; 

static void backed_set(tuple_handler th, symbol n, value v, status_handler s)
{
    backed_thandler bt = (backed_thandler)th;
    // ignore?
    log_set(bt->f->fs->tl, (value)th, n, v, ignore_status);
    bt->set(bt->f, n, v, s);
}

static void backed_get(tuple_handler th, symbol n, value_handler v)
{
    // need to implement or here
    get(((backed_thandler)th)->backing, n, v);
}

static void backed_iterate(tuple_handler th, iterate_each e)
{
    iterate(((backed_thandler)th)->backing, e);
}   

tuple_handler backed_alloc(fsfile f,
                           void (*set)(fsfile f, symbol, value, status_handler),
                           void (*get)(fsfile f, symbol, value_handler))
{
    backed_thandler b = (backed_thandler)allocate_tuple_handler(sizeof(struct backed_thandler));
    b->th.get = backed_get;
    b->th.set = backed_set;
    b->th.iterate = backed_iterate;
    b->backing = allocate_tuple();
    b->set = set;
    b->get = get;    
    b->f = f;
    return (tuple_handler)b;
}
