#include <runtime.h>
#include <tfs.h>

// ok, we wanted to make the inode number extensional, but holes
// and random access writes make that difficult, so this is stateful
// with an inode

typedef struct log *log;

typedef struct filesystem {
    heap storage;
    rtrie free;
    heap h;
    heap fh; // 4k aligned for elf
    int alignment;  // block alignment - isn't this storage->pagesize? should be
    block_read r;
    block_write w;    
    log tl;
    tuple root;
} *filesystem;

void extent_update(fsfile f, symbol foff, tuple value);

typedef struct fsfile *fsfile;

log log_create(heap h, filesystem fs, status_handler sh);
void log_write(log tl, tuple t, thunk complete);
void log_write_eav(log tl, tuple e, symbol a, value v, thunk complete);

// xxx - tlog.c is using rolling to hold the staging buffer, which currently doesn't deal with multiple
// allocations properly - take this out of backed
#define INITIAL_LOG_SIZE (3*KB)
#define INITIAL_FS_SIZE (20 * MB)
void log_flush(log tl);
    
typedef closure_type(buffer_status, buffer, status);

merge allocate_merge(heap h, status_handler completion);
value allocate_fsfile(filesystem fs);
void log_set(log tl, tuple t, symbol n, value v, status_handler complete);

static tuple_handler thalloc(bytes size,
                             filesystem fs,
                             void (*set_special)(struct tuple_handler *, symbol, value, status_handler),
                             tuple md);


struct fsfile {
    rtrie extents;
    filesystem fs;
    tuple_handler children;
    tuple_handler extentst;    
};

tuple_handler backed_alloc(fsfile f,
                           void (*set)(fsfile f, symbol, value, status_handler),
                           void (*get)(fsfile f, symbol, value_handler));

value tree_merge(heap h, tuple sd, tuple, status_handler complete);


