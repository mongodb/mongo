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

#include "test_harness/test.h"
#include "test_harness/util/api_const.h"
#include "test_harness/workload/random_generator.h"

using namespace test_harness;

/*
 * In this test, we want to verify search_near with prefix enabled when performing unique index
 * insertions. For the test duration:
 *  - N thread will perform unique index insertions on existing keys in the table. These insertions
 * are expected to fail.
 *  - M threads will traverse the collections and ensure that the number of records in the
 * collections don't change.
 */
class search_near_03 : public test_harness::test {
    /* A 2D array consisted of a mapping between each collection and their inserted prefixes. */
    std::vector<std::vector<std::string>> prefixes_map;
    const std::string ALPHABET{"abcdefghijklmnopqrstuvwxyz"};

    public:
    search_near_03(const test_harness::test_args &args) : test(args) {}

    /*
     * Here's how we insert an entry into a unique index:
     * 1. Insert the prefix.
     * 2. Remove the prefix.
     * 3. Search near for the prefix. In the case we find a record, we stop here as a value with the
     * prefix already exists in the table. Otherwise if the record is not found, we can proceed to
     * insert the full value.
     * 4. Insert the full value (prefix, id).
     * All of these operations are wrapped in the same transaction.
     */
    static bool
    perform_unique_index_insertions(
      thread_context *tc, scoped_cursor &cursor, collection &coll, std::string &prefix_key)
    {
        std::string ret_key;
        const char *key_tmp;
        int exact_prefix, ret;

        /* Insert the prefix. */
        if (!tc->insert(cursor, coll.id, prefix_key))
            return false;

        /* Remove the prefix. */
        if (!tc->remove(cursor, coll.id, prefix_key))
            return false;

        /*
         * Prefix search near for the prefix. We expect that the prefix is not visible to us and a
         * WT_NOTFOUND error code is returned. If the prefix is present it means the (prefix, id)
         * has been inserted already. Double check that the prefix potion matches.
         */
        cursor->set_key(cursor.get(), prefix_key.c_str());
        ret = cursor->search_near(cursor.get(), &exact_prefix);
        testutil_assert(ret == 0 || ret == WT_NOTFOUND);
        if (ret == 0) {
            cursor->get_key(cursor.get(), &key_tmp);
            ret_key = get_prefix_from_key(std::string(key_tmp));
            testutil_assert(exact_prefix == 1);
            testutil_assert(prefix_key == ret_key);
            return false;
        }

        /* Now insert the key with prefix and id. Use thread id to guarantee uniqueness. */
        return tc->insert(cursor, coll.id, prefix_key + "," + std::to_string(tc->id));
    }

