/**
 *    Copyright 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/repl/rollback_test_fixture.h"

#include <memory>

#include "mongo/db/repl/abstract_oplog_fetcher_test_fixture.h"
#include "mongo/db/repl/oplog_interface_mock.h"
#include "mongo/db/repl/rollback_impl.h"
#include "mongo/db/repl/task_executor_mock.h"
#include "mongo/db/s/shard_identity_rollback_notifier.h"
#include "mongo/executor/task_executor.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

NamespaceString nss("local.oplog.rs");

/**
 * Unit test for rollback implementation introduced in 3.6.
 */
class RollbackImplTest : public RollbackTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    std::unique_ptr<TaskExecutorMock> _taskExecutorMock;
    Rollback::OnCompletionFn _onCompletion;
    StatusWith<OpTime> _onCompletionResult = executor::TaskExecutorTest::getDetectableErrorStatus();
    std::unique_ptr<OplogInterfaceMock> _localOplog;
    int _requiredRollbackId;
    std::unique_ptr<RollbackImpl> _rollback;
};

void RollbackImplTest::setUp() {
    RollbackTest::setUp();
    _taskExecutorMock = stdx::make_unique<TaskExecutorMock>(&_threadPoolExecutorTest.getExecutor());
    _localOplog = stdx::make_unique<OplogInterfaceMock>();
    HostAndPort syncSource("localhost", 1234);
    _requiredRollbackId = 1;
    _onCompletionResult = executor::TaskExecutorTest::getDetectableErrorStatus();
    _onCompletion = [this](const StatusWith<OpTime>& result) noexcept {
        _onCompletionResult = result;
    };
    _rollback = stdx::make_unique<RollbackImpl>(
        _taskExecutorMock.get(),
        _localOplog.get(),
        syncSource,
        nss,
        0,
        _requiredRollbackId,
        _coordinator,
        &_storageInterface,
        [this](const StatusWith<OpTime>& result) noexcept { _onCompletion(result); });
}

void RollbackImplTest::tearDown() {
    _threadPoolExecutorTest.shutdownExecutorThread();
    _threadPoolExecutorTest.joinExecutorThread();

    _rollback = {};
    _onCompletionResult = executor::TaskExecutorTest::getDetectableErrorStatus();
    _onCompletion = {};
    _requiredRollbackId = -1;
    _localOplog = {};
    _taskExecutorMock = {};
    RollbackTest::tearDown();
}

/**
 * Helper functions to make simple oplog entries with timestamps, terms, and hashes.
 */
BSONObj makeOp(OpTime time, long long hash) {
    return BSON("ts" << time.getTimestamp() << "h" << hash << "t" << time.getTerm() << "op"
                     << "i"
                     << "ui"
                     << UUID::gen());
}

BSONObj makeOp(int count) {
    return makeOp(OpTime(Timestamp(count, count), count), count);
}

/**
 * Helper functions to make pairs of oplog entries and recordIds for the OplogInterfaceMock used
 * to mock out the local oplog.
 */
int recordId = 0;
OplogInterfaceMock::Operation makeOpAndRecordId(const BSONObj& op) {
    return std::make_pair(op, RecordId(++recordId));
}

OplogInterfaceMock::Operation makeOpAndRecordId(OpTime time, long long hash) {
    return makeOpAndRecordId(makeOp(time, hash));
}

OplogInterfaceMock::Operation makeOpAndRecordId(int count) {
    return makeOpAndRecordId(makeOp(count));
}

OplogInterfaceMock::Operation makeOpAndRecordIdWithUnrecognizedOpType(int count) {
    OpTime opTime(Timestamp(count, count), count);
    return std::make_pair(BSON("ts" << opTime.getTimestamp() << "h" << count << "t"
                                    << opTime.getTerm()
                                    << "op"
                                    << "x"
                                    << "ns"
                                    << nss.ns()
                                    << "ui"
                                    << UUID::gen()
                                    << "o"
                                    << BSON("_id"
                                            << "mydocid"
                                            << "a"
                                            << 1)),
                          RecordId(++recordId));
}

OplogInterfaceMock::Operation makeOpAndRecordIdWithMissingUuidField(int count) {
    OpTime opTime(Timestamp(count, count), count);
    return std::make_pair(
        BSON("ts" << opTime.getTimestamp() << "h" << count << "t" << opTime.getTerm() << "op"
                  << "i"
                  << "ns"
                  << nss.ns()
                  << "o"
                  << BSON("_id" << 0)),
        RecordId(++recordId));
}

