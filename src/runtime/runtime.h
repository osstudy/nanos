#pragma once
#include <uniboot.h>
#if !defined(BOOT) && !defined(STAGE3)
#include <unix_process_runtime.h>
#endif
typedef u8 boolean;
typedef u32 character;

#define true 1
#define false 0
#define infinity (-1ull)
#define INVALID_PHYSICAL ((u64)infinity)
#define INVALID_ADDRESS ((void *)infinity)

typedef u64 timestamp;

void console_write(char *s, bytes count);

void print_u64(u64 s);

extern void halt(char *format, ...) __attribute__((noreturn));
extern void vm_exit(u8 code) __attribute__((noreturn));
extern void print_stack_from_here();

// make into no-op for production
#ifdef NO_ASSERT
#define assert(x) do { if((x)) { } } while(0)
#else
#define assert(x)                                   \
    do {                                            \
        if (!(x)) {                                 \
            print_stack_from_here();                \
            halt("assertion %s failed in " __FILE__ ": %s() on line %d; halt\n", #x, __func__, __LINE__); \
        }                                           \
    } while(0)
#endif

void runtime_memcpy(void *a, const void *b, bytes len);

void runtime_memset(u8 *a, u8 b, bytes len);

int runtime_memcmp(const void *a, const void *b, bytes len);

static inline int runtime_strlen(const char *a)
{
    int i = 0;
    for (; *a; a++, i++);
    return i;
}

static inline void console(char *s)
{
    console_write(s, runtime_strlen(s));
}

#define pad(__x, __s) ((((__x) - 1) & (~((__s) - 1))) + (__s))

#define find_order(x) ((x) > 1 ? msb((x) - 1) + 1 : 0)

#define U64_FROM_BIT(x) (1ull<<(x))
#define MASK(x) (U64_FROM_BIT(x)-1)

#ifndef MIN
#define MIN(x, y) ((x) < (y)? (x):(y))
#endif
#ifndef MAX
#define MAX(x, y) ((x) > (y)? (x):(y))
#endif

#define offsetof(__t, __e) u64_from_pointer(&((__t)0)->__e)

#define check_flags_and_clear(x, f) ({boolean match = ((x) & (f)) != 0; (x) &= ~(f); match;})

static inline void zero(void *x, bytes length)
{
    runtime_memset(x, 0, length);
}

typedef struct heap *heap;
#include <table.h>
#include <heap/heap.h>
#include <kernel_heaps.h>

// transient is supposed to be cleaned up when we can guarantee that
// its out of scope - so we argue its ok to make it global. however
// there isn't a very good definition of what the lifetime of it is.
// transient objects shouldnt be shared.
extern heap transient;

#include <buffer.h>

typedef u64 physical;

physical vtop(void *x);

// used by stage2/stage3, not process

heap zero_wrap(heap meta, heap parent);

boolean validate_virtual(void *base, u64 length);

void sha256(buffer dest, buffer source);

#define stack_allocate __builtin_alloca

typedef struct buffer *buffer;

void print_number(buffer s, u64 x, int base, int pad);

#include <text.h>
#include <vector.h>
#include <format.h>

/* XXX: Note that printing function names will reveal our internals to
   some degree. All the logging stuff needs more time in the oven. */

#define msg_err(fmt, ...) rprintf("%s error: " fmt, __func__,   \
				  ##__VA_ARGS__)

#ifdef ENABLE_MSG_WARN
#define msg_warn(fmt, ...) rprintf("%s warning: " fmt, __func__,   \
				  ##__VA_ARGS__)
#else
#define msg_warn(fmt, ...)
#endif

#ifdef ENABLE_MSG_DEBUG
#define msg_debug(fmt, ...) rprintf("%s debug: " fmt, __func__, \
				    ##__VA_ARGS__)
#else
#define msg_debug(fmt, ...)
#endif

// value is a pointer that we can meaningfully inquire about the type of 
typedef void *value;

// try not to go crazy here
#define tag_unknown (0ull)
#define tag_symbol (1ull)
#define tag_tuple (2ull)
#define tag_string (3ull)
#define tag_buffer_promise (4ull) //?
#define tag_max (5ull)

#include <symbol.h>

#include <closure.h>
#include <closure_templates.h>

typedef closure_type(thunk, void);

#include <list.h>
#include <bitmap.h>
#include <status.h>
#include <pqueue.h>
#include <timer.h>
#include <tuple.h>
#include <range.h>

#define PAGELOG 12
#define PAGESIZE U64_FROM_BIT(PAGELOG)
#define PAGELOG_2M 21
#define PAGESIZE_2M U64_FROM_BIT(PAGELOG_2M)
#ifndef physical_from_virtual
physical physical_from_virtual(void *x);
#endif
void dump_ptes(void *x);
void update_map_flags(u64 vaddr, u64 length, u64 flags);
void zero_mapped_pages(u64 vaddr, u64 length);
void unmap_pages_with_handler(u64 virtual, u64 length, range_handler rh);
static inline void unmap_pages(u64 virtual, u64 length)
{
    unmap_pages_with_handler(virtual, length, 0);
}

void remap_pages(u64 vaddr_new, u64 vaddr_old, u64 length, heap h);

typedef closure_type(buffer_handler, void, buffer);
typedef closure_type(block_io, void, void *, range, status_handler);

// should be  (parser, parser, character)
typedef closure_type(parser, void *, character);
// change to status_handler
typedef closure_type(parse_error, void, buffer);
typedef closure_type(parse_finish, void, void *);
parser tuple_parser(heap h, parse_finish c, parse_error err);
parser parser_feed (parser p, buffer b);

// RNG
void init_random();
u64 random_u64();
u64 random_buffer(buffer b);

typedef struct signature {
    u64 s[4];
} *signature;

void init_runtime(kernel_heaps kh);
heap allocate_tagged_region(kernel_heaps kh, u64 tag);
typedef closure_type(buffer_promise, void, buffer_handler);

extern thunk ignore;
extern status_handler ignore_status;

#include <metadata.h>

#define KB 1024
#define MB (KB*KB)
#define GB (KB*MB)

// fix transient - also should be legit to use the space between end and length w/o penalty
#define cstring(__b) ({buffer n = little_stack_buffer(512); push_buffer(n, __b); push_u8(n, 0); n->contents;})

extern heap transient;

typedef struct merge *merge;

merge allocate_merge(heap h, status_handler completion);
status_handler apply_merge(merge m);

void __stack_chk_guard_init();

#define _countof(a) (sizeof(a) / sizeof(*(a)))
