#include <tfs_internal.h>

//#define TFS_DEBUG
//#define TFS_REPORT_SHA256
#if defined(TFS_DEBUG)
#define tfs_debug(x, ...) do {rprintf("TFS: " x, ##__VA_ARGS__);} while(0)
#else
#define tfs_debug(x, ...)
#endif

#if defined(TFS_REPORT_SHA256) && !defined(BOOT)
static inline void report_sha256(buffer b)
{
    buffer sha = little_stack_buffer(32);
    sha256(sha, b);
    rprintf("   SHA256: %16lx%16lx%16lx%16lx\n",
            be64toh(*(u64*)buffer_ref(sha, 0)),
            be64toh(*(u64*)buffer_ref(sha, 8)),
            be64toh(*(u64*)buffer_ref(sha, 16)),
            be64toh(*(u64*)buffer_ref(sha, 24)));
}
#else
#define report_sha256(b)
#endif

struct fsfile {
    rangemap extentmap;
    filesystem fs;
    u64 length;
    tuple md;
};

u64 fsfile_get_length(fsfile f)
{
    return f->length;
}

void fsfile_set_length(fsfile f, u64 length)
{
    f->length = length;
}

/* range_from_rmnode for file extent range */
typedef struct extent {
    struct rmnode node;
    u64 block_start;
    u64 allocated;
} *extent;

static inline extent allocate_extent(heap h, range init_range, u64 block_start, u64 allocated)
{
    extent e = allocate(h, sizeof(struct extent));
    if (e == INVALID_ADDRESS)
        return e;
    rmnode_init(&e->node, init_range);
    e->block_start = block_start;
    e->allocated = allocated;
    return e;
}

static void filesystem_flush_log(filesystem fs)
{
    log_flush(fs->tl);
}

/* XXX don't ignore status
       set fs dirty bit and flush at end of fs operation
*/
void filesystem_write_tuple(filesystem fs, tuple t, status_handler sh)
{
    log_write(fs->tl, t, sh);
}

void filesystem_write_eav(filesystem fs, tuple t, symbol a, value v, status_handler sh)
{
    log_write_eav(fs->tl, t, a, v, sh);
}

static symbol fs_get_parent_child(filesystem fs, tuple cwd, const char *fp,
        tuple *parent, tuple *child)
{
    int fp_len = runtime_strlen(fp);
    char *fp_copy = allocate(fs->h, fp_len + 1);
    assert(fp_copy != INVALID_ADDRESS);
    runtime_memcpy(fp_copy, fp, fp_len);
    fp_copy[fp_len] = '\0';
    char *token, *rest = fp_copy;
    symbol child_sym = 0;
    while ((token = runtime_strtok_r(rest, "/", &rest))) {
        symbol s = sym_this(token);
        tuple t = lookup(cwd, s);
        if (*rest != '\0') {
            if (!t) {   /* entry not found */
                break;
            }
            cwd = t;
            continue;
        }
        if (parent) {
            *parent = cwd;
        }
        if (child) {
            *child = t;
        }
        child_sym = s;
        break;
    }
    deallocate(fs->h, fp_copy, fp_len + 1);
    return child_sym;
}

/* This can evolve into / be replaced by a more general page / buffer
   chace interface. We should be able to maintain and recycle dma
   buffers for anything in the system, or at least virtio. These
   could be kept in free lists, and also get passed down to virtqueue
   so it isn't calling physical_from_virtual() with each enqueue.
*/
typedef struct fs_dma_buf {
    void * buf;
    //u64 bus;           /* bus address would go here */
    u64 alloc_size;
    range blocks;        /* in sectors, not bytes */
    u64 start_offset;    /* offset of query start within first block */
    u64 data_length;
} *fs_dma_buf;

static fs_dma_buf fs_allocate_dma_buffer(filesystem fs, extent e, range i)
{
    fs_dma_buf db = allocate(fs->h, sizeof(struct fs_dma_buf));
    if (db == INVALID_ADDRESS)
        return db;
    bytes blocksize = fs->blocksize;
    bytes absolute = e->block_start + i.start - e->node.r.start;
    db->start_offset = absolute & (blocksize - 1);
    db->data_length = range_span(i);
    bytes padlength = pad(db->start_offset + db->data_length, blocksize);
    u64 start_block = absolute / blocksize; /* XXX need to stash blocksize log2 */
    u64 nblocks = padlength / blocksize;
    db->blocks = irange(start_block, start_block + nblocks);

    /* determine power-of-2 allocation size */
    u64 alloc_order = find_order(pad(padlength, fs->dma->pagesize));
    db->alloc_size = 1ull << alloc_order;
#ifndef BOOT
    db->buf = allocate(fs->dma, db->alloc_size);
    if (db->buf == INVALID_ADDRESS) {
        msg_err("failed to allocate dma buffer of size %ld\n", db->alloc_size);
        deallocate(fs->h, db, sizeof(struct fs_dma_buf));
        return INVALID_ADDRESS;
    }
#else
    db->buf = 0;                /* fixed up by caller for stage2 */
#endif
    return db;
}

