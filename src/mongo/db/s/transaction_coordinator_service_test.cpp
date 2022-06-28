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

#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/s/transaction_coordinator_service.h"
#include "mongo/db/s/transaction_coordinator_test_fixture.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const Date_t kCommitDeadline = Date_t::max();

const BSONObj kDummyWriteConcernError = BSON("code" << ErrorCodes::WriteConcernFailed << "errmsg"
                                                    << "dummy");

const StatusWith<BSONObj> kNoSuchTransactionAndWriteConcernError =
    BSON("ok" << 0 << "code" << ErrorCodes::NoSuchTransaction << "writeConcernError"
              << kDummyWriteConcernError);

const StatusWith<BSONObj> kNoSuchTransaction =
    BSON("ok" << 0 << "code" << ErrorCodes::NoSuchTransaction);
const StatusWith<BSONObj> kOk = BSON("ok" << 1);
const StatusWith<BSONObj> kOkButWriteConcernError =
    BSON("ok" << 1 << "writeConcernError" << kDummyWriteConcernError);

const StatusWith<BSONObj> kPrepareOk = BSON("ok" << 1 << "prepareTimestamp" << Timestamp(1, 1));
const StatusWith<BSONObj> kPrepareOkButWriteConcernError =
    BSON("ok" << 1 << "prepareTimestamp" << Timestamp(1, 1) << "writeConcernError"
              << kDummyWriteConcernError);

class TransactionCoordinatorServiceTestFixture : public TransactionCoordinatorTestFixture {
protected:
    void assertPrepareSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(
            PrepareTransaction::kCommandName, kPrepareOk, WriteConcernOptions::Majority);
    }

    void assertPrepareSentAndRespondWithSuccessAndWriteConcernError() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kPrepareOkButWriteConcernError,
                                        WriteConcernOptions::Majority);
        advanceClockAndExecuteScheduledTasks();
    }

    void assertPrepareSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(
            PrepareTransaction::kCommandName, kNoSuchTransaction, WriteConcernOptions::Majority);
    }

    void assertPrepareSentAndRespondWithNoSuchTransactionAndWriteConcernError() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kNoSuchTransactionAndWriteConcernError,
                                        WriteConcernOptions::Majority);
        advanceClockAndExecuteScheduledTasks();
    }

    // Abort responses

    void assertAbortSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith("abortTransaction", kOk, WriteConcernOptions::Majority);
    }

    void assertAbortSentAndRespondWithSuccessAndWriteConcernError() {
        assertCommandSentAndRespondWith(
            "abortTransaction", kOkButWriteConcernError, WriteConcernOptions::Majority);
        advanceClockAndExecuteScheduledTasks();
    }

    void assertAbortSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(
            "abortTransaction", kNoSuchTransaction, WriteConcernOptions::Majority);
    }

    void assertAbortSentAndRespondWithNoSuchTransactionAndWriteConcernError() {
        assertCommandSentAndRespondWith("abortTransaction",
                                        kNoSuchTransactionAndWriteConcernError,
                                        WriteConcernOptions::Majority);
        advanceClockAndExecuteScheduledTasks();
    }

    // Commit responses

    void assertCommitSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(
            CommitTransaction::kCommandName, kOk, WriteConcernOptions::Majority);
    }

    void assertCommitSentAndRespondWithSuccessAndWriteConcernError() {
        assertCommandSentAndRespondWith(CommitTransaction::kCommandName,
                                        kOkButWriteConcernError,
                                        WriteConcernOptions::Majority);
        advanceClockAndExecuteScheduledTasks();
    }

    void assertCommitSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(
            CommitTransaction::kCommandName, kRetryableError, WriteConcernOptions::Majority);
        advanceClockAndExecuteScheduledTasks();
    }

    // Other

    void assertNoMessageSent() {
        executor::NetworkInterfaceMock::InNetworkGuard networkGuard(network());
        ASSERT_FALSE(network()->hasReadyRequests());
    }

    /**
     * Goes through the steps to commit a transaction through the coordinator service  for a given
     * lsid and txnNumber. Useful when not explictly testing the commit protocol.
     */
    void commitTransaction(TransactionCoordinatorService& coordinatorService,
                           const LogicalSessionId& lsid,
                           const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                           const std::set<ShardId>& transactionParticipantShards) {
        auto commitDecisionFuture = *coordinatorService.coordinateCommit(
            operationContext(), lsid, txnNumberAndRetryCounter, transactionParticipantShards);

        for (size_t i = 0; i < transactionParticipantShards.size(); ++i) {
            assertPrepareSentAndRespondWithSuccess();
        }

        for (size_t i = 0; i < transactionParticipantShards.size(); ++i) {
            assertCommitSentAndRespondWithSuccess();
        }

        // Wait for commit to complete.
        commitDecisionFuture.get();
    }

    /**
     * Goes through the steps to abort a transaction through the coordinator service for a given
     * lsid and txnNumber. Useful when not explictly testing the abort protocol.
     */
    void abortTransaction(TransactionCoordinatorService& coordinatorService,
                          const LogicalSessionId& lsid,
                          const TxnNumberAndRetryCounter& txnNumberAndRetryCounter,
                          const std::set<ShardId>& shardIdSet,
                          const ShardId& abortingShard) {
        auto commitDecisionFuture = *coordinatorService.coordinateCommit(
            operationContext(), lsid, txnNumberAndRetryCounter, shardIdSet);

        // It is sufficient to abort just one of the participants
        assertPrepareSentAndRespondWithNoSuchTransaction();

        for (size_t i = 0; i < shardIdSet.size(); ++i) {
            assertAbortSentAndRespondWithSuccess();
        }

        // Wait for abort to complete.
        ASSERT_THROWS_CODE(
            commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    }

    auto* service() const {
        return TransactionCoordinatorService::get(operationContext());
    }

    TxnNumberAndRetryCounter _txnNumberAndRetryCounter{1, 1};
};

