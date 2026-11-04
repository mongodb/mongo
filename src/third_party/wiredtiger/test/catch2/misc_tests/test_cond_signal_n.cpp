/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <catch2/catch.hpp>
#include "wt_internal.h"
#include "../wrappers/mock_session.h"

namespace {

std::atomic<int> blocking(0);

/*
 * Run under the condition's mutex just before the waiter blocks, so once every waiter has run it, a
 * wake that takes the mutex finds them all blocked.
 */
bool
count_blocking(WT_SESSION_IMPL *)
{
    ++blocking;
    return (true);
}

/* Poll until the predicate holds, giving up after a deadline. */
template <typename Pred>
bool
wait_until(Pred pred)
{
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > deadline)
            return (false);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return (true);
}

/*
 * Block the given number of threads on the condition, wake them with the given call, and return how
 * many came back as signalled, or -1 if the waiters never blocked or fewer than the minimum woke
 * before the deadline.
 */
template <typename WakeFn>
int
count_woken(
  WT_SESSION_IMPL *session, WT_CONDVAR *cond, int thread_count, int min_woken, WakeFn wake)
{
    std::atomic<int> woken(0), done(0);
    std::vector<std::thread> threads;
    int result = -1;

    blocking = 0;
    for (int i = 0; i < thread_count; ++i)
        threads.emplace_back([&]() {
            bool signalled = false;
            __wt_cond_wait_signal(session, cond, 60 * WT_MILLION, count_blocking, &signalled);
            if (signalled)
                ++woken;
            ++done;
        });

    if (wait_until([&]() { return (blocking.load() == thread_count); })) {
        wake();
        if (wait_until([&]() { return (woken.load() >= min_woken); })) {
            /* Leave time for any wakes beyond the expected ones to show up. */
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            result = woken.load();
        }
    }

    /* Release the rest so the threads can be joined. */
    while (done.load() < thread_count) {
        __wt_cond_signal(session, cond);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (auto &t : threads)
        t.join();
    return (result);
}

} // namespace

TEST_CASE("Test cond signal_n: counted wake", "[cond]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();
    WT_CONDVAR *cond;
    REQUIRE(__wt_cond_alloc(session, "test cond", &cond) == 0);

    SECTION("Fewer than three quarters of the waiters wake n, not all")
    {
        int woken =
          count_woken(session, cond, 8, 3, [&]() { __wt_cond_signal_n(session, cond, 3); });
        CHECK(woken >= 3);
        CHECK(woken < 8);
    }

    SECTION("Three quarters or more of the waiters fall back to a broadcast")
    {
        CHECK(
          count_woken(session, cond, 8, 8, [&]() { __wt_cond_signal_n(session, cond, 6); }) == 8);
    }

    SECTION("More wakes than waiters wake everyone")
    {
        CHECK(
          count_woken(session, cond, 4, 4, [&]() { __wt_cond_signal_n(session, cond, 100); }) == 4);
    }

    SECTION("Zero is treated as one wake")
    {
        int woken =
          count_woken(session, cond, 8, 1, [&]() { __wt_cond_signal_n(session, cond, 0); });
        CHECK(woken >= 1);
        CHECK(woken < 8);
    }

    SECTION("No waiters leaves the condition signalled for the next waiter")
    {
        __wt_cond_signal_n(session, cond, 1);
        bool signalled = false;
        __wt_cond_wait_signal(session, cond, WT_MILLION, nullptr, &signalled);
        CHECK(signalled);
    }

    __wt_cond_destroy(session, &cond);
}
