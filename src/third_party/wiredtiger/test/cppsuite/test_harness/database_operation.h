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

#ifndef DATABASE_OPERATION_H
#define DATABASE_OPERATION_H

#include "database_model.h"
#include "workload_tracking.h"
#include "thread_context.h"

namespace test_harness {
class database_operation {
    public:
    /*
     * Function that performs the following steps using the configuration that is defined by the
     * test:
     *  - Create the working dir.
     *  - Open a connection.
     *  - Open a session.
     *  - Create n collections as per the configuration.
     *      - Open a cursor on each collection.
     *      - Insert m key/value pairs in each collection. Values are random strings which size is
     * defined by the configuration.
     *      - Store in memory the created collections and the generated keys that were inserted.
     */
    virtual void
    populate(database &database, timestamp_manager *_timestamp_manager, configuration *_config,
      workload_tracking *tracking)
    {
        WT_CURSOR *cursor;
        WT_SESSION *session;
        wt_timestamp_t ts;
        int64_t collection_count, key_count, key_cpt, key_size, value_size;
        std::string collection_name, config, home;
        key_value_t generated_key, generated_value;
        bool ts_enabled = _timestamp_manager->is_enabled();

        cursor = nullptr;
        collection_count = key_count = key_size = value_size = 0;

        /* Get a session. */
        session = connection_manager::instance().create_session();
        /* Create n collections as per the configuration and store each collection name. */
        testutil_check(_config->get_int(COLLECTION_COUNT, collection_count));
        for (int i = 0; i < collection_count; ++i) {
            collection_name = "table:collection" + std::to_string(i);
            database.collections[collection_name] = {};
            testutil_check(
              session->create(session, collection_name.c_str(), DEFAULT_FRAMEWORK_SCHEMA));
            ts = _timestamp_manager->get_next_ts();
            testutil_check(tracking->save(tracking_operation::CREATE, collection_name, 0, "", ts));
        }
        debug_print(std::to_string(collection_count) + " collections created", DEBUG_TRACE);

        /* Open a cursor on each collection and use the configuration to insert key/value pairs. */
        testutil_check(_config->get_int(KEY_COUNT, key_count));
        testutil_check(_config->get_int(VALUE_SIZE, value_size));
        testutil_assert(value_size > 0);
        testutil_check(_config->get_int(KEY_SIZE, key_size));
        testutil_assert(key_size > 0);
        /* Keys must be unique. */
        testutil_assert(key_count <= pow(10, key_size));

        for (const auto &it_collections : database.collections) {
            collection_name = it_collections.first;
            key_cpt = 0;
            /* WiredTiger lets you open a cursor on a collection using the same pointer. When a
             * session is closed, WiredTiger APIs close the cursors too. */
            testutil_check(
              session->open_cursor(session, collection_name.c_str(), NULL, NULL, &cursor));
            for (size_t j = 0; j < key_count; ++j) {
                /* Generation of a unique key. */
                generated_key = number_to_string(key_size, key_cpt);
                ++key_cpt;
                /*
                 * Generation of a random string value using the size defined in the test
                 * configuration.
                 */
                generated_value =
                  random_generator::random_generator::instance().generate_string(value_size);
                ts = _timestamp_manager->get_next_ts();
                if (ts_enabled)
                    testutil_check(session->begin_transaction(session, ""));
                testutil_check(insert(cursor, tracking, collection_name, generated_key.c_str(),
                  generated_value.c_str(), ts));
                if (ts_enabled) {
                    config = std::string(COMMIT_TS) + "=" + _timestamp_manager->decimal_to_hex(ts);
                    testutil_check(session->commit_transaction(session, config.c_str()));
                }
                /* Update the memory representation of the collections. */
                database.collections[collection_name].keys[generated_key].exists = true;
                /* Values are not stored here. */
                database.collections[collection_name].values = nullptr;
            }
        }
        debug_print("Populate stage done", DEBUG_TRACE);
    }