using TransactionCoordinatorServiceStepUpStepDownTest = TransactionCoordinatorServiceTestFixture;

TEST_F(TransactionCoordinatorServiceStepUpStepDownTest, OperationsFailBeforeStepUpStarts) {
    ASSERT_THROWS_CODE(service()->createCoordinator(operationContext(),
                                                    makeLogicalSessionIdForTest(),
                                                    _txnNumberAndRetryCounter,
                                                    kCommitDeadline),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);

    ASSERT_THROWS_CODE(service()->coordinateCommit(operationContext(),
                                                   makeLogicalSessionIdForTest(),
                                                   _txnNumberAndRetryCounter,
                                                   kTwoShardIdSet),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);

    ASSERT_THROWS_CODE(service()->recoverCommit(operationContext(),
                                                makeLogicalSessionIdForTest(),
                                                _txnNumberAndRetryCounter),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);
}

TEST_F(TransactionCoordinatorServiceStepUpStepDownTest, OperationsBlockBeforeStepUpCompletes) {
    service()->onStepUp(operationContext(), Milliseconds(1));
    ScopeGuard stepDownGuard([&] { service()->onStepDown(); });

    ASSERT_THROWS_CODE(operationContext()->runWithDeadline(
                           Date_t::now() + Milliseconds{5},
                           ErrorCodes::NetworkInterfaceExceededTimeLimit,
                           [&] {
                               return service()->coordinateCommit(operationContext(),
                                                                  makeLogicalSessionIdForTest(),
                                                                  _txnNumberAndRetryCounter,
                                                                  kTwoShardIdSet);
                           }),
                       AssertionException,
                       ErrorCodes::NetworkInterfaceExceededTimeLimit);

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(network());
        network()->advanceTime(network()->now() + Milliseconds(1));
    }

    ASSERT(service()->coordinateCommit(operationContext(),
                                       makeLogicalSessionIdForTest(),
                                       _txnNumberAndRetryCounter,
                                       kTwoShardIdSet) == boost::none);
}

