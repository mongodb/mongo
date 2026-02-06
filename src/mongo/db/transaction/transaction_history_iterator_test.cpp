/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/transaction/transaction_history_iterator.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/init.h"  // IWYU pragma: keep
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/mock_repl_coord_server_fixture.h"
#include "mongo/db/repl/oplog_entry.h"
#include "mongo/db/repl/oplog_entry_gen.h"
#include "mongo/db/repl/oplog_entry_test_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#include <memory>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {

using SessionHistoryIteratorTest = MockReplCoordServerFixture;
using IncludeCommitTimestamp = TransactionHistoryIterator::IncludeCommitTimestamp;

namespace {

/**
 * Creates an OplogEntry with defaults specific to this test suite.
 */
repl::OplogEntry makeOplogEntry(repl::OpTime opTime,
                                BSONObj docToInsert,
                                boost::optional<repl::OpTime> prevWriteOpTimeInTransaction) {
    return {repl::DurableOplogEntry(
        opTime,                                                 // optime
        repl::OpTypeEnum::kInsert,                              // opType
        NamespaceString::createNamespaceString_forTest("a.b"),  // namespace
        boost::none,                                            // uuid
        boost::none,                                            // fromMigrate
        boost::none,                                            // checkExistenceForDiffInsert
        boost::none,                                            // versionContext
        repl::OplogEntry::kOplogVersion,                        // version
        docToInsert,                                            // o
        boost::none,                                            // o2
        {},                                                     // sessionInfo
        boost::none,                                            // upsert
        Date_t(),                                               // wall clock time
        {},                                                     // statement ids
        prevWriteOpTimeInTransaction,  // optime of previous write within same transaction
        boost::none,                   // pre-image optime
        boost::none,                   // post-image optime
        boost::none,                   // ShardId of resharding recipient
        boost::none,                   // _id
        boost::none)};                 // needsRetryImage
}

}  // namespace

TEST_F(SessionHistoryIteratorTest, NormalHistory) {
    auto entry1 = makeOplogEntry(repl::OpTime(Timestamp(52, 345), 2),  // optime
                                 BSON("x" << 30),                      // o
                                 repl::OpTime());  //  optime of previous write in transaction

    insertOplogEntry(entry1);

    auto entry2 = makeOplogEntry(
        repl::OpTime(Timestamp(67, 54801), 2),  // optime
        BSON("y" << 50),                        // o
        repl::OpTime(Timestamp(52, 345), 2));   // optime of previous write in transaction
    insertOplogEntry(entry2);

    // Insert an unrelated entry in between
    auto entry3 = makeOplogEntry(
        repl::OpTime(Timestamp(83, 2), 2),    // optime
        BSON("z" << 40),                      // o
        repl::OpTime(Timestamp(22, 67), 2));  // optime of previous write in transaction
    insertOplogEntry(entry3);

    auto entry4 = makeOplogEntry(
        repl::OpTime(Timestamp(97, 2472), 2),    // optime
        BSON("a" << 3),                          // o
        repl::OpTime(Timestamp(67, 54801), 2));  // optime of previous write in transaction
    insertOplogEntry(entry4);

    TransactionHistoryIterator iter(repl::OpTime(Timestamp(97, 2472), 2), true);

    {
        ASSERT_TRUE(iter.hasNext());
        auto nextEntry = iter.next(opCtx());
        ASSERT_EQ(repl::OpTime(Timestamp(97, 2472), 2), nextEntry.getOpTime());
        ASSERT_BSONOBJ_EQ(BSON("a" << 3), nextEntry.getObject());
    }

    {
        ASSERT_TRUE(iter.hasNext());
        auto nextEntry = iter.next(opCtx());
        ASSERT_EQ(repl::OpTime(Timestamp(67, 54801), 2), nextEntry.getOpTime());
        ASSERT_BSONOBJ_EQ(BSON("y" << 50), nextEntry.getObject());
    }

    {
        ASSERT_TRUE(iter.hasNext());
        auto nextEntry = iter.next(opCtx());
        ASSERT_EQ(repl::OpTime(Timestamp(52, 345), 2), nextEntry.getOpTime());
        ASSERT_BSONOBJ_EQ(BSON("x" << 30), nextEntry.getObject());
    }

    ASSERT_FALSE(iter.hasNext());
}

