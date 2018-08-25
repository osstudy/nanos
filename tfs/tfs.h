typedef struct filesystem *filesystem;
typedef struct fsfile *fsfile;

void create_filesystem(heap h,
                       u64 alignment,
                       u64 size,
                       block_read read,
                       block_write write,
                       value_handler complete);

// there is a question as to whether tuple->fs file should be mapped inside out outside the filesystem
// status
void filesystem_read(tuple_handler t, void *dest, u64 offset, u64 length, status_handler completion);
void filesystem_write(tuple_handler t, buffer b, u64 offset, status_handler completion);
// set? bah
void flush(value, status_handler s);

// synch get
static inline u64 file_length(fsfile f)
{
    //    return(u64_from_value(table_find((table)t, sym(length))));
    return 0;
}
