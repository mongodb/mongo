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

#include "src/common/constants.h"
#include "src/common/logger.h"
#include "src/main/test.h"
#include "src/util/instruction_counter.h"

namespace test_harness {
/*
 * This test aims to measure the number of instructions cursor API calls take. The test has measures
 * in place to prevent background threads from taking resources:
 *  - We set the sweep server interval to be greater than the test duration. This means it never
 *    triggers.
 *  - Logging, and the log manager thread are disabled per the connection open configuration.
 *  - Prefetch, off by default
 *  - Background compact, disabled by in_memory.
 *  - Capacity server, disabled by in_memory.
 *  - Checkpoint server, disabled by in_memory.
 *  - Eviction, still runs but ideally we don't cross any threshold to trigger it.
 *  - Checkpoint cleanup, disabled by in_memory.
 *
 * Additionally to avoid I/O the connection is set to in_memory.
 */
class api_instruction_count_benchmarks : public test {
public:
    api_instruction_count_benchmarks(const test_args &args) : test(args)
    {
        init_operation_tracker();
    }

    void
    custom_operation(thread_worker *tc) override final
    {
        /* The test expects no more than one collection. */
        testutil_assert(tc->collection_count == 1);

        /* Assert that we are running in memory. */
        testutil_assert(_config->get_bool(IN_MEMORY));

        /*
         * Create the relevant metric collectors. Defined here so it's easy to tell what is being
         * tested without jumping around the function.
         */
        instruction_counter begin_transaction_ic("begin_transaction", test::_args.test_name);
        instruction_counter commit_transaction_ic("commit_transaction", test::_args.test_name);
        instruction_counter cursor_insert_ic("cursor_insert", test::_args.test_name);
        instruction_counter cursor_modify_ic("cursor_modify", test::_args.test_name);
        instruction_counter cursor_remove_ic("cursor_remove", test::_args.test_name);
        instruction_counter cursor_reset_ic("cursor_reset", test::_args.test_name);
        instruction_counter cursor_search_ic("cursor_search", test::_args.test_name);
        instruction_counter cursor_update_ic("cursor_update", test::_args.test_name);
        instruction_counter open_cursor_cached_ic("open_cursor_cached", test::_args.test_name);
        instruction_counter open_cursor_uncached_ic("open_cursor_uncached", test::_args.test_name);
        instruction_counter rollback_transaction_ic("rollback_transaction", test::_args.test_name);
        instruction_counter timestamp_transaction_uint_ic(
          "timestamp_transaction_uint", test::_args.test_name);

        collection &coll = tc->db.get_collection(0);
        scoped_cursor cursor = tc->session.open_scoped_cursor(coll.name);

        /*
         * We don't want to measure the getter, although it should be a constant overhead. For
         * consistency only use wt_cursor, and wt_session from this point forward. We can't enforce
         * this so hopefully this comment serves as a reminder.
         */
        WT_CURSOR *wt_cursor = cursor.get();
        WT_SESSION *wt_session = tc->session.get();

        /* Benchmark cursor->search and cursor->reset. */
        std::string key = tc->pad_string(std::to_string(coll.get_key_count() - 1), tc->key_size);
        wt_cursor->set_key(wt_cursor, key.c_str());
        testutil_check(
          cursor_search_ic.track([&wt_cursor]() -> int { return wt_cursor->search(wt_cursor); }));
        testutil_check(
          cursor_reset_ic.track([&wt_cursor]() -> int { return wt_cursor->reset(wt_cursor); }));

        /* Benchmark session->begin_transaction. */
        testutil_check(begin_transaction_ic.track(
          [&wt_session]() -> int { return wt_session->begin_transaction(wt_session, nullptr); }));

        /* Benchmark timestamp transaction. */
        auto timestamp = tc->tsm->get_next_ts();
        testutil_check(timestamp_transaction_uint_ic.track([&wt_session, &timestamp]() -> int {
            return wt_session->timestamp_transaction_uint(
              wt_session, WT_TS_TXN_TYPE_COMMIT, timestamp);
        }));

        /* Benchmark rollback transaction. */
        testutil_check(rollback_transaction_ic.track([&wt_session]() -> int {
            return wt_session->rollback_transaction(wt_session, nullptr);
        }));

        /* Begin a transaction that we will later commit. */
        testutil_check(wt_session->begin_transaction(wt_session, nullptr));

        /* Search before making modifications to avoid triggering a search internally. */
        wt_cursor->set_key(wt_cursor, key.c_str());
        testutil_check(wt_cursor->search(wt_cursor));

        /*
         * Benchmark cursor->update.
         *
         * We need to be careful here, if we don't call search before this we will unintentionally
         * be benchmarking search + update. Additionally setting a key on the cursor will trigger a
         * fresh search from the root.
         */
        wt_cursor->set_value(wt_cursor, "b");
        testutil_check(
          cursor_update_ic.track([&wt_cursor]() -> int { return wt_cursor->update(wt_cursor); }));

        /*
         * Benchmark commit transaction. Note we need one modification here in order to actually
         * commit.
         */
        testutil_check(commit_transaction_ic.track(
          [&wt_session]() -> int { return wt_session->commit_transaction(wt_session, nullptr); }));

        /* Re-search. */
        wt_cursor->set_key(wt_cursor, key.c_str());
        testutil_check(wt_cursor->search(wt_cursor));

        /*
         * Benchmark cursor->modify. Again we've positioned using a search to avoid searching,
         * internally.
         */
        testutil_check(wt_session->begin_transaction(wt_session, nullptr));
        WT_MODIFY mod;
        mod.data.data = "c";
        mod.data.size = 1;
        mod.offset = 0;
        mod.size = mod.data.size;
        testutil_check(cursor_modify_ic.track(
          [&wt_cursor, &mod]() -> int { return wt_cursor->modify(wt_cursor, &mod, 1); }));
        testutil_check(wt_session->rollback_transaction(wt_session, nullptr));

        /* Re-search. Setup the overwrite config. */
        testutil_check(wt_cursor->reconfigure(wt_cursor, "overwrite=true"));
        wt_cursor->set_key(wt_cursor, key.c_str());
        testutil_check(wt_cursor->search(wt_cursor));

        /*
         * Benchmark cursor->insert. Provide the overwrite configuration to avoid triggering a
         * search.
         */
        wt_cursor->set_value(wt_cursor, "a");
        testutil_check(
          cursor_insert_ic.track([&wt_cursor]() -> int { return wt_cursor->insert(wt_cursor); }));

        /* Re-search. */
        wt_cursor->set_key(wt_cursor, key.c_str());
        testutil_check(wt_cursor->search(wt_cursor));

        /*
         * Benchmark cursor->remove. Again we've positioned using a search to avoid searching,
         * internally.
         */
        testutil_check(
          cursor_remove_ic.track([&wt_cursor]() -> int { return wt_cursor->remove(wt_cursor); }));

        /* Benchmark session->open_cursor, this should NOT use a cached cursor. */
        WT_CURSOR *cursorp = nullptr;
        const char *cursor_uri = tc->db.get_collection(0).name.c_str();
        testutil_check(wt_session->reconfigure(wt_session, "cache_cursors=false"));
        testutil_check(open_cursor_uncached_ic.track([&wt_session, &cursor_uri, &cursorp]() -> int {
            return wt_session->open_cursor(wt_session, cursor_uri, nullptr, nullptr, &cursorp);
        }));
        testutil_check(wt_session->reconfigure(wt_session, "cache_cursors=true"));
        testutil_check(cursorp->close(cursorp));
        cursorp = nullptr;

        /* Open a cursor and close it to put it into the cache. */
        testutil_check(wt_session->open_cursor(wt_session, cursor_uri, nullptr, nullptr, &cursorp));
        testutil_check(cursorp->close(cursorp));
        cursorp = nullptr;

        /* Benchmark session->open_cursor, this should use a cached cursor. */
        testutil_check(open_cursor_cached_ic.track([&wt_session, &cursor_uri, &cursorp]() -> int {
            return wt_session->open_cursor(wt_session, cursor_uri, nullptr, nullptr, &cursorp);
        }));

        testutil_check(cursorp->close(cursorp));
        cursorp = nullptr;
    }
};

} // namespace test_harness
