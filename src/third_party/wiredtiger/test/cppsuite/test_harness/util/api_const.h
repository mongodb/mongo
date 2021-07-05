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

#ifndef API_CONST_H
#define API_CONST_H

/* Define all constants related to WiredTiger APIs and testing. */
namespace test_harness {

/* Component names. */
static const char *CHECKPOINT_MANAGER = "checkpoint_manager";
static const char *RUNTIME_MONITOR = "runtime_monitor";
static const char *TIMESTAMP_MANAGER = "timestamp_manager";
static const char *WORKLOAD_GENERATOR = "workload_generator";
static const char *WORKLOAD_TRACKING = "workload_tracking";

/* Configuration API consts. */
static const char *CACHE_SIZE_MB = "cache_size_mb";
static const char *COLLECTION_COUNT = "collection_count";
static const char *DURATION_SECONDS = "duration_seconds";
static const char *ENABLED = "enabled";
static const char *ENABLE_LOGGING = "enable_logging";
static const char *INSERT_CONFIG = "insert_config";
static const char *KEY_COUNT_PER_COLLECTION = "key_count_per_collection";
static const char *KEY_SIZE = "key_size";
static const char *LIMIT = "limit";
static const char *MAX = "max";
static const char *MIN = "min";
static const char *OLDEST_LAG = "oldest_lag";
static const char *OP_RATE = "op_rate";
static const char *OPS_PER_TRANSACTION = "ops_per_transaction";
static const char *POPULATE_CONFIG = "populate_config";
static const char *POSTRUN_STATISTICS = "postrun_statistics";
static const char *READ_CONFIG = "read_config";
static const char *STABLE_LAG = "stable_lag";
static const char *STAT_CACHE_SIZE = "stat_cache_size";
static const char *STAT_DB_SIZE = "stat_db_size";
static const char *STATISTICS_CONFIG = "statistics_config";
static const char *THREAD_COUNT = "thread_count";
static const char *TYPE = "type";
static const char *UPDATE_CONFIG = "update_config";
static const char *VALUE_SIZE = "value_size";

/* WiredTiger API consts. */
static const char *COMMIT_TS = "commit_timestamp";
static const char *CONNECTION_CREATE = "create";
static const char *OLDEST_TS = "oldest_timestamp";
static const char *STABLE_TS = "stable_timestamp";
static const char *STATISTICS_LOG = "statistics_log=(json,wait=1)";

/* Test harness consts. */
static const char *DEFAULT_FRAMEWORK_SCHEMA = "key_format=S,value_format=S";
static const char *TABLE_OPERATION_TRACKING = "table:operation_tracking";
static const char *TABLE_SCHEMA_TRACKING = "table:schema_tracking";
static const char *STATISTICS_URI = "statistics:";

} // namespace test_harness

#endif
