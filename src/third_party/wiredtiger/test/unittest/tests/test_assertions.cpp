/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <iostream>
#include <catch2/catch.hpp>

#include "utils.h"
#include "wiredtiger.h"
#include "wrappers/connection_wrapper.h"
#include "wt_internal.h"

/*
 * These values track the possible results from calling our assertion under unit tests.
 * However, since we're testing assertion logic these values actually describe the resulting control
 * flow from calling an assertion:
 * - ASSERT_PANIC that WT_RET_PANIC has been called as a result of a failing WT_RET_PANIC_ASSERT
 * - ASSERT_RET indicates that WT_RET_MSG has been called as a result of a failing WT_RET_ASSERT
 * - ASSERT_ERR that WT_ERR_MSG has been called as a result of a failing WT_ERR_ASSERT
 * and finally ASSERT_FIRED and NO_ASSERT_FIRED to indicate if an assertion - which would normally
 * abort WiredTiger - would have been triggered.
 */

#define ASSERT_PANIC -3
#define ASSERT_RET -2
#define ASSERT_ERR -1
#define NO_ASSERT_FIRED 0
#define ASSERT_FIRED 1

uint16_t DIAGNOSTIC_FLAGS[] = {WT_DIAGNOSTIC_ALL, WT_DIAGNOSTIC_CHECKPOINT_VALIDATE,
  WT_DIAGNOSTIC_CURSOR_CHECK, WT_DIAGNOSTIC_DISK_VALIDATE, WT_DIAGNOSTIC_EVICTION_CHECK,
  WT_DIAGNOSTIC_GENERATION_CHECK, WT_DIAGNOSTIC_HS_VALIDATE, WT_DIAGNOSTIC_KEY_OUT_OF_ORDER,
  WT_DIAGNOSTIC_LOG_VALIDATE, WT_DIAGNOSTIC_PREPARED, WT_DIAGNOSTIC_SLOW_OPERATION,
  WT_DIAGNOSTIC_TXN_VISIBILITY};

int
check_assertion_fired(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    ret = session->unittest_assert_hit ? ASSERT_FIRED : NO_ASSERT_FIRED;

    if (ret == ASSERT_FIRED) {
        // Clear the assertion flag and message for the next test step.
        session->unittest_assert_hit = false;
        memset(session->unittest_assert_msg, 0, WT_SESSION_UNITTEST_BUF_LEN);
    }

    return ret;
}

/*
 * Wrapper to call WT_RET_ASSERT. This returns different values depending on whether WT_RET_ASSERT
 * fires or not.
 */
int
call_wt_ret(WT_SESSION_IMPL *session, uint16_t category, bool assert_should_pass)
{
    WT_RET_ASSERT(session, category, assert_should_pass, ASSERT_RET, "WT_RET raised assert");

    return check_assertion_fired(session);
}

/*
 * Wrapper to call WT_ERR_ASSERT. This returns different values depending on whether WT_ERR_ASSERT
 * fires or not.
 */
int
call_wt_err(WT_SESSION_IMPL *session, uint16_t category, bool assert_should_pass)
{
    WT_DECL_RET;

    WT_ERR_ASSERT(session, category, assert_should_pass, ASSERT_ERR, "WT_ERR raised assert");

    ret = check_assertion_fired(session);

    if (0) {
err:
        ret = ASSERT_ERR;
    }
    return ret;
}

/*
 * Wrapper to call WT_RET_PANIC_ASSERT. This returns different values depending on whether
 * WT_RET_PANIC_ASSERT fires or not.
 */
int
call_wt_panic(WT_SESSION_IMPL *session, uint16_t category, bool assert_should_pass)
{
    WT_RET_PANIC_ASSERT(
      session, category, assert_should_pass, ASSERT_PANIC, "WT_PANIC raised assert");

    return check_assertion_fired(session);
}

/*
 * Wrapper to call WT_ASSERT_OPTIONAL. This returns different values depending on whether
 * WT_ASSERT_OPTIONAL fires or not.
 */
int
call_wt_optional(WT_SESSION_IMPL *session, uint16_t category, bool assert_should_pass)
{
    WT_ASSERT_OPTIONAL(session, category, assert_should_pass, "WT_OPTIONAL raised assert");

    return check_assertion_fired(session);
}

/* Assert that all diagnostic assert categories are off. */
void
all_diag_asserts_off(WT_SESSION_IMPL *session)
{
    for (uint16_t flag : DIAGNOSTIC_FLAGS) {
        REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, flag) == false);
    }
}

