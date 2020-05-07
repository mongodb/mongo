/*-
 * Public Domain 2014-2020 MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Configuration for the wts program is an array of string-based parameters. This is the structure
 * used to declare them.
 */
typedef struct {
    const char *name; /* Configuration item */
    const char *desc; /* Configuration description */

/* Value is a boolean, yes if roll of 1-to-100 is <= CONFIG->min. */
#define C_BOOL 0x01u

/* Not a simple randomization, handle outside the main loop. */
#define C_IGNORE 0x02u

/* Value was set from command-line or file, ignore for all runs. */
#define C_PERM 0x04u

/* Value isn't random for this run, ignore just for this run. */
#define C_TEMP 0x08u

/* Value is a string. */
#define C_STRING 0x20u
    u_int flags;

    uint32_t min;     /* Minimum value */
    uint32_t maxrand; /* Maximum value randomly chosen */
    uint32_t maxset;  /* Maximum value explicitly set */
    uint32_t *v;      /* Value for this run */
    char **vstr;      /* Value for string options */
} CONFIG;

#define COMPRESSION_LIST "(none | lz4 | snappy | zlib | zstd)"

static CONFIG c[] = {
  /* 5% */
  {"assert.commit_timestamp", "if assert commit_timestamp", C_BOOL, 5, 0, 0,
    &g.c_assert_commit_timestamp, NULL},

  /* 5% */
  {"assert.read_timestamp", "if assert read_timestamp", C_BOOL, 5, 0, 0, &g.c_assert_read_timestamp,
    NULL},

  /* 20% */
  {"backup", "if backups are enabled", C_BOOL, 20, 0, 0, &g.c_backups, NULL},

  {"backup.incremental", "type of backup (block | log | off)", C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_backup_incremental},

  {"btree.bitcnt", "number of bits for fixed-length column-store files", 0x0, 1, 8, 8, &g.c_bitcnt,
    NULL},

  {"btree.compression", "type of compression " COMPRESSION_LIST, C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_compression},

  /* 20% */
  {"btree.dictionary", "if values are dictionary compressed", C_BOOL, 20, 0, 0, &g.c_dictionary,
    NULL},

  /* 20% */
  {"btree.huffman_key", "if keys are huffman encoded", C_BOOL, 20, 0, 0, &g.c_huffman_key, NULL},

  /* 20% */
  {"btree.huffman_value", "if values are huffman encoded", C_BOOL, 20, 0, 0, &g.c_huffman_value,
    NULL},

  /* 95% */
  {"btree.internal_key_truncation", "if internal keys are truncated", C_BOOL, 95, 0, 0,
    &g.c_internal_key_truncation, NULL},

  {"btree.internal_page_max", "maximum size of Btree internal nodes", 0x0, 9, 17, 27,
    &g.c_intl_page_max, NULL},

  {"btree.key_gap", "gap between instantiated keys on a Btree page", 0x0, 0, 20, 20, &g.c_key_gap,
    NULL},

  {"btree.key_max", "maximum size of keys", 0x0, 20, 128, MEGABYTE(10), &g.c_key_max, NULL},

  {"btree.key_min", "minimum size of keys", 0x0, 10, 32, 256, &g.c_key_min, NULL},

  {"btree.leaf_page_max", "maximum size of Btree leaf nodes", 0x0, 9, 17, 27, &g.c_leaf_page_max,
    NULL},

  {"btree.memory_page_max", "maximum size of in-memory pages", 0x0, 1, 10, 128,
    &g.c_memory_page_max, NULL},

  /* 80% */
  {"btree.prefix_compression", "if keys are prefix compressed", C_BOOL, 80, 0, 0,
    &g.c_prefix_compression, NULL},

  {"btree.prefix_compression_min", "minimum gain before prefix compression is used", 0x0, 0, 8, 256,
    &g.c_prefix_compression_min, NULL},

  {"btree.repeat_data_pct", "percent duplicate values in row- or var-length column-stores", 0x0, 0,
    90, 90, &g.c_repeat_data_pct, NULL},

  /* 10% */
  {"btree.reverse", "collate in reverse order", C_BOOL, 10, 0, 0, &g.c_reverse, NULL},

  {"btree.split_pct", "page split size as a percentage of the maximum page size", 0x0, 50, 100, 100,
    &g.c_split_pct, NULL},

  {"btree.value_max", "maximum size of values", 0x0, 32, 4096, MEGABYTE(10), &g.c_value_max, NULL},

  {"btree.value_min", "minimum size of values", 0x0, 0, 20, 4096, &g.c_value_min, NULL},

  {"cache", "size of the cache in MB", 0x0, 1, 100, 100 * 1024, &g.c_cache, NULL},

  {"cache.evict_max", "the maximum number of eviction workers", 0x0, 0, 5, 100, &g.c_evict_max,
    NULL},

  {"cache.minimum", "minimum size of the cache in MB", C_IGNORE, 0, 0, 100 * 1024,
    &g.c_cache_minimum, NULL},

  {"checkpoint", "type of checkpoints (on | off | wiredtiger)", C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_checkpoint},

  {"checkpoint.log_size", "MB of log to wait if wiredtiger checkpoints configured", 0x0, 20, 200,
    1024, &g.c_checkpoint_log_size, NULL},

  {"checkpoint.wait", "seconds to wait if wiredtiger checkpoints configured", 0x0, 5, 100, 3600,
    &g.c_checkpoint_wait, NULL},

  {"disk.checksum", "type of checksums (on | off | uncompressed)", C_IGNORE | C_STRING, 0, 0, 0,
    NULL, &g.c_checksum},

  /* 5% */
  {"disk.data_extend", "if data files are extended", C_BOOL, 5, 0, 0, &g.c_data_extend, NULL},

  /* 0% */
  {"disk.direct_io", "if direct I/O is configured for data objects", C_IGNORE | C_BOOL, 0, 0, 1,
    &g.c_direct_io, NULL},

  {"disk.encryption", "type of encryption (none | rotn-7)", C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_encryption},

  /* 10% */
  {"disk.firstfit", "if allocation is firstfit", C_BOOL, 10, 0, 0, &g.c_firstfit, NULL},

  /* 90% */
  {"disk.mmap", "configure for mmap operations (readonly)", C_BOOL, 90, 0, 0, &g.c_mmap, NULL},

  /* 5% */
  {"disk.mmap_all", "configure for mmap operations (read and write)", C_BOOL, 5, 0, 0,
    &g.c_mmap_all, NULL},

  /* 0% */
  {"format.abort", "if timed run should drop core", C_BOOL, 0, 0, 0, &g.c_abort, NULL},

  /* 75% */
  {"format.independent_thread_rng", "if thread RNG space is independent", C_BOOL, 75, 0, 0,
    &g.c_independent_thread_rng, NULL},

  {"format.major_timeout", "timeout for long-running operations (minutes)", C_IGNORE, 0, 0, 1000,
    &g.c_major_timeout, NULL},

  /* 50% */
  {"logging", "if logging configured", C_BOOL, 50, 0, 0, &g.c_logging, NULL},

  /* 50% */
  {"logging.archive", "if log file archival configured", C_BOOL, 50, 0, 0, &g.c_logging_archive,
    NULL},

  {"logging.compression", "type of logging compression " COMPRESSION_LIST, C_IGNORE | C_STRING, 0,
    0, 0, NULL, &g.c_logging_compression},

  {"logging.file_max", "maximum log file size in KB", 0x0, 100, 512000, 2097152,
    &g.c_logging_file_max, NULL},

  /* 50% */
  {"logging.prealloc", "if log file pre-allocation configured", C_BOOL, 50, 0, 0,
    &g.c_logging_prealloc, NULL},

  /* 90% */
  {"lsm.auto_throttle", "if LSM inserts are throttled", C_BOOL, 90, 0, 0, &g.c_auto_throttle, NULL},

  /* 95% */
  {"lsm.bloom", "if bloom filters are configured", C_BOOL, 95, 0, 0, &g.c_bloom, NULL},

  {"lsm.bloom_bit_count", "number of bits per item for LSM bloom filters", 0x0, 4, 64, 1000,
    &g.c_bloom_bit_count, NULL},

  {"lsm.bloom_hash_count", "number of hash values per item for LSM bloom filters", 0x0, 4, 32, 100,
    &g.c_bloom_hash_count, NULL},

  /* 10% */
  {"lsm.bloom_oldest", "if bloom_oldest=true", C_BOOL, 10, 0, 0, &g.c_bloom_oldest, NULL},

  {"lsm.chunk_size", "LSM chunk size in MB", 0x0, 1, 10, 100, &g.c_chunk_size, NULL},

  {"lsm.merge_max", "the maximum number of chunks to include in a merge operation", 0x0, 4, 20, 100,
    &g.c_merge_max, NULL},

  {"lsm.worker_threads", "the number of LSM worker threads", 0x0, 3, 4, 20, &g.c_lsm_worker_threads,
    NULL},

  /* 10% */
  {"ops.alter", "if altering the table is enabled", C_BOOL, 10, 0, 0, &g.c_alter, NULL},

  /* 10% */
  {"ops.compaction", "if compaction is running", C_BOOL, 10, 0, 0, &g.c_compact, NULL},

  {"ops.pct.delete", "percent operations that are deletes", C_IGNORE, 0, 0, 100, &g.c_delete_pct,
    NULL},

  {"ops.pct.insert", "percent operations that are inserts", C_IGNORE, 0, 0, 100, &g.c_insert_pct,
    NULL},

  {"ops.pct.modify", "percent operations that are value modifications", C_IGNORE, 0, 0, 100,
    &g.c_modify_pct, NULL},

  {"ops.pct.read", "percent operations that are reads", C_IGNORE, 0, 0, 100, &g.c_read_pct, NULL},

  {"ops.pct.write", "percent operations that are value updates", C_IGNORE, 0, 0, 100,
    &g.c_write_pct, NULL},

  /* 5% */
  {"ops.prepare", "configure transaction prepare", C_BOOL, 5, 0, 0, &g.c_prepare, NULL},

  /* 10% */
  {"ops.random_cursor", "if random cursor reads configured", C_BOOL, 10, 0, 0, &g.c_random_cursor,
    NULL},

  /* 100% */
  {"ops.rebalance", "rebalance testing", C_BOOL, 100, 1, 0, &g.c_rebalance, NULL},

  /* 100% */
  {"ops.salvage", "salvage testing", C_BOOL, 100, 1, 0, &g.c_salvage, NULL},

  /* 100% */
  {"ops.truncate", "enable truncation", C_BOOL, 100, 0, 0, &g.c_truncate, NULL},

  /* 100% */
  {"ops.verify", "to regularly verify during a run", C_BOOL, 100, 1, 0, &g.c_verify, NULL},

  {"quiet", "quiet run (same as -q)", C_IGNORE | C_BOOL, 0, 0, 1, &g.c_quiet, NULL},

  {"runs", "the number of runs", C_IGNORE, 0, 0, UINT_MAX, &g.c_runs, NULL},

  {"runs.in_memory", "if in-memory configured", C_IGNORE | C_BOOL, 0, 0, 1, &g.c_in_memory, NULL},

  {"runs.ops", "the number of operations done per run", 0x0, 0, M(2), M(100), &g.c_ops, NULL},

  {"runs.rows", "the number of rows to create", 0x0, 10, M(1), M(100), &g.c_rows, NULL},

  {"runs.source", "data source (file | lsm | table)", C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_data_source},

  {"runs.threads", "the number of worker threads", 0x0, 1, 32, 128, &g.c_threads, NULL},

  {"runs.timer", "maximum time to run in minutes", C_IGNORE, 0, 0, UINT_MAX, &g.c_timer, NULL},

  {"runs.type", "type of store to create (fix | var | row)", C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_file_type},

  /* 20% */
  {"statistics", "maintain statistics", C_BOOL, 20, 0, 0, &g.c_statistics, NULL},

  /* 5% */
  {"statistics.server", "run the statistics server thread", C_BOOL, 5, 0, 0, &g.c_statistics_server,
    NULL},

  /* 2% */
  {"stress.aggressive_sweep", "stress aggressive sweep", C_BOOL, 2, 0, 0,
    &g.c_timing_stress_aggressive_sweep, NULL},

  /* 2% */
  {"stress.checkpoint", "stress checkpoints", C_BOOL, 2, 0, 0, &g.c_timing_stress_checkpoint, NULL},

  /* 2% */
  {"stress.hs_sweep", "stress history store sweep", C_BOOL, 2, 0, 0, &g.c_timing_stress_hs_sweep,
    NULL},

  /* 2% */
  {"stress.split_1", "stress splits (#1)", C_BOOL, 2, 0, 0, &g.c_timing_stress_split_1, NULL},

  /* 2% */
  {"stress.split_2", "stress splits (#2)", C_BOOL, 2, 0, 0, &g.c_timing_stress_split_2, NULL},

  /* 2% */
  {"stress.split_3", "stress splits (#3)", C_BOOL, 2, 0, 0, &g.c_timing_stress_split_3, NULL},

  /* 2% */
  {"stress.split_4", "stress splits (#4)", C_BOOL, 2, 0, 0, &g.c_timing_stress_split_4, NULL},

  /* 2% */
  {"stress.split_5", "stress splits (#5)", C_BOOL, 2, 0, 0, &g.c_timing_stress_split_5, NULL},

  /* 2% */
  {"stress.split_6", "stress splits (#6)", C_BOOL, 2, 0, 0, &g.c_timing_stress_split_6, NULL},

  /* 2% */
  {"stress.split_7", "stress splits (#7)", C_BOOL, 2, 0, 0, &g.c_timing_stress_split_7, NULL},

  /* 2% */
  {"stress.split_8", "stress splits (#8)", C_BOOL, 2, 0, 0, &g.c_timing_stress_split_8, NULL},

  {"transaction.frequency", "percent operations done inside an explicit transaction", 0x0, 1, 100,
    100, &g.c_txn_freq, NULL},

  {"transaction.isolation",
    "isolation level (random | read-uncommitted | read-committed | snapshot)", C_IGNORE | C_STRING,
    0, 0, 0, NULL, &g.c_isolation},

  /* 70% */
  {"transaction.timestamps", "enable transaction timestamp support", C_BOOL, 70, 0, 0,
    &g.c_txn_timestamps, NULL},

  {"wiredtiger.config", "configuration string used to wiredtiger_open", C_IGNORE | C_STRING, 0, 0,
    0, NULL, &g.c_config_open},

  /* 80% */
  {"wiredtiger.rwlock", "if wiredtiger read/write mutexes should be used", C_BOOL, 80, 0, 0,
    &g.c_wt_mutex, NULL},

  {"wiredtiger.leak_memory", "if memory should be leaked on close", C_BOOL, 0, 0, 0,
    &g.c_leak_memory, NULL},

  {NULL, NULL, 0x0, 0, 0, 0, NULL, NULL}};
