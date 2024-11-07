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

/*
 * session_config.cpp: Test one of the session configuration functions, __session_config_int. Ensure
 * that the relevant configurations correctly modify the session state.
 */

/*
 * Check a basic configuration that sets and clears a flag.
 */
static void
test_config_flag(WT_SESSION_IMPL *session, const std::string &config_param, const int flag)
{
    std::string param_true = config_param + "=true";
    std::string param_false = config_param + "=false";
    REQUIRE(__ut_session_config_int(session, param_true.c_str()) == 0);
    REQUIRE(F_ISSET(session, flag));
    REQUIRE(__ut_session_config_int(session, param_false.c_str()) == 0);
    REQUIRE(!F_ISSET(session, flag));
    REQUIRE(__ut_session_config_int(session, param_true.c_str()) == 0);
    REQUIRE(__ut_session_config_int(session, "") == 0);
    REQUIRE(F_ISSET(session, flag));
}

TEST_CASE("Session config - test setting and clearing a flag", "[session_config]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<MockSession> session_mock = MockSession::buildTestMockSession();
    WT_SESSION_IMPL *session = session_mock->getWtSessionImpl();

    SECTION("ignore_cache_size")
    {
        test_config_flag(session, "ignore_cache_size", WT_SESSION_IGNORE_CACHE_SIZE);
    }

    SECTION("cache_cursors")
    {
        test_config_flag(session, "cache_cursors", WT_SESSION_CACHE_CURSORS);
    }

    SECTION("debug.checkpoint_fail_before_turtle_update")
    {
        test_config_flag(session, "debug.checkpoint_fail_before_turtle_update",
          WT_SESSION_DEBUG_CHECKPOINT_FAIL_BEFORE_TURTLE_UPDATE);
    }

    SECTION("debug.release_evict_page")
    {
        test_config_flag(session, "debug.release_evict_page", WT_SESSION_DEBUG_RELEASE_EVICT);
    }
}

TEST_CASE("cache_max_wait_ms", "[session_config]")
{
    /* Build Mock session, this will automatically create a mock connection. */
    std::shared_ptr<MockSession> session_mock = MockSession::buildTestMockSession();
    WT_SESSION_IMPL *session = session_mock->getWtSessionImpl();

    REQUIRE(__ut_session_config_int(session, "cache_max_wait_ms=2000") == 0);
    REQUIRE(session->cache_max_wait_us == 2000 * WT_THOUSAND);

    /* Test that setting to zero works correctly. */
    REQUIRE(__ut_session_config_int(session, "cache_max_wait_ms=0") == 0);
    REQUIRE(session->cache_max_wait_us == 0);

    /*
     * This call should not error out or return WT_NOTFOUND with invalid strings. We should depend
     * __wt_config_getones unit tests for correctness. Currently that test does not exist.
     */
    REQUIRE(__ut_session_config_int(session, NULL) == 0);
    REQUIRE(__ut_session_config_int(session, "") == 0);
    REQUIRE(__ut_session_config_int(session, "foo=10000") == 0);
    REQUIRE(__ut_session_config_int(session, "bad_string") == 0);
    REQUIRE(session->cache_max_wait_us == 0);

    /*
     * WiredTiger config strings accept negative values, but the session variable is a uint64_t.
     * Overflow is allowed.
     */
    REQUIRE(__ut_session_config_int(session, "cache_max_wait_ms=-1") == 0);
    REQUIRE(session->cache_max_wait_us == 0xfffffffffffffc18);
}
