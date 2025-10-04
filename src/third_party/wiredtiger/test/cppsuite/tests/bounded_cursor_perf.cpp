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
#include "src/main/test.h"

using namespace test_harness;

/*
 * This test performs cursor traversal operations next() and prev() on a collection with both
 * bounded and normal cursors. The performance of both cursors are tracked and the average time
 * taken is added to the perf file. The test traverses all keys in the collection.
 */
class bounded_cursor_perf : public test {
public:
    bounded_cursor_perf(const test_args &args) : test(args)
    {
        init_operation_tracker();
    }

    static void
    set_bound_key_lower(scoped_cursor &cursor, execution_timer &timer, const char *cfg)
    {
        std::string lower_bound(1, ('0' - 1));
        cursor->set_key(cursor.get(), lower_bound.c_str());
        timer.track([&cursor, cfg]() -> int {
            testutil_check(cursor->bound(cursor.get(), cfg));
            return 0;
        });
    }

    static void
    set_bound_key_upper(scoped_cursor &cursor, execution_timer &timer, const char *cfg)
    {
        std::string upper_bound(1, ('9' + 1));
        cursor->set_key(cursor.get(), upper_bound.c_str());
        timer.track([&cursor, cfg]() -> int {
            testutil_check(cursor->bound(cursor.get(), cfg));
            return 0;
        });
    }

    void
    read_operation(thread_worker *tc) override final
    {
        /* This test will only work with one read thread. */
        testutil_assert(tc->thread_count == 1);

        /* Setup the compiled configuration strings. */
        WT_CONNECTION *conn = connection_manager::instance().get_connection();
        const char *compiled_ptr_lower;
        const char *compiled_ptr_upper;
        testutil_check(
          conn->compile_configuration(conn, "WT_CURSOR.bound", "bound=lower", &compiled_ptr_lower));
        testutil_check(
          conn->compile_configuration(conn, "WT_CURSOR.bound", "bound=upper", &compiled_ptr_upper));

        /* Initialize the different timers for each function. */
        execution_timer bounded_next("bounded_next", test::_args.test_name);
        execution_timer bounded_prev("bounded_prev", test::_args.test_name);
        execution_timer default_next("default_next", test::_args.test_name);
        execution_timer default_prev("default_prev", test::_args.test_name);
        execution_timer regular_config_timer("set_bounds_non-compiled", test::_args.test_name);
        execution_timer compiled_config_timer("set_bounds_compiled", test::_args.test_name);

        /* Get the collection to work on. */
        testutil_assert(tc->collection_count == 1);
        collection &coll = tc->db.get_collection(0);

        /* Open the cursors. */
        scoped_cursor next_cursor = tc->session.open_scoped_cursor(coll.name);
        scoped_cursor prev_cursor = tc->session.open_scoped_cursor(coll.name);
        scoped_cursor next_range_cursor = tc->session.open_scoped_cursor(coll.name);
        scoped_cursor prev_range_cursor = tc->session.open_scoped_cursor(coll.name);

        /*
         * The keys in the collection are contiguous from 0 -> key_count -1. Apply the range cursor
         * bounds outside of the key range for the purpose of this test.
         */
        bool set_bounds = true;
        bool set_compiled = true;
        while (tc->running()) {
            if (set_bounds) {
                /* Switch between setting pre-compiled strings and non-compiled. */
                if (set_compiled) {
                    set_bound_key_upper(
                      next_range_cursor, compiled_config_timer, compiled_ptr_upper);
                    set_bound_key_lower(
                      next_range_cursor, compiled_config_timer, compiled_ptr_lower);
                    set_bound_key_upper(
                      prev_range_cursor, compiled_config_timer, compiled_ptr_upper);
                    set_bound_key_lower(
                      prev_range_cursor, compiled_config_timer, compiled_ptr_lower);
                    set_compiled = false;
                } else {
                    set_bound_key_upper(next_range_cursor, regular_config_timer, "bound=upper");
                    set_bound_key_lower(next_range_cursor, regular_config_timer, "bound=lower");
                    set_bound_key_upper(prev_range_cursor, regular_config_timer, "bound=upper");
                    set_bound_key_lower(prev_range_cursor, regular_config_timer, "bound=lower");
                    set_compiled = true;
                }
                set_bounds = false;
            }

            /* Advance the cursors one position. */
            auto range_ret_next = bounded_next.track([&next_range_cursor]() -> int {
                return next_range_cursor->next(next_range_cursor.get());
            });
            auto ret_next = default_next.track(
              [&next_cursor]() -> int { return next_cursor->next(next_cursor.get()); });
            auto range_ret_prev = bounded_prev.track([&prev_range_cursor]() -> int {
                return prev_range_cursor->prev(prev_range_cursor.get());
            });
            auto ret_prev = default_prev.track(
              [&prev_cursor]() -> int { return prev_cursor->prev(prev_cursor.get()); });

            /* They should all finish their traversal at the same time. Assert that they do. */
            if (range_ret_next == WT_NOTFOUND || ret_next == WT_NOTFOUND ||
              range_ret_prev == WT_NOTFOUND || ret_prev == WT_NOTFOUND) {
                testutil_assert(range_ret_next == ret_next);
                testutil_assert(ret_next == range_ret_prev);
                testutil_assert(range_ret_prev == ret_prev);

                /* Reset the cursors and indicate that bounds need applying. */
                testutil_check(next_cursor->reset(next_cursor.get()));
                testutil_check(prev_cursor->reset(prev_cursor.get()));
                testutil_check(next_range_cursor->reset(next_range_cursor.get()));
                testutil_check(prev_range_cursor->reset(prev_range_cursor.get()));
                set_bounds = true;
            }
        }
    }
};