TEST_F(RollbackImplTest, TestFixtureSetUpInitializesStorageEngine) {
    auto serviceContext = _serviceContextMongoDTest.getServiceContext();
    ASSERT_TRUE(serviceContext);
    ASSERT_TRUE(serviceContext->getGlobalStorageEngine());
}

TEST_F(RollbackImplTest, TestFixtureSetUpInitializesTaskExecutor) {
    auto net = _threadPoolExecutorTest.getNet();
    ASSERT_TRUE(net);
    auto&& executor = _threadPoolExecutorTest.getExecutor();
    ASSERT_NOT_EQUALS(Date_t(), executor.now());
}

TEST_F(RollbackImplTest, InvalidConstruction) {
    auto executor = &_threadPoolExecutorTest.getExecutor();
    OplogInterfaceMock emptyOplog;
    HostAndPort syncSource("localhost", 1234);
    int requiredRollbackId = 1;
    auto replicationCoordinator = _coordinator;
    auto storageInterface = &_storageInterface;
    auto onCompletion = [](const StatusWith<OpTime>&) noexcept {};

    // Null task executor.
    // This check is performed in AbstractAsyncComponent's constructor.
    ASSERT_THROWS_CODE_AND_WHAT(RollbackImpl(nullptr,
                                             &emptyOplog,
                                             syncSource,
                                             nss,
                                             0,
                                             requiredRollbackId,
                                             replicationCoordinator,
                                             storageInterface,
                                             onCompletion),
                                UserException,
                                ErrorCodes::BadValue,
                                "task executor cannot be null");

    // Invalid sync source.
    ASSERT_THROWS_CODE_AND_WHAT(RollbackImpl(executor,
                                             &emptyOplog,
                                             HostAndPort(),
                                             nss,
                                             0,
                                             requiredRollbackId,
                                             replicationCoordinator,
                                             storageInterface,
                                             onCompletion),
                                UserException,
                                ErrorCodes::BadValue,
                                "sync source must be valid");
}

TEST_F(RollbackImplTest,
       StartupReturnsOperationFailedIfMockExecutorFailsToScheduleRollbackTransitionCallback) {
    _taskExecutorMock->shouldFailScheduleWorkRequest = []() { return true; };
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _rollback->startup());
}

TEST_F(
    RollbackImplTest,
    RollbackReturnsCallbackCanceledIfExecutorIsShutdownAfterSchedulingTransitionToRollbackCallback) {
    _taskExecutorMock->shouldDeferScheduleWorkRequestByOneSecond = []() { return true; };
    ASSERT_OK(_rollback->startup());
    _threadPoolExecutorTest.getExecutor().shutdown();
    _rollback->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _onCompletionResult);
}

TEST_F(
    RollbackImplTest,
    RollbackReturnsCallbackCanceledIfRollbackIsShutdownAfterSchedulingTransitionToRollbackCallback) {
    _taskExecutorMock->shouldDeferScheduleWorkRequestByOneSecond = []() { return true; };
    ASSERT_OK(_rollback->startup());
    _rollback->shutdown();
    _rollback->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _onCompletionResult);
}

TEST_F(RollbackImplTest, RollbackReturnsOperationFailedOnFailingToStartUpCommonPointResolver) {
    // RollbackImpl schedules the RollbackCommonPointResolver in _transitionToRollbackCallback()
    // after completing the transition to ROLLBACK.
    //
    // Since both _transitionToRollbackCallback() and RollbackCommonPointResolver::startup() are
    // scheduled using scheduleWork(), we have to make the second scheduleWork() request error out
    // in order to make RollbackCommonPointResolver::startup() fail.
    std::size_t numRequests = 0;
    _taskExecutorMock->shouldFailScheduleWorkRequest = [&numRequests]() -> bool {
        return numRequests++;  // false only if numRequests is 0
    };
    ASSERT_OK(_rollback->startup());
    _rollback->join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _onCompletionResult);
    ASSERT_EQUALS(2U, numRequests);
}

