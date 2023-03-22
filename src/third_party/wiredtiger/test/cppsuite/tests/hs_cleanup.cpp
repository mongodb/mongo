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

#include "src/common/logger.h"
#include "src/common/random_generator.h"
#include "src/main/test.h"

using namespace test_harness;

/*
 * Here we want to age out entire pages, i.e. the stop time pair on a page should be globally
 * visible. To do so we'll update ranges of keys with increasing timestamps which will age out the
 * pre-existing data. It may not trigger a cleanup on the data file but should result in data
 * getting cleaned up from the history store.
 *
 * This is then tracked using the associated statistic which can be found in the metrics_monitor.
 */
class hs_cleanup : public test {
public:
    hs_cleanup(const test_args &args) : test(args)
    {
        init_operation_tracker();
    }

    void
    update_operation(thread_worker *tc) override final
    {
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

        const uint64_t MAX_ROLLBACKS = 100;
        uint32_t rollback_retries = 0;

        /* In this test each thread gets a single collection. */
        testutil_assert(tc->db.get_collection_count() == tc->thread_count);

        collection &coll = tc->db.get_collection(tc->id);
        scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);

        /*
         * We don't know the key range we're operating over here so we can't be much smarter here.
         */
        while (tc->running()) {
            tc->sleep();

            /* Start a transaction if possible. */
            tc->txn.try_begin();

            auto ret = cursor->next(cursor.get());
            if (ret != 0) {
                if (ret == WT_NOTFOUND)
                    testutil_check(cursor->reset(cursor.get()));
                else if (ret == WT_ROLLBACK)
                    tc->txn.rollback();
                else
                    testutil_die(ret, "Unexpected error returned from cursor->next()");
                continue;
            }

            const char *key_tmp;
            testutil_check(cursor->get_key(cursor.get(), &key_tmp));

            /*
             * The retrieved key needs to be passed inside the update function. However, the update
             * API doesn't guarantee our buffer will still be valid once it is called, as such we
             * copy the buffer and then pass it into the API.
             */
            std::string value =
              random_generator::instance().generate_pseudo_random_string(tc->value_size);
            if (tc->update(cursor, coll.id, key_tmp, value)) {
                if (tc->txn.can_commit()) {
                    if (tc->txn.commit())
                        rollback_retries = 0;
                    else
                        ++rollback_retries;
                }
            } else {
                tc->txn.rollback();
                ++rollback_retries;
            }
            testutil_assert(rollback_retries < MAX_ROLLBACKS);
        }
        /* Ensure our last transaction is resolved. */
        tc->txn.try_rollback();
    }
};
