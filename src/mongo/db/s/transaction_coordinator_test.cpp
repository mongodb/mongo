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
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/server_transaction_coordinators_metrics.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_metrics_observer.h"
#include "mongo/db/s/transaction_coordinator_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/log.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {

using PrepareResponse = txn::PrepareResponse;
using TransactionCoordinatorDocument = txn::TransactionCoordinatorDocument;

const Hours kLongFutureTimeout(8);

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

    void waitUntilCoordinatorDocIsPresent() {
        DBDirectClient dbClient(operationContext());
        while (dbClient.findOne(NamespaceString::kTransactionCoordinatorsNamespace.ns(), Query())
                   .isEmpty())
            ;
    }

    /**
     * Precondition: A coordinator document exists with or without a decision.
     */
    void waitUntilCoordinatorDocHasDecision() {
        DBDirectClient dbClient(operationContext());
        TransactionCoordinatorDocument doc;
        do {
            doc = TransactionCoordinatorDocument::parse(
                IDLParserErrorContext("dummy"),
                dbClient.findOne(NamespaceString::kTransactionCoordinatorsNamespace.ns(), Query()));
        } while (!doc.getDecision());
    }

    void waitUntilNoCoordinatorDocIsPresent() {
        DBDirectClient dbClient(operationContext());
        while (!dbClient.findOne(NamespaceString::kTransactionCoordinatorsNamespace.ns(), Query())
                    .isEmpty())
            ;
    }

    void waitUntilMessageSent() {
        while (true) {
            executor::NetworkInterfaceMock::InNetworkGuard networkGuard(network());
            if (network()->hasReadyRequests()) {
                return;
            }
        }
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
       PersistParticipantListWhenDocumentWithConflictingParticipantListExistsFailsToPersistList) {
    auto opCtx = operationContext();
    std::vector<ShardId> participants{
        ShardId("shard0001"), ShardId("shard0002"), ShardId("shard0003")};
    persistParticipantListExpectSuccess(opCtx, _lsid, _txnNumber, participants);

    // We should retry until shutdown. The original participants should be persisted.

    std::vector<ShardId> smallerParticipantList{ShardId("shard0001"), ShardId("shard0002")};
    Future<void> future =
        txn::persistParticipantsList(*_aws, _lsid, _txnNumber, smallerParticipantList);

    _aws->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Shutdown for test"});
    advanceClockAndExecuteScheduledTasks();
    ASSERT_THROWS_CODE(
        future.get(), AssertionException, ErrorCodes::TransactionCoordinatorSteppingDown);

    auto allCoordinatorDocs = txn::readAllCoordinatorDocs(opCtx);
    ASSERT_EQUALS(allCoordinatorDocs.size(), size_t(1));
    assertDocumentMatches(allCoordinatorDocs[0], _lsid, _txnNumber, participants);
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
       PersistCommitDecisionWhenNoDocumentForTransactionExistsCanBeInterruptedAndReturnsError) {
    Future<void> future = txn::persistDecision(
        *_aws, _lsid, _txnNumber, _participants, _commitTimestamp /* commit */);
    _aws->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "Shutdown for test"});

    ASSERT_THROWS_CODE(
        future.get(), AssertionException, ErrorCodes::TransactionCoordinatorSteppingDown);
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
        Date_t::max());
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
        Date_t::max());
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
        Date_t::max());
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
        Date_t::max());
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
        Date_t::max());
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
        Date_t::max());
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
        Date_t::max());
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

class TransactionCoordinatorMetricsTest : public TransactionCoordinatorTestBase {
public:
    void setUp() override {
        TransactionCoordinatorTestBase::setUp();

        getServiceContext()->setPreciseClockSource(stdx::make_unique<ClockSourceMock>());

        auto tickSource = stdx::make_unique<TickSourceMock<Microseconds>>();
        tickSource->reset(1);
        getServiceContext()->setTickSource(std::move(tickSource));
    }

    ServerTransactionCoordinatorsMetrics* metrics() {
        return ServerTransactionCoordinatorsMetrics::get(getServiceContext());
    }

    ClockSourceMock* clockSource() {
        return dynamic_cast<ClockSourceMock*>(getServiceContext()->getPreciseClockSource());
    }

    TickSourceMock<Microseconds>* tickSource() {
        return dynamic_cast<TickSourceMock<Microseconds>*>(getServiceContext()->getTickSource());
    }

    struct Stats {
        // Start times
        boost::optional<Date_t> createTime;
        boost::optional<Date_t> writingParticipantListStartTime;
        boost::optional<Date_t> waitingForVotesStartTime;
        boost::optional<Date_t> writingDecisionStartTime;
        boost::optional<Date_t> waitingForDecisionAcksStartTime;
        boost::optional<Date_t> deletingCoordinatorDocStartTime;
        boost::optional<Date_t> endTime;

