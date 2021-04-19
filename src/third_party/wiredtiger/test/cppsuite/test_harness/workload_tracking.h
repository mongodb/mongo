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
enum class tracking_operation { CREATE, DELETE_COLLECTION, DELETE_KEY, INSERT, UPDATE };
/* Class used to track operations performed on collections */
class workload_tracking : public component {

    public:
    workload_tracking(configuration *_config, const std::string &operation_table_config,
      const std::string &operation_table_name, const std::string &schema_table_config,
      const std::string &schema_table_name)
        : component(_config), _cursor_operations(nullptr), _cursor_schema(nullptr),
          _operation_table_config(operation_table_config),
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
    load()
    {
        WT_SESSION *session;

        testutil_check(_config->get_bool(ENABLED, _enabled));
        if (!_enabled)
            return;

        /* Initiate schema tracking. */
        session = connection_manager::instance().create_session();
        testutil_check(
          session->create(session, _schema_table_name.c_str(), _schema_table_config.c_str()));
        testutil_check(
          session->open_cursor(session, _schema_table_name.c_str(), NULL, NULL, &_cursor_schema));
        debug_print("Schema tracking initiated", DEBUG_TRACE);

        /* Initiate operations tracking. */
        testutil_check(
          session->create(session, _operation_table_name.c_str(), _operation_table_config.c_str()));
        testutil_check(session->open_cursor(
          session, _operation_table_name.c_str(), NULL, NULL, &_cursor_operations));
        debug_print("Operations tracking created", DEBUG_TRACE);
    }

    void
    run()
    {
        /* Does not do anything. */
    }

    template <typename K, typename V>
    int
    save(const tracking_operation &operation, const std::string &collection_name, const K &key,
      const V &value, wt_timestamp_t ts)
    {
        WT_CURSOR *cursor;
        int error_code = 0;

        if (!_enabled)
            return (error_code);

        /* Select the correct cursor to save in the collection associated to specific operations. */
        switch (operation) {
        case tracking_operation::CREATE:
        case tracking_operation::DELETE_COLLECTION:
            cursor = _cursor_schema;
            cursor->set_key(cursor, collection_name.c_str(), ts);
            cursor->set_value(cursor, static_cast<int>(operation));
            break;

        default:
            cursor = _cursor_operations;
            cursor->set_key(cursor, collection_name.c_str(), key, ts);
            cursor->set_value(cursor, static_cast<int>(operation), value);
            break;
        }

        error_code = cursor->insert(cursor);

        if (error_code == 0)
            debug_print("Workload tracking saved operation.", DEBUG_TRACE);
        else
            debug_print("Workload tracking failed to save operation !", DEBUG_ERROR);

        return error_code;
    }

    private:
    WT_CURSOR *_cursor_operations;
    WT_CURSOR *_cursor_schema;
    const std::string _operation_table_config;
    const std::string _operation_table_name;
    const std::string _schema_table_config;
    const std::string _schema_table_name;
};
} // namespace test_harness

#endif
