/*-
 * Public Domain 2014-present MongoDB, Inc.
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

#define COMPRESSION_LIST " (none | lz4 | snappy | zlib | zstd)"

static CONFIG c[] = {
  /* 2% */
  {"assert.read_timestamp", "assert read_timestamp", C_BOOL, 2, 0, 0, &g.c_assert_read_timestamp,
    NULL},

  /* 2% */
  {"assert.write_timestamp", "set write_timestamp_usage and assert write_timestamp", C_BOOL, 2, 0,
    0, &g.c_assert_write_timestamp, NULL},

  /* 20% */
  {"backup", "configure backups", C_BOOL, 20, 0, 0, &g.c_backups, NULL},

  {"backup.incremental", "backup type (block | log | off)", C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_backup_incremental},

  {"backup.incr_granularity", "incremental backup block granularity (KB)", 0x0, 4, 16384, 16384,
    &g.c_backup_incr_granularity, NULL},

  /* 10% */
  {"block_cache", "enable the block cache", C_BOOL, 10, 0, 0, &g.c_block_cache, NULL},

  /* 30 */
  {"block_cache.cache_on_checkpoint", "block cache: cache checkpoint writes", C_BOOL, 30, 0, 0,
    &g.c_block_cache_cache_on_checkpoint, NULL},

  {"block_cache.size", "block cache size (MB)", 0x0, 1, 100, 100 * 1024, &g.c_block_cache_size,
    NULL},

  /* 60% */
  {"block_cache.cache_on_writes", "block cache: populate the cache on writes", C_BOOL, 60, 0, 0,
    &g.c_block_cache_cache_on_writes, NULL},

  {"btree.bitcnt", "fixed-length column-store object size (number of bits)", 0x0, 1, 8, 8,
    &g.c_bitcnt, NULL},

  {"btree.compression", "compression type" COMPRESSION_LIST, C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_compression},

  /* 20% */
  {"btree.dictionary", "configure dictionary compressed values", C_BOOL, 20, 0, 0, &g.c_dictionary,
    NULL},

  /* 20% */
  {"btree.huffman_value", "configure huffman encoded values", C_BOOL, 20, 0, 0, &g.c_huffman_value,
    NULL},

  /* 95% */
  {"btree.internal_key_truncation", "truncate internal keys", C_BOOL, 95, 0, 0,
    &g.c_internal_key_truncation, NULL},

  {"btree.internal_page_max", "btree internal node maximum size", 0x0, 9, 17, 27,
    &g.c_intl_page_max, NULL},

  {"btree.key_max", "maximum key size", 0x0, 20, 128, MEGABYTE(10), &g.c_key_max, NULL},

  /*
   * A minimum key size of 11 is necessary. Row-store keys have a leading 10-digit number and the
   * 11 guarantees we never see a key that we can't convert to a numeric value without formatting
   * it first because there's a trailing non-digit character in every key.
   */
  {"btree.key_min", "minimum key size", 0x0, 11, 32, 256, &g.c_key_min, NULL},

  {"btree.leaf_page_max", "btree leaf node maximum size", 0x0, 9, 17, 27, &g.c_leaf_page_max, NULL},

  {"btree.memory_page_max", "maximum cache page size", 0x0, 1, 10, 128, &g.c_memory_page_max, NULL},

  {"btree.prefix", "common key prefix", C_BOOL, 3, 0, 0, &g.c_prefix, NULL},

  /* 80% */
  {"btree.prefix_compression", "configure prefix compressed keys", C_BOOL, 80, 0, 0,
    &g.c_prefix_compression, NULL},

  {"btree.prefix_compression_min", "minimum gain before prefix compression is used (bytes)", 0x0, 0,
    8, 256, &g.c_prefix_compression_min, NULL},

  {"btree.repeat_data_pct", "duplicate values (percentage)", 0x0, 0, 90, 90, &g.c_repeat_data_pct,
    NULL},

  /* 10% */
  {"btree.reverse", "reverse order collation", C_BOOL, 10, 0, 0, &g.c_reverse, NULL},

  {"btree.split_pct", "page split size as a percentage of the maximum page size", 0x0, 50, 100, 100,
    &g.c_split_pct, NULL},

  {"btree.value_max", "maximum value size", 0x0, 32, 4096, MEGABYTE(10), &g.c_value_max, NULL},

  {"btree.value_min", "minimum value size", 0x0, 0, 20, 4096, &g.c_value_min, NULL},

  {"cache", "cache size (MB)", 0x0, 1, 100, 100 * 1024, &g.c_cache, NULL},

  {"cache.evict_max", "maximum number of eviction workers", 0x0, 0, 5, 100, &g.c_evict_max, NULL},

  {"cache.minimum", "minimum cache size (MB)", C_IGNORE, 0, 0, 100 * 1024, &g.c_cache_minimum,
    NULL},

  {"checkpoint", "checkpoint type (on | off | wiredtiger)", C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_checkpoint},

  {"checkpoint.log_size", "MB of log to wait if wiredtiger checkpoints configured", 0x0, 20, 200,
    1024, &g.c_checkpoint_log_size, NULL},

  {"checkpoint.wait", "seconds to wait if wiredtiger checkpoints configured", 0x0, 5, 100, 3600,
    &g.c_checkpoint_wait, NULL},

  {"disk.checksum", "checksum type (on | off | uncompressed | unencrypted)", C_IGNORE | C_STRING, 0,
    0, 0, NULL, &g.c_checksum},

  /* 5% */
  {"disk.data_extend", "configure data file extension", C_BOOL, 5, 0, 0, &g.c_data_extend, NULL},

  /* 0% */
  {"disk.direct_io", "configure direct I/O for data objects", C_IGNORE | C_BOOL, 0, 0, 1,
    &g.c_direct_io, NULL},

  {"disk.encryption", "encryption type (none | rotn-7)", C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_encryption},

  /* 10% */
  {"disk.firstfit", "configure first-fit allocation", C_BOOL, 10, 0, 0, &g.c_firstfit, NULL},

  /* 90% */
  {"disk.mmap", "configure mmap operations (reads only)", C_BOOL, 90, 0, 0, &g.c_mmap, NULL},

  /* 5% */
  {"disk.mmap_all", "configure mmap operations (read and write)", C_BOOL, 5, 0, 0, &g.c_mmap_all,
    NULL},

  /* 0% */
  {"format.abort", "drop core during timed run", C_BOOL, 0, 0, 0, &g.c_abort, NULL},

  /* 75% */
  {"format.independent_thread_rng", "configure independent thread RNG space", C_BOOL, 75, 0, 0,
    &g.c_independent_thread_rng, NULL},

  {"format.major_timeout", "long-running operations timeout (minutes)", C_IGNORE, 0, 0, 1000,
    &g.c_major_timeout, NULL},

  /*
   * 0%
   * FIXME-WT-7418 and FIXME-WT-7510: Temporarily disable import until WT_ROLLBACK error and
   * wt_copy_and_sync error is fixed. It should be (C_BOOL, 20, 0, 0).
   */
  {"import", "import table from newly created database", C_BOOL, 0, 0, 0, &g.c_import, NULL},

  /* 50% */
  {"logging", "configure logging", C_BOOL, 50, 0, 0, &g.c_logging, NULL},

  /* 50% */
  {"logging.archive", "configure log file archival", C_BOOL, 50, 0, 0, &g.c_logging_archive, NULL},

  {"logging.compression", "logging compression type" COMPRESSION_LIST, C_IGNORE | C_STRING, 0, 0, 0,
    NULL, &g.c_logging_compression},

  {"logging.file_max", "maximum log file size (KB)", 0x0, 100, 512000, 2097152,
    &g.c_logging_file_max, NULL},

  /* 50% */
  {"logging.prealloc", "configure log file pre-allocation", C_BOOL, 50, 0, 0, &g.c_logging_prealloc,
    NULL},

  /* 90% */
  {"lsm.auto_throttle", "throttle LSM inserts", C_BOOL, 90, 0, 0, &g.c_auto_throttle, NULL},

  /* 95% */
  {"lsm.bloom", "configure bloom filters", C_BOOL, 95, 0, 0, &g.c_bloom, NULL},

  {"lsm.bloom_bit_count", "number of bits per item for bloom filters", 0x0, 4, 64, 1000,
    &g.c_bloom_bit_count, NULL},

  {"lsm.bloom_hash_count", "number of hash values per item for bloom filters", 0x0, 4, 32, 100,
    &g.c_bloom_hash_count, NULL},

  /* 10% */
  {"lsm.bloom_oldest", "configure bloom_oldest=true", C_BOOL, 10, 0, 0, &g.c_bloom_oldest, NULL},

  {"lsm.chunk_size", "LSM chunk size (MB)", 0x0, 1, 10, 100, &g.c_chunk_size, NULL},

  {"lsm.merge_max", "maximum number of chunks to include in an LSM merge operation", 0x0, 4, 20,
    100, &g.c_merge_max, NULL},

  {"lsm.worker_threads", "number of LSM worker threads", 0x0, 3, 4, 20, &g.c_lsm_worker_threads,
    NULL},

  /* 10% */
  {"ops.alter", "configure table alterations", C_BOOL, 10, 0, 0, &g.c_alter, NULL},

  /* 10% */
  {"ops.compaction", "configure compaction", C_BOOL, 10, 0, 0, &g.c_compact, NULL},

  /* 50% */
  {"ops.hs_cursor", "configure history store cursor reads", C_BOOL, 50, 0, 0, &g.c_hs_cursor, NULL},

  {"ops.pct.delete", "delete operations (percentage)", C_IGNORE, 0, 0, 100, &g.c_delete_pct, NULL},

  {"ops.pct.insert", "insert operations (percentage)", C_IGNORE, 0, 0, 100, &g.c_insert_pct, NULL},

  {"ops.pct.modify", "modify operations (percentage)", C_IGNORE, 0, 0, 100, &g.c_modify_pct, NULL},

  {"ops.pct.read", "read operations (percentage)", C_IGNORE, 0, 0, 100, &g.c_read_pct, NULL},

  {"ops.pct.write", "update operations (percentage)", C_IGNORE, 0, 0, 100, &g.c_write_pct, NULL},

  /* 5% */
  {"ops.prepare", "configure transaction prepare", C_BOOL, 5, 0, 0, &g.c_prepare, NULL},

  /* 10% */
  {"ops.random_cursor", "configure random cursor reads", C_BOOL, 10, 0, 0, &g.c_random_cursor,
    NULL},

  /* 100% */
  {"ops.salvage", "configure salvage", C_BOOL, 100, 1, 0, &g.c_salvage, NULL},

  /* 100% */
  {"ops.truncate", "configure truncation", C_BOOL, 100, 0, 0, &g.c_truncate, NULL},

  /* 100% */
  {"ops.verify", "configure verify", C_BOOL, 100, 1, 0, &g.c_verify, NULL},

  {"quiet", "quiet run (same as -q)", C_IGNORE | C_BOOL, 0, 0, 1, &g.c_quiet, NULL},

  {"runs", "number of runs", C_IGNORE, 0, 0, UINT_MAX, &g.c_runs, NULL},

  {"runs.in_memory", "configure in-memory", C_IGNORE | C_BOOL, 0, 0, 1, &g.c_in_memory, NULL},

  {"runs.ops", "operations per run", 0x0, 0, M(2), M(100), &g.c_ops, NULL},

  {"runs.rows", "number of rows", 0x0, 10, M(1), M(100), &g.c_rows, NULL},

  {"runs.source", "data source type (file | lsm | table)", C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_data_source},

  {"runs.threads", "number of worker threads", 0x0, 1, 32, 128, &g.c_threads, NULL},

  {"runs.timer", "run time (minutes)", C_IGNORE, 0, 0, UINT_MAX, &g.c_timer, NULL},

  {"runs.type", "object type (fix | row | var)", C_IGNORE | C_STRING, 0, 0, 0, NULL,
    &g.c_file_type},

  {"runs.verify_failure_dump", "configure page dump on repeatable read error", C_IGNORE | C_BOOL, 0,
    0, 1, &g.c_verify_failure_dump, NULL},

  /* 20% */
  {"statistics", "configure statistics", C_BOOL, 20, 0, 0, &g.c_statistics, NULL},

  /* 5% */
  {"statistics.server", "configure statistics server thread", C_BOOL, 5, 0, 0,
    &g.c_statistics_server, NULL},

  /* 2% */
  {"stress.aggressive_sweep", "stress aggressive sweep", C_BOOL, 2, 0, 0,
    &g.c_timing_stress_aggressive_sweep, NULL},

  /* 2% */
  {"stress.checkpoint", "stress checkpoints", C_BOOL, 2, 0, 0, &g.c_timing_stress_checkpoint, NULL},

  /* 2% */
  {"stress.checkpoint_reserved_txnid_delay", "stress checkpoint invisible transaction id delay",
    C_BOOL, 2, 0, 0, &g.c_timing_stress_checkpoint_reserved_txnid_delay, NULL},

  /* 2% */
  {"stress.checkpoint_prepare", "stress checkpoint prepare", C_BOOL, 2, 0, 0,
    &g.c_timing_stress_checkpoint_prepare, NULL},

  /* 30% */
  {"stress.failpoint_hs_delete_key_from_ts", "stress failpoint history store delete key from ts",
    C_BOOL, 30, 0, 0, &g.c_timing_stress_failpoint_hs_delete_key_from_ts, NULL},

  /* 30% */
  {"stress.failpoint_hs_insert_1", "stress failpoint history store insert (#1)", C_BOOL, 30, 0, 0,
    &g.c_timing_stress_failpoint_hs_insert_1, NULL},

  /* 30% */
  {"stress.failpoint_hs_insert_2", "stress failpoint history store insert (#2)", C_BOOL, 30, 0, 0,
    &g.c_timing_stress_failpoint_hs_insert_2, NULL},

  /* 2% */
  {"stress.hs_checkpoint_delay", "stress history store checkpoint delay", C_BOOL, 2, 0, 0,
    &g.c_timing_stress_hs_checkpoint_delay, NULL},

  /* 2% */
  {"stress.hs_search", "stress history store search", C_BOOL, 2, 0, 0, &g.c_timing_stress_hs_search,
    NULL},

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

  {"transaction.implicit", "implicit, without timestamps, transactions (percentage)", 0x0, 0, 100,
    100, &g.c_txn_implicit, NULL},

  /* 70% */
  {"transaction.timestamps", "all transactions (or none), have timestamps", C_BOOL, 80, 0, 0,
    &g.c_txn_timestamps, NULL},

  {"wiredtiger.config", "wiredtiger_open API configuration string", C_IGNORE | C_STRING, 0, 0, 0,
    NULL, &g.c_config_open},

  /* 80% */
  {"wiredtiger.rwlock", "configure wiredtiger read/write mutexes", C_BOOL, 80, 0, 0, &g.c_wt_mutex,
    NULL},

  {"wiredtiger.leak_memory", "configure memory leaked on shutdown", C_BOOL, 0, 0, 0,
    &g.c_leak_memory, NULL},

  {NULL, NULL, 0x0, 0, 0, 0, NULL, NULL}};
