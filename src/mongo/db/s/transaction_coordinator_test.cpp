
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_test_fixture.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {

const Timestamp kDummyTimestamp = Timestamp::min();
const Date_t kCommitDeadline = Date_t::max();
const StatusWith<BSONObj> kNoSuchTransaction =
    BSON("ok" << 0 << "code" << ErrorCodes::NoSuchTransaction);
const StatusWith<BSONObj> kOk = BSON("ok" << 1);
const Timestamp kDummyPrepareTimestamp = Timestamp(1, 1);

StatusWith<BSONObj> makePrepareOkResponse(const Timestamp& timestamp) {
    return BSON("ok" << 1 << "prepareTimestamp" << timestamp);
}

const StatusWith<BSONObj> kPrepareOk = makePrepareOkResponse(kDummyPrepareTimestamp);

class TransactionCoordinatorTestBase : public TransactionCoordinatorTestFixture {
protected:
    void assertPrepareSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kPrepareOk,
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }

    void assertPrepareSentAndRespondWithSuccess(const Timestamp& timestamp) {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        makePrepareOkResponse(timestamp),
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }

    void assertPrepareSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kNoSuchTransaction,
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
    }

    void assertPrepareSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kRetryableError,
                                        WriteConcernOptions::InternalMajorityNoSnapshot);
        advanceClockAndExecuteScheduledTasks();
    }

    void assertAbortSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith("abortTransaction", kOk, boost::none);
    }

    void assertCommitSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(CommitTransaction::kCommandName, kOk, boost::none);
    }

    void assertCommitSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(
            CommitTransaction::kCommandName, kRetryableError, boost::none);
    }

    void assertNoMessageSent() {
        executor::NetworkInterfaceMock::InNetworkGuard networkGuard(network());
        ASSERT_FALSE(network()->hasReadyRequests());
    }

    /**
     * Goes through the steps to commit a transaction through the coordinator service for a given
     * lsid and txnNumber. Useful when not explictly testing the commit protocol.
     */
    void commitTransaction(const std::set<ShardId>& transactionParticipantShards) {
        for (size_t i = 0; i < transactionParticipantShards.size(); ++i) {
            assertPrepareSentAndRespondWithSuccess(kDummyPrepareTimestamp);
        }

        for (size_t i = 0; i < transactionParticipantShards.size(); ++i) {
            assertCommitSentAndRespondWithSuccess();
        }
    }

    /**
     * Goes through the steps to abort a transaction through the coordinator service for a given
     * lsid and txnNumber. Useful when not explictly testing the abort protocol.
     */
    void abortTransaction(const std::set<ShardId>& shardIdSet, const ShardId& abortingShard) {
        //    auto commitDecisionFuture =
        //        coordinatorService.coordinateCommit(operationContext(), lsid, txnNumber,
        //        shardIdSet);

        for (size_t i = 0; i < shardIdSet.size(); ++i) {
            assertPrepareSentAndRespondWithNoSuchTransaction();
        }

        // Abort gets sent to the second participant as soon as the first participant
        // receives a not-okay response to prepare.
        assertAbortSentAndRespondWithSuccess();

        // Wait for abort to complete.
        //    commitDecisionFuture.get();
    }

    LogicalSessionId _lsid{makeLogicalSessionIdForTest()};
    TxnNumber _txnNumber{1};
};

class TransactionCoordinatorDriverTest : public TransactionCoordinatorTestBase {
protected:
    void setUp() override {
        TransactionCoordinatorTestBase::setUp();
        _driver.emplace(getServiceContext());
    }

    void tearDown() override {
        _driver.reset();
        TransactionCoordinatorTestBase::tearDown();
    }

    boost::optional<TransactionCoordinatorDriver> _driver;
};

auto makeDummyPrepareCommand(const LogicalSessionId& lsid, const TxnNumber& txnNumber) {
    PrepareTransaction prepareCmd;
    prepareCmd.setDbName("admin");
    auto prepareObj = prepareCmd.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::InternalMajorityNoSnapshot));


    return prepareObj;
}

