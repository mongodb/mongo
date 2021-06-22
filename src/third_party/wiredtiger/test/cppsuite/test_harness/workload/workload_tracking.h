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
 * Default schema for tracking operations on collections (key_format: Collection name / Key /
 * Timestamp, value_format: Operation type / Value)
 */
#define OPERATION_TRACKING_KEY_FORMAT WT_UNCHECKED_STRING(SSQ)
#define OPERATION_TRACKING_VALUE_FORMAT WT_UNCHECKED_STRING(iS)
#define OPERATION_TRACKING_TABLE_CONFIG \
    "key_format=" OPERATION_TRACKING_KEY_FORMAT ",value_format=" OPERATION_TRACKING_VALUE_FORMAT

/*
 * Default schema for tracking schema operations on collections (key_format: Collection name /
 * Timestamp, value_format: Operation type)
 */
#define SCHEMA_TRACKING_KEY_FORMAT WT_UNCHECKED_STRING(SQ)
#define SCHEMA_TRACKING_VALUE_FORMAT WT_UNCHECKED_STRING(i)
#define SCHEMA_TRACKING_TABLE_CONFIG \
    "key_format=" SCHEMA_TRACKING_KEY_FORMAT ",value_format=" SCHEMA_TRACKING_VALUE_FORMAT

namespace test_harness {
/* Tracking operations. */
enum class tracking_operation { CREATE_COLLECTION, DELETE_COLLECTION, DELETE_KEY, INSERT, UPDATE };
/* Class used to track operations performed on collections */
class workload_tracking : public component {

    public:
    workload_tracking(configuration *_config, const std::string &operation_table_config,
      const std::string &operation_table_name, const std::string &schema_table_config,
      const std::string &schema_table_name)
        : component("workload_tracking", _config), _operation_table_config(operation_table_config),
          _operation_table_name(operation_table_name), _schema_table_config(schema_table_config),
          _schema_table_name(schema_table_name)
    {
    }

    const std::string &
    get_schema_table_name() const
    {
        return _schema_table_name;
    }

    const std::string &
    get_operation_table_name() const
    {
        return _operation_table_name;
    }

    void
    load() override final
    {
        WT_SESSION *session;

        component::load();

        if (!_enabled)
            return;

        /* Initiate schema tracking. */
        session = connection_manager::instance().create_session();
        testutil_check(
          session->create(session, _schema_table_name.c_str(), _schema_table_config.c_str()));
        testutil_check(session->open_cursor(
          session, _schema_table_name.c_str(), nullptr, nullptr, &_schema_track_cursor));
        debug_print("Schema tracking initiated", DEBUG_TRACE);

        /* Initiate operations tracking. */
        testutil_check(
          session->create(session, _operation_table_name.c_str(), _operation_table_config.c_str()));
        debug_print("Operations tracking created", DEBUG_TRACE);
    }

    void
    run() override final
    {
        /* Does not do anything. */
    }

    void
    save_schema_operation(
      const tracking_operation &operation, const std::string &collection_name, wt_timestamp_t ts)
    {
        std::string error_message;

        if (!_enabled)
            return;

        if (operation == tracking_operation::CREATE_COLLECTION ||
          operation == tracking_operation::DELETE_COLLECTION) {
            _schema_track_cursor->set_key(_schema_track_cursor, collection_name.c_str(), ts);
            _schema_track_cursor->set_value(_schema_track_cursor, static_cast<int>(operation));
            testutil_check(_schema_track_cursor->insert(_schema_track_cursor));
        } else {
            error_message = "save_schema_operation: invalid operation " +
              std::to_string(static_cast<int>(operation));
            testutil_die(EINVAL, error_message.c_str());
        }
        debug_print("save_schema_operation: workload tracking saved operation.", DEBUG_TRACE);
    }

    template <typename K, typename V>
    void
    save_operation(const tracking_operation &operation, const std::string &collection_name,
      const K &key, const V &value, wt_timestamp_t ts, WT_CURSOR *op_track_cursor)
    {
        std::string error_message;

        if (!_enabled)
            return;

        testutil_assert(op_track_cursor != nullptr);

        if (operation == tracking_operation::CREATE_COLLECTION ||
          operation == tracking_operation::DELETE_COLLECTION) {
            error_message =
              "save_operation: invalid operation " + std::to_string(static_cast<int>(operation));
            testutil_die(EINVAL, error_message.c_str());
        } else {
            op_track_cursor->set_key(op_track_cursor, collection_name.c_str(), key, ts);
            op_track_cursor->set_value(op_track_cursor, static_cast<int>(operation), value);
            testutil_check(op_track_cursor->insert(op_track_cursor));
        }
        debug_print("save_operation: workload tracking saved operation.", DEBUG_TRACE);
    }

    private:
    WT_CURSOR *_schema_track_cursor = nullptr;
    const std::string _operation_table_config;
    const std::string _operation_table_name;
    const std::string _schema_table_config;
    const std::string _schema_table_name;
};
} // namespace test_harness

#endif
