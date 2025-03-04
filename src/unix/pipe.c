#include <unix_internal.h>
#include <buffer.h>

#ifdef PIPE_DEBUG
#define pipe_debug(x, ...) do {log_printf("PIPE", x, ##__VA_ARGS__);} while(0)
#else
#define pipe_debug(x, ...)
#endif

#define INITIAL_PIPE_DATA_SIZE  100
#define DEFAULT_PIPE_MAX_SIZE   (16 * PAGESIZE) /* see pipe(7) */
#define PIPE_READ               0
#define PIPE_WRITE              1

typedef struct pipe *pipe;

typedef struct pipe_file *pipe_file;

struct pipe_file {
    struct fdesc f;       /* must be first */
    int fd;
    pipe pipe;
    blockq bq;
};

struct pipe {
    struct pipe_file files[2];
    process p;
    heap h;
    u64 ref_cnt;
    u64 max_size;               /* XXX: can change with F_SETPIPE_SZ */
    buffer data;
};

#define BUFFER_DEBUG(BUF,LENGTH) do { \
    pipe_debug("%s:%d - requested %d -- contents %p start/end %d/%d  -- len %d %d\n", \
        __func__, __LINE__, \
        (LENGTH), \
        (BUF)->contents, \
        (BUF)->start, \
        (BUF)->end, \
        (BUF)->length, buffer_length((BUF))); \
} while(0)

boolean pipe_init(unix_heaps uh)
{
    heap general = heap_general((kernel_heaps)uh);
    heap backed = heap_backed((kernel_heaps)uh);

    uh->pipe_cache = allocate_objcache(general, backed, sizeof(struct pipe), PAGESIZE);
    return (uh->pipe_cache == INVALID_ADDRESS ? false : true);
}

static inline void pipe_notify_reader(pipe_file pf, int events)
{
    pipe_file read_pf = &pf->pipe->files[PIPE_READ];
    if (read_pf->fd != -1) {
        if (events & EPOLLHUP)
            blockq_flush(read_pf->bq);
        else
            blockq_wake_one(read_pf->bq);
        notify_dispatch(read_pf->f.ns, events);
    }
}

static inline void pipe_notify_writer(pipe_file pf, int events)
{
    pipe_file write_pf = &pf->pipe->files[PIPE_WRITE];
    if (write_pf->fd != -1) {
        if (events & EPOLLHUP)
            blockq_flush(write_pf->bq);
        else
            blockq_wake_one(write_pf->bq);
        notify_dispatch(write_pf->f.ns, events);
    }
}

static void pipe_release(pipe p)
{
    if (!p->ref_cnt || (fetch_and_add(&p->ref_cnt, -1) == 1)) {
        pipe_debug("%s(%p): deallocating pipe\n", __func__, p);
        if (p->data != INVALID_ADDRESS)
            deallocate_buffer(p->data);

        unix_cache_free(get_unix_heaps(), pipe, p);
    }
}

static inline void pipe_dealloc_end(pipe p, pipe_file pf)
{
    if (pf->fd != -1) {
        if (&p->files[PIPE_READ] == pf) {
            pipe_notify_writer(pf, EPOLLHUP);
            pipe_debug("%s(%p): writer notified\n", __func__, p);
        }
        if (&p->files[PIPE_WRITE] == pf) {
            pipe_notify_reader(pf, EPOLLIN | EPOLLHUP);
            pipe_debug("%s(%p): reader notified\n", __func__, p);
        }
        deallocate_blockq(pf->bq);
        release_fdesc(&pf->f);
        pf->fd = -1;
        pipe_release(p);
    }
}

static CLOSURE_1_0(pipe_close, sysreturn, pipe_file);
static sysreturn pipe_close(pipe_file pf)
{
    pipe_dealloc_end(pf->pipe, pf);
    return 0;
}

static CLOSURE_5_1(pipe_read_bh, sysreturn,
        pipe_file, thread, void *, u64, io_completion,
        boolean);
static sysreturn pipe_read_bh(pipe_file pf, thread t, void *dest, u64 length,
        io_completion completion, boolean blocked)
{
    buffer b = pf->pipe->data;
    int real_length = MIN(buffer_length(b), length);
    if (real_length == 0) {
        if (pf->pipe->files[PIPE_WRITE].fd == -1)
            goto out;
        if (pf->f.flags & O_NONBLOCK) {
            real_length = -EAGAIN;
            goto out;
        }
        return infinity;
    }

    buffer_read(b, dest, real_length);
    pipe_notify_writer(pf, EPOLLOUT);

    // If we have consumed all of the buffer, reset it. This might prevent future writes to allocte new buffer
    // in buffer_write/buffer_extend. Can improve things until a proper circular buffer is available
    if (buffer_length(b) == 0) {
        buffer_clear(b);
        notify_dispatch(pf->f.ns, 0); /* for edge trigger */
    }
  out:
    if (blocked)
        blockq_set_completion(pf->bq, completion, t, real_length);

    return real_length;
}

static CLOSURE_1_6(pipe_read, sysreturn,
        pipe_file,
        void *, u64, u64, thread, boolean, io_completion);
static sysreturn pipe_read(pipe_file pf, void *dest, u64 length, u64 offset_arg,
        thread t, boolean bh, io_completion completion)
{
    if (length == 0)
        return 0;

    blockq_action ba = closure(pf->pipe->h, pipe_read_bh, pf, t, dest, length,
            completion);
    return blockq_check(pf->bq, !bh ? t : 0, ba);
}

