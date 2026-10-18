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

static constexpr const char *k_db_logrec = "WT_TEST.ckpt_snapshot_logrec_alloc";
static constexpr const char *k_db_stop = "WT_TEST.ckpt_snapshot_stop";

TEST_CASE("A log record buffer holds the record header plus the requested payload", "[txn_log]")
{
    std::filesystem::remove_all(k_db_logrec);
    connection_wrapper conn(k_db_logrec, "create,log=(enabled=true)");
    WT_SESSION_IMPL *session = conn.create_session();

    /* Cover several alignment boundaries, including payloads ending in a boundary's last bytes. */
    for (size_t size = 0; size <= 4 * 128; ++size) {
        WT_ITEM *logrec = nullptr;
        REQUIRE(__wt_logrec_alloc(session, size, &logrec) == 0);
        INFO("payload size " << size);
        CHECK(logrec->memsize >= logrec->size + size);
        __wt_logrec_free(session, &logrec);
    }
}

TEST_CASE(
  "Checkpoint log STOP fits a large snapshot in the log record buffer", "[checkpoint][txn_log]")
{
    std::filesystem::remove_all(k_db_stop);
    connection_wrapper conn(k_db_stop, "create,log=(enabled=true)");
    WT_SESSION_IMPL *session = conn.create_session();
    WT_TXN *txn = session->txn;

    /*
     * Transaction IDs in this range pack to 4 bytes each. Sweep the snapshot size so the packed
     * checkpoint record crosses every position relative to the log alignment.
     */
    const uint32_t max_snapshot =
      WT_MIN(64, static_cast<uint32_t>(S2C(session)->session_array.size));
    for (uint32_t count = 1; count <= max_snapshot; ++count) {
        INFO("snapshot count " << count);
        REQUIRE(__wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_PREPARE, nullptr) == 0);

        txn->snapshot_data.snapshot_count = count;
        for (uint32_t i = 0; i < count; ++i)
            txn->snapshot_data.snapshot[i] = 1001644 + i;
        REQUIRE(__wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_START, nullptr) == 0);
        txn->snapshot_data.snapshot_count = 0;

        /* Size the checkpoint record the same way STOP does and check its buffer can hold it. */
        size_t recsize = 0;
        REQUIRE(__wt_struct_size(session, &recsize, WT_UNCHECKED_STRING(IIIIu),
                  WT_LOGREC_CHECKPOINT, __wt_lsn_file(&txn->ckpt_lsn),
                  __wt_lsn_offset(&txn->ckpt_lsn), txn->ckpt_nsnapshot, txn->ckpt_snapshot) == 0);
        WT_ITEM *logrec = nullptr;
        REQUIRE(__wt_logrec_alloc(session, recsize, &logrec) == 0);
        const bool fits = logrec->memsize >= logrec->size + recsize;
        __wt_logrec_free(session, &logrec);
        /* Stop before STOP packs the record, which would overrun the buffer if it does not fit. */
        REQUIRE(fits);

        REQUIRE(__wt_checkpoint_log(session, true, WT_TXN_LOG_CKPT_STOP, nullptr) == 0);
    }
}
