#include <tfs_internal.h>

#define END_OF_LOG 1
#define TUPLE_AVAILABLE 2
#define END_OF_SEGMENT 3
#define EXTENSION 4

// the max encoding size of an extend record
#define TRAILER_SIZE 16

static void log_get(tuple_handler th, symbol n, value_handler complete);

typedef struct log {
    filesystem fs;
    u64 remainder;
    buffer staging;
    vector completions;
    table dictionary;
    u64 offset;
    heap h;
} *log;

typedef struct log_entry {
    struct tuple_handler th;
    log tl;
    table backing;
    u64 id;
} *log_entry;

static CLOSURE_1_1(log_write_completion, void, vector, status);
static void log_write_completion(vector v, status nothing)
{
    // reclaim the buffer now and the vector...make it a whole thing
    thunk i;
    vector_foreach(v, i) apply(i);
}

// xxx  currently we cant take writes during the flush
//      - just swap out staging

void log_flush(log tl)
{
    thunk i;
    buffer b = tl->staging;

    buffer_clear(tl->completions);
    push_u8(b, END_OF_LOG);
    apply(tl->fs->w,
          b,
          tl->offset + b->start, 
          closure(tl->h, log_write_completion, tl->completions));
    b->end -= 1;
}


void log_set(log tl, tuple t, symbol n, value v, status_handler complete)
{
    // dictionary...root..translate back
    push_u8(tl->staging, TUPLE_AVAILABLE);
    encode_eav(tl->staging, tl->dictionary, t, n, v);
    vector_push(tl->completions, complete);
    // flush!
}


CLOSURE_3_1(log_read_complete, void, log, table, status_handler, status);
void log_read_complete(log tl, table read_dictionary, status_handler sh, status s)
{
    buffer b = tl->staging;
    u8 frame = 0;

    if (s == 0) {
        // log extension - length at the beginnin and pointer at the end
        for (; frame = pop_u8(b), frame == TUPLE_AVAILABLE;) {
            tuple t = decode_value(tl->h, tl->dictionary, b);
        }
    }
    if (frame != END_OF_LOG) halt("bad log tag %p\n", frame);    
    apply(sh, 0);
}

void read_log(log tl, u64 offset, u64 size, table read_dictionary, status_handler sh)
{
    tl->staging = allocate_buffer(tl->h, size);
    // should be actually read...empty log case makes this special
    //    tl->staging->end = size;
    status_handler tlc = closure(tl->h, log_read_complete, tl, read_dictionary, sh);
    apply(tl->fs->r, tl->staging->contents, tl->staging->length, 0, tlc);
}

log log_create(heap h, filesystem fs, status_handler vh)
{
    log tl = allocate(h, sizeof(struct log));
    tl->h = h;
    tl->offset = 0;
    tl->fs = fs;
    tl->completions = allocate_vector(h, 10);
    tl->dictionary = allocate_table(h, identity_key, pointer_equal);
    fs->tl = tl;
    read_log(tl, 0, INITIAL_LOG_SIZE, allocate_table(h, identity_key, pointer_equal), vh);
    return tl;
}
