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


#include "mongo/db/s/transaction_coordinator_service.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/commands/txn_cmds_gen.h"
#include "mongo/db/commands/txn_two_phase_commit_cmds_gen.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/s/transaction_coordinator_document_gen.h"
#include "mongo/db/s/transaction_coordinator_test_fixture.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <cstdint>

#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/none_t.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const Date_t kCommitDeadline = Date_t::max();

const BSONObj kDummyWriteConcernError = BSON("code" << ErrorCodes::WriteConcernTimeout << "errmsg"
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
            PrepareTransaction::kCommandName, kPrepareOk, defaultMajorityWriteConcernDoNotUse());
    }

    void assertPrepareSentAndRespondWithSuccessAndWriteConcernError() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kPrepareOkButWriteConcernError,
                                        defaultMajorityWriteConcernDoNotUse());
        advanceClockAndExecuteScheduledTasks();
    }

    void assertPrepareSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kNoSuchTransaction,
                                        defaultMajorityWriteConcernDoNotUse());
    }

    void assertPrepareSentAndRespondWithNoSuchTransactionAndWriteConcernError() {
        assertCommandSentAndRespondWith(PrepareTransaction::kCommandName,
                                        kNoSuchTransactionAndWriteConcernError,
                                        defaultMajorityWriteConcernDoNotUse());
        advanceClockAndExecuteScheduledTasks();
    }

    // Abort responses

    void assertAbortSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(
            "abortTransaction", kOk, defaultMajorityWriteConcernDoNotUse());
    }

    void assertAbortSentAndRespondWithSuccessAndWriteConcernError() {
        assertCommandSentAndRespondWith(
            "abortTransaction", kOkButWriteConcernError, defaultMajorityWriteConcernDoNotUse());
        advanceClockAndExecuteScheduledTasks();
    }

    void assertAbortSentAndRespondWithNoSuchTransaction() {
        assertCommandSentAndRespondWith(
            "abortTransaction", kNoSuchTransaction, defaultMajorityWriteConcernDoNotUse());
    }

    void assertAbortSentAndRespondWithNoSuchTransactionAndWriteConcernError() {
        assertCommandSentAndRespondWith("abortTransaction",
                                        kNoSuchTransactionAndWriteConcernError,
                                        defaultMajorityWriteConcernDoNotUse());
        advanceClockAndExecuteScheduledTasks();
    }

    // Commit responses

    void assertCommitSentAndRespondWithSuccess() {
        assertCommandSentAndRespondWith(
            CommitTransaction::kCommandName, kOk, defaultMajorityWriteConcernDoNotUse());
    }

    void assertCommitSentAndRespondWithSuccessAndWriteConcernError() {
        assertCommandSentAndRespondWith(CommitTransaction::kCommandName,
                                        kOkButWriteConcernError,
                                        defaultMajorityWriteConcernDoNotUse());
        advanceClockAndExecuteScheduledTasks();
    }

    void assertCommitSentAndRespondWithRetryableError() {
        assertCommandSentAndRespondWith(CommitTransaction::kCommandName,
                                        kRetryableError,
                                        defaultMajorityWriteConcernDoNotUse());
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

/**
 * Test wrapper class for TransactionCoordinatorService that has access to protected methods
 * for testing.
 */
class TransactionCoordinatorServiceTestImpl : public TransactionCoordinatorService {
public:
    TransactionCoordinatorServiceTestImpl() : TransactionCoordinatorService() {}

    using TransactionCoordinatorService::getInitTerm;
    using TransactionCoordinatorService::pendingCleanup;

    bool isCatalogAndSchedulerActive(OperationContext* opCtx) {
        try {
            TransactionCoordinatorService::getCatalogAndScheduler(opCtx);
            return true;
        } catch (const ExceptionFor<ErrorCodes::NotWritablePrimary>&) {
            return false;
        }
    }
};

class TransactionCoordinatorServiceInitializationTest
    : public TransactionCoordinatorServiceTestFixture {
protected:
    void tearDown() override {
        _testService.interrupt();
        executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
        _testService.shutdown();

        TransactionCoordinatorTestFixture::tearDown();
    }

    TransactionCoordinatorServiceTestImpl _testService;
    TxnNumberAndRetryCounter _txnNumberAndRetryCounter{1, 1};
};

TEST_F(TransactionCoordinatorServiceInitializationTest, OperationsFailBeforeInitialized) {
    ASSERT_THROWS_CODE(_testService.createCoordinator(operationContext(),
                                                      makeLogicalSessionIdForTest(),
                                                      _txnNumberAndRetryCounter,
                                                      kCommitDeadline),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);

    ASSERT_THROWS_CODE(_testService.coordinateCommit(operationContext(),
                                                     makeLogicalSessionIdForTest(),
                                                     _txnNumberAndRetryCounter,
                                                     kTwoShardIdSet),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);

    ASSERT_THROWS_CODE(_testService.recoverCommit(operationContext(),
                                                  makeLogicalSessionIdForTest(),
                                                  _txnNumberAndRetryCounter),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);
}

TEST_F(TransactionCoordinatorServiceInitializationTest, OperationsBlockBeforeInitializeCompletes) {
    _testService.initializeIfNeeded(operationContext(), /* term */ 1, Milliseconds(1));

    ASSERT_THROWS_CODE(operationContext()->runWithDeadline(
                           Date_t::now() + Milliseconds{5},
                           ErrorCodes::NetworkInterfaceExceededTimeLimit,
                           [&] {
                               return _testService.coordinateCommit(operationContext(),
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

    ASSERT(_testService.coordinateCommit(operationContext(),
                                         makeLogicalSessionIdForTest(),
                                         _txnNumberAndRetryCounter,
                                         kTwoShardIdSet) == boost::none);
}

TEST_F(TransactionCoordinatorServiceInitializationTest,
       InitializeFailsDueToBadCoordinatorDocument) {
    DBDirectClient client(operationContext());

    auto response = client.insertAcknowledged(NamespaceString::kTransactionCoordinatorsNamespace,
                                              {BSON("IllegalKey" << 1)});
    ASSERT_OK(getStatusFromWriteCommandReply(response));
    ASSERT_EQ(1, response["n"].Int());

    _testService.initializeIfNeeded(operationContext(), /* term */ 1);

    ASSERT_THROWS_CODE(_testService.coordinateCommit(operationContext(),
                                                     makeLogicalSessionIdForTest(),
                                                     _txnNumberAndRetryCounter,
                                                     kTwoShardIdSet),
                       AssertionException,
                       ErrorCodes::TypeMismatch);

    ASSERT_THROWS_CODE(_testService.recoverCommit(operationContext(),
                                                  makeLogicalSessionIdForTest(),
                                                  _txnNumberAndRetryCounter),
                       AssertionException,
                       ErrorCodes::TypeMismatch);
}

TEST_F(TransactionCoordinatorServiceInitializationTest, ServiceRecoverTxnRetryCounter) {
    auto lsid = makeLogicalSessionIdForTest();

    OperationSessionInfo sessionInfo;
    sessionInfo.setSessionId(lsid);
    sessionInfo.setTxnNumber(_txnNumberAndRetryCounter.getTxnNumber());
    sessionInfo.setTxnRetryCounter(*_txnNumberAndRetryCounter.getTxnRetryCounter());

    txn::TransactionCoordinatorDocument doc;
    doc.setId(std::move(sessionInfo));
    doc.setParticipants(kOneShardIdList);

    DBDirectClient client(operationContext());
    auto response = client.insertAcknowledged(NamespaceString::kTransactionCoordinatorsNamespace,
                                              {doc.toBSON()});
    ASSERT_OK(getStatusFromWriteCommandReply(response));
    ASSERT_EQ(1, response["n"].Int());

    _testService.initializeIfNeeded(operationContext(), /* term */ 1);

    // Cannot recover commit with an incorrect txnRetryCounter.
    ASSERT(!_testService.recoverCommit(operationContext(),
                                       lsid,
                                       {_txnNumberAndRetryCounter.getTxnNumber(),
                                        *_txnNumberAndRetryCounter.getTxnRetryCounter() - 1}));
    ASSERT(!_testService.recoverCommit(operationContext(),
                                       lsid,
                                       {_txnNumberAndRetryCounter.getTxnNumber(),
                                        *_txnNumberAndRetryCounter.getTxnRetryCounter() + 1}));
    auto commitDecisionFuture =
        *_testService.recoverCommit(operationContext(), lsid, _txnNumberAndRetryCounter);

    assertPrepareSentAndRespondWithSuccess();
    assertCommitSentAndRespondWithSuccess();

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    ASSERT_EQ(static_cast<int>(commitDecisionFuture.get()),
              static_cast<int>(txn::CommitDecision::kCommit));
}

TEST_F(TransactionCoordinatorServiceInitializationTest, InterruptBeforeInitializeTaskCompleted) {
    // Call intialize with 1ms delay (meaning it will not actually execute until time is manually
    // advanced on the underlying executor).
    _testService.initializeIfNeeded(operationContext(), /* term */ 1, Milliseconds(1));

    // Should cancel all outstanding tasks (including the recovery task started by
    // initialize above, which has not yet run).
    _testService.interrupt();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    ASSERT_TRUE(_testService.pendingCleanup());

    // Do another initialize to ensure it runs successfully.
    _testService.initializeIfNeeded(operationContext(), /* term */ 2);
}

TEST_F(TransactionCoordinatorServiceInitializationTest, InitializingTwiceForSameTermIsIdempotent) {
    // Verify that there two threads calling initialize on the same node and term works.
    // See AF-651 for a detailed description of the bug where ShardingInitialization
    // and replication stepup hook could race.
    _testService.initializeIfNeeded(operationContext(), /* term */ 1);
    _testService.initializeIfNeeded(operationContext(), /* term */ 1);

    ASSERT_TRUE(_testService.isCatalogAndSchedulerActive(operationContext()));
    ASSERT_EQ(1, _testService.getInitTerm());
}

TEST_F(TransactionCoordinatorServiceInitializationTest,
       CannotInitializeForOldTermAfterInterruption) {
    _testService.initializeIfNeeded(operationContext(), /* term */ 1);
    _testService.interrupt();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    // Note: Replication machinery should not call initialize for an old term
    // after stepping down and interrupting, so this is purely for testing purposes.
    _testService.initializeIfNeeded(operationContext(), /* term */ 1);

    // Ensure initialization didn't execute.
    ASSERT_FALSE(_testService.isCatalogAndSchedulerActive(operationContext()));
}

TEST_F(TransactionCoordinatorServiceInitializationTest,
       ShouldNotReinitializeOnNewTermIfServiceIsActive) {
    _testService.initializeIfNeeded(operationContext(), /* term */ 1);
    ASSERT_TRUE(_testService.isCatalogAndSchedulerActive(operationContext()));

    _testService.initializeIfNeeded(operationContext(), /* term */ 2);
    ASSERT_TRUE(_testService.isCatalogAndSchedulerActive(operationContext()));
    ASSERT_EQ(2, _testService.getInitTerm());
}

TEST_F(TransactionCoordinatorServiceInitializationTest,
       InterruptedNodeCleanupsDuringNextInitialization) {
    _testService.initializeIfNeeded(operationContext(), /* term */ 1);

    _testService.interrupt();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    ASSERT_TRUE(_testService.pendingCleanup());

    _testService.initializeIfNeeded(operationContext(), /* term */ 2);

    ASSERT_FALSE(_testService.pendingCleanup());
    ASSERT_TRUE(_testService.isCatalogAndSchedulerActive(operationContext()));
}

TEST_F(TransactionCoordinatorServiceInitializationTest,
       OperationsFailsAfterServiceIsInterruptedUntilNextTerm) {
    _testService.initializeIfNeeded(operationContext(), /* term */ 1);
    _testService.interrupt();
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

    // Operations fails because service got interrupted.
    ASSERT_THROWS_CODE(_testService.coordinateCommit(operationContext(),
                                                     makeLogicalSessionIdForTest(),
                                                     _txnNumberAndRetryCounter,
                                                     kTwoShardIdSet),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);

    ASSERT_THROWS_CODE(_testService.recoverCommit(operationContext(),
                                                  makeLogicalSessionIdForTest(),
                                                  _txnNumberAndRetryCounter),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);

    ASSERT_THROWS_CODE(_testService.createCoordinator(operationContext(),
                                                      makeLogicalSessionIdForTest(),
                                                      _txnNumberAndRetryCounter,
                                                      kCommitDeadline),
                       AssertionException,
                       ErrorCodes::NotWritablePrimary);

    // New term, new initialization.
    _testService.initializeIfNeeded(operationContext(), /* term */ 2);

    ASSERT(_testService.coordinateCommit(operationContext(),
                                         makeLogicalSessionIdForTest(),
                                         _txnNumberAndRetryCounter,
                                         kTwoShardIdSet) == boost::none);

    ASSERT(_testService.recoverCommit(operationContext(),
                                      makeLogicalSessionIdForTest(),
                                      _txnNumberAndRetryCounter) == boost::none);

    _testService.createCoordinator(operationContext(),
                                   makeLogicalSessionIdForTest(),
                                   _txnNumberAndRetryCounter,
                                   kCommitDeadline);
}

class TransactionCoordinatorServiceTest : public TransactionCoordinatorServiceTestFixture {
protected:
    void setUp() override {
        TransactionCoordinatorServiceTestFixture::setUp();
        service()->initializeIfNeeded(operationContext(), /* term */ 1);
    }

    void tearDown() override {
        service()->interrupt();
        executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
        service()->shutdown();

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

    // Need to run network operations in another thread, since the exception handling path of
    // createCoordinator both cancels the commit and then waits for that cancellation to complete,
    // which requires the network interface to make progress.
    Atomic<bool> createDone{false};
    auto networkThread = stdx::thread([&] {
        while (!createDone.load()) {
            executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
            sleepFor(Milliseconds(5));
        }
    });
    ScopeGuard join([&] { networkThread.join(); });
    TxnNumberAndRetryCounter otherTxnNumberAndRetryCounter{
        _txnNumberAndRetryCounter.getTxnNumber(),
        *_txnNumberAndRetryCounter.getTxnRetryCounter() + 1};
    ASSERT_THROWS_CODE(
        coordinatorService->createCoordinator(
            operationContext(), _lsid, otherTxnNumberAndRetryCounter, kCommitDeadline),
        AssertionException,
        6032301);
    createDone.store(true);
    join.dismiss();
    networkThread.join();

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

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

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

    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();

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

    // Need to run network operations in another thread, since the exception handling path of
    // createCoordinator both cancels the commit and then waits for that cancellation to complete,
    // which requires the network interface to make progress.
    Atomic<bool> createDone{false};
    auto networkThread = stdx::thread([&] {
        while (!createDone.load()) {
            executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
            sleepFor(Milliseconds(5));
        }
    });
    ScopeGuard join([&] { networkThread.join(); });
    TxnNumberAndRetryCounter newTxnNumberAndRetryCounter{
        _txnNumberAndRetryCounter.getTxnNumber(),
        *_txnNumberAndRetryCounter.getTxnRetryCounter() + 1};
    ASSERT_THROWS_CODE(coordinatorService->createCoordinator(
                           operationContext(), _lsid, newTxnNumberAndRetryCounter, kCommitDeadline),
                       AssertionException,
                       6032300);
    createDone.store(true);
    join.dismiss();
    networkThread.join();

    assertCommandSentAndRespondWith("prepareTransaction", kNoSuchTransaction, boost::none);
    assertCommandSentAndRespondWith("abortTransaction", kOk, boost::none);
    executor::NetworkInterfaceMock::InNetworkGuard(network())->runReadyNetworkOperations();
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
                [&](const executor::RemoteCommandRequest&) {
                    return kPrepareOk;
                }});

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
                [&](const executor::RemoteCommandRequest& request) {
                    return kNoSuchTransaction;
                }});

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