static void fs_deallocate_dma_buffer(filesystem fs, fs_dma_buf db)
{
#ifndef BOOT
    deallocate(fs->dma, db->buf, db->alloc_size);
#endif
    deallocate(fs->h, db, sizeof(struct fs_dma_buf));
}

static CLOSURE_4_1(fs_read_extent_complete, void, filesystem, fs_dma_buf, void *, status_handler, status);
static void fs_read_extent_complete(filesystem fs, fs_dma_buf db, void * target, status_handler sh, status s)
{
    tfs_debug("fs_read_extent_complete: dma buf 0x%p, start_offset %ld, length %ld, target %p, status %v\n",
              db->buf, db->start_offset, db->data_length, target, s);
#ifndef BOOT
    if (is_ok(s))
        runtime_memcpy(target, db->buf + db->start_offset, db->data_length);
#endif
    fs_deallocate_dma_buffer(fs, db);
    apply(sh, s);
}

static CLOSURE_4_1(fs_read_extent, void,
                   filesystem, buffer, merge, range,
                   rmnode);
static void fs_read_extent(filesystem fs,
                           buffer target,
                           merge m,
                           range q,
                           rmnode node)
{
    range i = range_intersection(q, node->r);
    u64 target_offset = i.start - q.start;
    void *target_start = buffer_ref(target, target_offset);

    /* get and init dma buf */
    extent e = (extent)node;
    fs_dma_buf db = fs_allocate_dma_buffer(fs, e, i);
    if (db == INVALID_ADDRESS) {
        msg_err("failed; unable to allocate dma buffer, i span %ld bytes\n", range_span(i));
        return;
    }
#ifdef BOOT
    /* XXX To skip the copy in stage2, we're banking on the kernel
       being loaded in its entirety, with no partial-block reads
       (except the end, but that's fine). */
    assert(i.start == node->r.start);
    db->buf = target_start;
#endif

    tfs_debug("fs_read_extent: q %R, ex %R, blocks %R, start_offset %ld, i %R, "
              "target_offset %ld, target_start %p, length %ld, blocksize %ld\n",
              q, node->r, db->blocks, db->start_offset, i,
              target_offset, target_start, db->data_length, (u64)fs->blocksize);

    status_handler f = apply_merge(m);
    fetch_and_add(&target->end, db->data_length);
    status_handler copy = closure(fs->h, fs_read_extent_complete, fs, db, target_start, f);
    apply(fs->r, db->buf, db->blocks, copy);
}

static CLOSURE_3_1(fs_zero_hole, void, filesystem, buffer, range, range);
void fs_zero_hole(filesystem fs, buffer target, range q, range z)
{
    range i = range_intersection(q, z);
    u64 target_offset = i.start - q.start;
    void * target_start = buffer_ref(target, target_offset);
    u64 length = range_span(i);
    tfs_debug("fs_zero_hole: i %R, target_start %p, length %ld\n", i, target_start, length);
    runtime_memset(target_start, 0, length);
    fetch_and_add(&target->end, length);
}

io_status_handler ignore_io_status;

static CLOSURE_3_1(filesystem_read_complete, void, heap, io_status_handler, buffer, status);
static void filesystem_read_complete(heap h, io_status_handler c, buffer b, status s)
{
    tfs_debug("filesystem_read_complete: status %v, length %d\n", s, buffer_length(b));
    report_sha256(b);
    apply(c, s, is_ok(s) ? buffer_length(b) : 0);
    unwrap_buffer(h, b);
}

