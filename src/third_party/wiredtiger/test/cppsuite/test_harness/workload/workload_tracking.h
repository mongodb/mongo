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

#ifndef WORKLOAD_TRACKING_H
#define WORKLOAD_TRACKING_H

#include "../core/component.h"
#include "../timestamp_manager.h"
#include "../util/scoped_types.h"

/*
 * Default schema for tracking operations on collections (key_format: Collection id / Key /
 * Timestamp, value_format: Operation type / Value)
 */
#define OPERATION_TRACKING_KEY_FORMAT WT_UNCHECKED_STRING(QSQ)
#define OPERATION_TRACKING_VALUE_FORMAT WT_UNCHECKED_STRING(iS)
#define OPERATION_TRACKING_TABLE_CONFIG \
    "key_format=" OPERATION_TRACKING_KEY_FORMAT ",value_format=" OPERATION_TRACKING_VALUE_FORMAT

/*
 * Default schema for tracking schema operations on collections (key_format: Collection id /
 * Timestamp, value_format: Operation type)
 */
#define SCHEMA_TRACKING_KEY_FORMAT WT_UNCHECKED_STRING(QQ)
#define SCHEMA_TRACKING_VALUE_FORMAT WT_UNCHECKED_STRING(i)
#define SCHEMA_TRACKING_TABLE_CONFIG \
    "key_format=" SCHEMA_TRACKING_KEY_FORMAT ",value_format=" SCHEMA_TRACKING_VALUE_FORMAT

namespace test_harness {
/* Tracking operations. */
enum class tracking_operation { CREATE_COLLECTION, DELETE_COLLECTION, DELETE_KEY, INSERT };

/* Class used to track operations performed on collections */
class workload_tracking : public component {
    public:
    explicit workload_tracking(configuration *_config, const std::string &operation_table_config,
      const std::string &operation_table_name, const std::string &schema_table_config,
      const std::string &schema_table_name, const bool use_compression, timestamp_manager &tsm);
    virtual ~workload_tracking() = default;

    const std::string &get_schema_table_name() const;
    const std::string &get_operation_table_name() const;
    void load() override final;

    /*
     * As every operation is tracked in the tracking table we need to clear out obsolete operations
     * otherwise the file size grow continuously, as such we cleanup operations that are no longer
     * relevant, i.e. older than the oldest timestamp.
     */
    void do_work() override final;

    void save_schema_operation(
      const tracking_operation &operation, const uint64_t &collection_id, wt_timestamp_t ts);

    template <typename K, typename V>
    int
    save_operation(const tracking_operation &operation, const uint64_t &collection_id, const K &key,
      const V &value, wt_timestamp_t ts, scoped_cursor &op_track_cursor)
    {
        WT_DECL_RET;
        std::string error_message;

        if (!_enabled)
            return (0);

        testutil_assert(op_track_cursor.get() != nullptr);

        if (operation == tracking_operation::CREATE_COLLECTION ||
          operation == tracking_operation::DELETE_COLLECTION) {
            error_message =
              "save_operation: invalid operation " + std::to_string(static_cast<int>(operation));
            testutil_die(EINVAL, error_message.c_str());
        } else {
            op_track_cursor->set_key(op_track_cursor.get(), collection_id, key, ts);
            op_track_cursor->set_value(op_track_cursor.get(), static_cast<int>(operation), value);
            ret = op_track_cursor->insert(op_track_cursor.get());
        }
        return (ret);
    }

    private:
    scoped_session _session;
    scoped_cursor _schema_track_cursor;
    scoped_cursor _sweep_cursor;
    const std::string _operation_table_config;
    const std::string _operation_table_name;
    const std::string _schema_table_config;
    const std::string _schema_table_name;
    const bool _use_compression;
    timestamp_manager &_tsm;
};
} // namespace test_harness

#endif