TEST_F(TransactionCoordinatorServiceStepUpStepDownTest, StepUpFailsDueToBadCoordinatorDocument) {
    DBDirectClient client(operationContext());

    auto response = client.insertAcknowledged(
        NamespaceString::kTransactionCoordinatorsNamespace.ns(), {BSON("IllegalKey" << 1)});
    ASSERT_OK(getStatusFromWriteCommandReply(response));
    ASSERT_EQ(1, response["n"].Int());

    service()->onStepUp(operationContext());
    ScopeGuard stepDownGuard([&] { service()->onStepDown(); });

    ASSERT_THROWS_CODE(service()->coordinateCommit(operationContext(),
                                                   makeLogicalSessionIdForTest(),
                                                   _txnNumberAndRetryCounter,
                                                   kTwoShardIdSet),
                       AssertionException,
                       ErrorCodes::TypeMismatch);

    ASSERT_THROWS_CODE(service()->recoverCommit(operationContext(),
                                                makeLogicalSessionIdForTest(),
                                                _txnNumberAndRetryCounter),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(TransactionCoordinatorServiceStepUpStepDownTest, StepUpRecoverTxnRetryCounter) {
    auto lsid = makeLogicalSessionIdForTest();

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(_txnNumberAndRetryCounter.getTxnNumber());
    sessionInfo.setTxnRetryCounter(*_txnNumberAndRetryCounter.getTxnRetryCounter());

    txn::TransactionCoordinatorDocument doc;
    doc.setId(std::move(sessionInfo));
    doc.setParticipants(kOneShardIdList);

    DBDirectClient client(operationContext());
    auto response = client.insertAcknowledged(
        NamespaceString::kTransactionCoordinatorsNamespace.ns(), {doc.toBSON()});
    ASSERT_OK(getStatusFromWriteCommandReply(response));
    ASSERT_EQ(1, response["n"].Int());

    service()->onStepUp(operationContext());
    ScopeGuard stepDownGuard([&] { service()->onStepDown(); });

    // Cannot recover commit with an incorrect txnRetryCounter.
    ASSERT(!service()->recoverCommit(operationContext(),
                                     lsid,
                                     {_txnNumberAndRetryCounter.getTxnNumber(),
                                      *_txnNumberAndRetryCounter.getTxnRetryCounter() - 1}));
    ASSERT(!service()->recoverCommit(operationContext(),
                                     lsid,
                                     {_txnNumberAndRetryCounter.getTxnNumber(),
                                      *_txnNumberAndRetryCounter.getTxnRetryCounter() + 1}));
    auto commitDecisionFuture =
        *service()->recoverCommit(operationContext(), lsid, _txnNumberAndRetryCounter);

    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    ASSERT_EQ(static_cast<int>(commitDecisionFuture.get()),
              static_cast<int>(txn::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceStepUpStepDownTest, StepDownBeforeStepUpTaskCompleted) {
    // Call step-up with 1ms delay (meaning it will not actually execute until time is manually
    // advanced on the underlying executor)
    service()->onStepUp(operationContext(), Milliseconds(1));

    // Should cancel all outstanding tasks (including the recovery task started by onStepUp above,
    // which has not yet run)
    service()->onStepDown();

    // Do another onStepUp to ensure it runs successfully
    service()->onStepUp(operationContext());

    // Step-down the service so that the destructor does not complain
    service()->onStepDown();
}

class TransactionCoordinatorServiceTest : public TransactionCoordinatorServiceTestFixture {
protected:
    void setUp() override {
        TransactionCoordinatorServiceTestFixture::setUp();

        service()->onStepUp(operationContext());
    }

    void tearDown() override {
        service()->onStepDown();
        service()->joinPreviousRound();

        TransactionCoordinatorServiceTestFixture::tearDown();
    }

    LogicalSessionId _lsid{makeLogicalSessionIdForTest()};
    TxnNumberAndRetryCounter _txnNumberAndRetryCounter{1, 1};
};

TEST_F(TransactionCoordinatorServiceTest, CreateCoordinatorOnNewSessionSucceeds) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);
    commitTransaction(*coordinatorService, _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorForExistingSessionWithPreviouslyCommittedTxnSucceeds) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);
    commitTransaction(*coordinatorService, _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    TxnNumberAndRetryCounter otherTxnNumberAndRetryCounter{
        _txnNumberAndRetryCounter.getTxnNumber() + 1,
        *_txnNumberAndRetryCounter.getTxnRetryCounter()};
    coordinatorService->createCoordinator(
        operationContext(), _lsid, otherTxnNumberAndRetryCounter, kCommitDeadline);
    commitTransaction(*coordinatorService, _lsid, otherTxnNumberAndRetryCounter, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorForExistingSessionAndTxnNumberWithPreviouslyCommittedTxnRetryCounterFails) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    {
        // Set this server parameter so coordinateCommit returns the decision future instead of the
        // completion future.
        RAIIServerParameterControllerForTest controller{
            "coordinateCommitReturnImmediatelyAfterPersistingDecision", true};
        auto decisionFuture = *coordinatorService->coordinateCommit(
            operationContext(), _lsid, _txnNumberAndRetryCounter, kOneShardIdSet);
        assertCommandSentAndRespondWith("prepareTransaction", kPrepareOk, boost::none);
        decisionFuture.get();
    }

    TxnNumberAndRetryCounter otherTxnNumberAndRetryCounter{
        _txnNumberAndRetryCounter.getTxnNumber(),
        *_txnNumberAndRetryCounter.getTxnRetryCounter() + 1};
    ASSERT_THROWS_CODE(
        coordinatorService->createCoordinator(
            operationContext(), _lsid, otherTxnNumberAndRetryCounter, kCommitDeadline),
        AssertionException,
        6032301);

    auto completionFuture =
        *coordinatorService->recoverCommit(operationContext(), _lsid, _txnNumberAndRetryCounter);
    assertCommandSentAndRespondWith("commitTransaction", kOk, boost::none);
    completionFuture.get();
}

TEST_F(TransactionCoordinatorServiceTest,
       RetryingCreateCoordinatorForSameLsidAndTxnNumberSucceeds) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);
    // Retry create. This should succeed but not replace the old coordinator.
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    commitTransaction(*coordinatorService, _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorWithHigherTxnNumberThanOngoingCommittingTxnCommitsPreviousTxnAndSucceeds) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    // Progress the transaction up until the point where it has sent commit and is waiting for
    // commit acks.
    auto oldTxnCommitCompletionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    // Simulate all participants acking prepare/voting to commit.
    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    // Create a coordinator for a higher transaction number in the same session. This should
    // "tryAbort" on the old coordinator which should NOT abort it since it's already waiting for
    // commit acks.
    TxnNumberAndRetryCounter newTxnNumberAndRetryCounter{
        _txnNumberAndRetryCounter.getTxnNumber() + 1,
        *_txnNumberAndRetryCounter.getTxnRetryCounter()};
    coordinatorService->createCoordinator(
        operationContext(), _lsid, newTxnNumberAndRetryCounter, kCommitDeadline);

    // Finish committing the old transaction by sending it commit acks from both participants.
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    // The old transaction should now be committed.
    ASSERT_EQ(static_cast<int>(oldTxnCommitCompletionFuture.get()),
              static_cast<int>(txn::CommitDecision::kCommit));

    auto newTxnCompletionDecisionFuture = coordinatorService->coordinateCommit(
        operationContext(), _lsid, newTxnNumberAndRetryCounter, kTwoShardIdSet);

    commitTransaction(*coordinatorService, _lsid, newTxnNumberAndRetryCounter, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorWithHigherTxnRetryCounterThanOngoingAbortingTxnCanCommit) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    {
        // Progress the transaction up until the point where one participant voted to abort and the
        // coordinator has sent abort and is waiting for an abort ack.

        // Set this server parameter so coordinateCommit returns the decision future instead of the
        // completion future.
        RAIIServerParameterControllerForTest controller{
            "coordinateCommitReturnImmediatelyAfterPersistingDecision", true};

        auto oldTxnCommitDecisionFuture = *coordinatorService->coordinateCommit(
            operationContext(), _lsid, _txnNumberAndRetryCounter, kOneShardIdSet);
        assertPrepareSentAndRespondWithNoSuchTransaction();
        oldTxnCommitDecisionFuture.wait();
    }

    // Create a coordinator with a higher transaction retry counter.
    TxnNumberAndRetryCounter newTxnNumberAndRetryCounter{
        _txnNumberAndRetryCounter.getTxnNumber(),
        *_txnNumberAndRetryCounter.getTxnRetryCounter() + 1};
    coordinatorService->createCoordinator(
        operationContext(), _lsid, newTxnNumberAndRetryCounter, kCommitDeadline);

    auto oldTxnCommitCompletionFuture =
        *coordinatorService->recoverCommit(operationContext(), _lsid, _txnNumberAndRetryCounter);

    // Finish aborting the original commit by sending an abort ack.
    assertAbortSentAndRespondWithSuccess();

    auto newTxnCompletionDecisionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, newTxnNumberAndRetryCounter, kOneShardIdSet);

    // Simulate acking prepare/voting to commit.
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        oldTxnCommitCompletionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    ASSERT_EQ(static_cast<int>(newTxnCompletionDecisionFuture.get()),
              static_cast<int>(txn::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceTest, CoordinateCommitReturnsNoneIfNoCoordinatorEverExisted) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    auto commitDecisionFuture = coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);
    ASSERT(boost::none == commitDecisionFuture);
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinateCommitWithSameParticipantListJoinsOngoingCoordinationThatLeadsToAbort) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    auto commitDecisionFuture1 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    assertPrepareSentAndRespondWithNoSuchTransaction();
    advanceClockAndExecuteScheduledTasks();  // Make sure the cancellation callback is delivered

    auto commitDecisionFuture2 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture1.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    ASSERT_THROWS_CODE(
        commitDecisionFuture2.get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinateCommitWithSameParticipantListJoinsOngoingCoordinationThatLeadsToCommit) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    auto commitDecisionFuture1 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    assertPrepareSentAndRespondWithSuccess();

    auto commitDecisionFuture2 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(TransactionCoordinatorServiceTest, RecoverCommitJoinsOngoingCoordinationThatLeadsToAbort) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    auto commitDecisionFuture1 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    assertPrepareSentAndRespondWithNoSuchTransaction();
    advanceClockAndExecuteScheduledTasks();  // Make sure the cancellation callback is delivered

    auto commitDecisionFuture2 =
        *coordinatorService->recoverCommit(operationContext(), _lsid, _txnNumberAndRetryCounter);

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture1.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    ASSERT_THROWS_CODE(
        commitDecisionFuture2.get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorServiceTest, RecoverCommitJoinsOngoingCoordinationThatLeadsToCommit) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    auto commitDecisionFuture1 = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    assertPrepareSentAndRespondWithSuccess();

    auto commitDecisionFuture2 =
        *coordinatorService->recoverCommit(operationContext(), _lsid, _txnNumberAndRetryCounter);

    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(TransactionCoordinatorServiceTest,
       RecoverCommitWorksIfCommitNeverReceivedAndCoordinationCanceled) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());

    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    auto commitDecisionFuture =
        *coordinatorService->recoverCommit(operationContext(), _lsid, _txnNumberAndRetryCounter);

    // Cancel previous coordinator by creating a new coordinator at a higher txn number.
    TxnNumberAndRetryCounter newTxnNumberAndRetryCounter{
        _txnNumberAndRetryCounter.getTxnNumber() + 1,
        *_txnNumberAndRetryCounter.getTxnRetryCounter()};
    coordinatorService->createCoordinator(
        operationContext(), _lsid, newTxnNumberAndRetryCounter, kCommitDeadline);

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::TransactionCoordinatorCanceled);
}

