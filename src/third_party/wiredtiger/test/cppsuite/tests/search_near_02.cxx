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
 * In this test, we want to verify search_near with prefix enabled returns the correct key.
 * During the test duration:
 *  - N threads will keep inserting new random keys
 *  - M threads will execute search_near calls with prefix enabled using random prefixes as well.
 * Each search_near call with prefix enabled is verified using the default search_near.
 */
class search_near_02 : public test_harness::test {
    public:
    search_near_02(const test_harness::test_args &args) : test(args) {}

    void
    populate(test_harness::database &database, test_harness::timestamp_manager *,
      test_harness::configuration *config, test_harness::workload_tracking *) override final
    {
        /*
         * The populate phase only creates empty collections. The number of collections is defined
         * in the configuration.
         */
        int64_t collection_count = config->get_int(COLLECTION_COUNT);

        logger::log_msg(
          LOG_INFO, "Populate: " + std::to_string(collection_count) + " creating collections.");

        for (uint64_t i = 0; i < collection_count; ++i)
            database.add_collection();

        logger::log_msg(LOG_INFO, "Populate: finished.");
    }

    void
    insert_operation(test_harness::thread_context *tc) override final
    {
        /* Each insert operation will insert new keys in the collections. */
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

        /* Helper struct which stores a pointer to a collection and a cursor associated with it. */
        struct collection_cursor {
            collection_cursor(collection &coll, scoped_cursor &&cursor)
                : coll(coll), cursor(std::move(cursor))
            {
            }
            collection &coll;
            scoped_cursor cursor;
        };

        /* Collection cursor vector. */
        std::vector<collection_cursor> ccv;
        int64_t collection_count = tc->db.get_collection_count();
        int64_t collections_per_thread = collection_count / tc->thread_count;

        /* Must have unique collections for each thread. */
        testutil_assert(collection_count % tc->thread_count == 0);
        const uint64_t thread_offset = tc->id * collections_per_thread;
        for (uint64_t i = thread_offset;
             i < thread_offset + collections_per_thread && tc->running(); ++i) {
            collection &coll = tc->db.get_collection(i);
            scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name.c_str());
            ccv.push_back({coll, std::move(cursor)});
        }

        std::string key;
        const uint64_t MAX_ROLLBACKS = 100;
        uint64_t counter = 0;
        uint32_t rollback_retries = 0;

