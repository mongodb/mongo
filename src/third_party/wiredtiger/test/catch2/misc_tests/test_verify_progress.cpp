/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "../wrappers/mock_session.h"

/*
 * [verify_progress]:
 * Test the time-based throttle that gates WT_SESSION.verify progress reports, so a fast verify
 * stays quiet and a long-running one emits a periodic heartbeat.
 */

TEST_CASE("Verify progress reports are gated on elapsed time", "[verify_progress]")
{
    std::shared_ptr<mock_session> session_mock = mock_session::build_test_mock_session();
    WT_SESSION_IMPL *session = session_mock->get_wt_session_impl();

    uint64_t interval_ms = (uint64_t)WT_PROGRESS_MSG_PERIOD * WT_THOUSAND;
    WT_TIMER timer;
    __wt_timer_start(session, &timer);

    SECTION("No report is due immediately after the timer starts")
    {
        REQUIRE(__wti_verify_progress_due(session, &timer, interval_ms) == false);
    }

    SECTION("A report is due once the interval elapses, then the timer resets")
    {
        /* Backdate the last-report time so more than the interval has elapsed. */
        timer.tv_sec -= (time_t)(interval_ms / WT_THOUSAND + 1);
        REQUIRE(__wti_verify_progress_due(session, &timer, interval_ms) == true);

        /* Reporting restarts the timer, so a back-to-back call is not yet due again. */
        REQUIRE(__wti_verify_progress_due(session, &timer, interval_ms) == false);
    }

    SECTION("A zero interval always reports")
    {
        REQUIRE(__wti_verify_progress_due(session, &timer, 0) == true);
    }
}
