#include <unix_internal.h>


static CLOSURE_1_1(status_thread_wakeup, void, thread, status);
static void status_thread_wakeup(thread t, status s)
{
    // generalize error translation?
    enqueue(runqueue, t->run);
}

int read(int fd, u8 *dest, bytes length)
{
    file f = resolve_fd(current->p, fd);
    // consider an official per-syscall/transient heap 
    heap h = current->p->k->general;
    apply(f->read, dest, length, f->offset, current->status_wakeup);
    runloop();
}

int write(int fd, u8 *body, bytes length)
{
    file f = resolve_fd(current->p, fd);
    heap h = current->p->k->general;
    // store per thread?
    int res = apply(f->write, body, length, f->offset, current->status_wakeup);
    f->offset += length;
    runloop();
}

static int writev(int fd, iovec v, int count)
{
    int res;
    heap h = current->p->k->general;
    // store per thread?    
    merge m = allocate_merge(h, current->status_wakeup);
    file f = resolve_fd(current->p, fd);
    for (int i = 0; i < count; i++) {
        status_handler partial = apply(m);
        apply(f->write, v[i].address, v[i].length, f->offset, partial);
    }
    return res;
}

static int access(char *name, int mode)
{
    void *where;
    bytes length;
    if (!resolve_cstring(current->p->cwd, name)) {
        return -ENOENT;
    }
    return 0;
}


static CLOSURE_1_4(contents_read, int, value, void *, u64, u64, status_handler);
static int contents_read(value n, void *dest, u64 length, u64 offset, status_handler completion)
{
    kernel k = current->p->k;        
    filesystem_read(n, dest, length, offset, current->status_wakeup);
    runloop();
}

int openat(char *name, int flags, int mode)
{
    rprintf("openat not supportted, should be cake though\n");
    return -ENOENT;
}

static CLOSURE_1_0(file_close, int, file);
static int file_close(file f)
{
    kernel k = current->p->k;
    deallocate(k->file_cache, f, sizeof(struct file));
    return 0;
}

s64 open(char *name, int flags, int mode)
{
    tuple n;
    bytes length;
    kernel k = current->p->k;
    heap h = k->general;
    
    // fix - lookup should be robust
    if (name == 0) return -EINVAL;
    if (!(n = resolve_cstring(current->p->cwd, name))) {
        rprintf("open %s - not found\n", name);
        return -ENOENT;
    }

    file f = allocate(k->file_cache, sizeof(struct file));
    if (f == INVALID_ADDRESS) {
	msg_err("failed to allocate struct file\n");
	return -ENOMEM;
    }
    int fd = allocate_fd(current->p, f);
    if (fd == INVALID_PHYSICAL) {
	deallocate(k->file_cache, f, sizeof(struct file));
	return -EMFILE;
    }
    rprintf ("open %s %p\n", name, fd);
    f->n = n;
    f->read = closure(h, contents_read, n);
    f->close = closure(h, file_close, f);
    f->offset = 0;
    return fd;
}

static void fill_stat(tuple n, struct stat *s)
{
    buffer b;
    zero(s, sizeof(struct stat));
    s->st_dev = 0;
    s->st_ino = u64_from_pointer(n);
    // dir doesn't have contents
    if (!(b = table_find(n, sym(contents)))) {
        s->st_mode = S_IFDIR | 0777;
        return;
    }  else {
        s->st_mode = S_IFREG | 0644;
        s->st_size = buffer_length(b);
    }
}

static int fstat(int fd, struct stat *s)
{
    file f = resolve_fd(current->p, fd);            
    // take this from tuple space
    if (fd == 1) {
        s->st_mode = S_IFIFO;
        return 0;
    }
    fill_stat(f->n, s);
    return 0;
}


static int stat(char *name, struct stat *s)
{
    tuple n;

    if (!(n = resolve_cstring(current->p->cwd, name))) {    
        return -ENOENT;
    }
    fill_stat(n, s);
    return 0;
}

static u64 lseek(int fd, u64 offset, int whence)
{
    file f = resolve_fd(current->p, fd);            
    return f->offset;
}


static int uname(struct utsname *v)
{
    char rel[]= "4.4.0-87";
    char sys[] = "pugnix";
    runtime_memcpy(v->sysname,sys, sizeof(sys));
    runtime_memcpy(v->release, rel, sizeof(rel));
    return 0;
}

int getrlimit(int resource, struct rlimit *rlim)
{
    switch (resource) {
    case RLIMIT_STACK:
        rlim->rlim_cur = 2*1024*1024;
        rlim->rlim_max = 2*1024*1024;
        return 0;
    case RLIMIT_NOFILE:
        // we .. .dont really have one?
        rlim->rlim_cur = 65536;
        rlim->rlim_max = 65536;
        return 0;
    }
    return -1;
}

static char *getcwd(char *buf, u64 length)
{
    runtime_memcpy(buf, "/", 2);
    return buf;
}

static void *brk(void *x)
{
    process p = current->p;
    kernel k = p->k;

    if (x) {
        if (p->brk > x) {
            p->brk = x;
            // free
        } else {
            // I guess assuming we're aligned
            u64 alloc = pad(u64_from_pointer(x), PAGESIZE) - pad(u64_from_pointer(p->brk), PAGESIZE);
            map(u64_from_pointer(p->brk), allocate_u64(k->physical, alloc), alloc, k->pages);
            // people shouldn't depend on this
            zero(p->brk, alloc);
            p->brk += alloc;         
        }
    }
    return p->brk;
}

u64 readlink(const char *pathname, char *buf, u64 bufsiz)
{
    return -EINVAL;

}

int close(int fd)
{
    file f = resolve_fd(current->p, fd);
    if (f == INVALID_ADDRESS)
	return -EBADF;
    deallocate_fd(current->p, fd, f);
    if (f->close)
	return apply(f->close);
    msg_err("no close handler for fd %d\n", fd);
    return 0;
}

u64 fcntl(int fd, int cmd)
{
    return O_RDWR;
}

u64 syscall_ignore()
{
    return 0;
}

u64 getpid()
{
    return current->p->pid;
}

u64 sched_yield()
{
    set_syscall_return(current, 0);                                
    thread_wakeup(current);
    thread_sleep(current);
}

void exit(int code)
{
    halt("");
    while(1); //compiler put a noreturn on exit
}

void register_file_syscalls(void **map)
{
    register_syscall(map, SYS_read, read);
    register_syscall(map, SYS_write, write);
    register_syscall(map, SYS_open, open);
    register_syscall(map, SYS_fstat, fstat);
    register_syscall(map, SYS_stat, stat);
    register_syscall(map, SYS_writev, writev);
    register_syscall(map, SYS_access, access);
    register_syscall(map, SYS_lseek, lseek);
    register_syscall(map, SYS_fcntl, fcntl);
    register_syscall(map, SYS_getcwd, getcwd);
    register_syscall(map, SYS_readlink, readlink);
    register_syscall(map, SYS_close, close);
    register_syscall(map, SYS_sched_yield, sched_yield);
    register_syscall(map, SYS_brk, brk);
    register_syscall(map, SYS_uname, uname);
    register_syscall(map, SYS_getrlimit, getrlimit);
    register_syscall(map, SYS_getpid, getpid);    
    register_syscall(map, SYS_exit, exit);
}

