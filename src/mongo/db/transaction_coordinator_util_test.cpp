
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

#include "mongo/platform/basic.h"

#include "mongo/db/transaction_coordinator_util.h"
#include "mongo/s/shard_server_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const std::string kAbortDecision{"abort"};
const std::string kCommitDecision{"commit"};

class TransactionCoordinatorCollectionTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();
        _lsid = makeLogicalSessionIdForTest();
        _commitTimestamp = Timestamp(Date_t::now().toMillisSinceEpoch() / 1000, 0);
    }

    LogicalSessionId lsid() const {
        return _lsid;
    }

    TxnNumber txnNumber() const {
        return _txnNumber;
    }

    const std::vector<ShardId>& participants() const {
        return _participants;
    }

    const Timestamp& commitTimestamp() const {
        return _commitTimestamp;
    }

    void assertDocumentMatches(TransactionCoordinatorDocument doc,
                               LogicalSessionId expectedLsid,
                               TxnNumber expectedTxnNum,
                               std::vector<ShardId> expectedParticipants,
                               boost::optional<std::string> expectedDecision = boost::none,
                               boost::optional<Timestamp> expectedCommitTimestamp = boost::none) {
        ASSERT(doc.getId().getSessionId());
        ASSERT_EQUALS(*doc.getId().getSessionId(), expectedLsid);
        ASSERT(doc.getId().getTxnNumber());
        ASSERT_EQUALS(*doc.getId().getTxnNumber(), expectedTxnNum);

        ASSERT(doc.getParticipants() == expectedParticipants);

        if (expectedDecision) {
            ASSERT_EQUALS(*expectedDecision, doc.getDecision()->toString());
        } else {
            ASSERT(!doc.getDecision());
        }

        if (expectedCommitTimestamp) {
            ASSERT(doc.getCommitTimestamp());
            ASSERT_EQUALS(*expectedCommitTimestamp, *doc.getCommitTimestamp());
        } else {
            ASSERT(!doc.getCommitTimestamp());
        }
    }

    void persistParticipantListExpectSuccess(OperationContext* opCtx,
                                             LogicalSessionId lsid,
                                             TxnNumber txnNumber,
                                             const std::vector<ShardId>& participants) {
        txn::persistParticipantList(opCtx, lsid, txnNumber, participants);

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
        assertDocumentMatches(allCoordinatorDocs[0], lsid, txnNumber, participants);
    }

    void persistParticipantListExpectDuplicateKeyError(OperationContext* opCtx,
                                                       LogicalSessionId lsid,
                                                       TxnNumber txnNumber,
                                                       const std::vector<ShardId>& participants) {
        ASSERT_THROWS_CODE(txn::persistParticipantList(opCtx, lsid, txnNumber, participants),
                           AssertionException,
                           51025);
    }

    void persistDecisionExpectSuccess(OperationContext* opCtx,
                                      LogicalSessionId lsid,
                                      TxnNumber txnNumber,
                                      const std::vector<ShardId>& participants,
                                      const boost::optional<Timestamp>& commitTimestamp) {
        txn::persistDecision(opCtx, lsid, txnNumber, participants, commitTimestamp);

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
        if (commitTimestamp) {
            assertDocumentMatches(allCoordinatorDocs[0],
                                  lsid,
                                  txnNumber,
                                  participants,
                                  kCommitDecision,
                                  *commitTimestamp);
        } else {
            assertDocumentMatches(
                allCoordinatorDocs[0], lsid, txnNumber, participants, kAbortDecision);
        }
    }

    void persistDecisionExpectNoMatchingDocuments(
        OperationContext* opCtx,
        LogicalSessionId lsid,
        TxnNumber txnNumber,
        const std::vector<ShardId>& participants,
        const boost::optional<Timestamp>& commitTimestamp) {
        ASSERT_THROWS_CODE(
            txn::persistDecision(opCtx, lsid, txnNumber, participants, commitTimestamp),
            AssertionException,
            51026);
    }

    void deleteCoordinatorDocExpectSuccess(OperationContext* opCtx,
                                           LogicalSessionId lsid,
                                           TxnNumber txnNumber) {
        txn::deleteCoordinatorDoc(opCtx, lsid, txnNumber);

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(0));
    }

    void deleteCoordinatorDocExpectNoMatchingDocuments(OperationContext* opCtx,
                                                       LogicalSessionId lsid,
                                                       TxnNumber txnNumber) {
        ASSERT_THROWS_CODE(
            txn::deleteCoordinatorDoc(opCtx, lsid, txnNumber), AssertionException, 51027);
    }

    LogicalSessionId _lsid;
    const TxnNumber _txnNumber{0};
    const std::vector<ShardId> _participants{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003")};
    Timestamp _commitTimestamp;
};