static void filesystem_read_internal(filesystem fs, fsfile f, buffer b, u64 length, u64 offset,
                                     status_handler sh)
{
    merge m = allocate_merge(fs->h, sh);
    status_handler k = apply_merge(m); // hold a reference until we're sure we've issued everything
    u64 file_length = fsfile_get_length(f);
    u64 actual_length = MIN(length, file_length - offset);
    if (offset >= file_length || actual_length == 0) { /* XXX check */
        apply(k, STATUS_OK);
        return;
    }
    range total = irange(offset, offset + actual_length);

    /* read extent data */
    rangemap_range_lookup(f->extentmap, total, closure(fs->h, fs_read_extent, fs, b, m, total));

    /* zero areas corresponding to file holes */
    rangemap_range_find_gaps(f->extentmap, total, closure(fs->h, fs_zero_hole, fs, b, total));

    apply(k, STATUS_OK);
}

void filesystem_read(filesystem fs, tuple t, void *dest, u64 length, u64 offset,
                     io_status_handler io_complete)
{
    fsfile f;
    if (!(f = table_find(fs->files, t))) {
        tuple e = timm("result", "no such file %t", t);
        apply(io_complete, e, 0);
        return;
    }

    /* b->end will accumulate the read extent and hole lengths, thus
       effectively handing a read length to the completion. */
    buffer b = wrap_buffer(fs->h, dest, length);
    b->end = b->start;
    status_handler sh = closure(fs->h, filesystem_read_complete, fs->h, io_complete, b);
    filesystem_read_internal(fs, f, b, length, offset, sh);
}

static CLOSURE_3_1(read_entire_complete, void, buffer_handler, buffer, status_handler, status);
static void read_entire_complete(buffer_handler bh, buffer b, status_handler sh, status s)
{
    tfs_debug("read_entire_complete: status %v, addr %p, length %d\n",
              s, buffer_ref(b, 0), buffer_length(b));
    if (is_ok(s)) {
        report_sha256(b);
        apply(bh, b);
    } else {
        deallocate_buffer(b);
        apply(sh, s);
    }
}

void filesystem_read_entire(filesystem fs, tuple t, heap bufheap, buffer_handler c, status_handler sh)
{
    fsfile f;
    if (!(f = table_find(fs->files, t))) {
        tuple e = timm("result", "no such file %t", t);
        apply(sh, e);
        return;
    }

    u64 length = pad(fsfile_get_length(f), fs->blocksize);
    buffer b = allocate_buffer(bufheap, pad(length, bufheap->pagesize));
    filesystem_read_internal(fs, f, b, length, 0, closure(fs->h, read_entire_complete, c, b, sh));
}

/*
 * +       i.start--+        +--start_padded      i.end--+      +--end_padded
 * |                |        |                           |      |
 * |                v        v                           v      v
 * v                 <-head->                    <-tail->
 * |---------|------[========|=======....=======|========]------|
 *            <--blocksize-->                    <--blocksize-->
 */

static CLOSURE_3_1(fs_write_extent_complete, void, filesystem, fs_dma_buf, status_handler, status);
static void fs_write_extent_complete(filesystem fs, fs_dma_buf db, status_handler sh, status s)
{
    tfs_debug("fs_write_extent_complete: status %v\n", s);
    fs_deallocate_dma_buffer(fs, db);
    apply(sh, s);
}

/* In theory these writes could be split up, allowing the aligned
   write to commence without waiting for head/tail reads. Not clear if
   it matters. */
static CLOSURE_4_1(fs_write_extent_aligned, void, filesystem, fs_dma_buf, void *, status_handler, status);
static void fs_write_extent_aligned(filesystem fs, fs_dma_buf db, void * source, status_handler sh, status s)
{
    if (!is_ok(s)) {
        msg_err("read failed: %v\n", s);
        apply(sh, s);
        return;
    }
    void * dest = db->buf + db->start_offset;
    tfs_debug("fs_write_extent_complete: copy from 0x%p to 0x%p, len %ld\n", source, dest, db->data_length);
    runtime_memcpy(dest, source, db->data_length);
    tfs_debug("   write from 0x%p to block range %R\n", db->buf, db->blocks);
    status_handler complete = closure(fs->h, fs_write_extent_complete, fs, db, sh);
    apply(fs->w, db->buf, db->blocks, complete);
}

static void fs_write_extent_read_block(filesystem fs, fs_dma_buf db, u64 offset_block, status_handler sh)
{
    u64 absolute_block = db->blocks.start + offset_block;
    void * buf = db->buf + (offset_block * fs->blocksize);
    range r = irange(absolute_block, absolute_block + 1);
    tfs_debug("fs_write_extent_read_block: sector range %R, buf %p\n", r, buf);
    apply(fs->r, buf, r, sh);
}