/* Assert that all diagnostic assert categories are on. */
void
all_diag_asserts_on(WT_SESSION_IMPL *session)
{
    for (uint16_t flag : DIAGNOSTIC_FLAGS) {
        REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, flag) == true);
    }
}

/* Assert that all expected asserts (passed in via the "category" arg) fire. */
int
configured_asserts_abort(WT_SESSION_IMPL *session, uint16_t category)
{
    int ret = 0;

    REQUIRE(call_wt_optional(session, category, false) == ASSERT_FIRED);

    REQUIRE(call_wt_ret(session, category, false) == ASSERT_FIRED);

    REQUIRE(call_wt_err(session, category, false) == ASSERT_FIRED);

    REQUIRE(call_wt_panic(session, category, false) == ASSERT_FIRED);

    return ret;
}

/* Assert that the expected asserts don't fire (those not passed in via the "category" arg). */
int
configured_asserts_off(WT_SESSION_IMPL *session, u_int16_t category)
{
    int ret = 0;

    REQUIRE(call_wt_optional(session, category, false) == NO_ASSERT_FIRED);

    REQUIRE(call_wt_ret(session, category, false) == ASSERT_RET);

    REQUIRE(call_wt_err(session, category, false) == ASSERT_ERR);

    REQUIRE(call_wt_panic(session, category, false) == -31804);

    return ret;
}

#ifdef HAVE_DIAGNOSTIC
TEST_CASE("Must run in non-diagnostic mode!", "[assertions]")
{
    /*
     * Unit testing assertions requires that we have compiled with HAVE_DIAGNOSTIC=0 as
     * HAVE_DIAGNOSTIC=1 forces all assertion on at all times and we won't be able to test
     * functionality. This test catches exists to prevent us from testing in diagnostic mode.
     */
    std::cout << "Attempting to run assertion unit tests with HAVE_DIAGNOSTIC=1! These tests must "
                 "be run with HAVE_DIAGNOSTIC=0."
              << std::endl;
    exit(1);
}
#endif

/* Assert that regardless of connection configuration, asserts are always disabled/enabled. */
TEST_CASE("Connection config: off/on", "[assertions]")
{
    SECTION("Connection config: off")
    {
        ConnectionWrapper conn(DB_HOME, "create");
        WT_SESSION_IMPL *session = conn.createSession();

        /*
         * Assert that WT_ASSERT and WT_ASSERT_ALWAYS behave consistently regardless of the
         * diagnostic asserts configuration. This behavior occurs when running in non-diagnostic
         * mode: WT_ASSERT doesn't abort and WT_ASSERT_ALWAYS always aborts regardless of diagnostic
         * mode.
         */
        WT_ASSERT(session, false);
        REQUIRE_FALSE(session->unittest_assert_hit);
        REQUIRE(std::string(session->unittest_assert_msg).empty());

        WT_ASSERT_ALWAYS(session, false, "Values are not equal!");
        REQUIRE(session->unittest_assert_hit);
        REQUIRE(std::string(session->unittest_assert_msg) ==
          "WiredTiger assertion failed: 'false'. Values are not equal!");

        all_diag_asserts_off(session);
    }

    SECTION("Connection config: on")
    {
        ConnectionWrapper conn(DB_HOME, "create, extra_diagnostics=[all]");
        WT_SESSION_IMPL *session = conn.createSession();

        WT_ASSERT(session, false);
        REQUIRE_FALSE(session->unittest_assert_hit);
        REQUIRE(std::string(session->unittest_assert_msg).empty());

        WT_ASSERT_ALWAYS(session, false, "Values are not equal!");
        REQUIRE(session->unittest_assert_hit);
        REQUIRE(std::string(session->unittest_assert_msg) ==
          "WiredTiger assertion failed: 'false'. Values are not equal!");

        all_diag_asserts_on(session);
    }
}

/* When WT_DIAGNOSTIC_ALL is enabled, all asserts are enabled. */
TEST_CASE("Connection config: WT_DIAGNOSTIC_ALL", "[assertions]")
{
    ConnectionWrapper conn(DB_HOME, "create, extra_diagnostics= [all]");
    WT_SESSION_IMPL *session = conn.createSession();

    REQUIRE(configured_asserts_abort(session, WT_DIAGNOSTIC_ALL) == 0);

    all_diag_asserts_on(session);
}