TEST_F(RollbackImplTest, RollbackShutsDownCommonPointResolverOnShutdown) {
    ASSERT_OK(_rollback->startup());

    // Wait for common point resolver to schedule fetcher.
    auto net = _threadPoolExecutorTest.getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // Oplog query.
        auto noi = net->getNextReadyRequest();
        _threadPoolExecutorTest.assertRemoteCommandNameEquals("find", noi->getRequest());

        // Blackhole request. This request will be canceled when we shut down RollbackImpl.
        net->blackHole(noi);
    }

    // Shutting down RollbackImpl should in turn shut down the RollbackCommonPointResolver.
    _rollback->shutdown();

    // Run ready network operations so that the TaskExecutor delivers the CallbackCanceled signal
    // to the RollbackCommonPointResolver callback.
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();

    _rollback->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _onCompletionResult);
}

TEST_F(RollbackImplTest,
       RollbackReturnsIncompatibleRollbackAlgorithmOnMissingUuidFieldInOplogEntry) {
    // Remote oplog: commonOp
    // Local oplog: commonOp, opMissingUuid
    // op2 is an oplog entry representing a document insert. However, op2 is missing a collection
    // UUID field because it is generated by a 3.4 server.
    auto commonOp = makeOpAndRecordId(1);
    auto opMissingUuid = makeOpAndRecordIdWithMissingUuidField(2);
    _localOplog->setOperations({opMissingUuid, commonOp});

    ASSERT_OK(_rollback->startup());

    auto net = _threadPoolExecutorTest.getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // RollbackCommonPointResolver remote oplog query.
        net->scheduleSuccessfulResponse(
            AbstractOplogFetcherTest::makeCursorResponse(0, {commonOp.first}, true, nss));
        net->runReadyNetworkOperations();
    }

    _rollback->join();
    ASSERT_EQUALS(ErrorCodes::IncompatibleRollbackAlgorithm, _onCompletionResult);
}

DEATH_TEST_F(RollbackImplTest,
             RollbackTerminatesOnUnrecognizedOpType,
             "Unable to complete rollback. A full resync may be needed") {
    // Remote oplog: commonOp
    // Local oplog: commonOp, opWithBadOpType
    // op2 is an oplog entry representing a document insert. However, op2 is missing a collection
    // UUID field because it is generated by a 3.4 server.
    auto commonOp = makeOpAndRecordId(1);
    auto opWithBadOpType = makeOpAndRecordIdWithUnrecognizedOpType(2);
    _localOplog->setOperations({opWithBadOpType, commonOp});

    ASSERT_OK(_rollback->startup());

    auto net = _threadPoolExecutorTest.getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // RollbackCommonPointResolver remote oplog query.
        net->scheduleSuccessfulResponse(
            AbstractOplogFetcherTest::makeCursorResponse(0, {commonOp.first}, true, nss));
        net->runReadyNetworkOperations();
    }

    _rollback->join();
}

TEST_F(RollbackImplTest, RollbackReturnsLastAppliedOpTimeOnSuccess) {
    // Set both local and remote oplog so that the oplog entry representing the common point
    // is at the tail of each oplog.
    auto opAndRecordId = makeOpAndRecordId(1);
    _localOplog->setOperations({opAndRecordId});

    ASSERT_OK(_rollback->startup());

    auto net = _threadPoolExecutorTest.getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);

        // RollbackCommonPointResolver remote oplog query.
        net->scheduleSuccessfulResponse(
            AbstractOplogFetcherTest::makeCursorResponse(0, {opAndRecordId.first}, true, nss));
        net->runReadyNetworkOperations();
    }

    _rollback->join();
    ASSERT_EQUALS(OpTime(), unittest::assertGet(_onCompletionResult));
}

DEATH_TEST_F(RollbackImplTest, RollbackTerminatesIfCompletionCallbackThrowsException, "terminate") {
    _taskExecutorMock->shouldDeferScheduleWorkRequestByOneSecond = []() { return true; };
    ASSERT_OK(_rollback->startup());
    _onCompletion = [](const StatusWith<OpTime>&) noexcept {
        uassertStatusOK({ErrorCodes::InternalError,
                         "exception from RollbackTerminatesIfCompletionCallbackThrowsException"});
    };
    _rollback->shutdown();
    _rollback->join();
    MONGO_UNREACHABLE;
}

TEST_F(RollbackImplTest, RollbackReturnsNotSecondaryOnFailingToTransitionToRollback) {
    _coordinator->_failSetFollowerModeOnThisMemberState = MemberState::RS_ROLLBACK;
    ASSERT_OK(_rollback->startup());
    _rollback->join();
    ASSERT_EQUALS(ErrorCodes::NotSecondary, _onCompletionResult);
}

