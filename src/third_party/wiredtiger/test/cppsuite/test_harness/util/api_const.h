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

#include <string>

/* Define all constants related to WiredTiger APIs and testing. */
namespace test_harness {

/* Component names. */
extern const std::string CHECKPOINT_MANAGER;
extern const std::string RUNTIME_MONITOR;
extern const std::string TIMESTAMP_MANAGER;
extern const std::string WORKLOAD_GENERATOR;
extern const std::string WORKLOAD_TRACKING;

/* Configuration API consts. */
extern const std::string CACHE_SIZE_MB;
extern const std::string COLLECTION_COUNT;
extern const std::string COMPRESSION_ENABLED;
extern const std::string DURATION_SECONDS;
extern const std::string ENABLED;
extern const std::string ENABLE_LOGGING;
extern const std::string INSERT_CONFIG;
extern const std::string KEY_COUNT_PER_COLLECTION;
extern const std::string KEY_SIZE;
extern const std::string LIMIT;
extern const std::string MAX;
extern const std::string MIN;
extern const std::string OLDEST_LAG;
extern const std::string OP_RATE;
extern const std::string OPS_PER_TRANSACTION;
extern const std::string POPULATE_CONFIG;
extern const std::string POSTRUN_STATISTICS;
extern const std::string READ_CONFIG;
extern const std::string STABLE_LAG;
extern const std::string STAT_CACHE_SIZE;
extern const std::string STAT_DB_SIZE;
extern const std::string STATISTICS_CONFIG;
extern const std::string THREAD_COUNT;
extern const std::string TYPE;
extern const std::string UPDATE_CONFIG;
extern const std::string VALUE_SIZE;

/* WiredTiger API consts. */
extern const std::string COMMIT_TS;
extern const std::string CONNECTION_CREATE;
extern const std::string OLDEST_TS;
extern const std::string STABLE_TS;
extern const std::string STATISTICS_LOG;

/*
 * Use the Snappy compressor for stress testing to avoid excessive disk space usage. Our builds can
 * pre-specify 'EXTSUBPATH' to indicate any special sub-directories the module is located. If unset
 * we fallback to the '.libs' sub-directory used by autoconf.
 */
#define BLKCMP_PFX "block_compressor="
#define SNAPPY_BLK BLKCMP_PFX "snappy"
#define EXTPATH "../../ext/"
#ifndef EXTSUBPATH
#define EXTSUBPATH ".libs/"
#endif
#define SNAPPY_PATH EXTPATH "compressors/snappy/" EXTSUBPATH "libwiredtiger_snappy.so"
#define SNAPPY_EXT ",extensions=(" SNAPPY_PATH ")"

/* Test harness consts. */
extern const std::string DEFAULT_FRAMEWORK_SCHEMA;
extern const std::string TABLE_OPERATION_TRACKING;
extern const std::string TABLE_SCHEMA_TRACKING;
extern const std::string STATISTICS_URI;

} // namespace test_harness

#endif
