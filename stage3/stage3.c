#include <runtime.h>
#include <kvm_platform.h>
#include <tfs.h>
#include <unix.h>
#include <gdb.h>


static CLOSURE_6_1(read_program_complete, void, tuple, heap, heap, heap, heap, heap, value);
static void read_program_complete(tuple root, heap pages, heap general, heap physical, heap virtual, heap backed, value v)
{
    buffer b = v;
    // value error overload
    //    elf_symbols(exc, closure(general, prinsym));
    rprintf ("read program complete: %p\n", *(u64 *)buffer_ref(b, 0));
    exec_elf(b, root, root, general, physical, pages, virtual, backed);
}

void startup(heap pages,
             heap general,
             heap physical,
             heap virtual,
             tuple root)
{
    // xxx - loader had us throw away the first 4k page
    //    elf_symbols(START, closure(general, prinsym)); 
    init_unix(general, pages, physical, root);
    value p = table_find(root, sym(program));
    // error on not program 
    // copied from service.c - how much should we pass?
    heap virtual_pagesized = allocate_fragmentor(general, virtual, PAGESIZE);
    heap backed = physically_backed(general, virtual_pagesized, physical, pages);
    value_handler pg = closure(general, read_program_complete, root, pages, general, physical, virtual, backed);

    vector path = split(general, p, '/');
    tuple pro = resolve_path(root, path);
    get(pro, sym(contents), pg);
}