TEST_F(TransactionCoordinatorDriverTest, SendDecisionToParticipantShardReturnsOnImmediateSuccess) {
    Future<void> future = _driver->sendDecisionToParticipantShard(
        kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    assertPrepareSentAndRespondWithSuccess();
    future.get(operationContext());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardReturnsSuccessAfterOneFailureAndThenSuccess) {
    Future<void> future = _driver->sendDecisionToParticipantShard(
        kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithRetryableError();
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithSuccess();
    future.get(operationContext());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardReturnsSuccessAfterSeveralFailuresAndThenSuccess) {
    Future<void> future = _driver->sendDecisionToParticipantShard(
        kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    std::move(future).getAsync([](Status s) { ASSERT(s.isOK()); });

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardInterpretsVoteToAbortAsSuccess) {
    Future<void> future = _driver->sendDecisionToParticipantShard(
        kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    std::move(future).getAsync([](Status s) { ASSERT(s.isOK()); });

    assertPrepareSentAndRespondWithNoSuchTransaction();
}

TEST_F(TransactionCoordinatorDriverTest, SendPrepareToShardReturnsCommitDecisionOnOkResponse) {
    Future<PrepareResponse> future =
        _driver->sendPrepareToShard(kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    std::move(future).getAsync([](StatusWith<PrepareResponse> swResponse) {
        ASSERT_OK(swResponse.getStatus());
        auto response = swResponse.getValue();
        ASSERT(response.vote == txn::PrepareVote::kCommit);
        ASSERT(response.prepareTimestamp == kDummyPrepareTimestamp);
    });

    // Simulate a participant voting to commit.
    assertPrepareSentAndRespondWithSuccess();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardReturnsCommitDecisionOnRetryableErrorThenOkResponse) {
    Future<PrepareResponse> future =
        _driver->sendPrepareToShard(kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    std::move(future).getAsync([](StatusWith<PrepareResponse> swResponse) {
        ASSERT_OK(swResponse.getStatus());
        auto response = swResponse.getValue();
        ASSERT(response.vote == txn::PrepareVote::kCommit);
        ASSERT(response.prepareTimestamp == kDummyPrepareTimestamp);
    });

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();
}

TEST_F(
    TransactionCoordinatorDriverTest,
    SendPrepareToShardStopsRetryingAfterRetryableErrorAndReturnsNoneIfCoordinatorStateIsNotPrepare) {
    Future<PrepareResponse> future =
        _driver->sendPrepareToShard(kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    auto resultFuture = std::move(future).then([](PrepareResponse response) {
        ASSERT(response.vote == boost::none);
        ASSERT(response.prepareTimestamp == boost::none);
    });

    _driver->cancel();
    assertPrepareSentAndRespondWithRetryableError();
    resultFuture.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardReturnsAbortDecisionOnVoteAbortResponse) {
    auto future =
        _driver->sendPrepareToShard(kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber))
            .then([&](PrepareResponse response) {
                ASSERT(response.vote == txn::PrepareVote::kAbort);
                ASSERT(response.prepareTimestamp == boost::none);
                return response;
            });

    assertPrepareSentAndRespondWithNoSuchTransaction();

    auto response = future.get();
    ASSERT(response.vote == txn::PrepareVote::kAbort);
    ASSERT(response.prepareTimestamp == boost::none);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardReturnsAbortDecisionOnRetryableErrorThenVoteAbortResponse) {
    auto future =
        _driver->sendPrepareToShard(kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber))
            .then([&](PrepareResponse response) {
                ASSERT(response.vote == txn::PrepareVote::kAbort);
                ASSERT(response.prepareTimestamp == boost::none);
            });

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithNoSuchTransaction();
    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsAbortDecisionWhenFirstParticipantVotesAbortAndSecondVotesCommit) {
    auto future = _driver->sendPrepare(kTwoShardIdList, _lsid, _txnNumber)
                      .then([&](txn::PrepareVoteConsensus response) {
                          ASSERT(response.decision == txn::CommitDecision::kAbort);
                          ASSERT(response.maxPrepareTimestamp == boost::none);
                      });

    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertPrepareSentAndRespondWithSuccess();
    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsAbortDecisionWhenFirstParticipantVotesCommitAndSecondVotesAbort) {
    auto future = _driver->sendPrepare(kTwoShardIdList, _lsid, _txnNumber)
                      .then([&](txn::PrepareVoteConsensus response) {
                          ASSERT(response.decision == txn::CommitDecision::kAbort);
                          ASSERT(response.maxPrepareTimestamp == boost::none);
                      });

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithNoSuchTransaction();
    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsAbortDecisionWhenBothParticipantsVoteAbort) {
    auto future = _driver->sendPrepare(kTwoShardIdList, _lsid, _txnNumber)
                      .then([&](txn::PrepareVoteConsensus response) {
                          ASSERT(response.decision == txn::CommitDecision::kAbort);
                          ASSERT(response.maxPrepareTimestamp == boost::none);
                      });

    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertPrepareSentAndRespondWithNoSuchTransaction();
    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsCommitDecisionWhenBothParticipantsVoteCommit) {
    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    auto future = _driver->sendPrepare(kTwoShardIdList, _lsid, _txnNumber)
                      .then([&](txn::PrepareVoteConsensus response) {
                          ASSERT(response.decision == txn::CommitDecision::kCommit);
                          ASSERT(response.maxPrepareTimestamp == maxPrepareTimestamp);
                      });

    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);
    future.get();  // Should be able to return after the first participant responds.
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsCommitDecisionWithMaxPrepareTimestampWhenFirstParticipantHasMax) {
    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    auto future = _driver->sendPrepare(kTwoShardIdList, _lsid, _txnNumber)
                      .then([&](txn::PrepareVoteConsensus response) {
                          ASSERT(response.decision == txn::CommitDecision::kCommit);
                          ASSERT(response.maxPrepareTimestamp == maxPrepareTimestamp);
                      });

    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);
    future.get();  // Should be able to return after the first participant responds.
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsCommitDecisionWithMaxPrepareTimestampWhenLastParticipantHasMax) {
    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    auto future = _driver->sendPrepare(kTwoShardIdList, _lsid, _txnNumber)
                      .then([&](txn::PrepareVoteConsensus response) {
                          ASSERT(response.decision == txn::CommitDecision::kCommit);
                          ASSERT(response.maxPrepareTimestamp == maxPrepareTimestamp);
                      });

    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);
    future.get();  // Should be able to return after the first participant responds.
}


class TransactionCoordinatorDriverPersistenceTest : public TransactionCoordinatorDriverTest {
protected:
    static void assertDocumentMatches(
        TransactionCoordinatorDocument doc,
        LogicalSessionId expectedLsid,
        TxnNumber expectedTxnNum,
        std::vector<ShardId> expectedParticipants,
        boost::optional<txn::CommitDecision> expectedDecision = boost::none,
        boost::optional<Timestamp> expectedCommitTimestamp = boost::none) {
        ASSERT(doc.getId().getSessionId());
        ASSERT_EQUALS(*doc.getId().getSessionId(), expectedLsid);
        ASSERT(doc.getId().getTxnNumber());
        ASSERT_EQUALS(*doc.getId().getTxnNumber(), expectedTxnNum);

        ASSERT(doc.getParticipants() == expectedParticipants);

        auto decision = doc.getDecision();
        if (expectedDecision) {
            ASSERT(*expectedDecision == decision->decision);
        } else {
            ASSERT(!decision);
        }

        if (expectedCommitTimestamp) {
            ASSERT(decision->commitTimestamp);
            ASSERT_EQUALS(*expectedCommitTimestamp, *decision->commitTimestamp);
        } else if (decision) {
            ASSERT(!decision->commitTimestamp);
        }
    }

    void persistParticipantListExpectSuccess(OperationContext* opCtx,
                                             LogicalSessionId lsid,
                                             TxnNumber txnNumber,
                                             const std::vector<ShardId>& participants) {
        _driver->persistParticipantList(lsid, txnNumber, participants).get();

        auto allCoordinatorDocs = TransactionCoordinatorDriver::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
        assertDocumentMatches(allCoordinatorDocs[0], lsid, txnNumber, participants);
    }

    void persistDecisionExpectSuccess(OperationContext* opCtx,
                                      LogicalSessionId lsid,
                                      TxnNumber txnNumber,
                                      const std::vector<ShardId>& participants,
                                      const boost::optional<Timestamp>& commitTimestamp) {
        _driver->persistDecision(lsid, txnNumber, participants, commitTimestamp).get();

        auto allCoordinatorDocs = TransactionCoordinatorDriver::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
        if (commitTimestamp) {
            assertDocumentMatches(allCoordinatorDocs[0],
                                  lsid,
                                  txnNumber,
                                  participants,
                                  txn::CommitDecision::kCommit,
                                  *commitTimestamp);
        } else {
            assertDocumentMatches(
                allCoordinatorDocs[0], lsid, txnNumber, participants, txn::CommitDecision::kAbort);
        }
    }

    void deleteCoordinatorDocExpectSuccess(OperationContext* opCtx,
                                           LogicalSessionId lsid,
                                           TxnNumber txnNumber) {
        _driver->deleteCoordinatorDoc(lsid, txnNumber).get();

        auto allCoordinatorDocs = TransactionCoordinatorDriver::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(0));
    }

    const std::vector<ShardId> _participants{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003")};

    const Timestamp _commitTimestamp{Timestamp(Date_t::now().toMillisSinceEpoch() / 1000, 0)};
};

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistParticipantListWhenNoDocumentForTransactionExistsSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistParticipantListWhenMatchingDocumentForTransactionExistsSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistParticipantListWhenDocumentWithConflictingParticipantListExistsFails) {
    std::vector<ShardId> participants{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003")};
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, participants);

    std::vector<ShardId> smallerParticipantList{ShardId("shard0001"), ShardId("shard0002")};
    ASSERT_THROWS_CODE(
        _driver->persistParticipantList(_lsid, _txnNumber, smallerParticipantList).get(),
        AssertionException,
        51025);

    std::vector<ShardId> largerParticipantList{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003"), ShardId("shard0004")};
    ASSERT_THROWS_CODE(
        _driver->persistParticipantList(_lsid, _txnNumber, largerParticipantList).get(),
        AssertionException,
        51025);

    std::vector<ShardId> differentSameSizeParticipantList{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0004")};
    ASSERT_THROWS_CODE(
        _driver->persistParticipantList(_lsid, _txnNumber, differentSameSizeParticipantList).get(),
        AssertionException,
        51025);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistParticipantListForMultipleTransactionsOnSameSession) {
    for (int i = 1; i <= 3; i++) {
        auto txnNumber = TxnNumber{i};
        _driver->persistParticipantList(_lsid, txnNumber, _participants).get();

        auto allCoordinatorDocs =
            TransactionCoordinatorDriver::readAllCoordinatorDocs(operationContext());
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(i));
    }
}

TEST_F(TransactionCoordinatorDriverPersistenceTest, PersistParticipantListForMultipleSessions) {
    for (int i = 1; i <= 3; i++) {
        auto lsid = makeLogicalSessionIdForTest();
        _driver->persistParticipantList(lsid, _txnNumber, _participants).get();

        auto allCoordinatorDocs =
            TransactionCoordinatorDriver::readAllCoordinatorDocs(operationContext());
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(i));
    }
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistAbortDecisionWhenNoDocumentForTransactionExistsFails) {
    ASSERT_THROWS_CODE(
        _driver->persistDecision(_lsid, _txnNumber, _participants, boost::none /* abort */).get(),
        AssertionException,
        51026);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistAbortDecisionWhenDocumentExistsWithoutDecisionSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, boost::none /* abort */);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistAbortDecisionWhenDocumentExistsWithSameDecisionSucceeds) {

    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, boost::none /* abort */);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, boost::none /* abort */);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistCommitDecisionWhenNoDocumentForTransactionExistsFails) {
    ASSERT_THROWS_CODE(
        _driver->persistDecision(_lsid, _txnNumber, _participants, _commitTimestamp /* commit */)
            .get(),
        AssertionException,
        51026);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistCommitDecisionWhenDocumentExistsWithoutDecisionSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, _commitTimestamp /* commit */);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistCommitDecisionWhenDocumentExistsWithSameDecisionSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, _commitTimestamp /* commit */);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, _commitTimestamp /* commit */);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistCommitDecisionWhenDocumentExistsWithDifferentCommitTimestampFails) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, _commitTimestamp /* commit */);

    const Timestamp differentCommitTimestamp(Date_t::now().toMillisSinceEpoch() / 1000, 1);
    ASSERT_THROWS_CODE(
        _driver
            ->persistDecision(
                _lsid, _txnNumber, _participants, differentCommitTimestamp /* commit */)
            .get(),
        AssertionException,
        51026);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistAbortDecisionWhenDocumentExistsWithDifferentDecisionFails) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, _commitTimestamp /* commit */);

    ASSERT_THROWS_CODE(
        _driver->persistDecision(_lsid, _txnNumber, _participants, boost::none /* abort */).get(),
        AssertionException,
        51026);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistCommitDecisionWhenDocumentExistsWithDifferentDecisionFails) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, boost::none /* abort */);

    ASSERT_THROWS_CODE(
        _driver->persistDecision(_lsid, _txnNumber, _participants, _commitTimestamp /* abort */)
            .get(),
        AssertionException,
        51026);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest, DeleteCoordinatorDocWhenNoDocumentExistsFails) {
    ASSERT_THROWS_CODE(
        _driver->deleteCoordinatorDoc(_lsid, _txnNumber).get(), AssertionException, 51027);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       DeleteCoordinatorDocWhenDocumentExistsWithoutDecisionFails) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    ASSERT_THROWS_CODE(
        _driver->deleteCoordinatorDoc(_lsid, _txnNumber).get(), AssertionException, 51027);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       DeleteCoordinatorDocWhenDocumentExistsWithAbortDecisionSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, boost::none /* abort */);
    deleteCoordinatorDocExpectSuccess(operationContext(), _lsid, _txnNumber);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       DeleteCoordinatorDocWhenDocumentExistsWithCommitDecisionSucceeds) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, _commitTimestamp /* commit */);
    deleteCoordinatorDocExpectSuccess(operationContext(), _lsid, _txnNumber);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       MultipleCommitDecisionsPersistedAndDeleteOneSuccessfullyRemovesCorrectDecision) {
    const auto txnNumber1 = TxnNumber{4};
    const auto txnNumber2 = TxnNumber{5};

    // Insert coordinator documents for two transactions.
    _driver->persistParticipantList(_lsid, txnNumber1, _participants).get();
    _driver->persistParticipantList(_lsid, txnNumber2, _participants).get();

    auto allCoordinatorDocs =
        TransactionCoordinatorDriver::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(2));

    // Delete the document for the first transaction and check that only the second transaction's
    // document still exists.
    _driver->persistDecision(_lsid, txnNumber1, _participants, boost::none /* abort */).get();
    _driver->deleteCoordinatorDoc(_lsid, txnNumber1).get();

    allCoordinatorDocs = TransactionCoordinatorDriver::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
    assertDocumentMatches(allCoordinatorDocs[0], _lsid, txnNumber2, _participants);
}


