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

#include "test_harness/connection_manager.h"

using namespace test_harness;

/*
 * Here we want to age out entire pages, i.e. the stop time pair on a page should be globally
 * visible. To do so we'll update ranges of keys with increasing timestamps which will age out
 * the pre-existing data. It may not trigger a cleanup on the data file but should result in
 * data getting cleaned up from the history store.
 *
 * This is then tracked using the associated statistic which can be found in the runtime_monitor.
 */
class hs_cleanup : public test {
    public:
    hs_cleanup(const std::string &config, const std::string &name) : test(config, name) {}

    void
    update_operation(thread_context *tc) override final
    {
        WT_CURSOR *cursor = nullptr;
        WT_DECL_RET;
        const char *key_tmp;
        WT_SESSION *session = connection_manager::instance().create_session();
        std::string collection_name = tc->database.get_collection_name(tc->id);
        wt_timestamp_t ts;

        /* In this test each thread gets a single collection. */
        testutil_assert(tc->database.get_collection_count() == tc->thread_count);
        testutil_check(session->open_cursor(session,  collection_name.c_str(), nullptr, nullptr, &cursor));

        /* We don't know the keyrange we're operating over here so we can't be much smarter here. */
        while (tc->running()) {
            tc->sleep();
            ret = cursor->next(cursor);
            if (ret != 0) {
                if (ret == WT_NOTFOUND) {
                    testutil_check(cursor->reset(cursor));
                    continue;
                } else
                    testutil_die(ret, "cursor->next() failed unexpectedly.");
            }
            testutil_check(cursor->get_key(cursor, &key_tmp));

            /* Start a transaction if possible. */
            tc->transaction.try_begin(tc->session, "");

            ts = tc->timestamp_manager->get_next_ts();
            if (tc->timestamp_manager->enabled())
                tc->transaction.set_commit_timestamp(
                  tc->session, timestamp_manager::decimal_to_hex(ts));

            /* Update the record but take care to handle WT_ROLLBACK. */
            ret = update(tc->tracking, cursor, collection_name, key_value_t(key_tmp).c_str(),
             random_generator::instance().generate_string(tc->value_size).c_str(), ts);
            /* Increment the current op count for the current transaction. */
            tc->transaction.op_count++;
            if (ret == WT_ROLLBACK)
                tc->transaction.rollback(tc->session, "");
            else if (ret != 0)
                testutil_die(ret, "failed to update a key.");

            /* Commit our transaction. */
            tc->transaction.try_commit(tc->session, "");
        }
        /* Ensure our last transaction is resolved. */
        if (tc->transaction.active())
            tc->transaction.commit(tc->session, "");
    }
};