TEST_F(SessionHistoryIteratorTest, StartAtZeroTSShouldNotBeAbleToIterate) {
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(67, 54801), 2),  // optime
        BSON("y" << 50),                        // o
        repl::OpTime(Timestamp(52, 345), 1));   // optime of previous write in transaction
    insertOplogEntry(entry);

    TransactionHistoryIterator iter({});
    ASSERT_FALSE(iter.hasNext());
}

TEST_F(SessionHistoryIteratorTest, NextShouldAssertIfHistoryIsTruncated) {
    auto entry = makeOplogEntry(
        repl::OpTime(Timestamp(67, 54801), 2),  // optime
        BSON("y" << 50),                        // o
        repl::OpTime(Timestamp(52, 345), 1));   // optime of previous write in transaction
    insertOplogEntry(entry);

    repl::OpTime opTime(Timestamp(67, 54801), 2);
    TransactionHistoryIterator iter(opTime, true);
    ASSERT_TRUE(iter.hasNext());

    auto nextEntry = iter.next(opCtx());
    ASSERT_EQ(opTime, nextEntry.getOpTime());
    ASSERT_BSONOBJ_EQ(BSON("y" << 50), nextEntry.getObject());

    ASSERT_TRUE(iter.hasNext());
    ASSERT_THROWS_CODE(
        iter.next(opCtx()), AssertionException, ErrorCodes::IncompleteTransactionHistory);
}

TEST_F(SessionHistoryIteratorTest, OplogInWriteHistoryChainWithMissingPrevTSShouldAssert) {
    auto entry = makeOplogEntry(repl::OpTime(Timestamp(67, 54801), 2),  // optime
                                BSON("y" << 50),                        // o
                                boost::none);  // optime of previous write in transaction
    insertOplogEntry(entry);

    TransactionHistoryIterator iter(repl::OpTime(Timestamp(67, 54801), 2), true);
    ASSERT_TRUE(iter.hasNext());
    ASSERT_THROWS_CODE(iter.next(opCtx()), AssertionException, ErrorCodes::FailedToParse);
}

TEST_F(SessionHistoryIteratorTest, CommittedUnpreparedTransactionSingleApplyOps) {
    auto lsid = makeLogicalSessionIdForTest();
    auto txnNumber = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto uuid = UUID::gen();
    std::vector<repl::ReplOperation> ops;
    auto insertOp = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 1 << "x" << 10), BSON("_id" << 1));
    ops.push_back(insertOp);

    auto applyOpsOpTime = repl::OpTime(Timestamp(100, 0), 1);
    auto applyOpsEntry = repl::makeApplyOpsOplogEntry(applyOpsOpTime,
                                                      ops,
                                                      sessionInfo,
                                                      Date_t::now(),
                                                      {},              // stmtIds
                                                      repl::OpTime(),  // prevWriteOpTime
                                                      boost::none,     // multiOpType
                                                      repl::ApplyOpsType::kTerminal);
    insertOplogEntry(applyOpsEntry);
    auto commitTimestamp = applyOpsOpTime.getTimestamp();

    for (auto includeCommitTimestamp :
         {IncludeCommitTimestamp::kYes, IncludeCommitTimestamp::kNo}) {
        LOGV2(11910601,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "includeCommitTimestamp"_attr = includeCommitTimestamp);

        TransactionHistoryIterator iter(
            applyOpsOpTime, true /* permitYield */, includeCommitTimestamp);
        ASSERT_TRUE(iter.hasNext());

        auto entry = iter.next(opCtx());
        ASSERT_EQ(entry.getOpTime(), applyOpsOpTime);
        ASSERT_TRUE(entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);

        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_EQ(entry.getCommitTransactionTimestamp(), commitTimestamp);
        } else {
            ASSERT_FALSE(entry.getCommitTransactionTimestamp());
        }

        ASSERT_FALSE(iter.hasNext());
    }
}

