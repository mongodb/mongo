/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/session/session_txn_record_helpers.h"

#include "mongo/db/repl/optime.h"
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

}  // namespace
}  // namespace mongo
