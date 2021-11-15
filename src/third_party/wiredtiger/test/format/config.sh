#! /bin/sh

# This script creates format's config.h and config_def.c files. To change format's configuration,
# modify this file and then run it as a script.

fc="config_def.c"
fh="config.h"

cat<<END_OF_HEADER_FILE_PREFIX>$fh
/* DO NOT EDIT: automatically built by format/config.sh. */

#define C_TYPE_MATCH(cp, type)                                                                    \\
    (!F_ISSET(cp, (C_TYPE_FIX | C_TYPE_ROW | C_TYPE_VAR)) ||                                      \\
      ((type) == FIX && F_ISSET(cp, C_TYPE_FIX)) || ((type) == ROW && F_ISSET(cp, C_TYPE_ROW)) || \\
      ((type) == VAR && F_ISSET(cp, C_TYPE_VAR)))

typedef struct {
    const char *name; /* Configuration item */
    const char *desc; /* Configuration description */

#define C_BOOL 0x001u        /* Boolean (true if roll of 1-to-100 is <= CONFIG->min) */
#define C_IGNORE 0x002u      /* Not a simple randomization, configured specially */
#define C_STRING 0x004u      /* String (rather than integral) */
#define C_TABLE 0x008u       /* Value is per table, not global */
#define C_TYPE_FIX 0x010u    /* Value is only relevant to FLCS */
#define C_TYPE_LSM 0x020u    /* Value is only relevant to LSM */
#define C_TYPE_ROW 0x040u    /* Value is only relevant to RS */
#define C_TYPE_VAR 0x080u    /* Value is only relevant to VLCS */
#define C_ZERO_NOTSET 0x100u /* Ignore zero values */
    uint32_t flags;

    uint32_t min;     /* Minimum value */
    uint32_t maxrand; /* Maximum value randomly chosen */
    uint32_t maxset;  /* Maximum value explicitly set */

    u_int off; /* Value offset */
} CONFIG;

#define V_MAX_TABLES_CONFIG 1000

END_OF_HEADER_FILE_PREFIX

n=0
while IFS= read -r line; do
    case "$line" in
    '{"'*)
	tag=`echo "$line" |
	sed -e 's/{"//' \
	    -e 's/",.*//' \
	    -e 's/\./_/g' |
	tr '[:lower:]' '[:upper:]'`
	prefix="GLOBAL"
	if `echo "$line" | grep 'C_TABLE' > /dev/null`; then
	    prefix="TABLE"
	fi
	def="V_""$prefix""_""$tag"
	echo "$line" |
	sed -e "s/}/, $def},/" \
	    -e 's/\(^.*",\) \(.*\)/  \1\n    \2/'

	echo "#define $def $n" >> $fh

	n=`expr $n + 1`
	;;
    *)
	echo "$line"
    esac
done<<END_OF_INPUT>$fc
/* DO NOT EDIT: automatically built by format/config.sh. */

#include "format.h"