    /* Basic read operation that walks a cursors across all collections. */
    virtual void
    read_operation(thread_context &context, WT_SESSION *session)
    {
        WT_CURSOR *cursor;
        std::vector<WT_CURSOR *> cursors;

        testutil_assert(session != nullptr);
        /* Get a cursor for each collection in collection_names. */
        for (const auto &it : context.get_collection_names()) {
            testutil_check(session->open_cursor(session, it.c_str(), NULL, NULL, &cursor));
            cursors.push_back(cursor);
        }

        while (!cursors.empty() && context.is_running()) {
            /* Walk each cursor. */
            for (const auto &it : cursors) {
                if (it->next(it) != 0)
                    it->reset(it);
            }
        }
    }

    /*
     * Basic update operation that updates all the keys to a random value in each collection.
     */
    virtual void
    update_operation(thread_context &context, WT_SESSION *session)
    {
        WT_CURSOR *cursor;
        wt_timestamp_t ts;
        std::vector<WT_CURSOR *> cursors;
        std::string collection_name;
        std::vector<std::string> collection_names = context.get_collection_names();
        key_value_t generated_value, key;
        int64_t cpt, value_size = context.get_value_size();

        testutil_assert(session != nullptr);
        /* Get a cursor for each collection in collection_names. */
        for (const auto &it : collection_names) {
            testutil_check(session->open_cursor(session, it.c_str(), NULL, NULL, &cursor));
            cursors.push_back(cursor);
        }

        cpt = 0;
        /* Walk each cursor. */
        for (const auto &it : cursors) {
            collection_name = collection_names[cpt];
            /* Walk each key. */
            for (keys_iterator_t iter_key = context.get_collection_keys_begin(collection_name);
                 iter_key != context.get_collection_keys_end(collection_name); ++iter_key) {
                /* Do not process removed keys. */
                if (!iter_key->second.exists)
                    continue;

                /* Start a transaction if possible. */
                if (!context.is_in_transaction()) {
                    context.begin_transaction(session, "");
                    ts = context.set_commit_timestamp(session);
                }
                generated_value =
                  random_generator::random_generator::instance().generate_string(value_size);
                testutil_check(update(context.get_tracking(), it, collection_name,
                  iter_key->first.c_str(), generated_value.c_str(), ts));

                /* Commit the current transaction if possible. */
                context.increment_operation_count();
                if (context.can_commit_transaction())
                    context.commit_transaction(session, "");
            }
            ++cpt;
        }

        /*
         * The update operations will be later on inside a loop that will be managed through
         * throttle management.
         */
        while (context.is_running())
            std::this_thread::sleep_for(std::chrono::seconds(1));

        /* Make sure the last operation is committed now the work is finished. */
        if (context.is_in_transaction())
            context.commit_transaction(session, "");
    }

    private:
    /* WiredTiger APIs wrappers for single operations. */
    template <typename K, typename V>
    int
    insert(WT_CURSOR *cursor, workload_tracking *tracking, const std::string &collection_name,
      const K &key, const V &value, wt_timestamp_t ts)
    {
        int error_code;

        testutil_assert(cursor != nullptr);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        error_code = cursor->insert(cursor);

        if (error_code == 0) {
            debug_print("key/value inserted", DEBUG_TRACE);
            error_code =
              tracking->save(tracking_operation::INSERT, collection_name, key, value, ts);
        } else
            debug_print("key/value insertion failed", DEBUG_ERROR);

        return (error_code);
    }

    template <typename K, typename V>
    static int
    update(workload_tracking *tracking, WT_CURSOR *cursor, const std::string &collection_name,
      K key, V value, wt_timestamp_t ts)
    {
        int error_code;

        testutil_assert(tracking != nullptr);
        testutil_assert(cursor != nullptr);
        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);
        error_code = cursor->update(cursor);

        if (error_code == 0) {
            debug_print("key/value update", DEBUG_TRACE);
            error_code =
              tracking->save(tracking_operation::UPDATE, collection_name, key, value, ts);
        } else
            debug_print("key/value update failed", DEBUG_ERROR);

        return (error_code);
    }

    /*
     * Convert a number to a string. If the resulting string is less than the given length, padding
     * of '0' is added.
     */
    static std::string
    number_to_string(uint64_t size, uint64_t value)
    {
        std::string str, value_str = std::to_string(value);
        testutil_assert(size >= value_str.size());
        uint64_t diff = size - value_str.size();
        std::string s(diff, '0');
        str = s.append(value_str);
        return (str);
    }
};
} // namespace test_harness
#endif