TEST_F(SessionHistoryIteratorTest, CommittedUnpreparedTransactionMultiApplyOps) {
    auto lsid = makeLogicalSessionIdForTest();
    auto txnNumber = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto uuid = UUID::gen();
    std::vector<repl::ReplOperation> ops0;
    auto insertOp0 = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 1 << "x" << 10), BSON("_id" << 1));
    ops0.push_back(insertOp0);

    auto applyOpsOpTime0 = repl::OpTime(Timestamp(100, 0), 1);
    auto applyOpsEntry0 = repl::makeApplyOpsOplogEntry(applyOpsOpTime0,
                                                       ops0,
                                                       sessionInfo,
                                                       Date_t::now(),
                                                       {},              // stmtIds
                                                       repl::OpTime(),  // prevWriteOpTime
                                                       boost::none,     // multiOpType
                                                       repl::ApplyOpsType::kPartial);
    insertOplogEntry(applyOpsEntry0);

    std::vector<repl::ReplOperation> ops1;
    auto insertOp1 = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 2 << "x" << 20), BSON("_id" << 2));
    ops1.push_back(insertOp1);

    auto applyOpsOpTime1 = repl::OpTime(Timestamp(100, 1), 1);
    auto applyOpsEntry1 = repl::makeApplyOpsOplogEntry(applyOpsOpTime1,
                                                       ops1,
                                                       sessionInfo,
                                                       Date_t::now(),
                                                       {},               // stmtIds
                                                       applyOpsOpTime0,  // prevWriteOpTime
                                                       boost::none,      // multiOpType
                                                       repl::ApplyOpsType::kTerminal);
    insertOplogEntry(applyOpsEntry1);
    auto commitTimestamp = applyOpsOpTime1.getTimestamp();

    for (auto includeCommitTimestamp :
         {IncludeCommitTimestamp::kYes, IncludeCommitTimestamp::kNo}) {
        LOGV2(11910602,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "includeCommitTimestamp"_attr = includeCommitTimestamp);

        TransactionHistoryIterator iter(
            applyOpsOpTime1, true /* permitYield */, includeCommitTimestamp);
        ASSERT_TRUE(iter.hasNext());

        auto entry1 = iter.next(opCtx());
        ASSERT_EQ(entry1.getOpTime(), applyOpsOpTime1);
        ASSERT_TRUE(entry1.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_EQ(entry1.getCommitTransactionTimestamp(), commitTimestamp);
        } else {
            ASSERT_FALSE(entry1.getCommitTransactionTimestamp());
        }

        ASSERT_TRUE(iter.hasNext());

        auto entry0 = iter.next(opCtx());
        ASSERT_EQ(entry0.getOpTime(), applyOpsOpTime0);
        ASSERT_TRUE(entry0.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_EQ(entry0.getCommitTransactionTimestamp(), commitTimestamp);
        } else {
            ASSERT_FALSE(entry0.getCommitTransactionTimestamp());
        }

        ASSERT_FALSE(iter.hasNext());
    }
}

