/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "wrappers/mock_session.h"

/* Tests that __wt_single_thread_check_start assertion failure handles some fields being NULL. */

#if defined(HAVE_DIAGNOSTIC) && defined(HAVE_UNITTEST_ASSERTS)

TEST_CASE(
  "Single thread check: concurrent access with NULL fields produces valid assertion message",
  "[single_thread_check]")
{
    std::shared_ptr<mock_session> ms = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = ms->get_wt_session_impl();

    session->id = 1;

    /*
     * Simulate another thread holding the session lock. Taking it here causes
     * __wt_spin_trylock inside check_start to return EBUSY, which fires the assertion.
     * Real thread IDs are never 0, so owning_thread (0) != current_tid is guaranteed.
     */
    __wt_spin_lock(session, &session->thread_check.lock);
    __wt_single_thread_check_start(session);

    REQUIRE(session->unittest_assert_hit);

    uintmax_t current_tid;
    __wt_thread_id(&current_tid);

    /* name, last op, dhandle remain NULL, owning_thread is also 0. */
    std::string expected = std::string("WiredTiger assertion failed: 'ret == 0'. ") +
      "Session 1 is accessed concurrently by multiple threads: " + "current thread " +
      std::to_string(current_tid) +
      ", owning thread 0 (active op: none, last op: none, api depth: 0, dhandle: none)";
    REQUIRE(std::string(session->unittest_assert_msg) == expected);

    __wt_spin_unlock(session, &session->thread_check.lock);
}

#endif
