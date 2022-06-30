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

#include <memory>

#include "mongo/base/status.h"
#include "mongo/db/repl/abstract_async_component.h"
#include "mongo/db/repl/task_executor_mock.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/platform/mutex.h"

#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

/**
 * Mock implementation of AbstractAsyncComponent that supports returning errors from
 * _doStartup_inlock() and also tracks if this function has ever been called by
 * AbstractAsyncComponent.
 */
class MockAsyncComponent : public AbstractAsyncComponent {
public:
    explicit MockAsyncComponent(executor::TaskExecutor* executor);

    /**
     * Publicly visible versions of _checkForShutdownAndConvertStatus_inlock() for testing.
     */
    Status checkForShutdownAndConvertStatus_forTest(
        const executor::TaskExecutor::CallbackArgs& callbackArgs, const std::string& message);
    Status checkForShutdownAndConvertStatus_forTest(const Status& status,
                                                    const std::string& message);

    /**
     * Publicly visible versions of _scheduleWorkAndSaveHandle_inlock() and
     * _scheduleWorkAtAndSaveHandle_inlock() for testing.
     */
    Status scheduleWorkAndSaveHandle_forTest(executor::TaskExecutor::CallbackFn work,
                                             executor::TaskExecutor::CallbackHandle* handle,
                                             const std::string& name);

    /**
     * Publicly visible version of _scheduleWorkAtAndSaveHandle_inlock() for testing.
     */
    Status scheduleWorkAtAndSaveHandle_forTest(Date_t when,
                                               executor::TaskExecutor::CallbackFn work,
                                               executor::TaskExecutor::CallbackHandle* handle,
                                               const std::string& name);

    /**
     * Publicly visible version of _cancelHandle_inlock() for testing.
     */
    void cancelHandle_forTest(executor::TaskExecutor::CallbackHandle handle);

    /**
     * Publicly visible version of _startupComponent_inlock() for testing.
     */
    Status startupComponent_forTest(std::unique_ptr<MockAsyncComponent>& component);

    /**
     * Publicly visible version of _shutdownComponent_inlock() for testing.
     */
    void shutdownComponent_forTest(const std::unique_ptr<MockAsyncComponent>& component);

private:
    void _doStartup_inlock() override;
    void _doShutdown_inlock() noexcept override;
    void _preJoin() noexcept override {}
    Mutex* _getMutex() noexcept override;

    // Used by AbstractAsyncComponent to guard start changes.
    Mutex _mutex = MONGO_MAKE_LATCH("MockAsyncComponent::_mutex");

public:
    // Asserted to be OK by _doStartup_inlock(). Override for testing.
    Status doStartupResult = Status::OK();

    // Set to true when _doStartup_inlock() is called.
    bool doStartupCalled = false;
};

MockAsyncComponent::MockAsyncComponent(executor::TaskExecutor* executor)
    : AbstractAsyncComponent(executor, "mock component") {}

Status MockAsyncComponent::checkForShutdownAndConvertStatus_forTest(
    const executor::TaskExecutor::CallbackArgs& callbackArgs, const std::string& message) {
    return _checkForShutdownAndConvertStatus_inlock(callbackArgs, message);
}

Status MockAsyncComponent::checkForShutdownAndConvertStatus_forTest(const Status& status,
                                                                    const std::string& message) {
    return _checkForShutdownAndConvertStatus_inlock(status, message);
}

Status MockAsyncComponent::scheduleWorkAndSaveHandle_forTest(
    executor::TaskExecutor::CallbackFn work,
    executor::TaskExecutor::CallbackHandle* handle,
    const std::string& name) {
    stdx::lock_guard<Latch> lock(_mutex);
    return _scheduleWorkAndSaveHandle_inlock(std::move(work), handle, name);
}

Status MockAsyncComponent::scheduleWorkAtAndSaveHandle_forTest(
    Date_t when,
    executor::TaskExecutor::CallbackFn work,
    executor::TaskExecutor::CallbackHandle* handle,
    const std::string& name) {
    stdx::lock_guard<Latch> lock(_mutex);
    return _scheduleWorkAtAndSaveHandle_inlock(when, std::move(work), handle, name);
}

