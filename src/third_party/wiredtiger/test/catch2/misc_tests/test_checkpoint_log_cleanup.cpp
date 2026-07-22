/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include <catch2/catch.hpp>

#include <filesystem>

#include "wiredtiger.h"
#include "../utils.h"
#include "../wrappers/connection_wrapper.h"
#include "wt_internal.h"

static constexpr const char *k_db1 = "test_db_ckpt_log_stop_1";
static constexpr const char *k_db2 = "test_db_ckpt_log_stop_2";

/* Cleanup checkpoint log state on destruction. */
struct ckpt_log_state_guard {
    WT_SESSION_IMPL *session;

    ~ckpt_log_state_guard()
    {
        WT_TXN *txn = session->txn;
        if (txn->full_ckpt || txn->ckpt_snapshot != nullptr)
            WT_IGNORE_RET(__wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_CLEANUP, nullptr));
    }
};

/* Drive the checkpoint log state machine through PREPARE and START. */
static void
run_prepare_and_start(WT_SESSION_IMPL *session)
{
    WT_TXN *txn = session->txn;

    int ret = __wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_PREPARE, nullptr);
    REQUIRE(ret == 0);
    REQUIRE(txn->full_ckpt);

    ret = __wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_START, nullptr);
    REQUIRE(ret == 0);
    REQUIRE(txn->ckpt_snapshot != nullptr);
}

/* Force memory allocation inside STOP to fail by inflating the snapshot size. */
static int
inject_stop_failure(WT_SESSION_IMPL *session)
{
    session->txn->ckpt_snapshot->size = SIZE_MAX >> 2;
    return __wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_STOP, nullptr);
}

TEST_CASE("Checkpoint log STOP failure must reset full_ckpt and free ckpt_snapshot",
  "[checkpoint][txn_log]")
{
    std::filesystem::remove_all(k_db1);
    connection_wrapper conn(k_db1, "create,log=(enabled=true)");
    WT_SESSION_IMPL *session = conn.create_session();
    WT_TXN *txn = session->txn;
    REQUIRE(txn != nullptr);

    REQUIRE_FALSE(txn->full_ckpt);
    REQUIRE(txn->ckpt_snapshot == nullptr);

    run_prepare_and_start(session);
    REQUIRE(txn->full_ckpt);
    REQUIRE(txn->ckpt_snapshot != nullptr);

    ckpt_log_state_guard guard{session};

    int stop_ret = inject_stop_failure(session);
    REQUIRE(stop_ret != 0);

    REQUIRE_FALSE(txn->full_ckpt);
    REQUIRE(txn->ckpt_snapshot == nullptr);
}

TEST_CASE("After checkpoint STOP failure, full_ckpt must not suppress subsequent file-sync logging",
  "[checkpoint][txn_log]")
{
    std::filesystem::remove_all(k_db2);
    connection_wrapper conn(k_db2, "create,log=(enabled=true)");
    WT_SESSION_IMPL *session = conn.create_session();
    WT_TXN *txn = session->txn;
    REQUIRE(txn != nullptr);

    run_prepare_and_start(session);
    REQUIRE(txn->full_ckpt);

    ckpt_log_state_guard guard{session};

    int stop_ret = inject_stop_failure(session);
    REQUIRE(stop_ret != 0);

    REQUIRE_FALSE(txn->full_ckpt);
}
