/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include "wt_internal.h"
#include "../wrappers/connection_wrapper.h"

TEST_CASE("operation_timeout_us is cleared after a failed begin_transaction", "[txn_config]")
{
    connection_wrapper conn("WT_TEST.txn_config_timeout");
    WT_SESSION_IMPL *session = conn.create_session();
    WT_TXN *txn = session->txn;

    /* The timeout should start at zero. */
    REQUIRE(txn->operation_timeout_us == 0);

    /*
     * Begin a transaction with a valid operation_timeout_ms but an invalid read_timestamp. The
     * configuration will fail after the timeout has been stored.
     */
    REQUIRE(session->iface.begin_transaction(
              &session->iface, "operation_timeout_ms=100,read_timestamp=0") == EINVAL);

    /*
     * The timeout should be cleared on the error path; otherwise it leaks into the next
     * transaction.
     */
    REQUIRE(txn->operation_timeout_us == 0);

    /* A subsequent begin_transaction should not inherit the stale timeout. */
    REQUIRE(session->iface.begin_transaction(&session->iface, NULL) == 0);
    REQUIRE(txn->operation_timeout_us == 0);

    /* Rollback to clean up. */
    REQUIRE(session->iface.rollback_transaction(&session->iface, NULL) == 0);
}

TEST_CASE("ignore_cache_size is scoped to the transaction that set it", "[txn_config]")
{
    connection_wrapper conn("WT_TEST.txn_config_ignore_cache_size");
    WT_SESSION_IMPL *session = conn.create_session();

    REQUIRE(!F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));

    SECTION("cleared when the transaction is resolved")
    {
        REQUIRE(session->iface.begin_transaction(&session->iface, "ignore_cache_size=true") == 0);
        REQUIRE(F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));
        REQUIRE(session->iface.commit_transaction(&session->iface, NULL) == 0);
        REQUIRE(!F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));

        REQUIRE(session->iface.begin_transaction(&session->iface, "ignore_cache_size=true") == 0);
        REQUIRE(F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));
        REQUIRE(session->iface.rollback_transaction(&session->iface, NULL) == 0);
        REQUIRE(!F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));
    }

    SECTION("cleared on a failed begin_transaction")
    {
        REQUIRE(session->iface.begin_transaction(
                  &session->iface, "ignore_cache_size=true,read_timestamp=0") == EINVAL);
        REQUIRE(!F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));
    }

    SECTION("cleared when begin_transaction fails after the configuration was applied")
    {
        /*
         * Claiming a prepared transaction that does not exist fails after the configuration has
         * been applied, and the transaction never starts, so nothing else will release it.
         */
        REQUIRE(session->iface.begin_transaction(
                  &session->iface, "ignore_cache_size=true,claim_prepared_id=1") != 0);
        REQUIRE(!F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));

        /*
         * The failure must not leave the exemption recorded on the transaction: a later transaction
         * would then drop a session-level setting when it is released.
         */
        REQUIRE(session->iface.reconfigure(&session->iface, "ignore_cache_size=true") == 0);
        REQUIRE(session->iface.begin_transaction(&session->iface, NULL) == 0);
        REQUIRE(session->iface.commit_transaction(&session->iface, NULL) == 0);
        REQUIRE(F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));
    }

    SECTION("a session-level setting outlives the transaction")
    {
        REQUIRE(session->iface.reconfigure(&session->iface, "ignore_cache_size=true") == 0);
        REQUIRE(F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));
        REQUIRE(session->iface.begin_transaction(&session->iface, "ignore_cache_size=true") == 0);
        REQUIRE(session->iface.commit_transaction(&session->iface, NULL) == 0);
        REQUIRE(F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));
    }

    SECTION("a session-level reconfigure is rejected while a transaction is active")
    {
        REQUIRE(session->iface.begin_transaction(&session->iface, "ignore_cache_size=true") == 0);
        REQUIRE(session->iface.reconfigure(&session->iface, "ignore_cache_size=true") == EINVAL);
        REQUIRE(session->iface.reconfigure(&session->iface, "ignore_cache_size=false") == EINVAL);
        REQUIRE(F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));
        REQUIRE(session->iface.commit_transaction(&session->iface, NULL) == 0);
        REQUIRE(!F_ISSET(session, WT_SESSION_IGNORE_CACHE_SIZE));
    }
}