void MockAsyncComponent::cancelHandle_forTest(executor::TaskExecutor::CallbackHandle handle) {
    stdx::lock_guard<Latch> lock(_mutex);
    _cancelHandle_inlock(handle);
}

Status MockAsyncComponent::startupComponent_forTest(
    std::unique_ptr<MockAsyncComponent>& component) {
    return _startupComponent(component);
}

void MockAsyncComponent::shutdownComponent_forTest(
    const std::unique_ptr<MockAsyncComponent>& component) {
    _shutdownComponent(component);
}

void MockAsyncComponent::_doStartup_inlock() {
    doStartupCalled = true;
    uassertStatusOK(doStartupResult);
}

void MockAsyncComponent::_doShutdown_inlock() noexcept {}

Mutex* MockAsyncComponent::_getMutex() noexcept {
    return &_mutex;
}

class AbstractAsyncComponentTest : public executor::ThreadPoolExecutorTest {
private:
    void setUp() override;
    void tearDown() override;
};

void AbstractAsyncComponentTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();

    launchExecutorThread();
}

void AbstractAsyncComponentTest::tearDown() {
    shutdownExecutorThread();
    joinExecutorThread();
}

TEST_F(AbstractAsyncComponentTest, ConstructorThrowsUserAssertionOnNullTaskExecutor) {
    ASSERT_THROWS_CODE_AND_WHAT(MockAsyncComponent(nullptr),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "task executor cannot be null");
}

TEST_F(AbstractAsyncComponentTest, StateTransitionsToRunningAfterSuccessfulStartup) {
    MockAsyncComponent component(&getExecutor());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kPreStart, component.getState_forTest());
    ASSERT_FALSE(component.isActive());
    ASSERT_FALSE(component.doStartupCalled);

    ASSERT_OK(component.startup());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kRunning, component.getState_forTest());
    ASSERT_TRUE(component.isActive());
    ASSERT_TRUE(component.doStartupCalled);
}

TEST_F(AbstractAsyncComponentTest, StartupReturnsIllegalOperationIfAlreadyActive) {
    MockAsyncComponent component(&getExecutor());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kPreStart, component.getState_forTest());
    ASSERT_FALSE(component.isActive());
    ASSERT_FALSE(component.doStartupCalled);

    ASSERT_OK(component.startup());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kRunning, component.getState_forTest());
    ASSERT_TRUE(component.isActive());
    ASSERT_TRUE(component.doStartupCalled);

    component.doStartupCalled = false;

    auto status = component.startup();
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, status);
    ASSERT_EQUALS("mock component already started", status.reason());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kRunning, component.getState_forTest());
    ASSERT_TRUE(component.isActive());
    ASSERT_FALSE(component.doStartupCalled);
}

TEST_F(AbstractAsyncComponentTest, StartupReturnsShutdownInProgressIfComponentIsShuttingDown) {
    MockAsyncComponent component(&getExecutor());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kPreStart, component.getState_forTest());
    ASSERT_FALSE(component.isActive());
    ASSERT_FALSE(component.doStartupCalled);

    ASSERT_OK(component.startup());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kRunning, component.getState_forTest());
    ASSERT_TRUE(component.isActive());
    ASSERT_TRUE(component.doStartupCalled);

    component.doStartupCalled = false;
    component.shutdown();

    ASSERT_EQUALS(AbstractAsyncComponent::State::kShuttingDown, component.getState_forTest());
    ASSERT_TRUE(component.isActive());
    ASSERT_FALSE(component.doStartupCalled);

    auto status = component.startup();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status);
    ASSERT_EQUALS("mock component shutting down", status.reason());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kShuttingDown, component.getState_forTest());
    ASSERT_TRUE(component.isActive());
    ASSERT_FALSE(component.doStartupCalled);
}

TEST_F(AbstractAsyncComponentTest,
       StartupTransitionsToCompleteAndPassesThroughErrorFromDoStartupInLock) {
    MockAsyncComponent component(&getExecutor());
    component.doStartupResult = {ErrorCodes::OperationFailed, "mock component startup failed"};

    ASSERT_EQUALS(AbstractAsyncComponent::State::kPreStart, component.getState_forTest());
    ASSERT_FALSE(component.isActive());
    ASSERT_FALSE(component.doStartupCalled);

    auto status = component.startup();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, status);
    ASSERT_EQUALS("mock component startup failed", status.reason());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kComplete, component.getState_forTest());
    ASSERT_FALSE(component.isActive());
    ASSERT_TRUE(component.doStartupCalled);
}