static CLOSURE_5_1(pipe_write_bh, sysreturn,
        pipe_file, thread, void *, u64, io_completion,
        boolean);
static sysreturn pipe_write_bh(pipe_file pf, thread t, void *dest, u64 length,
        io_completion completion, boolean blocked)
{
    sysreturn rv = 0;
    pipe p = pf->pipe;
    buffer b = p->data;
    u64 avail = p->max_size - buffer_length(b);

    if (avail == 0) {
        if (pf->pipe->files[PIPE_READ].fd == -1) {
            rv = -EPIPE;
            goto out;
        }
        if (pf->f.flags & O_NONBLOCK) {
            rv = -EAGAIN;
            goto out;
        }
        return infinity;
    }

    u64 real_length = MIN(length, avail);
    buffer_write(b, dest, real_length);
    if (avail == length)
        notify_dispatch(pf->f.ns, 0); /* for edge trigger */

    pipe_notify_reader(pf, EPOLLIN);

    rv = real_length;
  out:
    if (blocked)
        blockq_set_completion(pf->bq, completion, t, rv);

    return rv;
}

static CLOSURE_1_6(pipe_write, sysreturn,
        pipe_file,
        void *, u64, u64, thread, boolean, io_completion);
static sysreturn pipe_write(pipe_file pf, void * dest, u64 length, u64 offset,
        thread t, boolean bh, io_completion completion)
{
    if (length == 0)
        return 0;

    blockq_action ba = closure(pf->pipe->h, pipe_write_bh, pf, t, dest, length,
            completion);
    return blockq_check(pf->bq, !bh ? t : 0, ba);
}

static CLOSURE_1_0(pipe_read_events, u32, pipe_file);
static u32 pipe_read_events(pipe_file pf)
{
    assert(pf->f.read);
    u32 events = buffer_length(pf->pipe->data) ? EPOLLIN : 0;
    if (pf->pipe->files[PIPE_WRITE].fd == -1)
        events |= EPOLLIN | EPOLLHUP;
    return events;
}

static CLOSURE_1_0(pipe_write_events, u32, pipe_file);
static u32 pipe_write_events(pipe_file pf)
{
    assert(pf->f.write);
    u32 events = buffer_length(pf->pipe->data) < pf->pipe->max_size ? EPOLLOUT : 0;
    if (pf->pipe->files[PIPE_READ].fd == -1)
        events |= EPOLLHUP;
    return events;
}

#define PIPE_BLOCKQ_LEN         32

int do_pipe2(int fds[2], int flags)
{
    unix_heaps uh = get_unix_heaps();

    pipe pipe = unix_cache_alloc(get_unix_heaps(), pipe);
    if (pipe == INVALID_ADDRESS) {
        msg_err("failed to allocate struct pipe\n");
        return -ENOMEM;
    }

    if (flags & ~(O_CLOEXEC | O_DIRECT | O_NONBLOCK))
        return -EINVAL;

    if (flags & O_DIRECT) {
        msg_err("O_DIRECT unsupported\n");
        return -EOPNOTSUPP;
    }

    pipe->data = INVALID_ADDRESS;
    pipe->files[PIPE_READ].fd = -1;
    pipe->files[PIPE_WRITE].fd = -1;
    pipe->h = heap_general((kernel_heaps)uh);
    pipe->p = current->p;
    pipe->files[PIPE_READ].pipe = pipe;
    pipe->files[PIPE_WRITE].pipe = pipe;
    pipe->ref_cnt = 0;
    pipe->max_size = DEFAULT_PIPE_MAX_SIZE;
    pipe->data = allocate_buffer(pipe->h, INITIAL_PIPE_DATA_SIZE);
    if (pipe->data == INVALID_ADDRESS) {
        msg_err("failed to allocate pipe's data buffer\n");
        pipe_release(pipe);
        return -ENOMEM;
    }

    pipe_file reader = &pipe->files[PIPE_READ];
    reader->fd = fds[PIPE_READ] = allocate_fd(pipe->p, reader);
    init_fdesc(pipe->h, &reader->f, FDESC_TYPE_PIPE);
    reader->bq = allocate_blockq(pipe->h, "pipe read", PIPE_BLOCKQ_LEN, 0);
    reader->f.read = closure(pipe->h, pipe_read, reader);
    reader->f.close = closure(pipe->h, pipe_close, reader);
    reader->f.events = closure(pipe->h, pipe_read_events, reader);
    reader->f.flags = (flags & O_NONBLOCK) | O_RDONLY;

    pipe_file writer = &pipe->files[PIPE_WRITE];
    init_fdesc(pipe->h, &writer->f, FDESC_TYPE_PIPE);
    writer->fd = fds[PIPE_WRITE] = allocate_fd(pipe->p, writer);
    writer->bq = allocate_blockq(pipe->h, "pipe write", PIPE_BLOCKQ_LEN, 0);
    writer->f.write = closure(pipe->h, pipe_write, writer);
    writer->f.close = closure(pipe->h, pipe_close, writer);
    writer->f.events = closure(pipe->h, pipe_write_events, writer);
    writer->f.flags = (flags & O_NONBLOCK) | O_WRONLY;

    pipe->ref_cnt = 2;

    return 0;
}