TEST_F(SessionHistoryIteratorTest, CommittedPreparedTransactionSingleApplyOps) {
    auto lsid = makeLogicalSessionIdForTest();
    auto txnNumber = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto uuid = UUID::gen();
    std::vector<repl::ReplOperation> ops;
    auto insertOp = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 1 << "x" << 10), BSON("_id" << 1));
    ops.push_back(insertOp);

    auto applyOpsOpTime = repl::OpTime(Timestamp(100, 0), 1);
    auto applyOpsEntry = repl::makeApplyOpsOplogEntry(applyOpsOpTime,
                                                      ops,
                                                      sessionInfo,
                                                      Date_t::now(),
                                                      {},              // stmtIds
                                                      repl::OpTime(),  // prevWriteOpTime
                                                      boost::none,     // multiOpType
                                                      repl::ApplyOpsType::kPrepare);
    insertOplogEntry(applyOpsEntry);

    auto commitTimestamp = Timestamp(100, 1);
    auto commitTxnOpTime = repl::OpTime(Timestamp(100, 2), 1);
    auto commitTxnEntry = repl::makeCommitTransactionOplogEntry(
        commitTxnOpTime, sessionInfo, commitTimestamp, applyOpsOpTime);
    insertOplogEntry(commitTxnEntry);

    for (auto includeCommitTimestamp :
         {IncludeCommitTimestamp::kYes, IncludeCommitTimestamp::kNo}) {
        LOGV2(11910603,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "includeCommitTimestamp"_attr = includeCommitTimestamp);

        TransactionHistoryIterator iter(
            commitTxnOpTime, true /* permitYield */, includeCommitTimestamp);
        ASSERT_TRUE(iter.hasNext());

        auto entry1 = iter.next(opCtx());
        ASSERT_EQ(entry1.getOpTime(), commitTxnOpTime);
        ASSERT_TRUE(entry1.getCommandType() == repl::OplogEntry::CommandType::kCommitTransaction);
        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_EQ(entry1.getCommitTransactionTimestamp(), commitTimestamp);
        } else {
            ASSERT_FALSE(entry1.getCommitTransactionTimestamp());
        }

        ASSERT_TRUE(iter.hasNext());

        auto entry0 = iter.next(opCtx());
        ASSERT_EQ(entry0.getOpTime(), applyOpsOpTime);
        ASSERT_TRUE(entry0.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_EQ(entry0.getCommitTransactionTimestamp(), commitTimestamp);
        } else {
            ASSERT_FALSE(entry0.getCommitTransactionTimestamp());
        }

        ASSERT_FALSE(iter.hasNext());
    }
}