DEATH_TEST_F(RollbackImplTest,
             RollbackTriggersFatalAssertionOnDetectingShardIdentityDocumentRollback,
             "shardIdentity document rollback detected.  Shutting down to clear in-memory sharding "
             "state.  Restarting this process should safely return it to a healthy state") {
    ASSERT_FALSE(ShardIdentityRollbackNotifier::get(_opCtx.get())->didRollbackHappen());
    ShardIdentityRollbackNotifier::get(_opCtx.get())->recordThatRollbackHappened();
    ASSERT_TRUE(ShardIdentityRollbackNotifier::get(_opCtx.get())->didRollbackHappen());

    _taskExecutorMock->shouldDeferScheduleWorkRequestByOneSecond = []() { return true; };
    ASSERT_OK(_rollback->startup());
    _rollback->shutdown();
    _rollback->join();
    MONGO_UNREACHABLE;
}

DEATH_TEST_F(
    RollbackImplTest,
    RollbackTriggersFatalAssertionOnFailingToTransitionFromRollbackToSecondaryDuringTearDownPhase,
    "Failed to transition into SECONDARY; expected to be in state ROLLBACK but found self in "
    "ROLLBACK") {
    _coordinator->_failSetFollowerModeOnThisMemberState = MemberState::RS_SECONDARY;

    ASSERT_OK(_rollback->startup());

    // We have to wait until we are in ROLLBACK before shutting down.
    // RollbackImpl schedules RollbackCommonPointResolver after it enters ROLLBACK so we wait until
    // RollbackCommonPointResolver schedules its first network request.
    auto net = _threadPoolExecutorTest.getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->blackHole(net->getNextReadyRequest());
    }

    // Shutting down RollbackImpl should in turn shut down the RollbackCommonPointResolver.
    _rollback->shutdown();

    // Run ready network operations so that the TaskExecutor delivers the CallbackCanceled signal
    // to the RollbackCommonPointResolver callback.
    executor::NetworkInterfaceMock::InNetworkGuard(net)->runReadyNetworkOperations();
    _rollback->join();
    MONGO_UNREACHABLE;
}

class SharedCallbackState {
    MONGO_DISALLOW_COPYING(SharedCallbackState);

public:
    explicit SharedCallbackState(bool* sharedCallbackStateDestroyed)
        : _sharedCallbackStateDestroyed(sharedCallbackStateDestroyed) {}
    ~SharedCallbackState() {
        *_sharedCallbackStateDestroyed = true;
    }

private:
    bool* _sharedCallbackStateDestroyed;
};

TEST_F(RollbackImplTest, RollbackResetsOnCompletionCallbackFunctionPointerUponCompletion) {
    bool sharedCallbackStateDestroyed = false;
    auto sharedCallbackData = std::make_shared<SharedCallbackState>(&sharedCallbackStateDestroyed);
    decltype(_onCompletionResult) lastApplied = _threadPoolExecutorTest.getDetectableErrorStatus();

    _rollback = stdx::make_unique<RollbackImpl>(
        _taskExecutorMock.get(),
        _localOplog.get(),
        HostAndPort("localhost", 1234),
        nss,
        0,
        _requiredRollbackId,
        _coordinator,
        &_storageInterface,
        [&lastApplied, sharedCallbackData ](const StatusWith<OpTime>& result) noexcept {
            lastApplied = result;
        });
    ON_BLOCK_EXIT([this]() { _taskExecutorMock->shutdown(); });

    // Completion callback will be invoked on errors after startup() returns successfully.
    // We cause the the rollback process to error out early by failing to transition to rollback.
    _coordinator->_failSetFollowerModeOnThisMemberState = MemberState::RS_ROLLBACK;

    ASSERT_OK(_rollback->startup());

    // After 'sharedCallbackData' is reset, RollbackImpl will hold the last reference count to
    // SharedCallbackState.
    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    _rollback->join();
    ASSERT_EQUALS(ErrorCodes::NotSecondary, lastApplied);

    // RollbackImpl should reset 'RollbackImpl::_onCompletion' after running callback function
    // for the last time before becoming inactive.
    // This ensures that we release resources associated with 'RollbackImpl::_onCompletion'.
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

}  // namespace
