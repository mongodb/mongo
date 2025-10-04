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

#include "src/util/execution_timer.h"
#include "src/util/instruction_counter.h"
#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/main/test.h"

namespace test_harness {
/*
 * Benchmark various frequently called session APIs. See the comment in
 * api_instruction_count_benchmarks for further details.
 */
class api_timing_benchmarks : public test {
    /* Loop each timer this many times to reduce noise. */
    const int _LOOP_COUNTER = 1000;

public:
    api_timing_benchmarks(const test_args &args) : test(args)
    {
        init_operation_tracker(nullptr);
    }

    void
    custom_operation(thread_worker *tc) override final
    {
        /* Assert there is only one collection. */
        testutil_assert(tc->collection_count == 1);

        /* Create the necessary timers. */
        execution_timer begin_transaction_timer("begin_transaction", test::_args.test_name);
        execution_timer commit_transaction_timer("commit_transaction", test::_args.test_name);
        execution_timer rollback_transaction_timer("rollback_transaction", test::_args.test_name);
        execution_timer timestamp_transaction_uint_timer(
          "timestamp_transaction_uint", test::_args.test_name);
        execution_timer cursor_reset_timer("cursor_reset", test::_args.test_name);
        execution_timer cursor_search_timer("cursor_search", test::_args.test_name);

        /*
         * Time begin transaction and commit transaction. In order for commit to do work we need at
         * least one modification on the transaction.
         */
        scoped_session &session = tc->session;
        scoped_cursor cursor = session.open_scoped_cursor(tc->db.get_collection(0).name);
        auto key_count = tc->db.get_collection(0).get_key_count();

        for (int i = 0; i < _LOOP_COUNTER / 10; i++) {
            testutil_check(begin_transaction_timer.track(
              [&session]() -> int { return session->begin_transaction(session.get(), nullptr); }));
            auto key = tc->pad_string(std::to_string(key_count + i), tc->key_size);
            auto value = "a";
            /* Add the modification. */
            if (!tc->insert(cursor, 0, key, value)) {
                i--;
                testutil_check(session->rollback_transaction(session.get(), nullptr));
                continue;
            }
            testutil_check(commit_transaction_timer.track(
              [&session]() -> int { return session->commit_transaction(session.get(), nullptr); }));
        }

        /* Time rollback transaction. */
        for (int i = 0; i < _LOOP_COUNTER; i++) {
            testutil_check(begin_transaction_timer.track(
              [&session]() -> int { return session->begin_transaction(session.get(), nullptr); }));
            testutil_check(rollback_transaction_timer.track([&session]() -> int {
                return session->rollback_transaction(session.get(), nullptr);
            }));
        }

        /* Time timestamp transaction_uint. */
        testutil_check(session->begin_transaction(session.get(), nullptr));
        for (int i = 0; i < _LOOP_COUNTER; i++) {
            auto timestamp = tc->tsm->get_next_ts();
            testutil_check(timestamp_transaction_uint_timer.track([&session, &timestamp]() -> int {
                return session->timestamp_transaction_uint(
                  session.get(), WT_TS_TXN_TYPE_COMMIT, timestamp);
            }));
        }
        testutil_check(session->rollback_transaction(session.get(), nullptr));
    }
};

} // namespace test_harness
