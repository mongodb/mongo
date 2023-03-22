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

#include "src/common/random_generator.h"
#include "src/main/test.h"

using namespace test_harness;

/*
 * This test inserts and reads a large quantity of data in bursts, this is intended to simulate a
 * mongod instance loading a large amount of data over a long period of time.
 */
class burst_inserts : public test {
public:
    burst_inserts(const test_args &args) : test(args)
    {
        _burst_duration = _config->get_int("burst_duration");
        logger::log_msg(LOG_INFO, "Burst duration set to: " + std::to_string(_burst_duration));
        init_operation_tracker();
    }

    /*
     * Insert operation that inserts continuously for insert_duration with no throttling. It then
     * sleeps for op_rate.
     */
    void
    insert_operation(thread_worker *tc) override final
    {
        logger::log_msg(
          LOG_INFO, type_string(tc->type) + " thread {" + std::to_string(tc->id) + "} commencing.");

        /* Helper struct which stores a pointer to a collection and a cursor associated with it. */
        struct collection_cursor {
            collection_cursor(
              collection &coll, scoped_cursor &&write_cursor, scoped_cursor &&read_cursor)
                : coll(coll), read_cursor(std::move(read_cursor)),
                  write_cursor(std::move(write_cursor))
            {
            }
            collection &coll;
            scoped_cursor read_cursor;
            scoped_cursor write_cursor;
        };

        /* Collection cursor vector. */
        std::vector<collection_cursor> ccv;
        uint64_t collection_count = tc->db.get_collection_count();
        uint64_t collections_per_thread = collection_count / tc->thread_count;
        /* Must have unique collections for each thread. */
        testutil_assert(collection_count % tc->thread_count == 0);
        int thread_offset = tc->id * collections_per_thread;
        for (int i = thread_offset; i < thread_offset + collections_per_thread && tc->running();
             ++i) {
            collection &coll = tc->db.get_collection(i);
            /*
             * Create a reading cursor that will read random documents for every next call. This
             * will help generate cache pressure.
             */
            ccv.push_back({coll, std::move(tc->session.open_scoped_cursor(coll.name)),
              std::move(tc->session.open_scoped_cursor(coll.name, "next_random=true"))});
        }

        uint64_t counter = 0;
        while (tc->running()) {
            uint64_t start_key = ccv[counter].coll.get_key_count();
            uint64_t added_count = 0;
            auto &cc = ccv[counter];
            auto burst_start = std::chrono::system_clock::now();
            while (tc->running() &&
              std::chrono::system_clock::now() - burst_start <
                std::chrono::seconds(_burst_duration)) {
                tc->txn.try_begin();
                auto key = tc->pad_string(std::to_string(start_key + added_count), tc->key_size);
                /* A return value of true implies the insert was successful. */
                auto value =
                  random_generator::instance().generate_pseudo_random_string(tc->value_size);
                if (!tc->insert(cc.write_cursor, cc.coll.id, key, value)) {
                    tc->txn.rollback();
                    added_count = 0;
                    continue;
                }
                added_count++;

                /* Walk our random reader intended to generate cache pressure. */
                int ret = 0;
                if ((ret = cc.read_cursor->next(cc.read_cursor.get())) != 0) {
                    if (ret == WT_NOTFOUND) {
                        testutil_check(cc.read_cursor->reset(cc.read_cursor.get()));
                    } else if (ret == WT_ROLLBACK) {
                        tc->txn.rollback();
                        added_count = 0;
                        continue;
                    } else {
                        testutil_die(ret, "Unhandled error in cursor->next()");
                    }
                }

                if (tc->txn.can_commit()) {
                    if (tc->txn.commit()) {
                        cc.coll.increase_key_count(added_count);
                        start_key = cc.coll.get_key_count();
                    }
                    added_count = 0;
                }
            }
            /* Close out our current txn. */
            if (tc->txn.active()) {
                if (tc->txn.commit()) {
                    logger::log_msg(LOG_TRACE,
                      "Committed an insertion of " + std::to_string(added_count) + " keys.");
                    cc.coll.increase_key_count(added_count);
                }
            }

            testutil_check(cc.write_cursor->reset(cc.write_cursor.get()));
            testutil_check(cc.read_cursor->reset(cc.read_cursor.get()));
            counter++;
            if (counter == collections_per_thread)
                counter = 0;
            testutil_assert(counter < collections_per_thread);
            tc->sleep();
        }
        /* Make sure the last transaction is rolled back now the work is finished. */
        tc->txn.try_rollback();
    }

private:
    int _burst_duration = 0;
};
