#include <runtime.h>
#include <x86_64.h>
#include <page.h>

typedef struct backed {
    struct heap h;
    heap physical;
    heap virtual;
    heap pages;
} *backed;

void physically_backed_dealloc_virtual(heap h, u64 x, bytes length)
{
    backed b = (backed)h;       /* XXX need to keep track of heap type... */
    u64 padlen = pad(length, h->pagesize);
    if ((x & (h->pagesize-1))) {
	msg_err("attempt to free unaligned area at %lx, length %x; leaking\n", x, length);
	return;
    }

    deallocate(b->virtual, pointer_from_u64(x), padlen);
    unmap(x, padlen, b->pages);
}

static void physically_backed_dealloc(heap h, u64 x, bytes length)
{
    backed b = (backed)h;
    u64 padlen = pad(length, h->pagesize);
    if ((x & (h->pagesize-1))) {
	msg_err("attempt to free unaligned area at %lx, length %x; leaking\n", x, length);
	return;
    }

    deallocate(b->physical, physical_from_virtual(pointer_from_u64(x)), padlen);
    deallocate(b->virtual, pointer_from_u64(x), padlen);
    unmap(x, padlen, b->pages);
}

static u64 physically_backed_alloc(heap h, bytes length)
{
    backed b = (backed)h;
    u64 len = pad(length, h->pagesize);
    u64 p = allocate_u64(b->physical, len);

    if (p != INVALID_PHYSICAL) {
        u64 v = allocate_u64(b->virtual, len);
        if (v != INVALID_PHYSICAL) {
            map(v, p, len, PAGE_WRITABLE | PAGE_NO_EXEC, b->pages);
            return v;
        }
    }
    return INVALID_PHYSICAL; 
}

heap physically_backed(heap meta, heap virtual, heap physical, heap pages, u64 pagesize)
{
    backed b = allocate(meta, sizeof(struct backed));
    b->h.alloc = physically_backed_alloc;
    b->h.dealloc = physically_backed_dealloc;
    b->physical = physical;
    b->virtual = virtual;
    b->pages = pages;
    b->h.pagesize = pagesize;
    return (heap)b;
}
