#include <runtime.h>
#include <region.h>

#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <tfs.h>
#include <errno.h>
#include <string.h>

static CLOSURE_1_3(bwrite, void, descriptor, void *, range, status_handler);
static void bwrite(descriptor d, void * s, range blocks, status_handler c)
{

}

static CLOSURE_2_3(bread, void, descriptor, u64, void *, range, status_handler);
static void bread(descriptor d, u64 fs_offset, void *dest, range blocks, status_handler c)
{
    ssize_t xfer, total = 0;
    u64 offset = fs_offset + (blocks.start << SECTOR_OFFSET);
    u64 length = range_span(blocks) << SECTOR_OFFSET;
    while (total < length) {
        xfer = pread(d, dest + total, length - total, offset + total);
        if (xfer < 0 && errno != EINTR) {
            apply(c, timm("read-error", "%s", strerror(errno)));
            return;
        }
        total += xfer;
    }
    apply(c, STATUS_OK);
}

CLOSURE_1_1(write_file, void, buffer, buffer);
void write_file(buffer path, buffer b)
{
    // openat would be nicer really
    char *z = cstring(path);
    int fd = open(z, O_CREAT|O_WRONLY, 0644);
    ssize_t xfer, len = buffer_length(b);
    while (len > 0) {
        xfer = write(fd, buffer_ref(b, 0), len);
        if (xfer < 0 && errno != EINTR) {
            perror("file write");
            close(fd);
            exit(EXIT_FAILURE);
        }
        len -= xfer;
        buffer_consume(b, xfer);
    }
    close(fd);
}

// h just for extending path
// isn't there an internal readdir?
void readdir(filesystem fs, heap h, tuple w, buffer path)
{
    table_foreach(w, k, v) {
        if (k == sym(children)) {
            mkdir(cstring(path), 0777);
            table_foreach((tuple)v, k, vc) {
                if (k == sym_this(".") || k == sym_this(".."))
                    continue;
                readdir(fs, h, (tuple)vc, aprintf(h, "%b/%b", path, symbol_string((symbol)k)));
            }
        }
        if (k == sym(extents))
            filesystem_read_entire(fs, w, h, closure(h, write_file, path), (void *)ignore);
    }
}

static CLOSURE_3_2(fsc, void, heap, buffer, tuple, filesystem, status);
static void fsc(heap h, buffer b, tuple root, filesystem fs, status s)
{
    if (!is_ok(s)) {
        rprintf("failed to initialize filesystem: %v\n", s);
        exit(EXIT_FAILURE);
    }

    buffer rb = allocate_buffer(h, PAGESIZE);
    print_root(rb, root);
    buffer_print(rb);
    rprintf("\n");
    deallocate_buffer(rb);

    readdir(fs, h, root, b);
}

static u64 get_fs_offset(descriptor fd)
{
    char buf[512];

    ssize_t nr = read(fd, buf, sizeof(buf));
    if (nr < 0 || nr < sizeof(buf)) {
        perror("read");
	exit(EXIT_FAILURE);
    }

    // last two bytes should be MBR signature
    u16 *mbr_sig = (u16 *) (buf + sizeof(buf) - sizeof(*mbr_sig));
    // FS region comes right before MBR partitions (see boot/stage1.s)
    region r = (region) ((char *) mbr_sig - (4 * 16) - sizeof(*r));

    if (*mbr_sig != 0xaa55 || r->type != REGION_FILESYSTEM) {
        // probably raw filesystem
        return 0;
    }

    u64 fs_offset = SECTOR_SIZE + r->length;
    rprintf("detected filesystem at 0x%lx\n", fs_offset);
    return fs_offset;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        const char *p = strrchr(argv[0], '/');
	p = p != NULL ? p + 1 : argv[0];
        fprintf(stderr, "usage: %s <fs image> <target dir>\n", p);
        exit(EXIT_FAILURE);
    }

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "couldn't open file %s: %s\n", argv[1], strerror(errno));
        exit(EXIT_FAILURE);
    }

    heap h = init_process_runtime();
    tuple root = allocate_tuple();
    create_filesystem(h,
                      SECTOR_SIZE,
                      10ull * 1024 * 1024 * 1024,
                      h,
                      closure(h, bread, fd, get_fs_offset(fd)),
                      closure(h, bwrite, fd),
                      root,
                      closure(h, fsc, h, alloca_wrap_buffer(argv[2], runtime_strlen(argv[2])), root));
    return EXIT_SUCCESS;
}