using TransactionCoordinatorTest = TransactionCoordinatorTestBase;

TEST_F(TransactionCoordinatorTest, RunCommitReturnsCorrectCommitDecisionOnAbort) {
    TransactionCoordinator coordinator(getServiceContext(), _lsid, _txnNumber);
    auto commitDecisionFuture = coordinator.runCommit(kTwoShardIdList);

    // Simulate a participant voting to abort.
    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertPrepareSentAndRespondWithSuccess();

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kAbort));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest, RunCommitReturnsCorrectCommitDecisionOnCommit) {
    TransactionCoordinator coordinator(getServiceContext(), _lsid, _txnNumber);
    auto commitDecisionFuture = coordinator.runCommit(kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kCommit));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitReturnsCorrectCommitDecisionOnAbortAfterNetworkRetriesOneParticipantAborts) {
    TransactionCoordinator coordinator(getServiceContext(), _lsid, _txnNumber);
    auto commitDecisionFuture = coordinator.runCommit(kTwoShardIdList);

    // One participant votes abort after retry.
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithNoSuchTransaction();

    // One participant votes commit.
    assertPrepareSentAndRespondWithSuccess();

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kAbort));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitReturnsCorrectCommitDecisionOnAbortAfterNetworkRetriesBothParticipantsAbort) {
    TransactionCoordinator coordinator(getServiceContext(), _lsid, _txnNumber);
    auto commitDecisionFuture = coordinator.runCommit(kTwoShardIdList);

    // One participant votes abort after retry.
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithNoSuchTransaction();

    // One participant votes abort.
    assertPrepareSentAndRespondWithNoSuchTransaction();

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kAbort));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitReturnsCorrectCommitDecisionOnCommitAfterNetworkRetries) {
    TransactionCoordinator coordinator(getServiceContext(), _lsid, _txnNumber);
    auto commitDecisionFuture = coordinator.runCommit(kTwoShardIdList);

    // One participant votes commit after retry.
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();

    // One participant votes commit after retry.
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kCommit));

    coordinator.onCompletion().get();
}

}  // namespace
}  // namespace mongo
