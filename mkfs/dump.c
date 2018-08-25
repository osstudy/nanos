#include <runtime.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <tfs.h>
#include <errno.h>

static CLOSURE_1_3(bwrite, void, descriptor, buffer, u64, status_handler);
static void bwrite(descriptor d, buffer s, u64 offset, status_handler c)
{

}

static CLOSURE_1_4(bread, void, descriptor, void *, u64, u64, status_handler);
static void bread(descriptor d, void *dest, u64 length, u64 offset, status_handler c)
{
    int xfer, total = 0;
    while (total < length) {
        xfer = pread(d, dest + total , length - total, offset + total);
        if (xfer == 0) apply(c, 0);
        if (xfer == -1) apply(c, timm("read-error", "%E", errno));
        total += xfer;
    }
    apply(c, STATUS_OK);
}


boolean compare_bytes(void *a, void *b, bytes len);

CLOSURE_1_1(write_file, void, buffer, value);
void write_file(buffer path, value v)
{
    buffer b = (value) v;
    // openat would be nicer really
    char *z = cstring(path);
    int fd = open(z, O_CREAT|O_WRONLY, 0644);
    write(fd, buffer_ref(b, 0), buffer_length(b));
    close(fd);
}


static CLOSURE_2_2(feach, void, heap, buffer, symbol, value);
static void feach(heap h, buffer path, symbol k, value v)
{
    if (k == sym(children)) {
        mkdir(cstring(path), 0777);
        iterate(v, closure(h, feach, h, aprintf(h, "%b/%b", path, symbol_string((symbol)k))));
    }
    // shouldn't need to poke at extents
    if (k == sym(extents))
        get(v, sym(contents), closure(h, write_file, path));
}
#define SECTOR_SIZE 512

static CLOSURE_2_1(fsc, void, heap, buffer, value);
static void fsc(heap h, buffer target_path, value root)
{
    rprintf ("meta: %v\n", root);
    iterate(root, closure(h, feach, h, target_path));
}

int main(int argc, char **argv)
{
    heap h = init_process_runtime();
    tuple root = allocate_tuple();
    int fd = open(argv[1], O_RDONLY);

    if (fd < 0) {
        rprintf("couldn't open file %s\n", argv[1]);
        exit(-1);
    }
    create_filesystem(h,
                      SECTOR_SIZE,
                      10ull * 1024 * 1024 * 1024,
                      closure(h, bread, fd),
                      closure(h, bwrite, fd),
                      closure(h, fsc, h, alloca_wrap_buffer(argv[2], runtime_strlen(argv[2]))));
}