TEST_F(AbstractAsyncComponentTest, ShutdownTransitionsStateToCompleteIfCalledBeforeStartup) {
    MockAsyncComponent component(&getExecutor());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kPreStart, component.getState_forTest());
    ASSERT_FALSE(component.isActive());
    ASSERT_FALSE(component.doStartupCalled);

    component.shutdown();

    ASSERT_EQUALS(AbstractAsyncComponent::State::kComplete, component.getState_forTest());
    ASSERT_FALSE(component.isActive());
    ASSERT_FALSE(component.doStartupCalled);

    auto status = component.startup();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, status);
    ASSERT_EQUALS("mock component completed", status.reason());

    ASSERT_EQUALS(AbstractAsyncComponent::State::kComplete, component.getState_forTest());
    ASSERT_FALSE(component.isActive());
    ASSERT_FALSE(component.doStartupCalled);
}

executor::TaskExecutor::CallbackArgs statusToCallbackArgs(const Status& status) {
    return executor::TaskExecutor::CallbackArgs(nullptr, {}, status);
}

TEST_F(AbstractAsyncComponentTest,
       CheckForShutdownAndConvertStatusReturnsCallbackCanceledIfComponentIsShuttingDown) {
    MockAsyncComponent component(&getExecutor());
    // Skipping checks on component state because these are done in
    // StartupReturnsShutdownInProgressIfComponentIsShuttingDown.
    ASSERT_OK(component.startup());
    component.shutdown();
    ASSERT_EQUALS(AbstractAsyncComponent::State::kShuttingDown, component.getState_forTest());

    auto status = getDetectableErrorStatus();
    ASSERT_NOT_EQUALS(ErrorCodes::CallbackCanceled, status);
    ASSERT_EQUALS(
        ErrorCodes::CallbackCanceled,
        component.checkForShutdownAndConvertStatus_forTest(statusToCallbackArgs(status), "mytask"));
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled,
                  component.checkForShutdownAndConvertStatus_forTest(status, "mytask"));
}

TEST_F(AbstractAsyncComponentTest,
       CheckForShutdownAndConvertStatusPrependsMessageToReasonInNewErrorStatus) {
    MockAsyncComponent component(&getExecutor());

    auto status = getDetectableErrorStatus();
    auto newStatus =
        component.checkForShutdownAndConvertStatus_forTest(statusToCallbackArgs(status), "mytask");
    ASSERT_EQUALS(status.code(), newStatus.code());
    ASSERT_EQUALS("mytask :: caused by :: " + status.reason(), newStatus.reason());

    newStatus = component.checkForShutdownAndConvertStatus_forTest(status, "mytask");
    ASSERT_EQUALS(status.code(), newStatus.code());
    ASSERT_EQUALS("mytask :: caused by :: " + status.reason(), newStatus.reason());
}

TEST_F(AbstractAsyncComponentTest, CheckForShutdownAndConvertStatusPassesThroughSuccessfulStatus) {
    MockAsyncComponent component(&getExecutor());

    auto status = Status::OK();
    ASSERT_OK(
        component.checkForShutdownAndConvertStatus_forTest(statusToCallbackArgs(status), "mytask"));
    ASSERT_OK(component.checkForShutdownAndConvertStatus_forTest(status, "mytask"));
}

TEST_F(AbstractAsyncComponentTest,
       ScheduleWorkAndSaveHandleReturnsCallbackCanceledIfComponentIsShuttingDown) {
    MockAsyncComponent component(&getExecutor());
    // Skipping checks on component state because these are done in
    // StartupReturnsShutdownInProgressIfComponentIsShuttingDown.
    ASSERT_OK(component.startup());
    component.shutdown();
    ASSERT_EQUALS(AbstractAsyncComponent::State::kShuttingDown, component.getState_forTest());

    auto callback = [](const executor::TaskExecutor::CallbackArgs&) {};
    executor::TaskExecutor::CallbackHandle handle;
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled,
                  component.scheduleWorkAndSaveHandle_forTest(callback, &handle, "mytask"));
}

