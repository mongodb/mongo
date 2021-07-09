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
    workload_tracking(configuration *_config, const std::string &operation_table_config,
      const std::string &operation_table_name, const std::string &schema_table_config,
      const std::string &schema_table_name, timestamp_manager &tsm)
        : component("workload_tracking", _config), _operation_table_config(operation_table_config),
          _operation_table_name(operation_table_name), _schema_table_config(schema_table_config),
          _schema_table_name(schema_table_name), _tsm(tsm)
    {
    }

    const std::string &
    get_schema_table_name() const
    {
        return (_schema_table_name);
    }

    const std::string &
    get_operation_table_name() const
    {
        return (_operation_table_name);
    }

    void
    load() override final
    {
        component::load();

        if (!_enabled)
            return;

        /* Initiate schema tracking. */
        _session = connection_manager::instance().create_session();
        testutil_check(_session->create(
          _session.get(), _schema_table_name.c_str(), _schema_table_config.c_str()));
        _schema_track_cursor = _session.open_scoped_cursor(_schema_table_name.c_str());
        log_msg(LOG_TRACE, "Schema tracking initiated");

        /* Initiate operations tracking. */
        testutil_check(_session->create(
          _session.get(), _operation_table_name.c_str(), _operation_table_config.c_str()));
        log_msg(LOG_TRACE, "Operations tracking created");

        /*
         * Open sweep cursor. This cursor will be used to clear out obsolete data from the tracking
         * table.
         */
        _sweep_cursor = _session.open_scoped_cursor(_operation_table_name.c_str());
        log_msg(LOG_TRACE, "Tracking table sweep initialized");
    }

    /*
     * As every operation is tracked in the tracking table we need to clear out obsolete operations
     * otherwise the file size grow continuously, as such we cleanup operations that are no longer
     * relevant, i.e. older than the oldest timestamp.
     */
    void
    do_work() override final
    {
        WT_DECL_RET;
        wt_timestamp_t ts, oldest_ts;
        uint64_t collection_id, sweep_collection_id;
        int op_type;
        const char *key, *value;
        char *sweep_key;
        bool globally_visible_update_found;

        key = sweep_key = nullptr;
        globally_visible_update_found = false;

        /* Take a copy of the oldest so that we sweep with a consistent timestamp. */
        oldest_ts = _tsm.get_oldest_ts();

        /* If we have a position, give it up. */
        testutil_check(_sweep_cursor->reset(_sweep_cursor.get()));

        while ((ret = _sweep_cursor->prev(_sweep_cursor.get())) == 0) {
            testutil_check(_sweep_cursor->get_key(_sweep_cursor.get(), &collection_id, &key, &ts));
            testutil_check(_sweep_cursor->get_value(_sweep_cursor.get(), &op_type, &value));
            /*
             * If we're on a new key, reset the check. We want to track whether we have a globally
             * visible update for the current key.
             */
            if (sweep_key == nullptr || sweep_collection_id != collection_id ||
              strcmp(sweep_key, key) != 0) {
                globally_visible_update_found = false;
                if (sweep_key != nullptr)
                    free(sweep_key);
                sweep_key = static_cast<char *>(dstrdup(key));
                sweep_collection_id = collection_id;
            }
            if (ts <= oldest_ts) {
                if (globally_visible_update_found) {
                    if (_trace_level == LOG_TRACE)
                        log_msg(LOG_TRACE,
                          std::string("workload tracking: Obsoleted update, key=") + sweep_key +
                            ", collection_id=" + std::to_string(collection_id) +
                            ", timestamp=" + std::to_string(ts) +
                            ", oldest_timestamp=" + std::to_string(oldest_ts) + ", value=" + value);
                    testutil_check(_sweep_cursor->remove(_sweep_cursor.get()));
                } else if (static_cast<tracking_operation>(op_type) == tracking_operation::INSERT) {
                    if (_trace_level == LOG_TRACE)
                        log_msg(LOG_TRACE,
                          std::string("workload tracking: Found globally visible update, key=") +
                            sweep_key + ", collection_id=" + std::to_string(collection_id) +
                            ", timestamp=" + std::to_string(ts) +
                            ", oldest_timestamp=" + std::to_string(oldest_ts) + ", value=" + value);
                    globally_visible_update_found = true;
                }
            }
        }

        free(sweep_key);

        if (ret != WT_NOTFOUND)
            testutil_die(LOG_ERROR,
              "Tracking table sweep failed: cursor->next() returned an unexpected error %d.", ret);
    }

    void
    save_schema_operation(
      const tracking_operation &operation, const uint64_t &collection_id, wt_timestamp_t ts)
    {
        std::string error_message;

        if (!_enabled)
            return;

        if (operation == tracking_operation::CREATE_COLLECTION ||
          operation == tracking_operation::DELETE_COLLECTION) {
            _schema_track_cursor->set_key(_schema_track_cursor.get(), collection_id, ts);
            _schema_track_cursor->set_value(
              _schema_track_cursor.get(), static_cast<int>(operation));
            testutil_check(_schema_track_cursor->insert(_schema_track_cursor.get()));
        } else {
            error_message = "save_schema_operation: invalid operation " +
              std::to_string(static_cast<int>(operation));
            testutil_die(EINVAL, error_message.c_str());
        }
    }

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
    timestamp_manager &_tsm;
};
} // namespace test_harness

#endif
