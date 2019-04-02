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

using PrepareResponse = txn::PrepareResponse;
using TransactionCoordinatorDocument = txn::TransactionCoordinatorDocument;

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
        assertCommandSentAndRespondWith(
            PrepareTransaction::kCommandName, kPrepareOk, WriteConcernOptions::Majority);
    }

    void assertPrepareSentAndRespondWithSuccess(const Timestamp& timestamp) {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        makePrepareOkResponse(timestamp),
                                        WriteConcernOptions::Majority);
    }

    void assertPrepareSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(
            PrepareTransaction::kCommandName, kNoSuchTransaction, WriteConcernOptions::Majority);
    }

    void assertPrepareSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(
            PrepareTransaction::kCommandName, kRetryableError, WriteConcernOptions::Majority);
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

    LogicalSessionId _lsid{makeLogicalSessionIdForTest()};
    TxnNumber _txnNumber{1};
};

class TransactionCoordinatorDriverTest : public TransactionCoordinatorTestBase {
protected:
    void setUp() override {
        TransactionCoordinatorTestBase::setUp();
        _aws.emplace(getServiceContext());
    }

    void tearDown() override {
        TransactionCoordinatorTestBase::tearDown();
    }

    boost::optional<txn::AsyncWorkScheduler> _aws;
};

auto makeDummyPrepareCommand(const LogicalSessionId& lsid, const TxnNumber& txnNumber) {
    PrepareTransaction prepareCmd;
    prepareCmd.setDbName(NamespaceString::kAdminDb);
    auto prepareObj = prepareCmd.toBSON(
        BSON("lsid" << lsid.toBSON() << "txnNumber" << txnNumber << "autocommit" << false
                    << WriteConcernOptions::kWriteConcernField
                    << WriteConcernOptions::Majority));


    return prepareObj;
}

TEST_F(TransactionCoordinatorDriverTest, SendDecisionToParticipantShardReturnsOnImmediateSuccess) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future = txn::sendDecisionToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithSuccess();

    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardReturnsSuccessAfterOneFailureAndThenSuccess) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future = txn::sendDecisionToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithRetryableError();
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithSuccess();

    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardReturnsSuccessAfterSeveralFailuresAndThenSuccess) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future = txn::sendDecisionToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithSuccess();

    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardInterpretsVoteToAbortAsSuccess) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future = txn::sendDecisionToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    assertPrepareSentAndRespondWithNoSuchTransaction();

    future.get();
}

TEST_F(TransactionCoordinatorDriverTest,
       SendDecisionToParticipantShardCanBeInterruptedAndReturnsError) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<void> future = txn::sendDecisionToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    assertPrepareSentAndRespondWithRetryableError();
    aws.shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Shutdown for test"});
    advanceClockAndExecuteScheduledTasks();

    ASSERT_THROWS_CODE(
        future.get(), AssertionException, ErrorCodes::TransactionCoordinatorSteppingDown);
}

