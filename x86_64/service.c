#include <sruntime.h>
#include <pci.h>
#include <virtio.h>

extern void startup();
extern void start_interrupts();


static u8 bootstrap_region[1024];
static u64 bootstrap_base = (unsigned long long)bootstrap_region;
static u64 bootstrap_alloc(heap h, bytes length)
{
    u64 result = bootstrap_base;
    if ((result + length) >=  (u64_from_pointer(bootstrap_region) + sizeof(bootstrap_region)))
        return INVALID_PHYSICAL;
    bootstrap_base += length;
    return result;
}

typedef struct backed {
    struct heap h;
    heap physical;
    heap virtual;
    heap pages;
} *backed;
    

static u64 physically_backed_alloc(heap h, bytes length)
{
    backed b = (backed)h;
    u64 len = pad(length, h->pagesize);
    u64 p = allocate_u64(b->physical, len);

    if (p != INVALID_PHYSICAL) {
        u64 v = allocate_u64(b->virtual, len);
        if (v != INVALID_PHYSICAL) {
            // map should return allocation status
            map(v, p, len, b->pages);
            return v;
        }
    }
    return INVALID_PHYSICAL; 
}

static heap physically_backed(heap meta, heap virtual, heap physical, heap pages)
{
    backed b = allocate(meta, sizeof(struct backed));
    b->h.alloc = physically_backed_alloc;
    // freelist
    b->h.dealloc = null_dealloc;
    b->physical = physical;
    b->virtual = virtual;
    b->pages = pages;
    b->h.pagesize = PAGESIZE;
    return (heap)b;
}


static CLOSURE_6_0(read_complete, void, void *, u64, heap, heap, heap, heap);
static void read_complete(void *target, u64 length, heap pages, heap general, heap physical, heap virtual)
{
    rprintf("read complete %p %p %p\n", physical_from_virtual(target), target, *(u64 *)target);
    buffer b = allocate_buffer(general, length);
    b->contents = target;
    b->start = 0;
    b->end = length;
    rprintf ("pog: %p\n", b);
    startup(pages, general, physical, virtual, b);
}


// bad global, put in the filesystem space
extern u64 storage_length;

// init linker set
void init_service()
{
    struct heap bootstrap;

    console("service\n");

    bootstrap.alloc = bootstrap_alloc;
    bootstrap.dealloc = null_dealloc;
    heap pages = region_allocator(&bootstrap, PAGESIZE, REGION_IDENTITY);
    heap physical = region_allocator(&bootstrap, PAGESIZE, REGION_PHYSICAL);    

    heap virtual = create_id_heap(&bootstrap, HUGE_PAGESIZE, (1ull<<VIRTUAL_ADDRESS_BITS)- HUGE_PAGESIZE, HUGE_PAGESIZE);
    heap backed = physically_backed(&bootstrap, virtual, physical, pages);
    
    // on demand stack allocation
    u64 stack_size = 32*PAGESIZE;
    u64 stack_location = allocate_u64(backed, stack_size);
    stack_location += stack_size - 16;
    asm ("mov %0, %%rsp": :"m"(stack_location));

    // stack was here, map this invalid so we get crashes
    // in the appropriate place
    map(0, INVALID_PHYSICAL, PAGESIZE, pages);
    
    // rdtsc is corrupting something oddly
    //    init_clock(backed);

    heap misc = allocate_rolling_heap(backed);
    start_interrupts(pages, misc, physical);
    init_symbols(misc);
    init_pci(misc);    
    init_virtio_storage(misc, backed, pages, virtual);
    init_virtio_network(misc, backed, pages);            
    pci_discover(pages, virtual);
    enable_interrupts();

    rprintf ("zig\n");
    u64 fs_offset;
    for (region e = regions; region_type(e); e -= 1) {
        if (region_type(e) == REGION_FILESYSTEM)
            fs_offset = region_base(e);
    }
    u64 len = storage_length - fs_offset;
    void *k = allocate(virtual, len);
    map(u64_from_pointer(k), allocate_u64(physical, len), len, pages);
    void *z = closure(misc, read_complete, k, len, pages, backed, physical, virtual);
    rprintf ("what: %p\n", z);
    storage_read(k, fs_offset, len, z);
    while (1) __asm__("hlt");
}
