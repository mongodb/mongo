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

#include "constants.h"

/* Define all constants related to WiredTiger APIs and testing. */
namespace test_harness {

/* Component names. */
const std::string OPERATION_TRACKER = "operation_tracker";
const std::string METRICS_MONITOR = "metrics_monitor";
const std::string TIMESTAMP_MANAGER = "timestamp_manager";
const std::string WORKLOAD_MANAGER = "workload_manager";

/* Configuration API consts. */
const std::string CACHE_HS_INSERT = "cache_hs_insert";
const std::string CACHE_MAX_WAIT_MS = "cache_max_wait_ms";
const std::string CACHE_SIZE_MB = "cache_size_mb";
const std::string CC_PAGES_REMOVED = "cc_pages_removed";
const std::string COLLECTION_COUNT = "collection_count";
const std::string CHECKPOINT_OP_CONFIG = "checkpoint_config";
const std::string COMPRESSION_ENABLED = "compression_enabled";
const std::string CUSTOM_OP_CONFIG = "custom_config";
const std::string DURATION_SECONDS = "duration_seconds";
const std::string ENABLED = "enabled";
const std::string ENABLE_LOGGING = "enable_logging";
const std::string INSERT_OP_CONFIG = "insert_config";
const std::string KEY_COUNT_PER_COLLECTION = "key_count_per_collection";
const std::string KEY_SIZE = "key_size";
const std::string LIMIT = "limit";
const std::string MAX = "max";
const std::string MIN = "min";
const std::string OLDEST_LAG = "oldest_lag";
const std::string OP_RATE = "op_rate";
const std::string OPS_PER_TRANSACTION = "ops_per_transaction";
const std::string POPULATE_CONFIG = "populate_config";
const std::string POSTRUN_STATISTICS = "postrun";
const std::string READ_OP_CONFIG = "read_config";
const std::string REMOVE_OP_CONFIG = "remove_config";
const std::string REVERSE_COLLATOR = "reverse_collator";
const std::string RUNTIME_STATISTICS = "runtime";
const std::string SAVE = "save";
const std::string STABLE_LAG = "stable_lag";
const std::string STAT_CACHE_SIZE = "stat_cache_size";
const std::string STAT_DB_SIZE = "stat_db_size";
const std::string STATISTICS_CONFIG = "statistics_config";
const std::string THREAD_COUNT = "thread_count";
const std::string TRACKING_KEY_FORMAT = "tracking_key_format";
const std::string TRACKING_VALUE_FORMAT = "tracking_value_format";
const std::string TYPE = "type";
const std::string UPDATE_OP_CONFIG = "update_config";
const std::string VALUE_SIZE = "value_size";

/* WiredTiger API consts. */
const std::string COMMIT_TS = "commit_timestamp";
const std::string CONNECTION_CREATE = "create";
const std::string OLDEST_TS = "oldest_timestamp";
const std::string STABLE_TS = "stable_timestamp";
const std::string STATISTICS_LOG = "statistics_log=(json,wait=1)";

/* Test harness consts. */
const std::string DEFAULT_FRAMEWORK_SCHEMA = "key_format=S,value_format=S,";
const std::string TABLE_OPERATION_TRACKING = "table:operation_tracking";
const std::string TABLE_SCHEMA_TRACKING = "table:schema_tracking";
const std::string STATISTICS_URI = "statistics:";

} // namespace test_harness