TEST_F(
    TransactionCoordinatorServiceTest,
    CreateCoordinatorWithHigherTxnNumberThanExistingButNotYetCommittingTxnCancelsPreviousTxnAndSucceeds) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    // Create a coordinator for a higher transaction number in the same session. This should
    // cancel commit on the old coordinator.
    TxnNumberAndRetryCounter newTxnNumberAndRetryCounter{
        _txnNumberAndRetryCounter.getTxnNumber() + 1,
        *_txnNumberAndRetryCounter.getTxnRetryCounter()};
    coordinatorService->createCoordinator(
        operationContext(), _lsid, newTxnNumberAndRetryCounter, kCommitDeadline);
    auto newTxnCompletionDecisionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, newTxnNumberAndRetryCounter, kTwoShardIdSet);

    // Since this transaction has already been canceled, this should return boost::none.
    auto oldTxnCommitCompletionFuture = coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    // The old transaction should now be canceled.
    if (oldTxnCommitCompletionFuture) {
        ASSERT_THROWS_CODE(oldTxnCommitCompletionFuture->get(),
                           AssertionException,
                           ErrorCodes::TransactionCoordinatorCanceled);
    }

    // Make sure the newly created one works fine too.
    commitTransaction(*coordinatorService, _lsid, newTxnNumberAndRetryCounter, kTwoShardIdSet);
}