TEST_F(AbstractAsyncComponentTest,
       ScheduleWorkAndSaveHandlePassesThroughErrorFromTaskExecutorScheduleWork) {
    TaskExecutorMock taskExecutorMock(&getExecutor());
    MockAsyncComponent component(&taskExecutorMock);

    taskExecutorMock.shouldFailScheduleWorkRequest = []() { return true; };

    auto callback = [](const executor::TaskExecutor::CallbackArgs&) {};
    executor::TaskExecutor::CallbackHandle handle;
    ASSERT_EQUALS(ErrorCodes::OperationFailed,
                  component.scheduleWorkAndSaveHandle_forTest(callback, &handle, "mytask"));
}

TEST_F(AbstractAsyncComponentTest, ScheduleWorkAndSaveHandleSchedulesTaskSuccessfully) {
    auto executor = &getExecutor();
    MockAsyncComponent component(executor);
    auto status = getDetectableErrorStatus();
    auto callback = [&status](const executor::TaskExecutor::CallbackArgs& callbackArgs) {
        status = callbackArgs.status;
    };
    executor::TaskExecutor::CallbackHandle handle;
    ASSERT_OK(component.scheduleWorkAndSaveHandle_forTest(callback, &handle, "mytask"));
    ASSERT_TRUE(handle.isValid());
    executor->wait(handle);
    ASSERT_OK(status);
}

TEST_F(AbstractAsyncComponentTest,
       ScheduleWorkAtAndSaveHandleReturnsCallbackCanceledIfComponentIsShuttingDown) {
    MockAsyncComponent component(&getExecutor());
    // Skipping checks on component state because these are done in
    // StartupReturnsShutdownInProgressIfComponentIsShuttingDown.
    ASSERT_OK(component.startup());
    component.shutdown();
    ASSERT_EQUALS(AbstractAsyncComponent::State::kShuttingDown, component.getState_forTest());

    auto when = getExecutor().now() + Seconds(1);
    auto callback = [](const executor::TaskExecutor::CallbackArgs&) {};
    executor::TaskExecutor::CallbackHandle handle;
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled,
                  component.scheduleWorkAtAndSaveHandle_forTest(when, callback, &handle, "mytask"));
}

TEST_F(AbstractAsyncComponentTest,
       ScheduleWorkAtAndSaveHandlePassesThroughErrorFromTaskExecutorScheduleWork) {
    TaskExecutorMock taskExecutorMock(&getExecutor());
    MockAsyncComponent component(&taskExecutorMock);

    taskExecutorMock.shouldFailScheduleWorkAtRequest = []() { return true; };

    auto when = getExecutor().now() + Seconds(1);
    auto callback = [](const executor::TaskExecutor::CallbackArgs&) {};
    executor::TaskExecutor::CallbackHandle handle;
    ASSERT_EQUALS(ErrorCodes::OperationFailed,
                  component.scheduleWorkAtAndSaveHandle_forTest(when, callback, &handle, "mytask"));
}

TEST_F(AbstractAsyncComponentTest, ScheduleWorkAtAndSaveHandleSchedulesTaskSuccessfully) {
    auto executor = &getExecutor();
    MockAsyncComponent component(executor);
    auto when = executor->now() + Seconds(1);
    auto status = getDetectableErrorStatus();
    auto callback = [&status](const executor::TaskExecutor::CallbackArgs& callbackArgs) {
        status = callbackArgs.status;
    };
    executor::TaskExecutor::CallbackHandle handle;
    ASSERT_OK(component.scheduleWorkAtAndSaveHandle_forTest(when, callback, &handle, "mytask"));
    ASSERT_TRUE(handle.isValid());

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_GREATER_THAN(when, net->now());
        ASSERT_EQUALS(when, net->runUntil(when));
    }

    executor->wait(handle);
    ASSERT_OK(status);
}

TEST_F(AbstractAsyncComponentTest, CancelHandleDoesNothingOnInvalidHandle) {
    MockAsyncComponent component(&getExecutor());
    component.cancelHandle_forTest({});
}