TEST_F(TransactionCoordinatorDriverTest, SendPrepareToShardReturnsCommitDecisionOnOkResponse) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<PrepareResponse> future = txn::sendPrepareToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithSuccess();

    auto response = future.get();
    ASSERT(response.vote == txn::PrepareVote::kCommit);
    ASSERT(response.prepareTimestamp == kDummyPrepareTimestamp);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardReturnsCommitDecisionOnRetryableErrorThenOkResponse) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<PrepareResponse> future = txn::sendPrepareToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithRetryableError();
    ASSERT(!future.isReady());

    assertPrepareSentAndRespondWithSuccess();

    auto response = future.get();
    ASSERT(response.vote == txn::PrepareVote::kCommit);
    ASSERT(response.prepareTimestamp == kDummyPrepareTimestamp);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardCanBeInterruptedAndReturnsNoDecisionIfNotServiceShutdown) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<PrepareResponse> future = txn::sendPrepareToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    assertPrepareSentAndRespondWithRetryableError();
    aws.shutdown({ErrorCodes::TransactionCoordinatorReachedAbortDecision, "Retry interrupted"});
    advanceClockAndExecuteScheduledTasks();

    auto response = future.get();
    ASSERT(response.vote == boost::none);
    ASSERT(response.prepareTimestamp == boost::none);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardCanBeInterruptedAndThrowsExceptionIfServiceShutdown) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    Future<PrepareResponse> future = txn::sendPrepareToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    assertPrepareSentAndRespondWithRetryableError();
    aws.shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Service shutting down"});
    advanceClockAndExecuteScheduledTasks();

    ASSERT_THROWS_CODE(
        future.get(), AssertionException, ErrorCodes::TransactionCoordinatorSteppingDown);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardReturnsAbortDecisionOnVoteAbortResponse) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepareToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    assertPrepareSentAndRespondWithNoSuchTransaction();

    auto response = future.get();
    ASSERT(response.vote == txn::PrepareVote::kAbort);
    ASSERT(response.prepareTimestamp == boost::none);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareToShardReturnsAbortDecisionOnRetryableErrorThenVoteAbortResponse) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepareToShard(
        getServiceContext(), aws, kTwoShardIdList[0], makeDummyPrepareCommand(_lsid, _txnNumber));

    assertPrepareSentAndRespondWithRetryableError();
    assertPrepareSentAndRespondWithNoSuchTransaction();

    auto response = future.get();
    ASSERT(response.vote == txn::PrepareVote::kAbort);
    ASSERT(response.prepareTimestamp == boost::none);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsAbortDecisionWhenFirstParticipantVotesAbortAndSecondVotesCommit) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(), aws, _lsid, _txnNumber, kTwoShardIdList);

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; },
                [&](const executor::RemoteCommandRequest& request) { return kPrepareOk; }});

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kAbort);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsAbortDecisionWhenFirstParticipantVotesCommitAndSecondVotesAbort) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(), aws, _lsid, _txnNumber, kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithNoSuchTransaction();

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kAbort);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsAbortDecisionWhenBothParticipantsVoteAbort) {
    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(), aws, _lsid, _txnNumber, kTwoShardIdList);

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; },
                [&](const executor::RemoteCommandRequest& request) { return kPrepareOk; }});

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kAbort);
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsCommitDecisionWhenBothParticipantsVoteCommit) {
    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(), aws, _lsid, _txnNumber, kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kCommit);
    ASSERT_EQ(maxPrepareTimestamp, *decision.getCommitTimestamp());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsCommitDecisionWithMaxPrepareTimestampWhenFirstParticipantHasMax) {
    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(), aws, _lsid, _txnNumber, kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kCommit);
    ASSERT_EQ(maxPrepareTimestamp, *decision.getCommitTimestamp());
}