TEST_F(TransactionCoordinatorServiceTest,
       CreateCoordinatorWithHigherTxnRetryCounterThanExistingButNotYetCommittingTxnFails) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    auto completionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kOneShardIdSet);

    TxnNumberAndRetryCounter newTxnNumberAndRetryCounter{
        _txnNumberAndRetryCounter.getTxnNumber(),
        *_txnNumberAndRetryCounter.getTxnRetryCounter() + 1};
    ASSERT_THROWS_CODE(coordinatorService->createCoordinator(
                           operationContext(), _lsid, newTxnNumberAndRetryCounter, kCommitDeadline),
                       AssertionException,
                       6032300);

    assertCommandSentAndRespondWith("prepareTransaction", kNoSuchTransaction, boost::none);
    assertCommandSentAndRespondWith("abortTransaction", kOk, boost::none);
    ASSERT_THROWS_CODE(completionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorServiceTest, CoordinatorRetriesOnWriteConcernErrorToPrepare) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    // Coordinator sends prepare.
    auto commitDecisionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    // One participant responds with writeConcern error.
    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccessAndWriteConcernError();

    // Coordinator retries prepare against participant that responded with writeConcern error until
    // participant responds without writeConcern error.
    assertPrepareSentAndRespondWithSuccessAndWriteConcernError();
    assertPrepareSentAndRespondWithSuccessAndWriteConcernError();
    assertPrepareSentAndRespondWithSuccessAndWriteConcernError();
    assertPrepareSentAndRespondWithNoSuchTransactionAndWriteConcernError();
    assertPrepareSentAndRespondWithNoSuchTransactionAndWriteConcernError();
    assertPrepareSentAndRespondWithSuccessAndWriteConcernError();
    assertPrepareSentAndRespondWithSuccess();

    // Coordinator sends commit.
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    // The transaction should now be committed.
    ASSERT_EQ(static_cast<int>(commitDecisionFuture.get()),
              static_cast<int>(txn::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceTest, CoordinatorRetriesOnWriteConcernErrorToAbort) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    // Coordinator sends prepare.
    auto commitDecisionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    // One participant votes to abort.
    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithNoSuchTransaction();

    // One participant responds to abort with success.
    assertAbortSentAndRespondWithSuccess();

    // Coordinator retries abort against other participant until other participant responds without
    // writeConcern error.
    assertAbortSentAndRespondWithSuccessAndWriteConcernError();
    assertAbortSentAndRespondWithSuccessAndWriteConcernError();
    assertAbortSentAndRespondWithSuccessAndWriteConcernError();
    assertAbortSentAndRespondWithSuccessAndWriteConcernError();
    assertAbortSentAndRespondWithNoSuchTransactionAndWriteConcernError();
    assertAbortSentAndRespondWithNoSuchTransactionAndWriteConcernError();
    assertAbortSentAndRespondWithNoSuchTransaction();

    // The transaction should now be aborted.
    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorServiceTest, CoordinatorRetriesOnWriteConcernErrorToCommit) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);

    // Coordinator sends prepare.
    auto commitDecisionFuture = *coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    // Both participants vote to commit.
    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();

    // One participant responds to commit with success.
    assertCommitSentAndRespondWithSuccess();

    // Coordinator retries commit against other participant until other participant responds without
    // writeConcern error.
    assertCommitSentAndRespondWithSuccessAndWriteConcernError();
    assertCommitSentAndRespondWithSuccessAndWriteConcernError();
    assertCommitSentAndRespondWithSuccessAndWriteConcernError();
    assertCommitSentAndRespondWithSuccessAndWriteConcernError();
    assertCommitSentAndRespondWithSuccessAndWriteConcernError();
    assertCommitSentAndRespondWithSuccess();

    // The transaction should now be committed.
    ASSERT_EQ(static_cast<int>(commitDecisionFuture.get()),
              static_cast<int>(txn::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinatorIsCanceledIfDeadlinePassesAndHasNotReceivedParticipantList) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    const auto deadline = executor()->now() + Milliseconds(1000 * 60 * 10 /* 10 hours */);
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, deadline);

    // Reach the deadline.
    network()->enterNetwork();
    network()->advanceTime(deadline);
    network()->exitNetwork();

    // The coordinator should no longer exist.
    ASSERT(boost::none ==
           coordinatorService->coordinateCommit(
               operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet));
}