static void fs_write_extent(filesystem fs, buffer source, merge m, range q, rmnode node)
{
    range i = range_intersection(q, node->r);
    u64 source_offset = i.start - q.start;
    void * source_start = buffer_ref(source, source_offset);

#ifdef BOOT
    msg_err("File writing unsupported in stage2.\n");
    return;
#endif

    extent e = (extent)node;
    fs_dma_buf db = fs_allocate_dma_buffer(fs, e, i);
    if (db == INVALID_ADDRESS) {
        msg_err("failed; unable to allocate dma buffer, i span %ld bytes\n", range_span(i));
        return;
    }

    tfs_debug("fs_write_extent: source (+off) %p, buf len %d, q %R, node %R,\n"
              "                 i %R, i len %ld, ext start 0x%lx, dma buf %p\n",
              source_start, buffer_length(source), q, node->r, i, range_span(i),
              ((extent)node)->block_start, db->buf);

    /* Check for unaligned block writes and initiate reads for them.
       This would all be obviated by a diskcache. */
    boolean tail_rmw = ((db->data_length + db->start_offset) & (fs->blocksize - 1)) != 0 &&
        (i.end != node->r.end); /* no need to rmw tail if we're at the end of the extent */
    boolean plural = range_span(db->blocks) > 1;

    /* just do a head op if one block and either head or tail are misaligned */
    boolean head = db->start_offset != 0 || (tail_rmw && !plural);
    boolean tail = tail_rmw && plural;

    status_handler sh = apply_merge(m);
    if (head || tail) {
        merge m2 = allocate_merge(fs->h, closure(fs->h, fs_write_extent_aligned,
                                                 fs, db, source_start, sh));
        status_handler k = apply_merge(m2);
        if (head)
            fs_write_extent_read_block(fs, db, 0, apply_merge(m2));
        if (tail)
            fs_write_extent_read_block(fs, db, range_span(db->blocks) - 1, apply_merge(m2));
        apply(k, STATUS_OK);
        return;
    }

    /* everything is aligned, so proceed to the write */
    fs_write_extent_aligned(fs, db, source_start, sh, STATUS_OK);
}

// wrap in an interface
static tuple soft_create(filesystem fs, tuple t, symbol a, merge m)
{
    tuple v;
    if (!(v = table_find(t, a))) {
        v = allocate_tuple();
        table_set(t, a, v);
        filesystem_write_eav(fs, t, a, v, apply_merge(m));
    }
    return v;
}

/* create a new extent in the filesystem

   The life an extent depends on a particular allocation of contiguous
   storage space. The extent is tied to this allocated area (nominally
   page size). Only the extent data length may be updated; the file
   offset, block start and allocation size are immutable. As an
   optimization, adjacent extents on the disk could be joined into
   larger extents with only a meta update.

*/

static extent create_extent(fsfile f, range r, merge m)
{
    heap h = f->fs->h;
    u64 length = range_span(r);
    u64 alignment = f->fs->alignment;
    u64 alloc_order = find_order(pad(length, alignment));
    u64 alloc_bytes = MAX(1 << alloc_order, MIN_EXTENT_SIZE);

#ifdef BOOT
    /* No writes from the bootloader, please. */
    return INVALID_ADDRESS;
#endif

    tfs_debug("create_extent: align %d, offset %ld, length %ld, alloc_order %ld, alloc_bytes %ld\n",
              alignment, r.start, length, alloc_order, alloc_bytes);

    u64 block_start = allocate_u64(f->fs->storage, alloc_bytes);
    if (block_start == u64_from_pointer(INVALID_ADDRESS)) {
        msg_err("out of storage");
        return INVALID_ADDRESS;
    }
    tfs_debug("   block_start 0x%lx\n", block_start);

    /* XXX this extend / alloc stuff is getting redone */
    extent ex = allocate_extent(h, r, block_start, alloc_bytes);
    if (ex == INVALID_ADDRESS)
        halt("out of memory\n");
    assert(rangemap_insert(f->extentmap, &ex->node));

    // XXX encode this as an immediate bitstring
    tuple e = timm("length", "%ld", length);
    string offset = aprintf(h, "%ld", block_start);
    table_set(e, sym(offset), offset);
    string allocated = aprintf(h, "%ld", alloc_bytes);
    table_set(e, sym(allocated), allocated);
    symbol offs = intern_u64(r.start);

    tuple extents = soft_create(f->fs, f->md, sym(extents), m);
    table_set(extents, offs, e);
    filesystem_write_eav(f->fs, extents, offs, e, apply_merge(m));
    return ex;
}