TEST_F(SessionHistoryIteratorTest, CommittedPreparedTransactionMultiApplyOps) {
    auto lsid = makeLogicalSessionIdForTest();
    auto txnNumber = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto uuid = UUID::gen();
    std::vector<repl::ReplOperation> ops0;
    auto insertOp0 = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 1 << "x" << 10), BSON("_id" << 1));
    ops0.push_back(insertOp0);

    auto applyOpsOpTime0 = repl::OpTime(Timestamp(100, 0), 1);
    auto applyOpsEntry0 = repl::makeApplyOpsOplogEntry(applyOpsOpTime0,
                                                       ops0,
                                                       sessionInfo,
                                                       Date_t::now(),
                                                       {},              // stmtIds
                                                       repl::OpTime(),  // prevWriteOpTime
                                                       boost::none,     // multiOpType
                                                       repl::ApplyOpsType::kPartial);
    insertOplogEntry(applyOpsEntry0);

    std::vector<repl::ReplOperation> ops1;
    auto insertOp1 = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 2 << "x" << 20), BSON("_id" << 2));
    ops1.push_back(insertOp1);

    auto applyOpsOpTime1 = repl::OpTime(Timestamp(100, 1), 1);
    auto applyOpsEntry1 = repl::makeApplyOpsOplogEntry(applyOpsOpTime1,
                                                       ops1,
                                                       sessionInfo,
                                                       Date_t::now(),
                                                       {},               // stmtIds
                                                       applyOpsOpTime0,  // prevWriteOpTime
                                                       boost::none,      // multiOpType
                                                       repl::ApplyOpsType::kPrepare);
    insertOplogEntry(applyOpsEntry1);

    auto commitTimestamp = Timestamp(100, 2);
    auto commitTxnOpTime = repl::OpTime(Timestamp(100, 3), 1);
    auto commitTxnEntry = repl::makeCommitTransactionOplogEntry(
        commitTxnOpTime, sessionInfo, commitTimestamp, applyOpsOpTime1);
    insertOplogEntry(commitTxnEntry);

    for (auto includeCommitTimestamp :
         {IncludeCommitTimestamp::kYes, IncludeCommitTimestamp::kNo}) {
        LOGV2(11910604,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "includeCommitTimestamp"_attr = includeCommitTimestamp);

        TransactionHistoryIterator iter(
            commitTxnOpTime, true /* permitYield */, includeCommitTimestamp);
        ASSERT_TRUE(iter.hasNext());

        auto entry2 = iter.next(opCtx());
        ASSERT_EQ(entry2.getOpTime(), commitTxnOpTime);
        ASSERT_TRUE(entry2.getCommandType() == repl::OplogEntry::CommandType::kCommitTransaction);
        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_EQ(entry2.getCommitTransactionTimestamp(), commitTimestamp);
        } else {
            ASSERT_FALSE(entry2.getCommitTransactionTimestamp());
        }

        ASSERT_TRUE(iter.hasNext());

        auto entry1 = iter.next(opCtx());
        ASSERT_EQ(entry1.getOpTime(), applyOpsOpTime1);
        ASSERT_TRUE(entry1.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_EQ(entry1.getCommitTransactionTimestamp(), commitTimestamp);
        } else {
            ASSERT_FALSE(entry1.getCommitTransactionTimestamp());
        }

        ASSERT_TRUE(iter.hasNext());

        auto entry0 = iter.next(opCtx());
        ASSERT_EQ(entry0.getOpTime(), applyOpsOpTime0);
        ASSERT_TRUE(entry0.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_EQ(entry0.getCommitTransactionTimestamp(), commitTimestamp);
        } else {
            ASSERT_FALSE(entry0.getCommitTransactionTimestamp());
        }

        ASSERT_FALSE(iter.hasNext());
    }
}

TEST_F(SessionHistoryIteratorTest, AbortedPreparedTransaction) {
    auto lsid = makeLogicalSessionIdForTest();
    auto txnNumber = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto uuid = UUID::gen();
    std::vector<repl::ReplOperation> ops;
    auto insertOp = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 1 << "x" << 10), BSON("_id" << 1));
    ops.push_back(insertOp);

    auto applyOpsOpTime = repl::OpTime(Timestamp(100, 0), 1);
    auto applyOpsEntry = repl::makeApplyOpsOplogEntry(applyOpsOpTime,
                                                      ops,
                                                      sessionInfo,
                                                      Date_t::now(),
                                                      {},              // stmtIds
                                                      repl::OpTime(),  // prevWriteOpTime
                                                      boost::none,     // multiOpType
                                                      repl::ApplyOpsType::kPrepare);
    insertOplogEntry(applyOpsEntry);

    auto abortTxnOpTime = repl::OpTime(Timestamp(100, 1), 1);
    auto abortTxnEntry =
        repl::makeAbortTransactionOplogEntry(abortTxnOpTime, sessionInfo, applyOpsOpTime);
    insertOplogEntry(abortTxnEntry);

    for (auto includeCommitTimestamp :
         {IncludeCommitTimestamp::kYes, IncludeCommitTimestamp::kNo}) {
        LOGV2(11910605,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "includeCommitTimestamp"_attr = includeCommitTimestamp);

        TransactionHistoryIterator iter(
            abortTxnOpTime, true /* permitYield */, includeCommitTimestamp);
        ASSERT_TRUE(iter.hasNext());

        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_THROWS_CODE(iter.next(opCtx()), DBException, 11910612);
            continue;
        }

        auto entry1 = iter.next(opCtx());
        ASSERT_EQ(entry1.getOpTime(), abortTxnOpTime);
        ASSERT_TRUE(entry1.getCommandType() == repl::OplogEntry::CommandType::kAbortTransaction);
        ASSERT_FALSE(entry1.getCommitTransactionTimestamp());

        ASSERT_TRUE(iter.hasNext());

        auto entry0 = iter.next(opCtx());
        ASSERT_EQ(entry0.getOpTime(), applyOpsOpTime);
        ASSERT_TRUE(entry0.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        ASSERT_FALSE(entry0.getCommitTransactionTimestamp());

        ASSERT_FALSE(iter.hasNext());
    }
}