TEST_F(TransactionCoordinatorServiceTest, CoordinatorAbortsIfDeadlinePassesAndStillPreparing) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    const auto deadline = executor()->now() + Milliseconds(1000 * 60 * 10 /* 10 hours */);
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, deadline);

    ASSERT(boost::none !=
           coordinatorService->coordinateCommit(
               operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet));

    // This ensures that the VectorClock and the participants persistence step executes
    advanceClockAndExecuteScheduledTasks();

    // This ensures that the coordinator will reach the deadline and cause it to abort the
    // transaction
    network()->enterNetwork();
    network()->advanceTime(deadline);
    network()->exitNetwork();

    // The coordinator should still exist.
    auto commitDecisionFuture = coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);
    ASSERT(boost::none != commitDecisionFuture);

    // ... and should run the abort sequence
    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture->get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorServiceTest,
       CoordinatorContinuesCommittingIfDeadlinePassesAndCommitWasDecided) {
    auto coordinatorService = TransactionCoordinatorService::get(operationContext());
    const auto deadline = executor()->now() + Milliseconds(1000 * 60 * 10 /* 10 hours */);
    coordinatorService->createCoordinator(
        operationContext(), _lsid, _txnNumberAndRetryCounter, deadline);

    // Deliver the participant list before the deadline.
    ASSERT(boost::none !=
           coordinatorService->coordinateCommit(
               operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet));

    // Vote commit before the deadline
    onCommands({[&](const executor::RemoteCommandRequest&) { return kPrepareOk; },
                [&](const executor::RemoteCommandRequest&) { return kPrepareOk; }});

    // Reach the deadline.
    network()->enterNetwork();
    network()->advanceTime(deadline);
    network()->exitNetwork();

    // The coordinator should still exist.
    auto commitDecisionFuture = coordinatorService->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);
    ASSERT(boost::none != commitDecisionFuture);

    // ... and should run the commit sequence
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    ASSERT_EQ(int(txn::CommitDecision::kCommit), int(commitDecisionFuture->get()));
}