static inline boolean ingest_parse_int(tuple value, symbol s, u64 * i)
{
    buffer b = table_find(value, s);
    /* bark, because these shouldn't really happen */
    if (!b) {
        msg_err("value missing %b\n", symbol_string(s));
        return false;
    }

    /* XXX gross, but we're having issues with too many allocas in stage2 */
    bytes start = b->start;
    boolean retval = parse_int(b, 10, i);
    b->start = start;
    return retval;
}

void ingest_extent(fsfile f, symbol off, tuple value)
{
    tfs_debug("ingest_extent: f %p, off %b, value %v\n", f, symbol_string(off), value);
    u64 length, file_offset, block_start, allocated;
    assert(off);
    assert(parse_int(alloca_wrap(symbol_string(off)), 10, &file_offset));
    assert(ingest_parse_int(value, sym(length), &length));
    assert(ingest_parse_int(value, sym(offset), &block_start));
    assert(ingest_parse_int(value, sym(allocated), &allocated));
    tfs_debug("   file offset %ld, length %ld, block_start 0x%lx, allocated %ld\n",
              file_offset, length, block_start, allocated);
#ifndef BOOT
    if (!id_heap_set_area(f->fs->storage, block_start, allocated, true, true)) {
        /* soft error... */
        msg_err("unable to reserve storage at start 0x%lx, len 0x%lx\n",
                block_start, allocated);
    }
#endif
    range r = irange(file_offset, file_offset + length);
    extent ex = allocate_extent(f->fs->h, r, block_start, allocated);
    if (ex == INVALID_ADDRESS)
        halt("out of memory\n");
    assert(rangemap_insert(f->extentmap, &ex->node));
}

boolean set_extent_length(fsfile f, extent ex, u64 length, merge m)
{
    tfs_debug("set_extent_length: range %R, allocated %ld, new length %ld\n",
              ex->node.r, ex->allocated, length);
    if (length > ex->allocated) {
        tfs_debug("failed: new length %ld > ex->allocated %ld\n",
                  length, ex->allocated);
        return false;
    }

    range r = ex->node.r;
    r.end = ex->node.r.start + length;

    if (rangemap_range_lookup(f->extentmap, r, 0)) {
        tfs_debug("failed: collides with existing extent\n");
        return false;
    }

    tuple extents = table_find(f->md, sym(extents));
    if (!extents) {
        tfs_debug("failed: can't find extents in f->md\n");
        return false;
    }

    symbol offs = intern_u64(r.start);
    tuple extent_tuple = table_find(extents, offs);
    if (!extent_tuple) {
        tfs_debug("failed: can't find extent tuple\n");
        return false;
    }

    /* re-insert in rangemap */
    rangemap_remove_node(f->extentmap, &ex->node);

    if (!rangemap_insert(f->extentmap, &ex->node)) {
        tfs_debug("failed: rangemap_insert failed\n");
        return false;
    }

    /* update length in tuple and log */
    string v = aprintf(f->fs->h, "%ld", length);
    table_set(extent_tuple, sym(length), v);
    filesystem_write_eav(f->fs, extents, offs, extent_tuple, apply_merge(m));
    return true;
}

static CLOSURE_2_1(filesystem_write_meta_complete, void, range, io_status_handler, status);
static void filesystem_write_meta_complete(range q, io_status_handler ish, status s)
{
    u64 n = range_span(q);
    tfs_debug("%s: range %R, bytes %ld, status %v\n", __func__, q, n, s);
    apply(ish, s, is_ok(s) ? n : 0);
}

static CLOSURE_5_1(filesystem_write_data_complete, void, fsfile, tuple, range, merge, status_handler, status);
static void filesystem_write_data_complete(fsfile f, tuple t, range q, merge m_meta, status_handler m_sh,
                                           status s)
{
    filesystem fs = f->fs;
    tfs_debug("%s: range %R, status %v\n", __func__, q, s);

    if (!is_ok(s)) {
        /* XXX need to cancel meta update rather than just flush... */
        filesystem_flush_log(fs);
        apply(m_sh, s);
        return;
    }

    if (fsfile_get_length(f) < q.end) {
        /* XXX bother updating resident filelength tuple? */
        fsfile_set_length(f, q.end);
        filesystem_write_eav(fs, t, sym(filelength), value_from_u64(fs->h, q.end), apply_merge(m_meta));
    }

    filesystem_flush_log(fs);
    apply(m_sh, STATUS_OK);
}