TEST_F(SessionHistoryIteratorTest, UncommittedPreparedTransaction) {
    auto lsid = makeLogicalSessionIdForTest();
    auto txnNumber = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto uuid = UUID::gen();
    std::vector<repl::ReplOperation> ops0;
    auto insertOp0 = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 1 << "x" << 10), BSON("_id" << 1));
    ops0.push_back(insertOp0);

    auto applyOpsOpTime = repl::OpTime(Timestamp(100, 0), 1);
    auto applyOpsEntry = repl::makeApplyOpsOplogEntry(applyOpsOpTime,
                                                      ops0,
                                                      sessionInfo,
                                                      Date_t::now(),
                                                      {},              // stmtIds
                                                      repl::OpTime(),  // prevWriteOpTime
                                                      boost::none,     // multiOpType
                                                      repl::ApplyOpsType::kPrepare);
    insertOplogEntry(applyOpsEntry);

    for (auto includeCommitTimestamp :
         {IncludeCommitTimestamp::kYes, IncludeCommitTimestamp::kNo}) {
        LOGV2(11910606,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "includeCommitTimestamp"_attr = includeCommitTimestamp);

        TransactionHistoryIterator iter(
            applyOpsOpTime, true /* permitYield */, includeCommitTimestamp);
        ASSERT_TRUE(iter.hasNext());

        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_THROWS_CODE(iter.next(opCtx()), DBException, 11910613);
            continue;
        }

        auto entry = iter.next(opCtx());
        ASSERT_EQ(entry.getOpTime(), applyOpsOpTime);
        ASSERT_TRUE(entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        ASSERT_FALSE(entry.getCommitTransactionTimestamp());

        ASSERT_FALSE(iter.hasNext());
    }
}

TEST_F(SessionHistoryIteratorTest, UncommittedPartialTransactionSingleApplyOps) {
    auto lsid = makeLogicalSessionIdForTest();
    auto txnNumber = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto uuid = UUID::gen();
    std::vector<repl::ReplOperation> ops0;
    auto insertOp0 = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 1 << "x" << 10), BSON("_id" << 1));
    ops0.push_back(insertOp0);

    auto applyOpsOpTime = repl::OpTime(Timestamp(100, 0), 1);
    auto applyOpsEntry = repl::makeApplyOpsOplogEntry(applyOpsOpTime,
                                                      ops0,
                                                      sessionInfo,
                                                      Date_t::now(),
                                                      {},              // stmtIds
                                                      repl::OpTime(),  // prevWriteOpTime
                                                      boost::none,     // multiOpType
                                                      repl::ApplyOpsType::kPartial);
    insertOplogEntry(applyOpsEntry);

    for (auto includeCommitTimestamp :
         {IncludeCommitTimestamp::kYes, IncludeCommitTimestamp::kNo}) {
        LOGV2(11910607,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "includeCommitTimestamp"_attr = includeCommitTimestamp);

        TransactionHistoryIterator iter(
            applyOpsOpTime, true /* permitYield */, includeCommitTimestamp);
        ASSERT_TRUE(iter.hasNext());

        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_THROWS_CODE(iter.next(opCtx()), DBException, 11910613);
            continue;
        }

        auto entry = iter.next(opCtx());
        ASSERT_EQ(entry.getOpTime(), applyOpsOpTime);
        ASSERT_TRUE(entry.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        ASSERT_FALSE(entry.getCommitTransactionTimestamp());

        ASSERT_FALSE(iter.hasNext());
    }
}

