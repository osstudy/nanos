#include <unix_process_runtime.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <tfs.h>
#include <dirent.h>
#include <errno.h>

#define SECTOR_SIZE 512

static buffer read_stdin(heap h)
{
    buffer in = allocate_buffer(h, 1024);
    int r, k;
    while ((r = in->length - in->end) &&
           ((k = read(0, in->contents + in->end, r)), in->end += k, k == r)) 
        buffer_extend(in, 1024);
    return in;
}

// its nice that we can append a file to any existing buffer, but harsh we have to grow the buffer
void read_file(buffer dest, buffer name)
{
    // mode bit metadata
    struct stat st;
    int fd = open(cstring(name), O_RDONLY);
    if (fd < 0) halt("couldn't open file %b\n", name);
    fstat(fd, &st);
    u64 size = st.st_size;
    buffer_extend(dest, pad(st.st_size, SECTOR_SIZE));
    read(fd, buffer_ref(dest, 0), size);
    dest->end += size;
}

heap malloc_allocator();

tuple root;
CLOSURE_1_1(finish, void, heap, void*);
void finish(heap h, void *v)
{
    root = v;
}

CLOSURE_0_1(perr, void, string);
void perr(string s)
{
    rprintf("parse error %b\n", s);
}

// status
void includedir(tuple dest, buffer path)
{
    DIR *d = opendir(cstring(path));
    struct dirent di, *dip;
    while (readdir_r(d, &di, &dip), dip) {
    }
}


static CLOSURE_1_3(bwrite, void, descriptor, buffer, u64, status_handler);
static void bwrite(descriptor d, buffer s, u64 offset, status_handler c)
{
    int res = pwrite(d, buffer_ref(s, 0), buffer_length(s),  offset);
    apply(c, STATUS_OK);
}

static CLOSURE_1_4(bread, void, descriptor, void *, u64, u64, status_handler);
static void bread(descriptor d, void *source, u64 length, u64 offset, status_handler completion)
{
    apply(completion, timm("error", "empty file"));
}

static CLOSURE_0_1(err, void, status);
static void err(status s)
{
    rprintf ("reported error\n");
}

static CLOSURE_2_0(defer_write, void, tuple, tuple);
static void defer_write(tuple f, tuple body)
{
    value path = table_find((table)body, sym(host));
    // tfs write file should take a reader
    buffer dest = allocate_buffer(transient, 10);
    read_file(dest, path);
    set(f, sym(contents), dest, ignore_status);
    flush(f, ignore_status);
}

static CLOSURE_3_2(translate_each, void, heap, vector, tuple, symbol, value);
static void translate_each(heap h, vector worklist, tuple parent, symbol k, value v)
{
    buffer b;
    if (k == sym(contents)) {
        vector_push(worklist, closure(h, defer_write,parent, v));
    } else {
        if (tagof(v) == tag_tuple) {
            iterate(v, closure(h, translate_each, h, worklist, parent));
        } else {
            set(parent, k, v, ignore_status);
        }
    }
}

extern heap init_process_runtime();
#include <stdio.h>

static CLOSURE_2_1(fsc, void, heap, descriptor, value);
static void fsc(heap h, descriptor out, value root)
{
    // root could be an error
    vector worklist = allocate_vector(h, 10);
    iterate(root, closure(h, translate_each, h, worklist, root));    
    while (vector_length(worklist))
        apply((thunk)vector_pop(worklist));
    close(out);
}

int main(int argc, char **argv)
{
    heap h = init_process_runtime();
    descriptor out = open(argv[1], O_CREAT|O_WRONLY, 0644);
    if (out < 0) {
        halt("couldn't open output file %s\n", argv[1]);
    }

    parser p = tuple_parser(h, closure(h, finish, h), closure(h, perr));
    // this can be streaming
    parser_feed (p, read_stdin(h));
    // fixing the size doesn't make sense in this context?
    // filesystem root? no union today
    create_filesystem(h,
                      SECTOR_SIZE,
                      10ull * 1024 * 1024 * 1024,
                      closure(h, bread, out),
                      closure(h, bwrite, out),
                      closure(h, fsc, h, out));
}
