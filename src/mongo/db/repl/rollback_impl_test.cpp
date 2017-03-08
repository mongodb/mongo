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

namespace {

using namespace mongo;
using namespace mongo::repl;

const OplogInterfaceMock::Operations kEmptyMockOperations;

/**
 * Unit test for rollback implementation introduced in 3.6.
 */
class RollbackImplTest : public RollbackTest {
private:
    void setUp() override;
    void tearDown() override;

protected:
    TaskExecutorMock::ShouldFailRequestFn _shouldFailScheduleRemoteCommandRequest;
    std::unique_ptr<TaskExecutorMock> _taskExecutorMock;
    Rollback::OnCompletionFn _onCompletion;
    StatusWith<OpTime> _onCompletionResult = executor::TaskExecutorTest::getDetectableErrorStatus();
    std::unique_ptr<OplogInterfaceMock> _localOplog;
    int _requiredRollbackId;
    std::unique_ptr<RollbackImpl> _rollback;
};

void RollbackImplTest::setUp() {
    RollbackTest::setUp();
    _shouldFailScheduleRemoteCommandRequest = [](const executor::RemoteCommandRequest& request) {
        return false;
    };
    _taskExecutorMock = stdx::make_unique<TaskExecutorMock>(
        &_threadPoolExecutorTest.getExecutor(),
        [this](const executor::RemoteCommandRequest& request) {
            return _shouldFailScheduleRemoteCommandRequest(request);
        });
    _localOplog = stdx::make_unique<OplogInterfaceMock>(kEmptyMockOperations);
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
    _shouldFailScheduleRemoteCommandRequest = {};
    RollbackTest::tearDown();
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
    OplogInterfaceMock emptyOplog(kEmptyMockOperations);
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
    _taskExecutorMock->shouldFailScheduleWork = true;
    ASSERT_EQUALS(ErrorCodes::OperationFailed, _rollback->startup());
}

TEST_F(
    RollbackImplTest,
    RollbackReturnsCallbackCanceledIfExecutorIsShutdownAfterSchedulingTransitionToRollbackCallback) {
    _taskExecutorMock->shouldDeferScheduleWorkByOneSecond = true;
    ASSERT_OK(_rollback->startup());
    _threadPoolExecutorTest.getExecutor().shutdown();
    _rollback->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _onCompletionResult);
}

TEST_F(
    RollbackImplTest,
    RollbackReturnsCallbackCanceledIfRollbackIsShutdownAfterSchedulingTransitionToRollbackCallback) {
    _taskExecutorMock->shouldDeferScheduleWorkByOneSecond = true;
    ASSERT_OK(_rollback->startup());
    _rollback->shutdown();
    _rollback->join();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, _onCompletionResult);
}

DEATH_TEST_F(RollbackImplTest, RollbackTerminatesIfCompletionCallbackThrowsException, "terminate") {
    _taskExecutorMock->shouldDeferScheduleWorkByOneSecond = true;
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

    ASSERT_OK(_rollback->startup());
    _rollback->join();
}

DEATH_TEST_F(
    RollbackImplTest,
    RollbackTriggersFatalAssertionOnFailingToTransitionFromRollbackToSecondaryDuringTearDownPhase,
    "Failed to transition into SECONDARY; expected to be in state ROLLBACK but found self in "
    "ROLLBACK") {
    _coordinator->_failSetFollowerModeOnThisMemberState = MemberState::RS_SECONDARY;
    ASSERT_OK(_rollback->startup());
    _rollback->join();
}

TEST_F(RollbackImplTest, RollbackReturnsLastAppliedOpTimeOnSuccess) {
    ASSERT_OK(_rollback->startup());
    _rollback->join();
    ASSERT_EQUALS(OpTime(), unittest::assertGet(_onCompletionResult));
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