    static void
    populate_worker(thread_context *tc)
    {
        logger::log_msg(LOG_INFO, "Populate with thread id: " + std::to_string(tc->id));

        std::string prefix_key;
        const uint64_t MAX_ROLLBACKS = 100;
        uint32_t rollback_retries = 0;

        /*
         * Each populate thread perform unique index insertions on each collection, with a randomly
         * generated prefix and thread id.
         */
        collection &coll = tc->db.get_collection(tc->id);
        scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);
        cursor->reconfigure(cursor.get(), "prefix_search=true");
        for (uint64_t count = 0; count < tc->key_count; ++count) {
            tc->transaction.begin();
            /*
             * Generate the prefix key, and append a random generated key string based on the key
             * size configuration.
             */
            prefix_key = random_generator::instance().generate_random_string(tc->key_size);
            if (perform_unique_index_insertions(tc, cursor, coll, prefix_key)) {
                tc->transaction.commit();
            } else {
                tc->transaction.rollback();
                ++rollback_retries;
                if (count > 0)
                    --count;
            }
            testutil_assert(rollback_retries < MAX_ROLLBACKS);
        }
    }

    static const std::string
    get_prefix_from_key(const std::string &s)
    {
        const std::string::size_type pos = s.find(',');
        return pos != std::string::npos ? s.substr(0, pos) : nullptr;
    }

    void
    populate(test_harness::database &database, test_harness::timestamp_manager *tsm,
      test_harness::configuration *config, test_harness::workload_tracking *tracking) override final
    {
        uint64_t collection_count, key_count, key_size;
        std::vector<thread_context *> workers;
        thread_manager tm;

        /* Validate our config. */
        collection_count = config->get_int(COLLECTION_COUNT);
        key_count = config->get_int(KEY_COUNT_PER_COLLECTION);
        key_size = config->get_int(KEY_SIZE);
        testutil_assert(collection_count > 0);
        testutil_assert(key_count > 0);
        testutil_assert(key_size > 0);

        logger::log_msg(LOG_INFO,
          "Populate configuration with " + std::to_string(collection_count) +
            "collections, number of keys: " + std::to_string(key_count) +
            ", key size: " + std::to_string(key_size));

        /* Create n collections as per the configuration. */
        for (uint64_t i = 0; i < collection_count; ++i)
            /*
             * The database model will call into the API and create the collection, with its own
             * session.
             */
            database.add_collection();

        /* Spawn a populate thread for each collection in the database. */
        for (uint64_t i = 0; i < collection_count; ++i) {
            thread_context *tc = new thread_context(i, thread_type::INSERT, config,
              connection_manager::instance().create_session(), tsm, tracking, database);
            workers.push_back(tc);
            tm.add_thread(populate_worker, tc);
        }

        /* Wait for our populate threads to finish and then join them. */
        logger::log_msg(LOG_INFO, "Populate: waiting for threads to complete.");
        tm.join();

        /* Cleanup our workers. */
        for (auto &it : workers) {
            delete it;
            it = nullptr;
        }

        /*
         * Construct a mapping of all the inserted prefixes to their respective collections. We
         * traverse through each collection using a cursor to collect the prefix and push it into a
         * 2D vector.
         */
        scoped_session session = connection_manager::instance().create_session();
        const char *key_tmp;
        int ret = 0;
        for (uint64_t i = 0; i < database.get_collection_count(); i++) {
            collection &coll = database.get_collection(i);
            scoped_cursor cursor = session.open_scoped_cursor(coll.name);
            std::vector<std::string> prefixes;
            ret = 0;
            while (ret == 0) {
                ret = cursor->next(cursor.get());
                testutil_assertfmt(ret == 0 || ret == WT_NOTFOUND,
                  "Unexpected error %d returned from cursor->next()", ret);
                if (ret == WT_NOTFOUND)
                    continue;
                cursor->get_key(cursor.get(), &key_tmp);
                prefixes.push_back(std::string(key_tmp));
            }
            prefixes_map.push_back(prefixes);
        }
        logger::log_msg(LOG_INFO, "Populate: finished.");
    }

    void
    insert_operation(test_harness::thread_context *tc) override final
    {
        std::map<uint64_t, scoped_cursor> cursors;
        std::string prefix_key;
        size_t random_index;

        /*
         * Each insert operation will attempt to perform unique index insertions with an existing
         * prefix on a collection.
         */
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

        while (tc->running()) {
            /* Get a collection and find a cached cursor. */
            collection &coll = tc->db.get_random_collection();
            if (cursors.find(coll.id) == cursors.end()) {
                scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);
                cursor->reconfigure(cursor.get(), "prefix_search=true");
                cursors.emplace(coll.id, std::move(cursor));
            }

            /* Do a second lookup now that we know it exists. */
            auto &cursor = cursors[coll.id];
            tc->transaction.begin();
            /*
             * Grab a random existing prefix and perform unique index insertion. We expect it to
             * fail to insert, because it should already exist.
             */
            random_index = random_generator::instance().generate_integer(
              static_cast<size_t>(0), prefixes_map.at(coll.id).size() - 1);
            prefix_key = get_prefix_from_key(prefixes_map.at(coll.id).at(random_index));
            logger::log_msg(LOG_INFO,
              type_string(tc->type) +
                " thread: Perform unique index insertions with existing prefix key " + prefix_key +
                ".");
            testutil_assert(!perform_unique_index_insertions(tc, cursor, coll, prefix_key));
            testutil_check(cursor->reset(cursor.get()));
            tc->transaction.rollback();
        }
    }

    void
    read_operation(test_harness::thread_context *tc) override final
    {
        uint64_t key_count = 0;
        int ret = 0;
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");
        /*
         * Each read thread will count the number of keys in each collection, and will double check
         * if the size of the table hasn't changed.
         */
        tc->transaction.begin();
        while (tc->running()) {
            for (int i = 0; i < tc->db.get_collection_count(); i++) {
                collection &coll = tc->db.get_collection(i);
                scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);
                ret = 0;
                while (ret == 0) {
                    ret = cursor->next(cursor.get());
                    testutil_assertfmt(ret == 0 || ret == WT_NOTFOUND,
                      "Unexpected error %d returned from cursor->next()", ret);
                    if (ret == WT_NOTFOUND)
                        continue;
                    key_count++;
                }
                tc->sleep();
            }
            if (tc->running()) {
                logger::log_msg(LOG_INFO,
                  type_string(tc->type) +
                    " thread: calculated count: " + std::to_string(key_count) + " expected size: " +
                    std::to_string(prefixes_map.size() * prefixes_map.at(0).size()));
                testutil_assert(key_count == prefixes_map.size() * prefixes_map.at(0).size());
            }
            key_count = 0;
        }
        tc->transaction.rollback();
    }
};