        // Durations
        boost::optional<Microseconds> totalDuration;
        boost::optional<Microseconds> twoPhaseCommitDuration;
        boost::optional<Microseconds> writingParticipantListDuration;
        boost::optional<Microseconds> waitingForVotesDuration;
        boost::optional<Microseconds> writingDecisionDuration;
        boost::optional<Microseconds> waitingForDecisionAcksDuration;
        boost::optional<Microseconds> deletingCoordinatorDocDuration;
    };

    void checkStats(const SingleTransactionCoordinatorStats& stats, const Stats& expected) {

        // Start times

        if (expected.createTime) {
            ASSERT_EQ(*expected.createTime, stats.getCreateTime());
        }

        if (expected.writingParticipantListStartTime) {
            ASSERT(*expected.writingParticipantListStartTime ==
                   stats.getWritingParticipantListStartTime());
        }

        if (expected.waitingForVotesStartTime) {
            ASSERT(*expected.waitingForVotesStartTime == stats.getWaitingForVotesStartTime());
        }

        if (expected.writingDecisionStartTime) {
            ASSERT(*expected.writingDecisionStartTime == stats.getWritingDecisionStartTime());
        }

        if (expected.waitingForDecisionAcksStartTime) {
            ASSERT(*expected.waitingForDecisionAcksStartTime ==
                   stats.getWaitingForDecisionAcksStartTime());
        }

        if (expected.deletingCoordinatorDocStartTime) {
            ASSERT(*expected.deletingCoordinatorDocStartTime ==
                   stats.getDeletingCoordinatorDocStartTime());
        }

        if (expected.endTime) {
            ASSERT(*expected.endTime == stats.getEndTime());
        }

        // Durations

        if (expected.totalDuration) {
            ASSERT_EQ(*expected.totalDuration,
                      stats.getDurationSinceCreation(tickSource(), tickSource()->getTicks()));
        }

        if (expected.twoPhaseCommitDuration) {
            ASSERT_EQ(*expected.twoPhaseCommitDuration,
                      stats.getTwoPhaseCommitDuration(tickSource(), tickSource()->getTicks()));
        }

        if (expected.writingParticipantListDuration) {
            ASSERT_EQ(
                *expected.writingParticipantListDuration,
                stats.getWritingParticipantListDuration(tickSource(), tickSource()->getTicks()));
        }

        if (expected.waitingForVotesDuration) {
            ASSERT_EQ(*expected.waitingForVotesDuration,
                      stats.getWaitingForVotesDuration(tickSource(), tickSource()->getTicks()));
        }

        if (expected.writingDecisionDuration) {
            ASSERT_EQ(*expected.writingDecisionDuration,
                      stats.getWritingDecisionDuration(tickSource(), tickSource()->getTicks()));
        }

        if (expected.waitingForDecisionAcksDuration) {
            ASSERT_EQ(
                *expected.waitingForDecisionAcksDuration,
                stats.getWaitingForDecisionAcksDuration(tickSource(), tickSource()->getTicks()));
        }

        if (expected.deletingCoordinatorDocDuration) {
            ASSERT_EQ(
                *expected.deletingCoordinatorDocDuration,
                stats.getDeletingCoordinatorDocDuration(tickSource(), tickSource()->getTicks()));
        }
    }

    struct Metrics {
        // Totals
        std::int64_t totalCreated{0};
        std::int64_t totalStartedTwoPhaseCommit{0};

        // Current in steps
        std::int64_t currentWritingParticipantList{0};
        std::int64_t currentWaitingForVotes{0};
        std::int64_t currentWritingDecision{0};
        std::int64_t currentWaitingForDecisionAcks{0};
        std::int64_t currentDeletingCoordinatorDoc{0};
    };

    void checkMetrics(const Metrics& expectedMetrics) {
        // Totals
        ASSERT_EQ(expectedMetrics.totalCreated, metrics()->getTotalCreated());
        ASSERT_EQ(expectedMetrics.totalStartedTwoPhaseCommit,
                  metrics()->getTotalStartedTwoPhaseCommit());

        // Current in steps
        ASSERT_EQ(expectedMetrics.currentWritingParticipantList,
                  metrics()->getCurrentWritingParticipantList());
        ASSERT_EQ(expectedMetrics.currentWaitingForVotes, metrics()->getCurrentWaitingForVotes());
        ASSERT_EQ(expectedMetrics.currentWritingDecision, metrics()->getCurrentWritingDecision());
        ASSERT_EQ(expectedMetrics.currentWaitingForDecisionAcks,
                  metrics()->getCurrentWaitingForDecisionAcks());
        ASSERT_EQ(expectedMetrics.currentDeletingCoordinatorDoc,
                  metrics()->getCurrentDeletingCoordinatorDoc());
    }