TEST_F(TransactionCoordinatorDriverTest,
       SendPrepareReturnsCommitDecisionWithMaxPrepareTimestampWhenLastParticipantHasMax) {
    const auto firstPrepareTimestamp = Timestamp(1, 1);
    const auto maxPrepareTimestamp = Timestamp(2, 1);

    txn::AsyncWorkScheduler aws(getServiceContext());
    auto future = txn::sendPrepare(getServiceContext(), aws, _lsid, _txnNumber, kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess(firstPrepareTimestamp);
    assertPrepareSentAndRespondWithSuccess(maxPrepareTimestamp);

    auto decision = future.get().decision();
    ASSERT(decision.getDecision() == txn::CommitDecision::kCommit);
    ASSERT_EQ(maxPrepareTimestamp, *decision.getCommitTimestamp());
}


class TransactionCoordinatorDriverPersistenceTest : public TransactionCoordinatorDriverTest {
protected:
    void setUp() override {
        TransactionCoordinatorDriverTest::setUp();
        _aws.emplace(getServiceContext());
    }

    void tearDown() override {
        _aws.reset();
        TransactionCoordinatorDriverTest::tearDown();
    }

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
            ASSERT(*expectedDecision == decision->getDecision());
        } else {
            ASSERT(!decision);
        }

        if (expectedCommitTimestamp) {
            ASSERT(decision->getCommitTimestamp());
            ASSERT_EQUALS(*expectedCommitTimestamp, *decision->getCommitTimestamp());
        } else if (decision) {
            ASSERT(!decision->getCommitTimestamp());
        }
    }

    void persistParticipantListExpectSuccess(OperationContext* opCtx,
                                             LogicalSessionId lsid,
                                             TxnNumber txnNumber,
                                             const std::vector<ShardId>& participants) {
        txn::persistParticipantsList(*_aws, lsid, txnNumber, participants).get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
        assertDocumentMatches(allCoordinatorDocs[0], lsid, txnNumber, participants);
    }

    void persistDecisionExpectSuccess(OperationContext* opCtx,
                                      LogicalSessionId lsid,
                                      TxnNumber txnNumber,
                                      const std::vector<ShardId>& participants,
                                      const boost::optional<Timestamp>& commitTimestamp) {
        txn::persistDecision(*_aws, lsid, txnNumber, participants, commitTimestamp).get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
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
        txn::deleteCoordinatorDoc(*_aws, lsid, txnNumber).get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(0));
    }

    const std::vector<ShardId> _participants{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003")};

    const Timestamp _commitTimestamp{Timestamp(Date_t::now().toMillisSinceEpoch() / 1000, 0)};

    boost::optional<txn::AsyncWorkScheduler> _aws;
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
        txn::persistParticipantsList(*_aws, _lsid, _txnNumber, smallerParticipantList).get(),
        AssertionException,
        51025);

    std::vector<ShardId> largerParticipantList{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003"), ShardId("shard0004")};
    ASSERT_THROWS_CODE(
        txn::persistParticipantsList(*_aws, _lsid, _txnNumber, largerParticipantList).get(),
        AssertionException,
        51025);

    std::vector<ShardId> differentSameSizeParticipantList{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0004")};
    ASSERT_THROWS_CODE(
        txn::persistParticipantsList(*_aws, _lsid, _txnNumber, differentSameSizeParticipantList)
            .get(),
        AssertionException,
        51025);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistParticipantListForMultipleTransactionsOnSameSession) {
    for (int i = 1; i <= 3; i++) {
        auto txnNumber = TxnNumber{i};
        txn::persistParticipantsList(*_aws, _lsid, txnNumber, _participants).get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(i));
    }
}

TEST_F(TransactionCoordinatorDriverPersistenceTest, PersistParticipantListForMultipleSessions) {
    for (int i = 1; i <= 3; i++) {
        auto lsid = makeLogicalSessionIdForTest();
        txn::persistParticipantsList(*_aws, lsid, _txnNumber, _participants).get();

        auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
        ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(i));
    }
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistAbortDecisionWhenNoDocumentForTransactionExistsFails) {
    ASSERT_THROWS_CODE(
        txn::persistDecision(*_aws, _lsid, _txnNumber, _participants, boost::none /* abort */)
            .get(),
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
        txn::persistDecision(*_aws, _lsid, _txnNumber, _participants, _commitTimestamp /* commit */)
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
        txn::persistDecision(
            *_aws, _lsid, _txnNumber, _participants, differentCommitTimestamp /* commit */)
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
        txn::persistDecision(*_aws, _lsid, _txnNumber, _participants, boost::none /* abort */)
            .get(),
        AssertionException,
        51026);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       PersistCommitDecisionWhenDocumentExistsWithDifferentDecisionFails) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    persistDecisionExpectSuccess(
        operationContext(), _lsid, _txnNumber, _participants, boost::none /* abort */);

    ASSERT_THROWS_CODE(
        txn::persistDecision(*_aws, _lsid, _txnNumber, _participants, _commitTimestamp /* abort */)
            .get(),
        AssertionException,
        51026);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest, DeleteCoordinatorDocWhenNoDocumentExistsFails) {
    ASSERT_THROWS_CODE(
        txn::deleteCoordinatorDoc(*_aws, _lsid, _txnNumber).get(), AssertionException, 51027);
}

