// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/session/session_txn_record_helpers.h"

#include "mongo/db/repl/optime.h"
#include "mongo/db/replicated_fast_count/durable_size_metadata_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(SessionTxnRecordForPrepareRecoveryTest, ValidatesNecessaryFields) {
    {
        // Create a session txn record without affectedNamespaces.
        SessionTxnRecord txnRecord;
        txnRecord.setState(DurableTxnStateEnum::kPrepared);
        txnRecord.setSessionId(makeLogicalSessionIdForTest());
        txnRecord.setTxnNum(11);
        txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(1, 0), 0));
        txnRecord.setLastWriteDate(Date_t::now());

        ASSERT_THROWS_CODE(
            SessionTxnRecordForPrepareRecovery(std::move(txnRecord)), AssertionException, 11372904);
    }

    {
        // Create a session txn record without a state.
        SessionTxnRecord txnRecord;
        txnRecord.setSessionId(makeLogicalSessionIdForTest());
        txnRecord.setTxnNum(11);
        txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(1, 0), 0));
        txnRecord.setLastWriteDate(Date_t::now());
        txnRecord.setAffectedNamespaces({{}});

        ASSERT_THROWS_CODE(
            SessionTxnRecordForPrepareRecovery(std::move(txnRecord)), AssertionException, 11372905);
    }

    {
        // Create a session txn record with a state other than prepared.
        SessionTxnRecord txnRecord;
        txnRecord.setState(DurableTxnStateEnum::kInProgress);
        txnRecord.setSessionId(makeLogicalSessionIdForTest());
        txnRecord.setTxnNum(11);
        txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(1, 0), 0));
        txnRecord.setLastWriteDate(Date_t::now());
        txnRecord.setAffectedNamespaces({{}});

        ASSERT_THROWS_CODE(
            SessionTxnRecordForPrepareRecovery(std::move(txnRecord)), AssertionException, 11372906);
    }
}

TEST(SessionTxnRecordForPrepareRecoveryTest, PrepareTimestampEqualsLastWriteOpTimestamp) {
    SessionTxnRecord txnRecord;
    txnRecord.setState(DurableTxnStateEnum::kPrepared);
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(11);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(1, 0), 0));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setAffectedNamespaces({{}});

    SessionTxnRecordForPrepareRecovery txnRecordForRecovery(std::move(txnRecord));
    ASSERT_EQ(txnRecordForRecovery.getPrepareTimestamp(),
              txnRecord.getLastWriteOpTime().getTimestamp());
}

TEST(SessionTxnRecordForPrepareRecoveryTest, PreparedSizeMetadataSurvivesRecovery) {
    const auto collUuid = UUID::gen();
    const int64_t expectedSz = 1024;
    const int64_t expectedCt = 3;

    MultiOpSizeMetadata meta;
    meta.setUuid(collUuid);
    meta.setSz(expectedSz);
    meta.setCt(expectedCt);

    SessionTxnRecord txnRecord;
    txnRecord.setState(DurableTxnStateEnum::kPrepared);
    txnRecord.setSessionId(makeLogicalSessionIdForTest());
    txnRecord.setTxnNum(11);
    txnRecord.setLastWriteOpTime(repl::OpTime(Timestamp(1, 0), 0));
    txnRecord.setLastWriteDate(Date_t::now());
    txnRecord.setAffectedNamespaces({{}});
    txnRecord.setSizeMetadata(std::vector<MultiOpSizeMetadata>{meta});

    SessionTxnRecordForPrepareRecovery txnRecordForRecovery(std::move(txnRecord));

    const auto& sizeMetadata = txnRecordForRecovery.getSizeMetadata();
    ASSERT_TRUE(sizeMetadata.has_value());
    ASSERT_EQ(sizeMetadata->size(), 1u);
    ASSERT_EQ((*sizeMetadata)[0].getUuid(), collUuid);
    ASSERT_EQ((*sizeMetadata)[0].getSz(), expectedSz);
    ASSERT_EQ((*sizeMetadata)[0].getCt(), expectedCt);
}

}  // namespace
}  // namespace mongo