TEST_F(TransactionCoordinatorCollectionTest,
       PersistParticipantListWhenNoDocumentForTransactionExistsSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistParticipantListWhenMatchingDocumentForTransactionExistsSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistParticipantListWhenDocumentWithConflictingParticipantListExistsFails) {
    std::vector<ShardId> participants{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003")};
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants);

    std::vector<ShardId> smallerParticipantList{ShardId("shard0001"), ShardId("shard0002")};
    persistParticipantListExpectDuplicateKeyError(
        operationContext(), lsid(), txnNumber(), smallerParticipantList);

    std::vector<ShardId> largerParticipantList{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003"), ShardId("shard0004")};
    persistParticipantListExpectDuplicateKeyError(
        operationContext(), lsid(), txnNumber(), largerParticipantList);

    std::vector<ShardId> differentSameSizeParticipantList{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0004")};
    persistParticipantListExpectDuplicateKeyError(
        operationContext(), lsid(), txnNumber(), differentSameSizeParticipantList);
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistParticipantListForMultipleTransactionsOnSameSession) {
    for (int i = 1; i <= 3; i++) {
        auto txnNumber = TxnNumber{i};

        txn::persistParticipantList(operationContext(), lsid(), txnNumber, participants());

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(i));
    }
}

TEST_F(TransactionCoordinatorCollectionTest, PersistParticipantListForMultipleSessions) {
    for (int i = 1; i <= 3; i++) {
        auto lsid = makeLogicalSessionIdForTest();
        txn::persistParticipantList(operationContext(), lsid, txnNumber(), participants());

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(i));
    }
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistAbortDecisionWhenNoDocumentForTransactionExistsFails) {
    persistDecisionExpectNoMatchingDocuments(
        operationContext(), lsid(), txnNumber(), participants(), boost::none /* abort */);
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistAbortDecisionWhenDocumentExistsWithoutDecisionSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), boost::none /* abort */);
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistAbortDecisionWhenDocumentExistsWithSameDecisionSucceeds) {

    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), boost::none /* abort */);
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), boost::none /* abort */);
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistCommitDecisionWhenNoDocumentForTransactionExistsFails) {
    persistDecisionExpectNoMatchingDocuments(
        operationContext(), lsid(), txnNumber(), participants(), commitTimestamp() /* commit */);
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistCommitDecisionWhenDocumentExistsWithoutDecisionSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), commitTimestamp() /* commit */);
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistCommitDecisionWhenDocumentExistsWithSameDecisionSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), commitTimestamp() /* commit */);
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), commitTimestamp() /* commit */);
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistCommitDecisionWhenDocumentExistsWithDifferentCommitTimestampFails) {
    auto differentCommitTimestamp = Timestamp(Date_t::now().toMillisSinceEpoch() / 1000, 1);

    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), commitTimestamp() /* commit */);
    persistDecisionExpectNoMatchingDocuments(operationContext(),
                                             lsid(),
                                             txnNumber(),
                                             participants(),
                                             differentCommitTimestamp /* commit */);
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistAbortDecisionWhenDocumentExistsWithDifferentDecisionFails) {
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), commitTimestamp() /* commit */);
    persistDecisionExpectNoMatchingDocuments(
        operationContext(), lsid(), txnNumber(), participants(), boost::none /* abort */);
}

TEST_F(TransactionCoordinatorCollectionTest,
       PersistCommitDecisionWhenDocumentExistsWithDifferentDecisionFails) {
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), boost::none /* abort */);
    persistDecisionExpectNoMatchingDocuments(
        operationContext(), lsid(), txnNumber(), participants(), commitTimestamp() /* commit */);
}

TEST_F(TransactionCoordinatorCollectionTest, DeleteCoordinatorDocWhenNoDocumentExistsFails) {
    deleteCoordinatorDocExpectNoMatchingDocuments(operationContext(), lsid(), txnNumber());
}

TEST_F(TransactionCoordinatorCollectionTest,
       DeleteCoordinatorDocWhenDocumentExistsWithoutDecisionFails) {
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    deleteCoordinatorDocExpectNoMatchingDocuments(operationContext(), lsid(), txnNumber());
}

TEST_F(TransactionCoordinatorCollectionTest,
       DeleteCoordinatorDocWhenDocumentExistsWithAbortDecisionSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), boost::none /* abort */);
    deleteCoordinatorDocExpectSuccess(operationContext(), lsid(), txnNumber());
}

TEST_F(TransactionCoordinatorCollectionTest,
       DeleteCoordinatorDocWhenDocumentExistsWithCommitDecisionSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), lsid(), txnNumber(), participants());
    persistDecisionExpectSuccess(
        operationContext(), lsid(), txnNumber(), participants(), commitTimestamp() /* commit */);
    deleteCoordinatorDocExpectSuccess(operationContext(), lsid(), txnNumber());
}

TEST_F(TransactionCoordinatorCollectionTest,
       MultipleCommitDecisionsPersistedAndDeleteOneSuccessfullyRemovesCorrectDecision) {
    const auto txnNumber1 = TxnNumber{4};
    const auto txnNumber2 = TxnNumber{5};

    // Insert coordinator documents for two transactions.
    txn::persistParticipantList(operationContext(), lsid(), txnNumber1, participants());
    txn::persistParticipantList(operationContext(), lsid(), txnNumber2, participants());

    auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(2));

    // Delete the document for the first transaction and check that only the second transaction's
    // document still exists.
    txn::persistDecision(
        operationContext(), lsid(), txnNumber1, participants(), boost::none /* abort */);
    txn::deleteCoordinatorDoc(operationContext(), lsid(), txnNumber1);

    allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
    assertDocumentMatches(allCoordinatorDocs[0], lsid(), txnNumber2, participants());
}

}  // namespace
}  // namespace mongo
