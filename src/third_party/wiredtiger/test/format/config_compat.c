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

#include "format.h"

static const char *list[] = {
  "abort=",
  "format.abort",
  "alter=",
  "ops.alter",
  "assert_commit_timestamp=",
  "assert.commit_timestamp",
  "assert_read_timestamp=",
  "assert.read_timestamp",
  "auto_throttle=",
  "lsm.auto_throttle",
  "backup_incremental=",
  "backup.incremental",
  "backups=",
  "backup",
  "bitcnt=",
  "btree.bitcnt",
  "bloom=",
  "lsm.bloom",
  "bloom_bit_count=",
  "lsm.bloom_bit_count",
  "bloom_hash_count=",
  "lsm.bloom_hash_count",
  "bloom_oldest=",
  "lsm.bloom_oldest",
  "cache=",
  "cache",
  "cache_minimum=",
  "cache.minimum",
  "checkpoint_log_size=",
  "checkpoint.log_size",
  "checkpoint_wait=",
  "checkpoint.wait",
  "checkpoints=",
  "checkpoint",
  "checksum=",
  "disk.checksum",
  "chunk_size=",
  "lsm.chunk_size",
  "compaction=",
  "ops.compaction",
  "compression=",
  "btree.compression",
  "data_extend=",
  "disk.data_extend",
  "data_source=",
  "runs.source",
  "delete_pct=",
  "ops.pct.delete",
  "dictionary=",
  "btree.dictionary",
  "direct_io=",
  "disk.direct_io",
  "encryption=",
  "disk.encryption",
  "evict_max=",
  "cache.evict_max",
  "file_type=",
  "runs.type",
  "firstfit=",
  "disk.firstfit",
  "huffman_value=",
  "btree.huffman_value",
  "in_memory=",
  "runs.in_memory",
  "independent_thread_rng=",
  "format.independent_thread_rng",
  "insert_pct=",
  "ops.pct.insert",
  "internal_key_truncation=",
  "btree.internal_key_truncation",
  "internal_page_max=",
  "btree.internal_page_max",
  "key_max=",
  "btree.key_max",
  "key_min=",
  "btree.key_min",
  "leaf_page_max=",
  "btree.leaf_page_max",
  "leak_memory=",
  "wiredtiger.leak_memory",
  "logging.archive=",
  "logging.remove",
  "logging_archive=",
  "logging.remove",
  "logging_compression=",
  "logging.compression",
  "logging_file_max=",
  "logging.file_max",
  "logging_prealloc=",
  "logging.prealloc",
  "lsm_worker_threads=",
  "lsm.worker_threads",
  "major_timeout=",
  "format.major_timeout",
  "memory_page_max=",
  "btree.memory_page_max",
  "merge_max=",
  "lsm.merge_max",
  "mmap=",
  "disk.mmap",
  "mmap_all=",
  "disk.mmap_all",
  "modify_pct=",
  "ops.pct.modify",
  "ops=",
  "runs.ops",
  "prefix_compression=",
  "btree.prefix_compression",
  "prefix_compression_min=",
  "btree.prefix_compression_min",
  "prepare=",
  "ops.prepare",
  "random_cursor=",
  "ops.random_cursor",
  "read_pct=",
  "ops.pct.read",
  "repeat_data_pct=",
  "btree.repeat_data_pct",
  "reverse=",
  "btree.reverse",
  "rows=",
  "runs.rows",
  "salvage=",
  "ops.salvage",
  "split_pct=",
  "btree.split_pct",
  "statistics=",
  "statistics",
  "statistics_server=",
  "statistics.server",
  "threads=",
  "runs.threads",
  "timer=",
  "runs.timer",
  "timing_stress_aggressive_sweep=",
  "stress.aggressive_sweep",
  "timing_stress_checkpoint=",
  "stress.checkpoint",
  "timing_stress_hs_sweep=",
  "stress.hs_sweep",
  "timing_stress_split_1=",
  "stress.split_1",
  "timing_stress_split_2=",
  "stress.split_2",
  "timing_stress_split_3=",
  "stress.split_3",
  "timing_stress_split_4=",
  "stress.split_4",
  "timing_stress_split_5=",
  "stress.split_5",
  "timing_stress_split_6=",
  "stress.split_6",
  "timing_stress_split_7=",
  "stress.split_7",
  "transaction-frequency=",
  "transaction.frequency",
  "transaction_timestamps=",
  "transaction.timestamps",
  "truncate=",
  "ops.truncate",
  "value_max=",
  "btree.value_max",
  "value_min=",
  "btree.value_min",
  "verify=",
  "ops.verify",
  "wiredtiger_config=",
  "wiredtiger.config",
  "write_pct=",
  "ops.pct.write",
  NULL,
  NULL,
};

/*
 * config_compat --
 *     Convert old names to new ones.
 */
void
config_compat(const char **origp)
{
    static char conv[100];
    const char *equalp, *orig, **p;

    orig = *origp;
    if ((equalp = strchr(orig, '=')) == NULL)
        return;

    for (p = list; *p != NULL; p += 2)
        if (strncmp(orig, *p, (size_t)((equalp - orig) + 1)) == 0) {
            testutil_check(__wt_snprintf(conv, sizeof(conv), "%s%s", *++p, equalp));
            *origp = conv;
            break;
        }
}