TEST_F(TransactionCoordinatorDriverPersistenceTest,
       DeleteCoordinatorDocWhenDocumentExistsWithoutDecisionFails) {
    persistParticipantListExpectSuccess(operationContext(), _lsid, _txnNumber, _participants);
    ASSERT_THROWS_CODE(
        txn::deleteCoordinatorDoc(*_aws, _lsid, _txnNumber).get(), AssertionException, 51027);
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
    txn::persistParticipantsList(*_aws, _lsid, txnNumber1, _participants).get();
    txn::persistParticipantsList(*_aws, _lsid, txnNumber2, _participants).get();

    auto allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(2));

    // Delete the document for the first transaction and check that only the second transaction's
    // document still exists.
    txn::persistDecision(*_aws, _lsid, txnNumber1, _participants, boost::none /* abort */).get();
    txn::deleteCoordinatorDoc(*_aws, _lsid, txnNumber1).get();

    allCoordinatorDocs = txn::readAllCoordinatorDocs(operationContext());
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
    assertDocumentMatches(allCoordinatorDocs[0], _lsid, txnNumber2, _participants);
}


using TransactionCoordinatorTest = TransactionCoordinatorTestBase;

TEST_F(TransactionCoordinatorTest, RunCommitProducesCommitDecisionOnTwoCommitResponses) {
    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        boost::none);
    coordinator.runCommit(kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kCommit));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest, RunCommitProducesAbortDecisionOnAbortAndCommitResponses) {
    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        boost::none);
    coordinator.runCommit(kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; },
                [&](const executor::RemoteCommandRequest& request) { return kPrepareOk; }});

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kAbort));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest, RunCommitProducesAbortDecisionOnCommitAndAbortResponses) {
    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        boost::none);
    coordinator.runCommit(kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    onCommands({[&](const executor::RemoteCommandRequest& request) { return kPrepareOk; },
                [&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; }});

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kAbort));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest, RunCommitProducesAbortDecisionOnSingleAbortResponseOnly) {
    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        boost::none);
    coordinator.runCommit(kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    assertPrepareSentAndRespondWithNoSuchTransaction();
    advanceClockAndExecuteScheduledTasks();  // Make sure the cancellation callback is delivered

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kAbort));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesAbortDecisionOnOneCommitResponseAndOneAbortResponseAfterRetry) {
    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        boost::none);
    coordinator.runCommit(kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    // One participant votes commit and other encounters retryable error
    onCommands({[&](const executor::RemoteCommandRequest& request) { return kPrepareOk; },
                [&](const executor::RemoteCommandRequest& request) { return kRetryableError; }});
    advanceClockAndExecuteScheduledTasks();  // Make sure the scheduled retry executes

    // One participant votes abort after retry.
    assertPrepareSentAndRespondWithNoSuchTransaction();

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kAbort));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesAbortDecisionOnOneAbortResponseAndOneRetryableAbortResponse) {
    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        boost::none);
    coordinator.runCommit(kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

    // One participant votes abort and other encounters retryable error
    onCommands({[&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; },
                [&](const executor::RemoteCommandRequest& request) { return kRetryableError; }});
    advanceClockAndExecuteScheduledTasks();  // Make sure the cancellation callback is delivered

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kAbort));

    coordinator.onCompletion().get();
}

TEST_F(TransactionCoordinatorTest,
       RunCommitProducesCommitDecisionOnCommitAfterMultipleNetworkRetries) {
    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        boost::none);
    coordinator.runCommit(kTwoShardIdList);
    auto commitDecisionFuture = coordinator.getDecision();

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

TEST_F(TransactionCoordinatorTest, AbandonNewlyCreatedCoordinator) {
    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        network()->now() + Seconds{30});
}

}  // namespace
}  // namespace mongo