/* Holes over the query range will be filled with extents before later
   being filled with content. However, there are at least two
   differing ways that we can do this function:

   1) Use only single page-sized extents when writing. The reasons are
      two-fold: With our storage allocator, larger, non power-of-2
      sizes will lead to more wasted allocated space, and larger,
      varied allocations leads to more fragmentation of storage space.

      This comes at the cost of meta for every page worth of file
      data. However, extent meta for contiguous areas can be
      aggregated. This is aided by the storage allocator issuing
      allocations in-order when possible. (Note that we can add a
      "release" function to the id heap to complement reserve. This
      would allow us to arbitrarily deallocate blocks - and not
      necessarily in allocation alignments (as with reserve).)

   2) An alternative approach is to break large extent requests into
      allocation sizes that can be filled completely, descending in
      order. This would address the wasted allocated space issue but
      not the fragmentation issue.

   In any case, the attempt is to isolate the logic for mapping a
   write to a series of extents to this function alone. So any
   implementation of the above methods merely takes place in the logic
   below.
*/

/* XXX This needs to additionally block if a log flush is in flight. */
void filesystem_write(filesystem fs, tuple t, buffer b, u64 offset, io_status_handler ish)
{
    u64 len = buffer_length(b);
    range q = irange(offset, offset + len);
    u64 curr = offset;

    fsfile f;
    if (!(f = table_find(fs->files, t))) {
        apply(ish, timm("result", "no such file %t", t), 0);
        return;
    }

    tfs_debug("filesystem_write: tuple %p, buffer %p, q %R\n", t, b, q);

    rmnode node = rangemap_lookup_at_or_next(f->extentmap, q.start);

    /* meta merge completion is gated by data merge completion, thus the initial m_meta apply */
    merge m_meta = allocate_merge(fs->h, closure(fs->h, filesystem_write_meta_complete, q, ish));
    merge m_data = allocate_merge(fs->h, closure(fs->h, filesystem_write_data_complete,
                                                 f, t, q, m_meta, apply_merge(m_meta)));

    /* hold data merge open until all extent operations have been initiated */
    status_handler sh = apply_merge(m_data);
    do {
        /* detect and fill any hole before extent (or to end) */
        u64 limit = node != INVALID_ADDRESS ? node->r.start : q.end;
        if (curr < limit) {
            range hole = irange(curr, limit);
            range fill = range_intersection(q, hole);

            /* XXX optimization: check for a preceding extent and
               inflate if possible */

            /* just doing min-sized extents for now */
            s64 remain = range_span(fill);

            do {
                /* create_extent will allocate a minimum of pagesize */
                u64 length = MIN(MAX_EXTENT_SIZE, remain);
                range r = irange(curr, curr + length);
                extent ex = create_extent(f, r, m_meta);
                if (ex == INVALID_ADDRESS) {
                    msg_err("failed to create extent\n");
                    goto fail;
                }
                tfs_debug("   writing new extent %R\n", r);
                fs_write_extent(f->fs, b, m_data, q, &ex->node);
                curr += length;
                remain -= length;
            } while (remain > 0);
        }

        if (node != INVALID_ADDRESS) {
            /* overwrite any overlap with extent */
            range i = range_intersection(q, node->r);
            if (range_span(i)) {
                tfs_debug("   updating extent at %R (intersection %R)\n", node->r, i);
                fs_write_extent(f->fs, b, m_data, q, node);
            }
            curr = node->r.end;
            node = rangemap_next_node(f->extentmap, node);
        }
    } while(curr < q.end);

    /* all data I/O has been queued */
    apply(sh, STATUS_OK);
    return;

  fail:
    /* apply merge fail */
    apply(sh, timm("result", "write failed"));
    return;
}

boolean filesystem_truncate(filesystem fs, fsfile f, u64 len,
        status_handler completion)
{
    if (fsfile_get_length(f) == len) {
        return true;
    }
    fsfile_set_length(f, len);
    filesystem_write_eav(fs, f->md, sym(filelength), value_from_u64(fs->h, len),
            completion);
    filesystem_flush_log(fs);
    return false;
}