TEST_F(AbstractAsyncComponentTest,
       ScheduledTaskShouldFailWithCallbackCanceledIfHandleIsCanceledUsingCancelHandle) {
    auto executor = &getExecutor();
    MockAsyncComponent component(executor);
    auto status = getDetectableErrorStatus();
    auto callback = [&status](const executor::TaskExecutor::CallbackArgs& callbackArgs) {
        status = callbackArgs.status;
    };
    auto handle =
        unittest::assertGet(executor->scheduleWorkAt(executor->now() + Seconds(1), callback));
    component.cancelHandle_forTest(handle);
    executor->wait(handle);
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status);
}

TEST_F(AbstractAsyncComponentTest,
       StartupComponentStartsUpChildComponentAndReturnsSuccessIfComponentIsRunning) {
    MockAsyncComponent component(&getExecutor());
    ASSERT_OK(component.startup());
    ASSERT_EQUALS(AbstractAsyncComponent::State::kRunning, component.getState_forTest());

    // Create a child component to pass to _startupComponent_inlock().
    auto childComponent = std::make_unique<MockAsyncComponent>(&getExecutor());
    ASSERT_OK(component.startupComponent_forTest(childComponent));
    ASSERT_EQUALS(AbstractAsyncComponent::State::kRunning, childComponent->getState_forTest());
}

TEST_F(AbstractAsyncComponentTest,
       StartupComponentReturnsCallbackCanceledIfComponentIsShuttingDown) {
    MockAsyncComponent component(&getExecutor());
    // Skipping checks on component state because these are done in
    // StartupReturnsShutdownInProgressIfComponentIsShuttingDown.
    ASSERT_OK(component.startup());
    component.shutdown();
    ASSERT_EQUALS(AbstractAsyncComponent::State::kShuttingDown, component.getState_forTest());

    // Create a child component to pass to _startupComponent_inlock().
    // _startupComponent_inlock() should return early because 'component' is shutting down and
    // reset the unique_ptr for the child component.
    auto childComponent = std::make_unique<MockAsyncComponent>(&getExecutor());
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, component.startupComponent_forTest(childComponent));
    ASSERT_FALSE(childComponent);
}

TEST_F(AbstractAsyncComponentTest,
       StartupComponentResetsChildComponentIfChildComponentStartupFails) {
    MockAsyncComponent component(&getExecutor());
    ASSERT_OK(component.startup());
    ASSERT_EQUALS(AbstractAsyncComponent::State::kRunning, component.getState_forTest());

    // Create a child component to pass to _startupComponent_inlock(). Transition child component's
    // state to Complete so that calling startup() will fail.
    auto childComponent = std::make_unique<MockAsyncComponent>(&getExecutor());
    ASSERT_OK(childComponent->startup());
    childComponent->shutdown();
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, childComponent->startup());
    ASSERT_EQUALS(AbstractAsyncComponent::State::kShuttingDown, childComponent->getState_forTest());

    // _startupComponent_inlock() should pass through the startup() error from the child component
    // and reset the unique_ptr for the child component.
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress,
                  component.startupComponent_forTest(childComponent));
    ASSERT_FALSE(childComponent);
}

TEST_F(AbstractAsyncComponentTest, ShutdownComponentDoesNothingOnNullChildComponent) {
    MockAsyncComponent component(&getExecutor());
    component.shutdownComponent_forTest({});
}

TEST_F(AbstractAsyncComponentTest,
       ShutdownComponentTransitionsChildComponentToShuttingDownOnSuccess) {
    MockAsyncComponent component(&getExecutor());
    ASSERT_OK(component.startup());
    ASSERT_EQUALS(AbstractAsyncComponent::State::kRunning, component.getState_forTest());

    // Create a child component to pass to _startupComponent_inlock().
    auto childComponent = std::make_unique<MockAsyncComponent>(&getExecutor());
    ASSERT_OK(childComponent->startup());
    ASSERT_EQUALS(AbstractAsyncComponent::State::kRunning, childComponent->getState_forTest());

    component.shutdownComponent_forTest(childComponent);
    ASSERT_EQUALS(AbstractAsyncComponent::State::kShuttingDown, childComponent->getState_forTest());
}

}  // namespace
