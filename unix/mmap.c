#include <unix_internal.h>

CLOSURE_2_1(mmap_read_complete, void, thread, u64, status);
void mmap_read_complete(thread t, u64 where, status s)
{
    // maybe read zeros? seems like we should be able to find out the
    // actual length?
    //    if (len > msize) {
    //  bss doesn't need to be contiguous
    //u64 bss = pad(len, PAGESIZE) - msize;
    //        map(where + msize, allocate_u64(p->physical, bss), bss, p->pages);
    //        zero(pointer_from_u64(where+msize), bss);
    //    }
    
    // ok, if we change pages entries we need to flush the tlb...dont need
    // to do this every time.. there is also a per-page variant. va range
    // tree would let us know
    u64 x;
    mov_from_cr("cr3", x);
    mov_to_cr("cr3", x);
    set_syscall_return(t, where);
    enqueue(runqueue, t->run);
}
                       
void *mremap(void *old_address, u64 old_size,  u64 new_size, int flags,  void *new_address )
{
    // this seems poorly thought out - what if there is a backing file?
    // and if its anonymous why do we care where it is..i guess this
    // is just for large realloc operations? if these aren't aligned
    // its completely unclear what to do
    u64 align =  ~MASK(PAGELOG);
    if (new_size > old_size) {
        u64 diff = pad(new_size - old_size, PAGESIZE);
        u64 base = u64_from_pointer(old_address + old_size) & align;
        void *r = allocate(current->p->physical,diff);
        if (u64_from_pointer(r) == INVALID_PHYSICAL) {
            // MAP_FAILED
            return r;
        }
        map(base, physical_from_virtual(r), diff, current->p->pages);
        zero(pointer_from_u64(base), diff); 
    }
    //    map(u64_from_pointer(new_address)&align, physical_from_virtual(old_address), old_size, current->p->pages);
    return old_address;
}


static int mincore(void *addr, u64 length, u8 *vec)
{
    if (validate_virtual(addr, length)) {
        u32 vlen = pad(length, PAGESIZE) >> PAGELOG;
        // presumably it wants the right valid bits set? - go doesn't seem to use it this way
        for (int i = 0; i< vlen; i++) vec[i] = 1;
        return 0;
    }
    return -ENOMEM;
}


static void *mmap(void *target, u64 size, int prot, int flags, int fd, u64 offset)
{
    process p = current->p;
    // its really unclear whether this should be extended or truncated
    u64 len = pad(size, PAGESIZE);
    //gack
    len = len & MASK(32);
    u64 where = u64_from_pointer(target);

    // xx - go wants to specify target without map fixed, and has some strange
    // retry logic around it
    if (!(flags &MAP_FIXED) && !target) {
        if (flags & MAP_32BIT)
            where = allocate_u64(current->p->virtual32, len);
        else
            where = allocate_u64(current->p->virtual, len);
    }

    rprintf("map at %p %p\n", where, size);
    // make a generic zero page function
    if (flags & MAP_ANONYMOUS) {
        u64  m = allocate_u64(p->physical, len);
        if (m == INVALID_PHYSICAL) return pointer_from_u64(m);
        map(where, m, len, p->pages);
        zero(pointer_from_u64(where), len);
        return pointer_from_u64(where);
    }

    // backing file case. xxx - this should be demand paged and set
    // up to handle writes
    file f = current->p->files[fd];
    u64 backing = allocate_u64(p->physical, size);
    // check fail!
    // truncate and page pad the read?
    // mutal misalignment?...discontiguous backing?
    map(where, backing, size, p->pages);


    // issue #34 - on demand page mapping - what about non-files?
    apply(f->read, pointer_from_u64(where), size, offset, closure(p->h, mmap_read_complete, current, where));
    runloop();
}

void register_mmap_syscalls(void **map)
{
    register_syscall(map, SYS_mincore, mincore);
    register_syscall(map, SYS_mmap, mmap);
    register_syscall(map, SYS_mremap, mremap);        
    register_syscall(map, SYS_munmap, syscall_ignore);
    register_syscall(map, SYS_mprotect, syscall_ignore);
}