/**
 * Fixture that during setUp automatically creates a coordinator for a default lsid/txnNumber pair.
 */
class TransactionCoordinatorServiceTestSingleTxn : public TransactionCoordinatorServiceTest {
public:
    void setUp() final {
        TransactionCoordinatorServiceTest::setUp();
        TransactionCoordinatorService::get(operationContext())
            ->createCoordinator(
                operationContext(), _lsid, _txnNumberAndRetryCounter, kCommitDeadline);
    }

    TransactionCoordinatorService* coordinatorService() {
        return TransactionCoordinatorService::get(operationContext());
    }
};

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitReturnsCorrectCommitDecisionOnAbort) {

    auto commitDecisionFuture = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    // Simulate a participant voting to abort.
    onCommands({[&](const executor::RemoteCommandRequest& request) { return kPrepareOk; },
                [&](const executor::RemoteCommandRequest& request) { return kNoSuchTransaction; }});

    assertAbortSentAndRespondWithSuccess();
    assertAbortSentAndRespondWithSuccess();

    ASSERT_THROWS_CODE(
        commitDecisionFuture.get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       CoordinateCommitReturnsCorrectCommitDecisionOnCommit) {

    auto commitDecisionFuture = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    ASSERT_FALSE(commitDecisionFuture.isReady());

    assertPrepareSentAndRespondWithSuccess();
    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    auto commitDecision = commitDecisionFuture.get();
    ASSERT_EQ(static_cast<int>(commitDecision), static_cast<int>(txn::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ConcurrentCallsToCoordinateCommitReturnSameDecisionOnCommit) {

    auto commitDecisionFuture1 = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);
    auto commitDecisionFuture2 = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    commitTransaction(*coordinatorService(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    ASSERT_EQ(static_cast<int>(commitDecisionFuture1.get()),
              static_cast<int>(commitDecisionFuture2.get()));
}

TEST_F(TransactionCoordinatorServiceTestSingleTxn,
       ConcurrentCallsToCoordinateCommitReturnSameDecisionOnAbort) {

    auto commitDecisionFuture1 = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);
    auto commitDecisionFuture2 = *coordinatorService()->coordinateCommit(
        operationContext(), _lsid, _txnNumberAndRetryCounter, kTwoShardIdSet);

    abortTransaction(*coordinatorService(),
                     _lsid,
                     _txnNumberAndRetryCounter,
                     kTwoShardIdSet,
                     kTwoShardIdList[0]);

    ASSERT_THROWS_CODE(
        commitDecisionFuture1.get(), AssertionException, ErrorCodes::NoSuchTransaction);
    ASSERT_THROWS_CODE(
        commitDecisionFuture2.get(), AssertionException, ErrorCodes::NoSuchTransaction);
}

}  // namespace
}  // namespace mongo