        while (tc->running()) {

            auto &cc = ccv[counter];
            tc->transaction.begin();

            while (tc->transaction.active() && tc->running()) {

                /* Generate a random key. */
                key = random_generator::instance().generate_random_string(tc->key_size);

                /* Insert a key value pair. */
                if (tc->insert(cc.cursor, cc.coll.id, key)) {
                    if (tc->transaction.can_commit()) {
                        /* We are not checking the result of commit as it is not necessary. */
                        if (tc->transaction.commit())
                            rollback_retries = 0;
                        else
                            ++rollback_retries;
                    }
                } else {
                    tc->transaction.rollback();
                    ++rollback_retries;
                }
                testutil_assert(rollback_retries < MAX_ROLLBACKS);

                /* Sleep the duration defined by the configuration. */
                tc->sleep();
            }

            /* Rollback any transaction that could not commit before the end of the test. */
            if (tc->transaction.active())
                tc->transaction.rollback();

            /* Reset our cursor to avoid pinning content. */
            testutil_check(cc.cursor->reset(cc.cursor.get()));
            if (++counter == ccv.size())
                counter = 0;
            testutil_assert(counter < collections_per_thread);
        }
    }

    void
    read_operation(test_harness::thread_context *tc) override final
    {
        /*
         * Each read operation performs search_near calls with and without prefix enabled on random
         * collections. Each prefix is randomly generated. The result of the seach_near call with
         * prefix enabled is then validated using the search_near call without prefix enabled.
         */
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

        const char *key_prefix;
        int exact_prefix, ret;
        int64_t prefix_size;
        std::map<uint64_t, scoped_cursor> cursors;
        std::string generated_prefix, key_prefix_str;

        while (tc->running()) {
            /* Get a random collection to work on. */
            collection &coll = tc->db.get_random_collection();

            /* Find a cached cursor or create one if none exists. */
            if (cursors.find(coll.id) == cursors.end()) {
                cursors.emplace(
                  coll.id, std::move(tc->session.open_scoped_cursor(coll.name.c_str())));
                auto &cursor_prefix = cursors[coll.id];
                /* The cached cursors have the prefix configuration enabled. */
                testutil_check(
                  cursor_prefix.get()->reconfigure(cursor_prefix.get(), "prefix_search=true"));
            }

            auto &cursor_prefix = cursors[coll.id];

            /*
             * Pick a random timestamp between the oldest and now. Get rid of the last 32 bits as
             * they represent an increment for uniqueness.
             */
            wt_timestamp_t ts = random_generator::instance().generate_integer(
              (tc->tsm->get_oldest_ts() >> 32), (tc->tsm->get_next_ts() >> 32));
            /* Put back the timestamp in the correct format. */
            ts <<= 32;

            /*
             * The oldest timestamp might move ahead and the reading timestamp might become invalid.
             * To tackle this issue, we round the timestamp to the oldest timestamp value.
             */
            tc->transaction.begin(
              "roundup_timestamps=(read=true),read_timestamp=" + tc->tsm->decimal_to_hex(ts));

            while (tc->transaction.active() && tc->running()) {
                /*
                 * Generate a random prefix. For this, we start by generating a random size and then
                 * its value.
                 */
                prefix_size = random_generator::instance().generate_integer(
                  static_cast<int64_t>(1), tc->key_size);
                generated_prefix = random_generator::instance().generate_random_string(
                  prefix_size, characters_type::ALPHABET);

                /* Call search near with the prefix cursor. */
                cursor_prefix->set_key(cursor_prefix.get(), generated_prefix.c_str());
                ret = cursor_prefix->search_near(cursor_prefix.get(), &exact_prefix);
                testutil_assert(ret == 0 || ret == WT_NOTFOUND);
                if (ret == 0) {
                    testutil_check(cursor_prefix->get_key(cursor_prefix.get(), &key_prefix));
                    key_prefix_str = key_prefix;
                } else {
                    key_prefix_str = "";
                }

                /* Open a cursor with the default configuration on the selected collection. */
                scoped_cursor cursor_default(tc->session.open_scoped_cursor(coll.name.c_str()));

                /* Verify the prefix search_near output using the default cursor. */
                validate_prefix_search_near(
                  ret, exact_prefix, key_prefix_str, cursor_default, generated_prefix);

                tc->transaction.add_op();
                tc->transaction.try_rollback();
                tc->sleep();
            }
            testutil_check(cursor_prefix->reset(cursor_prefix.get()));
        }
        /* Roll back the last transaction if still active now the work is finished. */
        if (tc->transaction.active())
            tc->transaction.rollback();
    }

    private:
    /* Validate prefix search_near call outputs using a cursor without prefix key enabled. */
    void
    validate_prefix_search_near(int ret_prefix, int exact_prefix, const std::string &key_prefix,
      scoped_cursor &cursor_default, const std::string &prefix)
    {
        /* Call search near with the default cursor using the given prefix. */
        int exact_default;
        cursor_default->set_key(cursor_default.get(), prefix.c_str());
        int ret_default = cursor_default->search_near(cursor_default.get(), &exact_default);

        /*
         * It is not possible to have a prefix search near call successful and the default search
         * near call unsuccessful.
         */
        testutil_assert(
          ret_default == ret_prefix || (ret_default == 0 && ret_prefix == WT_NOTFOUND));

        /* We only have to perform validation when the default search near call is successful. */
        if (ret_default == 0) {
            /* Both calls are successful. */
            if (ret_prefix == 0)
                validate_successful_calls(
                  ret_prefix, exact_prefix, key_prefix, cursor_default, exact_default, prefix);
            /* The prefix search near call failed. */
            else
                validate_unsuccessful_prefix_call(cursor_default, prefix, exact_default);
        }
    }

    /*
     * Validate a successful prefix enabled search near call using a successful default search near
     * call.
     * The exact value set by the prefix search near call has to be either 0 or 1. Indeed, it cannot
     * be -1 as the key needs to contain the prefix.
     * - If it is 0, both search near calls should return the same outputs and both cursors should
     * be positioned on the prefix we are looking for.
     * - If it is 1, it will depend on the exact value set by the default search near call which can
     * be -1 or 1. If it is -1, calling next on the default cursor should get us ti the key found by
     * the prefix search near call. If it is 1, it means both search near calls have found the same
     * key that is lexicographically greater than the prefix but still contains the prefix.
     */
    void
    validate_successful_calls(int ret_prefix, int exact_prefix, const std::string &key_prefix,
      scoped_cursor &cursor_default, int exact_default, const std::string &prefix)
    {
        const char *k;
        std::string k_str;

        /*
         * The prefix search near call cannot retrieve a key with a smaller value than the prefix we
         * searched.
         */
        testutil_assert(exact_prefix >= 0);

        /* The key at the prefix cursor should contain the prefix. */
        testutil_assert(key_prefix.substr(0, prefix.size()) == prefix);

        /* Retrieve the key the default cursor is pointing at. */
        const char *key_default;
        testutil_check(cursor_default->get_key(cursor_default.get(), &key_default));
        std::string key_default_str = key_default;

        logger::log_msg(LOG_TRACE,
          "search_near (normal) exact " + std::to_string(exact_default) + " key " + key_default);
        logger::log_msg(LOG_TRACE,
          "search_near (prefix) exact " + std::to_string(exact_prefix) + " key " + key_prefix);

        /* Example: */
        /* keys: a, bb, bba. */
        /* Only bb is not visible. */
        /* Default search_near(bb) returns a, exact < 0. */
        /* Prefix search_near(bb) returns bba, exact > 0. */
        if (exact_default < 0) {
            /* The key at the default cursor should not contain the prefix. */
            testutil_assert((key_default_str.substr(0, prefix.size()) != prefix));

            /*
             * The prefix cursor should be positioned at a key lexicographically greater than the
             * prefix.
             */
            testutil_assert(exact_prefix > 0);

            /*
             * The next key of the default cursor should be equal to the key pointed by the prefix
             * cursor.
             */
            testutil_assert(cursor_default->next(cursor_default.get()) == 0);
            testutil_check(cursor_default->get_key(cursor_default.get(), &k));
            testutil_assert(k == key_prefix);
        }
        /* Example: */
        /* keys: a, bb, bba */
        /* Case 1: all keys are visible. */
        /* Default search_near(bb) returns bb, exact = 0 */
        /* Prefix search_near(bb) returns bb, exact = 0 */
        /* Case 2: only bb is not visible. */
        /* Default search_near(bb) returns bba, exact > 0. */
        /* Prefix search_near(bb) returns bba, exact > 0. */
        else {
            /* Both cursors should be pointing at the same key. */
            testutil_assert(exact_prefix == exact_default);
            testutil_assert(key_default_str == key_prefix);
            /* Both cursors should have found the exact key. */
            if (exact_default == 0)
                testutil_assert(key_default_str == prefix);
            /* Both cursors have found a key that is lexicographically greater than the prefix. */
            else
                testutil_assert(key_default_str != prefix);
        }
    }

    /*
     * Validate that no keys with the prefix used for the search have been found.
     * To validate this, we can use the exact value set by the default search near. Since the prefix
     * search near failed, the exact value set by the default search near call has to be either -1
     * or 1:
     * - If it is -1, we need to check the next key, if it exists, is lexicographically greater than
     * the prefix we looked for.
     * - If it is 1, we need to check the previous keys, if it exists, if lexicographically smaller
     * than the prefix we looked for.
     */
    void
    validate_unsuccessful_prefix_call(
      scoped_cursor &cursor_default, const std::string &prefix, int exact_default)
    {
        int ret;
        const char *k;
        std::string k_str;

        /*
         * The exact value from the default search near call cannot be 0, otherwise the prefix
         * search near should be successful too.
         */
        testutil_assert(exact_default != 0);

        /* Retrieve the key at the default cursor. */
        const char *key_default;
        testutil_check(cursor_default->get_key(cursor_default.get(), &key_default));
        std::string key_default_str = key_default;

        /* The key at the default cursor should not contain the prefix. */
        testutil_assert(key_default_str.substr(0, prefix.size()) != prefix);

        /* Example: */
        /* keys: a, bb, bbb. */
        /* All keys are visible. */
        /* Default search_near(bba) returns bb, exact < 0. */
        /* Prefix search_near(bba) returns WT_NOTFOUND. */
        if (exact_default < 0) {
            /*
             * The current key of the default cursor should be lexicographically smaller than the
             * prefix.
             */
            testutil_assert(std::lexicographical_compare(
              key_default_str.begin(), key_default_str.end(), prefix.begin(), prefix.end()));

            /*
             * The next key of the default cursor should be lexicographically greater than the
             * prefix if it exists.
             */
            ret = cursor_default->next(cursor_default.get());
            if (ret == 0) {
                testutil_check(cursor_default->get_key(cursor_default.get(), &k));
                k_str = k;
                testutil_assert(!std::lexicographical_compare(
                  k_str.begin(), k_str.end(), prefix.begin(), prefix.end()));
            } else {
                /* End of the table. */
                testutil_assert(ret == WT_NOTFOUND);
            }
        }
        /* Example: */
        /* keys: a, bb, bbb. */
        /* All keys are visible. */
        /* Default search_near(bba) returns bbb, exact > 0. */
        /* Prefix search_near(bba) returns WT_NOTFOUND. */
        else {
            /*
             * The current key of the default cursor should be lexicographically greater than the
             * prefix.
             */
            testutil_assert(!std::lexicographical_compare(
              key_default_str.begin(), key_default_str.end(), prefix.begin(), prefix.end()));

            /*
             * The next key of the default cursor should be lexicographically smaller than the
             * prefix if it exists.
             */
            ret = cursor_default->prev(cursor_default.get());
            if (ret == 0) {
                testutil_check(cursor_default->get_key(cursor_default.get(), &k));
                k_str = k;
                testutil_assert(std::lexicographical_compare(
                  k_str.begin(), k_str.end(), prefix.begin(), prefix.end()));
            } else {
                /* End of the table. */
                testutil_assert(ret == WT_NOTFOUND);
            }
        }
    }
};
