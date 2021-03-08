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
 * Default schema for tracking table key_format : Collection name / Key value_format : Operation
 * type / Value / Timestamp
 */
#define DEFAULT_TRACKING_KEY_FORMAT WT_UNCHECKED_STRING(Si)
#define DEFAULT_TRACKING_VALUE_FORMAT WT_UNCHECKED_STRING(iSi)
#define DEFAULT_TRACKING_TABLE_SCHEMA \
    "key_format=" DEFAULT_TRACKING_KEY_FORMAT ",value_format=" DEFAULT_TRACKING_VALUE_FORMAT

namespace test_harness {
/* Tracking operations. */
enum class tracking_operation { CREATE, INSERT };
/* Class used to track operations performed on collections */
class workload_tracking {

    public:
    workload_tracking(const std::string &collection_name)
        : _collection_name(collection_name), _timestamp(0U)
    {
    }

    int
    load(const std::string &table_schema = DEFAULT_TRACKING_TABLE_SCHEMA)
    {
        WT_SESSION *session;

        /* Create tracking collection. */
        session = connection_manager::instance().create_session();
        testutil_check(session->create(session, _collection_name.c_str(), table_schema.c_str()));
        testutil_check(
          session->open_cursor(session, _collection_name.c_str(), NULL, NULL, &_cursor));
        debug_info("Tracking collection created", _trace_level, DEBUG_INFO);

        return (0);
    }

    template <typename K, typename V>
    int
    save(const tracking_operation &operation, const std::string &collection_name, const K &key,
      const V &value)
    {
        int error_code;

        _cursor->set_key(_cursor, collection_name.c_str(), key);
        _cursor->set_value(_cursor, static_cast<int>(operation), value, _timestamp++);
        error_code = _cursor->insert(_cursor);

        if (error_code == 0) {
            debug_info("Workload tracking saved operation.", _trace_level, DEBUG_INFO);
        } else {
            debug_info("Workload tracking failed to save operation !", _trace_level, DEBUG_ERROR);
        }

        return error_code;
    }

    private:
    const std::string _collection_name;
    WT_CURSOR *_cursor = nullptr;
    uint64_t _timestamp;
};
} // namespace test_harness

#endif
