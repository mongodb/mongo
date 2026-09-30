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

static constexpr const char *k_db = "WT_TEST.ckpt_snapshot_size";

TEST_CASE("WT_TXN_LOG_CKPT_START must set ckpt_snapshot->size to the number of encoded bytes",
  "[checkpoint][txn_log]")
{
    std::filesystem::remove_all(k_db);
    connection_wrapper conn(k_db, "create,log=(enabled=true)");
    WT_SESSION_IMPL *session = conn.create_session();
    WT_TXN *txn = session->txn;
    REQUIRE(txn != nullptr);

    REQUIRE(__wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_PREPARE, nullptr) == 0);
    REQUIRE(txn->full_ckpt);

    constexpr uint64_t k_txn_id = 42;
    txn->snapshot_data.snapshot_count = 1;
    txn->snapshot_data.snapshot[0] = k_txn_id;

    REQUIRE(__wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_START, nullptr) == 0);
    REQUIRE(txn->ckpt_nsnapshot == 1);
    REQUIRE(txn->ckpt_snapshot != nullptr);

    size_t const expected_size = __wt_vsize_uint(k_txn_id);
    REQUIRE(expected_size > 0);
    REQUIRE(txn->ckpt_snapshot->size == expected_size);

    const uint8_t *p = static_cast<const uint8_t *>(txn->ckpt_snapshot->data);
    uint64_t decoded_id = 0;
    REQUIRE(__wt_vunpack_uint(&p, txn->ckpt_snapshot->size, &decoded_id) == 0);
    CHECK(decoded_id == k_txn_id);

    txn->snapshot_data.snapshot_count = 0;
    WT_IGNORE_RET(__wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_CLEANUP, nullptr));
}