/* When a category is enabled, all asserts for that category are enabled. */
TEST_CASE("Connection config: check one enabled category", "[assertions]")
{
    ConnectionWrapper conn(DB_HOME, "create, extra_diagnostics=[key_out_of_order]");
    WT_SESSION_IMPL *session = conn.createSession();

    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_KEY_OUT_OF_ORDER) == true);
    REQUIRE(configured_asserts_abort(session, WT_DIAGNOSTIC_KEY_OUT_OF_ORDER) == 0);
}

/* Asserts that categories are enabled/disabled following the connection configuration. */
TEST_CASE("Connection config: check multiple enabled categories", "[assertions]")
{
    ConnectionWrapper conn(DB_HOME, "create, extra_diagnostics= [txn_visibility, prepared]");
    WT_SESSION_IMPL *session = conn.createSession();

    REQUIRE(configured_asserts_abort(session, WT_DIAGNOSTIC_TXN_VISIBILITY) == 0);

    // Checking state.
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_TXN_VISIBILITY) == true);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_PREPARED) == true);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_SLOW_OPERATION) == false);
}

/* Asserts that categories are enabled/disabled following the connection configuration. */
TEST_CASE("Connection config: check disabled category", "[assertions]")
{
    ConnectionWrapper conn(DB_HOME, "create, extra_diagnostics = [eviction_check]");
    WT_SESSION_IMPL *session = conn.createSession();

    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_SLOW_OPERATION) == false);
    REQUIRE(configured_asserts_off(session, WT_DIAGNOSTIC_SLOW_OPERATION) == 0);
}

/* Reconfigure with extra_diagnostics not provided. */
TEST_CASE("Reconfigure: extra_diagnostics not provided", "[assertions]")
{
    ConnectionWrapper conn(DB_HOME, "create");
    auto connection = conn.getWtConnection();
    WT_SESSION_IMPL *session = conn.createSession();

    connection->reconfigure(connection, "");
    all_diag_asserts_off(session);
}

/* Reconfigure the connection with extra_diagnostics as an empty list. */
TEST_CASE("Reconfigure: extra_diagnostics empty list", "[assertions]")
{
    ConnectionWrapper conn(DB_HOME);
    auto connection = conn.getWtConnection();
    WT_SESSION_IMPL *session = conn.createSession();

    all_diag_asserts_off(session);
    connection->reconfigure(connection, "extra_diagnostics=[]");
    all_diag_asserts_off(session);
}

/* Reconfigure the connection with extra_diagnostics as a list with invalid item. */
TEST_CASE("Reconfigure: extra_diagnostics with invalid item", "[assertions]")
{
    ConnectionWrapper conn(DB_HOME);
    auto connection = conn.getWtConnection();
    WT_SESSION_IMPL *session = conn.createSession();

    all_diag_asserts_off(session);
    REQUIRE(connection->reconfigure(
      connection, "extra_diagnostics=[slow_operation, eviction_check, INVALID]"));
    all_diag_asserts_off(session);
}

/* Reconfigure the connection with extra_diagnostics as a list of valid items. */
TEST_CASE("Reconfigure: extra_diagnostics with valid items", "[assertions]")
{
    ConnectionWrapper conn(DB_HOME);
    auto connection = conn.getWtConnection();
    WT_SESSION_IMPL *session = conn.createSession();

    connection->reconfigure(
      connection, "extra_diagnostics=[checkpoint_validate, log_validate, eviction_check]");

    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_CHECKPOINT_VALIDATE) == true);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_LOG_VALIDATE) == true);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_EVICTION_CHECK) == true);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_PREPARED) == false);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_KEY_OUT_OF_ORDER) == false);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_SLOW_OPERATION) == false);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_TXN_VISIBILITY) == false);
}

/* Reconfigure with assertion categories changed from enabled->disabled and vice-versa. */
TEST_CASE("Reconfigure: Transition cases", "[assertions]")
{
    ConnectionWrapper conn(DB_HOME, "create, extra_diagnostics= [prepared, key_out_of_order]");
    auto connection = conn.getWtConnection();
    WT_SESSION_IMPL *session = conn.createSession();

    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_PREPARED) == true);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_KEY_OUT_OF_ORDER) == true);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_CHECKPOINT_VALIDATE) == false);

    connection->reconfigure(
      connection, "extra_diagnostics=[checkpoint_validate, key_out_of_order, slow_operation]");
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_KEY_OUT_OF_ORDER) == true);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_CHECKPOINT_VALIDATE) == true);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_SLOW_OPERATION) == true);

    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_PREPARED) == false);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_TXN_VISIBILITY) == false);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_LOG_VALIDATE) == false);
    REQUIRE(EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_EVICTION_CHECK) == false);
}