boolean filesystem_flush(filesystem fs, tuple t, status_handler completion)
{
    /* A write() call returns after everything is sent to disk, so nothing to
     * do here. The only work that might be pending is when directory entries
     * are modified, see do_mkentry(); to deal with that, flush the filesystem
     * log.
     */
    return log_flush_complete(fs->tl, completion);
}

fsfile allocate_fsfile(filesystem fs, tuple md)
{
    fsfile f = allocate(fs->h, sizeof(struct fsfile));
    f->extentmap = allocate_rangemap(fs->h);
    f->fs = fs;
    f->md = md;
    f->length = 0;
    table_set(fs->files, f->md, f);
    return f;
}

#if 0
void link(tuple dir, fsfile f, buffer name)
{
    filesystem_write_eav(f->fs, soft_create(f->fs, dir, sym(children)),
                         intern(name), f->md);
    filesystem_flush_log(f->fs);
}
#endif

void fixup_directory(tuple parent, tuple dir)
{
    tuple c = children(dir);
    if (!c)
        return;

    table_foreach(c, k, v) {
        (void) k;
        if (tagof(v) == tag_tuple)
            fixup_directory(dir, v);
    }

    table_set(c, sym_this("."), dir);
    table_set(c, sym_this(".."), parent);
}

static void cleanup_directory(tuple dir)
{
    tuple c = children(dir);
    if (!c) {
        return;
    }
    table_set(c, sym_this("."), 0);
    table_set(c, sym_this(".."), 0);
    table_foreach(c, k, v) {
        (void) k;
        if (tagof(v) == tag_tuple) {
            cleanup_directory(v);
        }
    }
}

static void fs_set_dir_entry(filesystem fs, tuple parent, symbol name_sym,
        tuple child, status_handler sh)
{
    if (child) {
        /* If this is a directory, remove its . and .. directory entries, which
         * must not be written in the log. */
        cleanup_directory(child);
    }
    tuple c = children(parent);
    table_set(c, name_sym, child);
    if (sh) {
        filesystem_write_eav(fs, c, name_sym, child, sh);
        filesystem_flush_log(fs);
    }
    else {
        filesystem_write_eav(fs, c, name_sym, child, ignore_status);
    }
    if (child) {
        /* If this is a directory, re-add its . and .. directory entries. */
        fixup_directory(parent, child);
    }
}

static void do_mkentry(filesystem fs, tuple parent, const char *name, tuple entry, boolean persistent)
{
    symbol name_sym = sym_this(name);
    tuple c = children(parent);
    table_set(c, name_sym, entry);

    /* XXX rather than ignore, there should be a wakeup on a sync blockq */
    if (persistent) {
        filesystem_write_eav(fs, c, name_sym, entry, ignore_status);
        filesystem_flush_log(fs);
    }

    fixup_directory(parent, entry);
}

fs_status filesystem_mkentry(filesystem fs, tuple cwd, const char *fp, tuple entry, boolean persistent, boolean recursive)
{
    tuple parent = cwd ? cwd : fs->root;
    assert(children(parent));

    char *token, *rest;
    fs_status status = FS_STATUS_OK;

    int fp_len = runtime_strlen(fp);
    char *fp_copy = allocate(fs->h, fp_len + 1);
    assert(fp_copy != INVALID_ADDRESS);
    runtime_memcpy(fp_copy, fp, fp_len);
    fp_copy[fp_len] = '\0';
    rest = fp_copy;

    /* find the folder we need to mkentry in */
    while ((token = runtime_strtok_r(rest, "/", &rest))) {
        boolean final = *rest == '\0';
        tuple t = lookup(parent, sym_this(token));
        if (!t) {
            if (!final) {
                if (recursive) {
                    /* create intermediate directory */
                    tuple dir = allocate_tuple();
                    table_set(dir, sym(children), allocate_tuple());
                    do_mkentry(fs, parent, token, dir, persistent);

                    parent = dir;
                    continue;
                }

                msg_err("a path component (\"%s\") is missing\n", token);
                status = FS_STATUS_NOENT;
                break;
            }

            do_mkentry(fs, parent, token, entry, persistent);
            break;
        }

        if (final) {
            msg_err("final path component (\"%s\") already exists\n", token);
            status = FS_STATUS_EXIST;
            break;
        }

        if (!children(t)) {
            msg_debug("a path component (\"%s\") is not a folder\n", token);
            status = FS_STATUS_NOTDIR;
            break;
        }

        parent = t;
    }

    deallocate(fs->h, fp_copy, fp_len + 1);
    return status;
}