    void checkServerStatus() {
        TransactionCoordinatorsSSS tcsss;
        BSONElement dummy;
        const auto serverStatusSection = tcsss.generateSection(operationContext(), dummy);
        ASSERT_EQ(metrics()->getTotalCreated(), serverStatusSection["totalCreated"].Long());
        ASSERT_EQ(metrics()->getTotalStartedTwoPhaseCommit(),
                  serverStatusSection["totalStartedTwoPhaseCommit"].Long());
        ASSERT_EQ(
            metrics()->getCurrentWritingParticipantList(),
            serverStatusSection.getObjectField("currentInSteps")["writingParticipantList"].Long());
        ASSERT_EQ(metrics()->getCurrentWaitingForVotes(),
                  serverStatusSection.getObjectField("currentInSteps")["waitingForVotes"].Long());
        ASSERT_EQ(metrics()->getCurrentWritingDecision(),
                  serverStatusSection.getObjectField("currentInSteps")["writingDecision"].Long());
        ASSERT_EQ(
            metrics()->getCurrentWaitingForDecisionAcks(),
            serverStatusSection.getObjectField("currentInSteps")["waitingForDecisionAcks"].Long());
        ASSERT_EQ(
            metrics()->getCurrentDeletingCoordinatorDoc(),
            serverStatusSection.getObjectField("currentInSteps")["deletingCoordinatorDoc"].Long());
    }

    Date_t advanceClockSourceAndReturnNewNow() {
        const auto newNow = Date_t::now();
        clockSource()->reset(newNow);
        return newNow;
    }

    void runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines() {
        startCapturingLogMessages();

        TransactionCoordinator coordinator(
            getServiceContext(),
            _lsid,
            _txnNumber,
            std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
            Date_t::max());

        coordinator.runCommit(kTwoShardIdList);

        assertPrepareSentAndRespondWithSuccess();
        assertPrepareSentAndRespondWithSuccess();
        assertCommitSentAndRespondWithSuccess();
        assertCommitSentAndRespondWithSuccess();

        stopCapturingLogMessages();
    }
};