TEST_F(SessionHistoryIteratorTest, UncommittedPartialTransactionMultiApplyOps) {
    auto lsid = makeLogicalSessionIdForTest();
    auto txnNumber = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto uuid = UUID::gen();
    std::vector<repl::ReplOperation> ops0;
    auto insertOp0 = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 1 << "x" << 10), BSON("_id" << 1));
    ops0.push_back(insertOp0);

    auto applyOpsOpTime0 = repl::OpTime(Timestamp(100, 0), 1);
    auto applyOpsEntry0 = repl::makeApplyOpsOplogEntry(applyOpsOpTime0,
                                                       ops0,
                                                       sessionInfo,
                                                       Date_t::now(),
                                                       {},              // stmtIds
                                                       repl::OpTime(),  // prevWriteOpTime
                                                       boost::none,     // multiOpType
                                                       repl::ApplyOpsType::kPartial);
    insertOplogEntry(applyOpsEntry0);

    std::vector<repl::ReplOperation> ops1;
    auto insertOp1 = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 2 << "x" << 20), BSON("_id" << 2));
    ops1.push_back(insertOp1);

    auto applyOpsOpTime1 = repl::OpTime(Timestamp(100, 1), 1);
    auto applyOpsEntry1 = repl::makeApplyOpsOplogEntry(applyOpsOpTime1,
                                                       ops1,
                                                       sessionInfo,
                                                       Date_t::now(),
                                                       {},               // stmtIds
                                                       applyOpsOpTime0,  // prevWriteOpTime
                                                       boost::none,      // multiOpType
                                                       repl::ApplyOpsType::kPartial);
    insertOplogEntry(applyOpsEntry1);

    for (auto includeCommitTimestamp :
         {IncludeCommitTimestamp::kYes, IncludeCommitTimestamp::kNo}) {
        LOGV2(11910608,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "includeCommitTimestamp"_attr = includeCommitTimestamp);

        TransactionHistoryIterator iter(
            applyOpsOpTime1, true /* permitYield */, includeCommitTimestamp);
        ASSERT_TRUE(iter.hasNext());

        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_THROWS_CODE(iter.next(opCtx()), DBException, 11910613);
            continue;
        }

        auto entry1 = iter.next(opCtx());
        ASSERT_EQ(entry1.getOpTime(), applyOpsOpTime1);
        ASSERT_TRUE(entry1.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        ASSERT_FALSE(entry1.getCommitTransactionTimestamp());

        ASSERT_TRUE(iter.hasNext());

        auto entry0 = iter.next(opCtx());
        ASSERT_EQ(entry0.getOpTime(), applyOpsOpTime0);
        ASSERT_TRUE(entry0.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        ASSERT_FALSE(entry0.getCommitTransactionTimestamp());

        ASSERT_FALSE(iter.hasNext());
    }
}

TEST_F(SessionHistoryIteratorTest, RetryableWritesNotApplyOps) {
    auto lsid = makeLogicalSessionIdForTest();
    auto txnNumber = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto uuid = UUID::gen();

    auto stmtId = 1;
    auto insertOpTime = repl::OpTime(Timestamp(100, 1), 1);
    auto insertEntry = repl::makeInsertDocumentOplogEntryWithSessionInfoAndStmtIds(
        insertOpTime,
        nss,
        uuid,
        BSON("_id" << 1 << "x" << 10),
        lsid,
        txnNumber,
        {stmtId},         // stmtIds
        repl::OpTime());  // prevWriteOpTime
    insertOplogEntry(insertEntry);

    for (auto includeCommitTimestamp :
         {IncludeCommitTimestamp::kYes, IncludeCommitTimestamp::kNo}) {
        LOGV2(11910609,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "includeCommitTimestamp"_attr = includeCommitTimestamp);

        TransactionHistoryIterator iter(
            insertOpTime, true /* permitYield */, includeCommitTimestamp);
        ASSERT_TRUE(iter.hasNext());

        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_THROWS_CODE(iter.next(opCtx()), DBException, 11910611);
            continue;
        }

        auto entry = iter.next(opCtx());
        ASSERT_EQ(entry.getOpTime(), insertOpTime);
        ASSERT_TRUE(entry.getOpType() == repl::OpTypeEnum::kInsert);
        ASSERT_FALSE(entry.getCommitTransactionTimestamp());

        ASSERT_FALSE(iter.hasNext());
    }
}

