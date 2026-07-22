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