CONFIG configuration_list[] = {
{"assert.read_timestamp", "assert read_timestamp", C_BOOL, 2, 0, 0}

{"assert.write_timestamp", "set write_timestamp_usage and assert write_timestamp", C_BOOL, 2, 0, 0}

{"backup", "configure backups", C_BOOL, 20, 0, 0}

{"backup.incremental", "backup type (off | block | log)", C_IGNORE | C_STRING, 0, 0, 0}

{"backup.incr_granularity", "incremental backup block granularity (KB)", 0x0, 4, 16384, 16384}

{"block_cache", "enable the block cache", C_BOOL, 10, 0, 0}

{"block_cache.cache_on_checkpoint", "block cache: cache checkpoint writes", C_BOOL, 30, 0, 0}

{"block_cache.cache_on_writes", "block cache: populate the cache on writes", C_BOOL, 60, 0, 0}

{"block_cache.size", "block cache size (MB)", 0x0, 1, 100, 100 * 1024}

{"btree.bitcnt", "fixed-length column-store object size (number of bits)", C_TABLE | C_TYPE_FIX, 1, 8, 8}

{"btree.compression", "data compression (off | lz4 | snappy | zlib | zstd)", C_IGNORE | C_STRING | C_TABLE, 0, 0, 0}

{"btree.dictionary", "configure dictionary compressed values", C_BOOL | C_TABLE | C_TYPE_ROW | C_TYPE_VAR, 20, 0, 0}

{"btree.huffman_value", "configure huffman encoded values", C_BOOL | C_TABLE | C_TYPE_ROW | C_TYPE_VAR, 20, 0, 0}

{"btree.internal_key_truncation", "truncate internal keys", C_BOOL | C_TABLE, 95, 0, 0}

{"btree.internal_page_max", "btree internal node maximum size", C_TABLE, 9, 17, 27}

{"btree.key_max", "maximum key size", C_TABLE | C_TYPE_ROW, 20, 128, MEGABYTE(10)}

{"btree.key_min", "minimum key size", C_TABLE | C_TYPE_ROW, KEY_LEN_CONFIG_MIN, 32, 256}

{"btree.leaf_page_max", "btree leaf node maximum size", C_TABLE, 9, 17, 27}

{"btree.memory_page_max", "maximum cache page size", C_TABLE, 1, 10, 128}

{"btree.prefix_len", "common key prefix", C_TABLE | C_TYPE_ROW | C_ZERO_NOTSET, PREFIX_LEN_CONFIG_MIN, PREFIX_LEN_CONFIG_MAX, PREFIX_LEN_CONFIG_MAX}

{"btree.prefix_compression", "configure prefix compressed keys", C_BOOL | C_TABLE | C_TYPE_ROW, 80, 0, 0}

{"btree.prefix_compression_min", "minimum gain before prefix compression is used (bytes)", C_TABLE | C_TYPE_ROW, 0, 8, 256}

{"btree.repeat_data_pct", "duplicate values (percentage)", C_TABLE | C_TYPE_VAR, 0, 90, 90}

{"btree.reverse", "reverse order collation", C_BOOL | C_TABLE | C_TYPE_ROW, 10, 0, 0}

{"btree.split_pct", "page split size as a percentage of the maximum page size", C_TABLE, 50, 100, 100}

{"btree.value_max", "maximum value size", C_TABLE | C_TYPE_ROW | C_TYPE_VAR, 32, 4096, MEGABYTE(10)}

{"btree.value_min", "minimum value size", C_TABLE | C_TYPE_ROW | C_TYPE_VAR, 0, 20, 4096}

{"cache", "cache size (MB)", 0x0, 1, 100, 100 * 1024}

{"cache.evict_max", "maximum number of eviction workers", 0x0, 0, 5, 100}

{"cache.minimum", "minimum cache size (MB)", C_IGNORE, 0, 0, 100 * 1024}

{"checkpoint", "checkpoint type (on | off | wiredtiger)", C_IGNORE | C_STRING, 0, 0, 0}

{"checkpoint.log_size", "MB of log to wait if wiredtiger checkpoints configured", 0x0, 20, 200, 1024}

{"checkpoint.wait", "seconds to wait if wiredtiger checkpoints configured", 0x0, 5, 100, 3600}

{"disk.checksum", "checksum type (on | off | uncompressed | unencrypted)", C_IGNORE | C_STRING | C_TABLE, 0, 0, 0}

{"disk.data_extend", "configure data file extension", C_BOOL, 5, 0, 0}

{"disk.direct_io", "configure direct I/O for data objects", C_BOOL | C_IGNORE, 0, 0, 1}

{"disk.encryption", "encryption type (off | rotn-7)", C_IGNORE | C_STRING, 0, 0, 0}

{"disk.firstfit", "configure first-fit allocation", C_BOOL | C_TABLE, 10, 0, 0}

{"disk.mmap", "configure mmap operations (reads only)", C_BOOL, 90, 0, 0}

{"disk.mmap_all", "configure mmap operations (read and write)", C_BOOL, 5, 0, 0}

{"format.abort", "drop core during timed run", C_BOOL, 0, 0, 0}

{"format.independent_thread_rng", "configure independent thread RNG space", C_BOOL, 75, 0, 0}

{"format.major_timeout", "long-running operations timeout (minutes)", C_IGNORE, 0, 0, 1000}

/*
 * 0%
 * FIXME-WT-7510: Temporarily disable import until WT_ROLLBACK error and wt_copy_and_sync error is
 * fixed. It should be (C_BOOL, 20, 0, 0).
 */
{"import", "import table from newly created database", C_BOOL, 0, 0, 0}

{"logging", "configure logging", C_BOOL, 50, 0, 0}

{"logging.archive", "configure log file archival", C_BOOL, 50, 0, 0}

{"logging.compression", "logging compression (off | lz4 | snappy | zlib | zstd)", C_IGNORE | C_STRING, 0, 0, 0}

{"logging.file_max", "maximum log file size (KB)", 0x0, 100, 512000, 2097152}

{"logging.prealloc", "configure log file pre-allocation", C_BOOL, 50, 0, 0}

{"lsm.auto_throttle", "throttle LSM inserts", C_BOOL | C_TABLE | C_TYPE_LSM, 90, 0, 0}

{"lsm.bloom", "configure bloom filters", C_BOOL | C_TABLE | C_TYPE_LSM, 95, 0, 0}

{"lsm.bloom_bit_count", "number of bits per item for bloom filters", C_TABLE | C_TYPE_LSM, 4, 64, 1000}

{"lsm.bloom_hash_count", "number of hash values per item for bloom filters", C_TABLE | C_TYPE_LSM, 4, 32, 100}

{"lsm.bloom_oldest", "configure bloom_oldest=true", C_BOOL | C_TABLE | C_TYPE_LSM, 10, 0, 0}

{"lsm.chunk_size", "LSM chunk size (MB)", C_TABLE | C_TYPE_LSM, 1, 10, 100}

{"lsm.merge_max", "maximum number of chunks to include in an LSM merge operation", C_TABLE | C_TYPE_LSM, 4, 20, 100}

{"lsm.worker_threads", "number of LSM worker threads", C_TYPE_LSM, 3, 4, 20}

{"ops.alter", "configure table alterations", C_BOOL, 10, 0, 0}

{"ops.compaction", "configure compaction", C_BOOL, 10, 0, 0}

{"ops.hs_cursor", "configure history store cursor reads", C_BOOL, 50, 0, 0}

{"ops.pct.delete", "delete operations (percentage)", C_IGNORE | C_TABLE, 0, 0, 100}

{"ops.pct.insert", "insert operations (percentage)", C_IGNORE | C_TABLE, 0, 0, 100}

{"ops.pct.modify", "modify operations (percentage)", C_IGNORE | C_TABLE, 0, 0, 100}

{"ops.pct.read", "read operations (percentage)", C_IGNORE | C_TABLE, 0, 0, 100}

{"ops.pct.write", "update operations (percentage)", C_IGNORE | C_TABLE, 0, 0, 100}

{"ops.prepare", "configure transaction prepare", C_BOOL, 5, 0, 0}

{"ops.random_cursor", "configure random cursor reads", C_BOOL, 10, 0, 0}

{"ops.salvage", "configure salvage", C_BOOL, 100, 1, 0}

{"ops.truncate", "configure truncation", C_BOOL | C_TABLE, 100, 0, 0}

{"ops.verify", "configure verify", C_BOOL, 100, 1, 0}

{"quiet", "quiet run (same as -q)", C_BOOL | C_IGNORE, 0, 0, 1}

{"runs.in_memory", "configure in-memory", C_BOOL | C_IGNORE, 0, 0, 1}

{"runs.ops", "operations per run", 0x0, 0, M(2), M(100)}

{"runs.rows", "number of rows", C_TABLE, 10, M(1), M(100)}

{"runs.source", "data source type (file | lsm | table)", C_IGNORE | C_STRING | C_TABLE, 0, 0, 0}

{"runs.tables", "number of tables", 0x0, 1, 32, V_MAX_TABLES_CONFIG}

{"runs.threads", "number of worker threads", 0x0, 1, 32, 128}

{"runs.timer", "run time (minutes)", C_IGNORE, 0, 0, UINT_MAX}

{"runs.type", "object type (fix | row | var)", C_IGNORE | C_STRING | C_TABLE, 0, 0, 0}

{"runs.verify_failure_dump", "configure page dump on repeatable read error", C_BOOL | C_IGNORE, 0, 0, 1}

{"statistics", "configure statistics", C_BOOL, 20, 0, 0}

{"statistics.server", "configure statistics server thread", C_BOOL, 5, 0, 0}

{"stress.aggressive_sweep", "stress aggressive sweep", C_BOOL, 2, 0, 0}

{"stress.checkpoint", "stress checkpoints", C_BOOL, 2, 0, 0}

{"stress.checkpoint_reserved_txnid_delay", "stress checkpoint invisible transaction id delay", C_BOOL, 2, 0, 0}

{"stress.checkpoint_prepare", "stress checkpoint prepare", C_BOOL, 2, 0, 0}

{"stress.failpoint_hs_delete_key_from_ts", "stress failpoint history store delete key from ts", C_BOOL, 30, 0, 0}

{"stress.failpoint_hs_insert_1", "stress failpoint history store insert (#1)", C_BOOL, 30, 0, 0}

{"stress.failpoint_hs_insert_2", "stress failpoint history store insert (#2)", C_BOOL, 30, 0, 0}

{"stress.hs_checkpoint_delay", "stress history store checkpoint delay", C_BOOL, 2, 0, 0}

{"stress.hs_search", "stress history store search", C_BOOL, 2, 0, 0}

{"stress.hs_sweep", "stress history store sweep", C_BOOL, 2, 0, 0}

{"stress.split_1", "stress splits (#1)", C_BOOL, 2, 0, 0}

{"stress.split_2", "stress splits (#2)", C_BOOL, 2, 0, 0}

{"stress.split_3", "stress splits (#3)", C_BOOL, 2, 0, 0}

{"stress.split_4", "stress splits (#4)", C_BOOL, 2, 0, 0}

{"stress.split_5", "stress splits (#5)", C_BOOL, 2, 0, 0}

{"stress.split_6", "stress splits (#6)", C_BOOL, 2, 0, 0}

{"stress.split_7", "stress splits (#7)", C_BOOL, 2, 0, 0}

{"transaction.implicit", "implicit, without timestamps, transactions (percentage)", 0, 0, 100, 100}

{"transaction.timestamps", "all transactions (or none), have timestamps", C_BOOL, 80, 0, 0}

{"wiredtiger.config", "wiredtiger_open API configuration string", C_IGNORE | C_STRING, 0, 0, 0}

{"wiredtiger.rwlock", "configure wiredtiger read/write mutexes", C_BOOL, 80, 0, 0}

{"wiredtiger.leak_memory", "leak memory on wiredtiger shutdown", C_BOOL, 0, 0, 0}
END_OF_INPUT

(
echo
echo '  {NULL, NULL, 0x0, 0, 0, 0, 0}'
echo '};') >> $fc

(
echo
echo "#define V_ELEMENT_COUNT $n") >> $fh