TEST_F(SessionHistoryIteratorTest, RetryableWritesApplyOps) {
    auto lsid = makeLogicalSessionIdForTest();
    auto txnNumber = 1LL;
    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(txnNumber);

    auto nss = NamespaceString::createNamespaceString_forTest("testDb.testColl");
    auto uuid = UUID::gen();
    std::vector<repl::ReplOperation> ops0;
    auto insertOp0 = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 1 << "x" << 10), BSON("_id" << 1));
    ops0.push_back(insertOp0);

    auto applyOpsOpTime0 = repl::OpTime(Timestamp(100, 0), 1);
    auto applyOpsEntry0 =
        repl::makeApplyOpsOplogEntry(applyOpsOpTime0,
                                     ops0,
                                     sessionInfo,
                                     Date_t::now(),
                                     {},              // stmtIds
                                     repl::OpTime(),  // prevWriteOpTime
                                     repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);
    insertOplogEntry(applyOpsEntry0);

    std::vector<repl::ReplOperation> ops1;
    auto insertOp1 = repl::MutableOplogEntry::makeInsertOperation(
        nss, uuid, BSON("_id" << 2 << "x" << 20), BSON("_id" << 2));
    ops1.push_back(insertOp1);

    auto applyOpsOpTime1 = repl::OpTime(Timestamp(100, 1), 1);
    auto applyOpsEntry1 =
        repl::makeApplyOpsOplogEntry(applyOpsOpTime1,
                                     ops1,
                                     sessionInfo,
                                     Date_t::now(),
                                     {},               // stmtIds
                                     applyOpsOpTime0,  // prevWriteOpTime
                                     repl::MultiOplogEntryType::kApplyOpsAppliedSeparately);
    insertOplogEntry(applyOpsEntry1);

    for (auto includeCommitTimestamp :
         {IncludeCommitTimestamp::kYes, IncludeCommitTimestamp::kNo}) {
        LOGV2(11910610,
              "Running case",
              "test"_attr = unittest::getTestName(),
              "includeCommitTimestamp"_attr = includeCommitTimestamp);

        TransactionHistoryIterator iter(
            applyOpsOpTime1, true /* permitYield */, includeCommitTimestamp);
        ASSERT_TRUE(iter.hasNext());

        if (includeCommitTimestamp == IncludeCommitTimestamp::kYes) {
            ASSERT_THROWS_CODE(iter.next(opCtx()), DBException, 11910611);
            continue;
        }

        auto entry1 = iter.next(opCtx());
        ASSERT_EQ(entry1.getOpTime(), applyOpsOpTime1);
        ASSERT_TRUE(entry1.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        ASSERT_FALSE(entry1.getCommitTransactionTimestamp());

        ASSERT_TRUE(iter.hasNext());

        auto entry0 = iter.next(opCtx());
        ASSERT_EQ(entry0.getOpTime(), applyOpsOpTime0);
        ASSERT_TRUE(entry0.getCommandType() == repl::OplogEntry::CommandType::kApplyOps);
        ASSERT_FALSE(entry0.getCommitTransactionTimestamp());

        ASSERT_FALSE(iter.hasNext());
    }
}

}  // namespace mongo