fs_status filesystem_mkdir(filesystem fs, tuple cwd, const char *fp, boolean persistent)
{
    tuple dir = allocate_tuple();
    /* 'make it a folder' by attaching a children node to the tuple */
    table_set(dir, sym(children), allocate_tuple());

    return filesystem_mkentry(fs, cwd, fp, dir, persistent, false);
}

fs_status filesystem_creat(filesystem fs, tuple cwd, const char *fp, boolean persistent)
{
    tuple dir = allocate_tuple();
    static buffer off = 0;

    if (!off)
        off = wrap_buffer_cstring(fs->h, "0");

    /* 'make it a file' by adding an empty extents list */
    table_set(dir, sym(extents), allocate_tuple());
    table_set(dir, sym(filelength), off);

    fsfile f = allocate_fsfile(fs, dir);
    fsfile_set_length(f, 0);

    return filesystem_mkentry(fs, cwd, fp, dir, persistent, false);
}

void filesystem_delete(filesystem fs, tuple cwd, const char *fp,
        status_handler completion)
{
    tuple parent;
    symbol child_sym = fs_get_parent_child(fs, cwd, fp, &parent, 0);
    if (!child_sym) {
        return;
    }
    fs_set_dir_entry(fs, parent, child_sym, 0, completion);
}

void filesystem_rename(filesystem fs, tuple oldwd, const char *oldfp,
        tuple newwd, const char *newfp, status_handler completion)
{
    tuple oldparent;
    tuple t;
    symbol oldchild_sym = fs_get_parent_child(fs, oldwd, oldfp, &oldparent, &t);
    if (!oldchild_sym) {
        return;
    }
    tuple newparent;
    symbol newchild_sym = fs_get_parent_child(fs, newwd, newfp, &newparent, 0);
    if (!newchild_sym) {
        return;
    }
    fs_set_dir_entry(fs, oldparent, oldchild_sym, 0, 0);
    fs_set_dir_entry(fs, newparent, newchild_sym, t, completion);
}

void filesystem_exchange(filesystem fs, tuple wd1, const char *fp1,
        tuple wd2, const char *fp2, status_handler completion)
{
    tuple parent1;
    tuple child1;
    symbol child1_sym = fs_get_parent_child(fs, wd1, fp1, &parent1, &child1);
    if (!child1_sym) {
        return;
    }
    tuple parent2;
    tuple child2;
    symbol child2_sym = fs_get_parent_child(fs, wd2, fp2, &parent2, &child2);
    if (!child2_sym) {
        return;
    }
    fs_set_dir_entry(fs, parent1, child1_sym, child2, 0);
    fs_set_dir_entry(fs, parent2, child2_sym, child1, completion);
}

fsfile fsfile_from_node(filesystem fs, tuple n)
{
    return table_find(fs->files, n);
}

static CLOSURE_2_1(log_complete, void, filesystem_complete, filesystem, status);
static void log_complete(filesystem_complete fc, filesystem fs, status s)
{
    fixup_directory(fs->root, fs->root);
    apply(fc, fs, s);
}

static CLOSURE_0_2(ignore_io_body, void, status, bytes);
static void ignore_io_body(status s, bytes length){}

void create_filesystem(heap h,
                       u64 alignment,
                       u64 size,
                       heap dma,
                       block_io read,
                       block_io write,
                       tuple root,
                       filesystem_complete complete)
{
    tfs_debug("create_filesystem: ...\n");
    filesystem fs = allocate(h, sizeof(struct filesystem));
    ignore_io_status = closure(h, ignore_io_body);
    fs->files = allocate_table(h, identity_key, pointer_equal);
    fs->extents = allocate_table(h, identity_key, pointer_equal);
    fs->dma = dma;
    fs->r = read;
    fs->h = h;
    fs->w = write;
    fs->root = root;
    fs->alignment = alignment;
    fs->blocksize = SECTOR_SIZE;
#ifndef BOOT
    fs->storage = create_id_heap(h, 0, infinity, SECTOR_SIZE);
    assert(fs->storage != INVALID_ADDRESS);
    assert(id_heap_set_area(fs->storage, 0, INITIAL_LOG_SIZE, true, true));
#endif
    fs->tl = log_create(h, fs, closure(h, log_complete, complete, fs));
}

tuple filesystem_getroot(filesystem fs)
{
    return fs->root;
}
