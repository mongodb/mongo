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
 * visible. To do so we'll update ranges of keys with increasing timestamps which will age out the
 * pre-existing data. It may not trigger a cleanup on the data file but should result in data
 * getting cleaned up from the history store.
 *
 * This is then tracked using the associated statistic which can be found in the runtime_monitor.
 */
class hs_cleanup : public test {
    public:
    hs_cleanup(const test_args &args) : test(args) {}

    void
    update_operation(thread_context *tc) override final
    {
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

        const char *key_tmp;
        const uint64_t MAX_ROLLBACKS = 100;
        uint32_t rollback_retries = 0;

        collection &coll = tc->db.get_collection(tc->id);

        /* In this test each thread gets a single collection. */
        testutil_assert(tc->db.get_collection_count() == tc->thread_count);
        scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);

        /* We don't know the keyrange we're operating over here so we can't be much smarter here. */
        while (tc->running()) {
            tc->sleep();

            auto ret = cursor->next(cursor.get());
            if (ret != 0) {
                if (ret == WT_NOTFOUND) {
                    cursor->reset(cursor.get());
                    continue;
                }
                if (ret == WT_ROLLBACK) {
                    /*
                     * As a result of the logic in this test its possible that the previous next
                     * call can happen outside the context of a transaction. Assert that we are in
                     * one if we got a rollback.
                     */
                    testutil_check(tc->transaction.can_rollback());
                    tc->transaction.rollback();
                    continue;
                }
                testutil_die(ret, "Unexpected error returned from cursor->next()");
            }

            testutil_check(cursor->get_key(cursor.get(), &key_tmp));

            /* Start a transaction if possible. */
            tc->transaction.try_begin();

            /*
             * The retrieved key needs to be passed inside the update function. However, the update
             * API doesn't guarantee our buffer will still be valid once it is called, as such we
             * copy the buffer and then pass it into the API.
             */
            if (tc->update(cursor, coll.id, key_value_t(key_tmp))) {
                if (tc->transaction.can_commit()) {
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
        }
        /* Ensure our last transaction is resolved. */
        if (tc->transaction.active())
            tc->transaction.rollback();
    }
};
