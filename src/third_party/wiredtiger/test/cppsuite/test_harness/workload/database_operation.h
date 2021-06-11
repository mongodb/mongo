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

#include <map>
#include <thread>

#include "database_model.h"
#include "workload_tracking.h"
#include "thread_context.h"
#include "random_generator.h"
#include "../thread_manager.h"

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
     *      - Store in memory the created collections.
     */
    virtual void
    populate(database &database, timestamp_manager *tsm, configuration *config,
      workload_tracking *tracking)
    {
        WT_SESSION *session;
        std::vector<std::string> collection_names;
        int64_t collection_count, key_count, key_size, thread_count, value_size;
        std::string collection_name;
        thread_manager tm;

        /* Get a session. */
        session = connection_manager::instance().create_session();

        /* Get our configuration values, validating that they make sense. */
        collection_count = config->get_int(COLLECTION_COUNT);
        key_count = config->get_int(KEY_COUNT_PER_COLLECTION);
        value_size = config->get_int(VALUE_SIZE);
        thread_count = config->get_int(THREAD_COUNT);
        testutil_assert(collection_count % thread_count == 0);
        testutil_assert(value_size > 0);
        key_size = config->get_int(KEY_SIZE);
        testutil_assert(key_size > 0);

        /* Keys must be unique. */
        testutil_assert(key_count <= pow(10, key_size));

        /* Create n collections as per the configuration and store each collection name. */
        for (int64_t i = 0; i < collection_count; ++i) {
            /* FIXME-T-F: Should we just give collection creation power to the database? */
            collection_name = database.add_collection();
            testutil_check(
              session->create(session, collection_name.c_str(), DEFAULT_FRAMEWORK_SCHEMA));
            tracking->save_schema_operation(
              tracking_operation::CREATE_COLLECTION, collection_name, tsm->get_next_ts());
            collection_names.push_back(collection_name);
        }
        debug_print(
          "Populate: " + std::to_string(collection_count) + " collections created.", DEBUG_INFO);

        /*
         * Spawn thread_count threads to populate the database, theoretically we should be IO bound
         * here.
         */
        for (int64_t i = 0; i < thread_count; ++i) {
            int64_t collections_per_thread = collection_count / thread_count;
            std::vector<std::string> thread_collections;
            for (size_t j = i * collections_per_thread;
                 j < i * collections_per_thread + collections_per_thread; j++) {
                debug_print("Populate: adding collection: " + collection_names[j] + " to thread " +
                    std::to_string(i),
                  DEBUG_TRACE);
                thread_collections.push_back(collection_names[j]);
            }
            tm.add_thread(populate_worker, i, thread_collections,
              connection_manager::instance().create_session(), tsm, tracking, key_count, key_size,
              value_size);
        }

        /* Wait for our populate threads to finish and then join them. */
        debug_print("Populate: waiting for threads to complete.", DEBUG_INFO);
        tm.join();

        debug_print("Populate: finished.", DEBUG_INFO);
    }

    /* Basic insert operation that adds a new key every rate tick. */
    virtual void
    insert_operation(thread_context *tc)
    {
        debug_print(type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.",
          DEBUG_INFO);
    }

    /* Basic read operation that walks a cursors across all collections. */
    virtual void
    read_operation(thread_context *tc)
    {
        debug_print(type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.",
          DEBUG_INFO);
        WT_CURSOR *cursor;
        std::vector<WT_CURSOR *> cursors;

        /* Get a cursor for each collection in collection_names. */
        for (const auto &it : tc->database.get_collection_names()) {
            testutil_check(tc->session->open_cursor(tc->session, it.c_str(), NULL, NULL, &cursor));
            cursors.push_back(cursor);
            debug_print("Adding collection to read thread: " + it, DEBUG_TRACE);
        }

        while (tc->running()) {
            /* Walk each cursor. */
            for (const auto &it : cursors) {
                if (it->next(it) != 0)
                    it->reset(it);
            }
            tc->sleep();
        }
    }

    /*
     * Basic update operation that uses a random cursor to update values in a randomly chosen
     * collection.
     */
    virtual void
    update_operation(thread_context *tc)
    {
        debug_print(type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.",
          DEBUG_INFO);

        /* A structure that's used to track which cursors we've opened for which collection. */
        struct collection_cursors {
            const std::string collection_name;
            WT_CURSOR *random_cursor;
            WT_CURSOR *update_cursor;
        };

        WT_DECL_RET;
        wt_timestamp_t ts;
        std::map<uint64_t, collection_cursors> collections;
        key_value_t key, generated_value;
        std::string collection_name;
        const char *key_tmp;
        uint64_t collection_count = 0, collection_id = 0;
        bool using_timestamps = tc->timestamp_manager->enabled();

        /*
         * Loop while the test is running.
         */
        while (tc->running()) {
            /*
             * Sleep the period defined by the op_rate in the configuration. Do this at the start of
             * the loop as it could be skipped by a subsequent continue call.
             */
            tc->sleep();

            /* Pick a random collection to update if there are any, taking care to subtract -1. */
            collection_count = tc->database.get_collection_count();
            if (collection_count == 0)
                continue;

            collection_id =
              random_generator::instance().generate_integer<uint64_t>(0, collection_count - 1);

            if (collections.find(collection_id) == collections.end()) {
                WT_CURSOR *random_cursor = nullptr, *update_cursor = nullptr;
                /* Retrieve the collection name associated with our id. */
                collection_name = std::move(tc->database.get_collection_name(collection_id));
                debug_print("Thread {" + std::to_string(tc->id) +
                    "} Creating cursor for collection: " + collection_name,
                  DEBUG_TRACE);

                /* Open a random cursor for that collection. */
                tc->session->open_cursor(tc->session, collection_name.c_str(), nullptr,
                  "next_random=true", &random_cursor);
                /*
                 * We can't call update on a random cursor so we open two cursors here, one to do
                 * the randomized next and one to subsequently update the key.
                 */
                tc->session->open_cursor(
                  tc->session, collection_name.c_str(), nullptr, nullptr, &update_cursor);

                collections.emplace(
                  collection_id, collection_cursors{collection_name, random_cursor, update_cursor});
            }

            /* Start a transaction. */
            if (!tc->transaction.active())
                tc->transaction.begin(tc->session, "");

            /* Get the random cursor associated with the collection. */
            auto collection = collections[collection_id];
            /* Call next to pick a new random record. */
            ret = collection.random_cursor->next(collection.random_cursor);
            if (ret == WT_NOTFOUND)
                continue;
            else if (ret != 0)
                testutil_die(ret, "unhandled error returned by cursor->next()");

            /* Get the record's key. */
            testutil_check(collection.random_cursor->get_key(collection.random_cursor, &key_tmp));

            /*
             * The retrieved key needs to be passed inside the update function. However, the update
             * API doesn't guarantee our buffer will still be valid once it is called, as such we
             * copy the buffer and then pass it into the API.
             */
            key = key_value_t(key_tmp);

            /* Generate a new value for the record. */
            generated_value =
              random_generator::random_generator::instance().generate_string(tc->value_size);

            /*
             * Get a timestamp to apply to the update. We still do this even if timestamps aren't
             * enabled as it will return WT_TS_NONE, which is then inserted into the tracking table.
             */
            ts = tc->timestamp_manager->get_next_ts();
            if (using_timestamps)
                tc->transaction.set_commit_timestamp(
                  tc->session, timestamp_manager::decimal_to_hex(ts));

            /*
             * Update the record but take care to handle WT_ROLLBACK as we may conflict with another
             * running transaction. Here we call the pre-defined wrappers as they also update the
             * tracking table, which is later used for validation.
             *
             * Additionally first get the update_cursor.
             */
            ret = update(tc->tracking, collection.update_cursor, collection.collection_name,
              key.c_str(), generated_value.c_str(), ts);

            /* Increment the current op count for the current transaction. */
            tc->transaction.op_count++;

            /*
             * If the wiredtiger API has returned rollback, comply. This will need to rollback
             * tracking table operations in the future but currently won't.
             */
            if (ret == WT_ROLLBACK)
                tc->transaction.rollback(tc->session, "");

            /* Commit the current transaction if we're able to. */
            if (tc->transaction.can_commit())
                tc->transaction.commit(tc->session, "");
        }

        /* Make sure the last operation is committed now the work is finished. */
        if (tc->transaction.active())
            tc->transaction.commit(tc->session, "");
    }

    private:
    /* WiredTiger APIs wrappers for single operations. */
    template <typename K, typename V>
    static int
    insert(WT_CURSOR *cursor, workload_tracking *tracking, const std::string &collection_name,
      const K &key, const V &value, wt_timestamp_t ts)
    {
        WT_DECL_RET;
        testutil_assert(cursor != nullptr);

        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);

        ret = cursor->insert(cursor);
        if (ret != 0) {
            if (ret == WT_ROLLBACK)
                return (ret);
            else
                testutil_die(ret, "unhandled error while trying to insert a key.");
        }

        debug_print("key/value inserted", DEBUG_TRACE);
        tracking->save_operation(tracking_operation::INSERT, collection_name, key, value, ts);
        return (0);
    }

    template <typename K, typename V>
    static int
    update(workload_tracking *tracking, WT_CURSOR *cursor, const std::string &collection_name,
      K key, V value, wt_timestamp_t ts)
    {
        WT_DECL_RET;
        testutil_assert(tracking != nullptr);
        testutil_assert(cursor != nullptr);

        cursor->set_key(cursor, key);
        cursor->set_value(cursor, value);

        ret = cursor->update(cursor);
        if (ret != 0) {
            if (ret == WT_ROLLBACK)
                return (ret);
            else
                testutil_die(ret, "unhandled error while trying to update a key");
        }

        debug_print("key/value updated", DEBUG_TRACE);
        tracking->save_operation(tracking_operation::UPDATE, collection_name, key, value, ts);
        return (0);
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

    private:
    static void
    populate_worker(uint64_t worker_id, std::vector<std::string> collections, WT_SESSION *session,
      timestamp_manager *tsm, workload_tracking *tracking, int64_t key_count, int64_t key_size,
      int64_t value_size)
    {
        WT_DECL_RET;
        WT_CURSOR *cursor;
        std::string cfg;
        wt_timestamp_t ts;
        key_value_t generated_key, generated_value;

        for (const auto &next_collection : collections) {
            /*
             * WiredTiger lets you open a cursor on a collection using the same pointer. When a
             * session is closed, WiredTiger APIs close the cursors too.
             */
            testutil_check(
              session->open_cursor(session, next_collection.c_str(), NULL, NULL, &cursor));
            for (uint64_t i = 0; i < key_count; ++i) {
                /* Generation of a unique key. */
                generated_key = number_to_string(key_size, i);
                /*
                 * Generation of a random string value using the size defined in the test
                 * configuration.
                 */
                generated_value =
                  random_generator::random_generator::instance().generate_string(value_size);
                ts = tsm->get_next_ts();

                /* Start a txn. */
                testutil_check(session->begin_transaction(session, nullptr));

                ret = insert(cursor, tracking, next_collection, generated_key.c_str(),
                  generated_value.c_str(), ts);

                /* This may require some sort of "stuck" mechanism but for now is fine. */
                if (ret == WT_ROLLBACK)
                    testutil_die(-1, "Got a rollback in populate, this is currently not handled.");

                if (tsm->enabled())
                    cfg = std::string(COMMIT_TS) + "=" + timestamp_manager::decimal_to_hex(ts);
                else
                    cfg = "";

                testutil_check(session->commit_transaction(session, cfg.c_str()));
            }
        }
        debug_print("Populate: thread {" + std::to_string(worker_id) + "} finished", DEBUG_TRACE);
    }
};
} // namespace test_harness
#endif