TEST_F(TransactionCoordinatorMetricsTest, SingleCoordinatorStatsSimpleTwoPhaseCommit) {
    Stats expectedStats;
    TransactionCoordinatorMetricsObserver coordinatorObserver;
    const auto& stats = coordinatorObserver.getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);

    // Stats are updated on onCreate.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    coordinatorObserver.onCreate(metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes the total duration to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onStartWritingParticipantList.

    expectedStats.writingParticipantListStartTime = advanceClockSourceAndReturnNewNow();
    expectedStats.twoPhaseCommitDuration = Microseconds(0);
    expectedStats.writingParticipantListDuration = Microseconds(0);
    coordinatorObserver.onStartWritingParticipantList(
        metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes the total duration, two-phase commit duration, and duration writing
    // participant list to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.writingParticipantListDuration =
        *expectedStats.writingParticipantListDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onStartWaitingForVotes.

    expectedStats.waitingForVotesStartTime = advanceClockSourceAndReturnNewNow();
    expectedStats.waitingForVotesDuration = Microseconds(0);
    coordinatorObserver.onStartWaitingForVotes(metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes only the total duration, two-phase commit duration, and duration
    // waiting for votes to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.waitingForVotesDuration =
        *expectedStats.waitingForVotesDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onStartWritingDecision.

    expectedStats.writingDecisionStartTime = advanceClockSourceAndReturnNewNow();
    expectedStats.writingDecisionDuration = Microseconds(0);
    coordinatorObserver.onStartWritingDecision(metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes only the total duration, two-phase commit duration, and duration
    // writing decision to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.writingDecisionDuration =
        *expectedStats.writingDecisionDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onStartWaitingForDecisionAcks.

    expectedStats.waitingForDecisionAcksStartTime = advanceClockSourceAndReturnNewNow();
    expectedStats.waitingForDecisionAcksDuration = Microseconds(0);
    coordinatorObserver.onStartWaitingForDecisionAcks(
        metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes only the total duration, two-phase commit duration, and duration
    // waiting for decision acks to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.waitingForDecisionAcksDuration =
        *expectedStats.waitingForDecisionAcksDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onStartDeletingCoordinatorDoc.

    expectedStats.deletingCoordinatorDocStartTime = advanceClockSourceAndReturnNewNow();
    expectedStats.deletingCoordinatorDocDuration = Microseconds(0);
    coordinatorObserver.onStartDeletingCoordinatorDoc(
        metrics(), tickSource(), clockSource()->now());
    checkStats(stats, expectedStats);

    // Advancing the time causes only the total duration, two-phase commit duration, and duration
    // deleting the coordinator doc to increase.
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.deletingCoordinatorDocDuration =
        *expectedStats.deletingCoordinatorDocDuration + Microseconds(100);
    checkStats(stats, expectedStats);

    // Stats are updated on onEnd.

    expectedStats.endTime = advanceClockSourceAndReturnNewNow();
    coordinatorObserver.onEnd(metrics(),
                              tickSource(),
                              clockSource()->now(),
                              TransactionCoordinator::Step::kDeletingCoordinatorDoc);
    checkStats(stats, expectedStats);

    // Once onEnd has been called, advancing the time does not cause any duration to increase.
    tickSource()->advance(Microseconds(100));
    checkStats(stats, expectedStats);
}

TEST_F(TransactionCoordinatorMetricsTest, ServerWideMetricsSimpleTwoPhaseCommit) {
    TransactionCoordinatorMetricsObserver coordinatorObserver;
    Metrics expectedMetrics;
    checkMetrics(expectedMetrics);

    // Metrics are updated on onCreate.
    expectedMetrics.totalCreated++;
    coordinatorObserver.onCreate(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onStartWritingParticipantList.
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingParticipantList++;
    coordinatorObserver.onStartWritingParticipantList(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onStartWaitingForVotes.
    expectedMetrics.currentWritingParticipantList--;
    expectedMetrics.currentWaitingForVotes++;
    coordinatorObserver.onStartWaitingForVotes(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onStartWritingDecision.
    expectedMetrics.currentWaitingForVotes--;
    expectedMetrics.currentWritingDecision++;
    coordinatorObserver.onStartWritingDecision(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onStartWaitingForDecisionAcks.
    expectedMetrics.currentWritingDecision--;
    expectedMetrics.currentWaitingForDecisionAcks++;
    coordinatorObserver.onStartWaitingForDecisionAcks(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onStartDeletingCoordinatorDoc.
    expectedMetrics.currentWaitingForDecisionAcks--;
    expectedMetrics.currentDeletingCoordinatorDoc++;
    coordinatorObserver.onStartDeletingCoordinatorDoc(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    // Metrics are updated on onEnd.
    expectedMetrics.currentDeletingCoordinatorDoc--;
    coordinatorObserver.onEnd(metrics(),
                              tickSource(),
                              clockSource()->now(),
                              TransactionCoordinator::Step::kDeletingCoordinatorDoc);
    checkMetrics(expectedMetrics);
}

TEST_F(TransactionCoordinatorMetricsTest, ServerWideMetricsSimpleTwoPhaseCommitTwoCoordinators) {
    TransactionCoordinatorMetricsObserver coordinatorObserver1;
    TransactionCoordinatorMetricsObserver coordinatorObserver2;
    Metrics expectedMetrics;
    checkMetrics(expectedMetrics);

    // Increment each coordinator one step at a time.

    expectedMetrics.totalCreated++;
    coordinatorObserver1.onCreate(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.totalCreated++;
    coordinatorObserver2.onCreate(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingParticipantList++;
    coordinatorObserver1.onStartWritingParticipantList(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingParticipantList++;
    coordinatorObserver2.onStartWritingParticipantList(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWritingParticipantList--;
    expectedMetrics.currentWaitingForVotes++;
    coordinatorObserver1.onStartWaitingForVotes(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWritingParticipantList--;
    expectedMetrics.currentWaitingForVotes++;
    coordinatorObserver2.onStartWaitingForVotes(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWaitingForVotes--;
    expectedMetrics.currentWritingDecision++;
    coordinatorObserver1.onStartWritingDecision(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWaitingForVotes--;
    expectedMetrics.currentWritingDecision++;
    coordinatorObserver2.onStartWritingDecision(metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWritingDecision--;
    expectedMetrics.currentWaitingForDecisionAcks++;
    coordinatorObserver1.onStartWaitingForDecisionAcks(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWritingDecision--;
    expectedMetrics.currentWaitingForDecisionAcks++;
    coordinatorObserver2.onStartWaitingForDecisionAcks(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWaitingForDecisionAcks--;
    expectedMetrics.currentDeletingCoordinatorDoc++;
    coordinatorObserver1.onStartDeletingCoordinatorDoc(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentWaitingForDecisionAcks--;
    expectedMetrics.currentDeletingCoordinatorDoc++;
    coordinatorObserver2.onStartDeletingCoordinatorDoc(
        metrics(), tickSource(), clockSource()->now());
    checkMetrics(expectedMetrics);

    expectedMetrics.currentDeletingCoordinatorDoc--;
    coordinatorObserver1.onEnd(metrics(),
                               tickSource(),
                               clockSource()->now(),
                               TransactionCoordinator::Step::kDeletingCoordinatorDoc);
    checkMetrics(expectedMetrics);

    expectedMetrics.currentDeletingCoordinatorDoc--;
    coordinatorObserver2.onEnd(metrics(),
                               tickSource(),
                               clockSource()->now(),
                               TransactionCoordinator::Step::kDeletingCoordinatorDoc);
    checkMetrics(expectedMetrics);
}

TEST_F(TransactionCoordinatorMetricsTest, SimpleTwoPhaseCommitRealCoordinator) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    log() << "Create the coordinator.";

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    log() << "Start two-phase commit (allow the coordinator to progress to writing the participant "
             "list).";

    expectedStats.writingParticipantListStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration = Microseconds(0);
    expectedStats.writingParticipantListDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingParticipantList++;

    setGlobalFailPoint("hangBeforeWaitingForParticipantListWriteConcern",
                       BSON("mode"
                            << "alwaysOn"
                            << "data"
                            << BSON("useUninterruptibleSleep" << 1)));
    coordinator.runCommit(kTwoShardIdList);
    waitUntilCoordinatorDocIsPresent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    log() << "Allow the coordinator to progress to waiting for votes.";

    expectedStats.waitingForVotesStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.writingParticipantListDuration =
        *expectedStats.writingParticipantListDuration + Microseconds(100);
    expectedStats.waitingForVotesDuration = Microseconds(0);
    expectedMetrics.currentWritingParticipantList--;
    expectedMetrics.currentWaitingForVotes++;

    setGlobalFailPoint("hangBeforeWaitingForParticipantListWriteConcern",
                       BSON("mode"
                            << "off"));
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    log() << "Allow the coordinator to progress to writing the decision.";

    expectedStats.writingDecisionStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.waitingForVotesDuration =
        *expectedStats.waitingForVotesDuration + Microseconds(100);
    expectedStats.writingDecisionDuration = Microseconds(0);
    expectedMetrics.currentWaitingForVotes--;
    expectedMetrics.currentWritingDecision++;

    setGlobalFailPoint("hangBeforeWaitingForDecisionWriteConcern",
                       BSON("mode"
                            << "alwaysOn"
                            << "data"
                            << BSON("useUninterruptibleSleep" << 1)));
    // Respond to the second prepare request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertPrepareSentAndRespondWithSuccess();
    auto future = launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); });
    waitUntilCoordinatorDocHasDecision();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    log() << "Allow the coordinator to progress to waiting for acks.";

    expectedStats.waitingForDecisionAcksStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.writingDecisionDuration =
        *expectedStats.writingDecisionDuration + Microseconds(100);
    expectedStats.waitingForDecisionAcksDuration = Microseconds(0);
    expectedMetrics.currentWritingDecision--;
    expectedMetrics.currentWaitingForDecisionAcks++;

    setGlobalFailPoint("hangBeforeWaitingForDecisionWriteConcern",
                       BSON("mode"
                            << "off"));
    // The last thing the coordinator will do on the hijacked prepare response thread is schedule
    // the commitTransaction network requests.
    future.timed_get(kLongFutureTimeout);
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    log() << "Allow the coordinator to progress to deleting the coordinator doc.";

    expectedStats.deletingCoordinatorDocStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.waitingForDecisionAcksDuration =
        *expectedStats.waitingForDecisionAcksDuration + Microseconds(100);
    expectedStats.deletingCoordinatorDocDuration = Microseconds(0);
    expectedMetrics.currentWaitingForDecisionAcks--;
    expectedMetrics.currentDeletingCoordinatorDoc++;

    setGlobalFailPoint("hangAfterDeletingCoordinatorDoc",
                       BSON("mode"
                            << "alwaysOn"
                            << "data"
                            << BSON("useUninterruptibleSleep" << 1)));
    // Respond to the second commit request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertCommitSentAndRespondWithSuccess();
    future = launchAsync([this] { assertCommitSentAndRespondWithSuccess(); });
    waitUntilNoCoordinatorDocIsPresent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    log() << "Allow the coordinator to complete.";

    expectedStats.endTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.twoPhaseCommitDuration =
        *expectedStats.twoPhaseCommitDuration + Microseconds(100);
    expectedStats.deletingCoordinatorDocDuration =
        *expectedStats.deletingCoordinatorDocDuration + Microseconds(100);
    expectedMetrics.currentDeletingCoordinatorDoc--;

    setGlobalFailPoint("hangAfterDeletingCoordinatorDoc",
                       BSON("mode"
                            << "off"));
    // The last thing the coordinator will do on the hijacked commit response thread is signal the
    // coordinator's completion.
    future.timed_get(kLongFutureTimeout);
    coordinator.onCompletion().get();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Slow log line is logged since the coordination completed successfully.
    stopCapturingLogMessages();
    ASSERT_EQUALS(1, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest, CoordinatorIsCanceledWhileInactive) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Cancel the coordinator.

    expectedStats.endTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);

    coordinator.cancelIfCommitNotYetStarted();
    coordinator.onCompletion().get();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Slow log line is not logged since the coordination did not complete successfully.
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest, CoordinatorsAWSIsShutDownWhileCoordinatorIsInactive) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    TransactionCoordinator coordinator(
        getServiceContext(), _lsid, _txnNumber, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    expectedStats.endTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    coordinator.onCompletion().get();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Slow log line is not logged since the coordination did not complete successfully.
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWritingParticipantList) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    TransactionCoordinator coordinator(
        getServiceContext(), _lsid, _txnNumber, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is writing the participant list.

    expectedStats.writingParticipantListStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.writingParticipantListDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingParticipantList++;

    FailPointEnableBlock fp("hangBeforeWaitingForParticipantListWriteConcern");
    coordinator.runCommit(kTwoShardIdList);
    waitUntilCoordinatorDocIsPresent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.writingParticipantListDuration =
        *expectedStats.writingParticipantListDuration + Microseconds(100);
    expectedMetrics.currentWritingParticipantList--;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    coordinator.onCompletion().get();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Slow log line is not logged since the coordination did not complete successfully.
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWaitingForVotes) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    TransactionCoordinator coordinator(
        getServiceContext(), _lsid, _txnNumber, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is waiting for votes.

    expectedStats.waitingForVotesStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.waitingForVotesDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWaitingForVotes++;

    coordinator.runCommit(kTwoShardIdList);
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.waitingForVotesDuration =
        *expectedStats.waitingForVotesDuration + Microseconds(100);
    expectedMetrics.currentWaitingForVotes--;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    network()->enterNetwork();
    network()->runReadyNetworkOperations();
    network()->exitNetwork();
    coordinator.onCompletion().get();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Slow log line is not logged since the coordination did not complete successfully.
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWritingDecision) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    TransactionCoordinator coordinator(
        getServiceContext(), _lsid, _txnNumber, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is writing the decision.

    expectedStats.writingDecisionStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.writingDecisionDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWritingDecision++;

    FailPointEnableBlock fp("hangBeforeWaitingForDecisionWriteConcern");

    coordinator.runCommit(kTwoShardIdList);
    // Respond to the second prepare request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertPrepareSentAndRespondWithSuccess();
    auto future = launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); });
    waitUntilCoordinatorDocHasDecision();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.writingDecisionDuration =
        *expectedStats.writingDecisionDuration + Microseconds(100);
    expectedMetrics.currentWritingDecision--;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    coordinator.onCompletion().get();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Slow log line is not logged since the coordination did not complete successfully.
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest,
       CoordinatorsAWSIsShutDownWhileCoordinatorIsWaitingForDecisionAcks) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    TransactionCoordinator coordinator(
        getServiceContext(), _lsid, _txnNumber, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is waiting for decision acks.

    expectedStats.waitingForDecisionAcksStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.waitingForDecisionAcksDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentWaitingForDecisionAcks++;

    coordinator.runCommit(kTwoShardIdList);
    // Respond to the second prepare request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertPrepareSentAndRespondWithSuccess();
    auto future = launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); });
    // The last thing the coordinator will do on the hijacked prepare response thread is schedule
    // the commitTransaction network requests.
    future.timed_get(kLongFutureTimeout);
    waitUntilMessageSent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.waitingForDecisionAcksDuration =
        *expectedStats.waitingForDecisionAcksDuration + Microseconds(100);
    expectedMetrics.currentWaitingForDecisionAcks--;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    network()->enterNetwork();
    network()->runReadyNetworkOperations();
    network()->exitNetwork();
    coordinator.onCompletion().get();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Slow log line is not logged since the coordination did not complete successfully.
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest, CoordinatorsAWSIsShutDownWhileCoordinatorIsDeletingDoc) {
    startCapturingLogMessages();

    Stats expectedStats;
    Metrics expectedMetrics;

    checkMetrics(expectedMetrics);

    // Create the coordinator.

    expectedStats.createTime = advanceClockSourceAndReturnNewNow();
    expectedStats.totalDuration = Microseconds(0);
    expectedMetrics.totalCreated++;

    auto aws = std::make_unique<txn::AsyncWorkScheduler>(getServiceContext());
    auto awsPtr = aws.get();
    TransactionCoordinator coordinator(
        getServiceContext(), _lsid, _txnNumber, std::move(aws), Date_t::max());
    const auto& stats =
        coordinator.getMetricsObserverForTest().getSingleTransactionCoordinatorStats();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Wait until the coordinator is deleting the coordinator doc.

    expectedStats.deletingCoordinatorDocStartTime = advanceClockSourceAndReturnNewNow();
    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.deletingCoordinatorDocDuration = Microseconds(0);
    expectedMetrics.totalStartedTwoPhaseCommit++;
    expectedMetrics.currentDeletingCoordinatorDoc++;

    FailPointEnableBlock fp("hangAfterDeletingCoordinatorDoc");

    coordinator.runCommit(kTwoShardIdList);
    // Respond to the second prepare request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertPrepareSentAndRespondWithSuccess();
    auto future = launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); });
    // The last thing the coordinator will do on the hijacked prepare response thread is
    // schedule the commitTransaction network requests.
    future.timed_get(kLongFutureTimeout);
    waitUntilMessageSent();
    // Respond to the second commit request in a separate thread, because the coordinator will
    // hijack that thread to run its continuation.
    assertCommitSentAndRespondWithSuccess();
    future = launchAsync([this] { assertCommitSentAndRespondWithSuccess(); });
    waitUntilNoCoordinatorDocIsPresent();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Shut down the coordinator's AWS.

    tickSource()->advance(Microseconds(100));
    expectedStats.totalDuration = *expectedStats.totalDuration + Microseconds(100);
    expectedStats.deletingCoordinatorDocDuration =
        *expectedStats.deletingCoordinatorDocDuration + Microseconds(100);
    expectedMetrics.currentDeletingCoordinatorDoc--;

    awsPtr->shutdown({ErrorCodes::TransactionCoordinatorSteppingDown, "dummy"});
    // The last thing the coordinator will do on the hijacked commit response thread is signal
    // the coordinator's completion.
    future.timed_get(kLongFutureTimeout);
    coordinator.onCompletion().get();

    checkStats(stats, expectedStats);
    checkMetrics(expectedMetrics);

    // Slow log line is not logged since the coordination did not complete successfully.
    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest, LogsTransactionAtLogLevelOne) {
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kTransaction,
                                                        logger::LogSeverity::Debug(1));
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    ASSERT_EQUALS(1, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest, DoesNotLogTransactionAtLogLevelZero) {
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kTransaction,
                                                        logger::LogSeverity::Log());
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    ASSERT_EQUALS(0, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest, DoesNotLogTransactionsUnderSlowMSThreshold) {
    // Set the log level to 0 so that the slow logging is only done if the transaction exceeds the
    // slowMS setting.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kTransaction,
                                                        logger::LogSeverity::Log());
    serverGlobalParams.slowMS = 100;
    startCapturingLogMessages();

    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    coordinator.runCommit(kTwoShardIdList);

    tickSource()->advance(Milliseconds(99));

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(
    TransactionCoordinatorMetricsTest,
    DoesNotLogTransactionsUnderSlowMSThresholdEvenIfCoordinatorHasExistedForLongerThanSlowThreshold) {
    // Set the log level to 0 so that the slow logging is only done if the transaction exceeds the
    // slowMS setting.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kTransaction,
                                                        logger::LogSeverity::Log());
    serverGlobalParams.slowMS = 100;
    startCapturingLogMessages();

    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    tickSource()->advance(Milliseconds(101));

    coordinator.runCommit(kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    stopCapturingLogMessages();
    ASSERT_EQUALS(0, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest, LogsTransactionsOverSlowMSThreshold) {
    // Set the log level to 0 so that the slow logging is only done if the transaction exceeds the
    // slowMS setting.
    logger::globalLogDomain()->setMinimumLoggedSeverity(logger::LogComponent::kTransaction,
                                                        logger::LogSeverity::Log());
    serverGlobalParams.slowMS = 100;
    startCapturingLogMessages();

    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    coordinator.runCommit(kTwoShardIdList);

    tickSource()->advance(Milliseconds(101));

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    stopCapturingLogMessages();

    ASSERT_EQUALS(1, countLogLinesContaining("two-phase commit parameters:"));
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesTransactionParameters) {
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    BSONObjBuilder lsidBob;
    _lsid.serialize(&lsidBob);
    ASSERT_EQUALS(
        1,
        countLogLinesContaining(str::stream() << "parameters:{ lsid: " << lsidBob.done().toString()
                                              << ", txnNumber: "
                                              << _txnNumber));
}

TEST_F(TransactionCoordinatorMetricsTest,
       SlowLogLineIncludesTerminationCauseAndCommitTimestampForCommitDecision) {
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    ASSERT_EQUALS(
        1, countLogLinesContaining("terminationCause:committed, commitTimestamp: Timestamp(1, 1)"));
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesTerminationCauseForAbortDecision) {
    startCapturingLogMessages();

    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    coordinator.runCommit(kTwoShardIdList);

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithNoSuchTransaction();
    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    stopCapturingLogMessages();

    ASSERT_EQUALS(1, countLogLinesContaining("terminationCause:aborted"));
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesNumParticipants) {
    runSimpleTwoPhaseCommitWithCommitDecisionAndCaptureLogLines();
    ASSERT_EQUALS(1, countLogLinesContaining("numParticipants:2"));
}

TEST_F(TransactionCoordinatorMetricsTest, SlowLogLineIncludesStepDurationsAndTotalDuration) {
    startCapturingLogMessages();

    TransactionCoordinator coordinator(
        getServiceContext(),
        _lsid,
        _txnNumber,
        std::make_unique<txn::AsyncWorkScheduler>(getServiceContext()),
        Date_t::max());

    {
        FailPointEnableBlock fp("hangBeforeWaitingForParticipantListWriteConcern",
                                BSON("useUninterruptibleSleep" << 1));

        coordinator.runCommit(kTwoShardIdList);
        waitUntilCoordinatorDocIsPresent();

        // Increase the duration spent writing the participant list.
        tickSource()->advance(Milliseconds(100));
    }

    waitUntilMessageSent();

    // Increase the duration spent waiting for votes.
    tickSource()->advance(Milliseconds(100));

    boost::optional<executor::NetworkTestEnv::FutureHandle<void>> futureOption;

    {
        FailPointEnableBlock fp("hangBeforeWaitingForDecisionWriteConcern",
                                BSON("useUninterruptibleSleep" << 1));

        // Respond to the second prepare request in a separate thread, because the coordinator will
        // hijack that thread to run its continuation.
        assertPrepareSentAndRespondWithSuccess();
        futureOption.emplace(launchAsync([this] { assertPrepareSentAndRespondWithSuccess(); }));
        waitUntilCoordinatorDocHasDecision();

        // Increase the duration spent writing the decision.
        tickSource()->advance(Milliseconds(100));
    }

    // The last thing the coordinator will do on the hijacked prepare response thread is schedule
    // the commitTransaction network requests.
    futureOption->timed_get(kLongFutureTimeout);
    waitUntilMessageSent();

    // Increase the duration spent waiting for decision acks.
    tickSource()->advance(Milliseconds(100));

    {
        FailPointEnableBlock fp("hangAfterDeletingCoordinatorDoc",
                                BSON("useUninterruptibleSleep" << 1));

        // Respond to the second commit request in a separate thread, because the coordinator will
        // hijack that thread to run its continuation.
        assertCommitSentAndRespondWithSuccess();
        futureOption.emplace(launchAsync([this] { assertCommitSentAndRespondWithSuccess(); }));
        waitUntilNoCoordinatorDocIsPresent();

        // Increase the duration spent deleting the coordinator doc.
        tickSource()->advance(Milliseconds(100));
    }

    // The last thing the coordinator will do on the hijacked commit response thread is signal the
    // coordinator's completion.
    futureOption->timed_get(kLongFutureTimeout);
    coordinator.onCompletion().get();

    stopCapturingLogMessages();

    // Note: The waiting for decision acks and deleting coordinator doc durations are not reported.
    ASSERT_EQUALS(1,
                  countLogLinesContaining("stepDurations:{ writingParticipantListMicros: "
                                          "100000, waitingForVotesMicros: 100000, "
                                          "writingDecisionMicros: 100000, "
                                          "waitingForDecisionAcksMicros: 100000, "
                                          "deletingCoordinatorDocMicros: 100000 }"));
    ASSERT_EQUALS(1, countLogLinesContaining(" 500ms\n") + countLogLinesContaining(" 500ms\r\n"));
}

TEST_F(TransactionCoordinatorMetricsTest, ServerStatusSectionIncludesTotalCreated) {
    metrics()->incrementTotalCreated();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest, ServerStatusSectionIncludesTotalStartedTwoPhaseCommit) {
    metrics()->incrementTotalStartedTwoPhaseCommit();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest,
       ServerStatusSectionIncludesCurrentWritingParticipantList) {
    metrics()->incrementCurrentWritingParticipantList();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest, ServerStatusSectionIncludesCurrentWaitingForVotes) {
    metrics()->incrementCurrentWaitingForVotes();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest, ServerStatusSectionIncludesCurrentWritingDecision) {
    metrics()->incrementCurrentWritingDecision();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest,
       ServerStatusSectionIncludesCurrentWaitingForDecisionAcks) {
    metrics()->incrementCurrentWaitingForDecisionAcks();
    checkServerStatus();
}

TEST_F(TransactionCoordinatorMetricsTest,
       ServerStatusSectionIncludesCurrentDeletingCoordinatorDoc) {
    metrics()->incrementCurrentDeletingCoordinatorDoc();
    checkServerStatus();
}

}  // namespace
}  // namespace mongo
