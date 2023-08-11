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


#include <algorithm>
#include <cstddef>
#include <fmt/format.h>
#include <initializer_list>
#include <list>
#include <memory>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_test_common.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace executor {
namespace {

using ExecutorFactory =
    std::function<std::shared_ptr<TaskExecutor>(std::unique_ptr<NetworkInterfaceMock>)>;

class CommonTaskExecutorTestFixture : public TaskExecutorTest {
public:
    CommonTaskExecutorTestFixture(ExecutorFactory makeExecutor)
        : _makeExecutor(std::move(makeExecutor)) {}

private:
    std::shared_ptr<TaskExecutor> makeTaskExecutor(
        std::unique_ptr<NetworkInterfaceMock> net) override {
        return _makeExecutor(std::move(net));
    }

    ExecutorFactory _makeExecutor;
};

using ExecutorTestCaseFactory =
    std::function<std::unique_ptr<CommonTaskExecutorTestFixture>(ExecutorFactory)>;
using ExecutorTestCaseMap = stdx::unordered_map<std::string, ExecutorTestCaseFactory>;

static ExecutorTestCaseMap& executorTestCaseRegistry() {
    static ExecutorTestCaseMap registry;
    return registry;
}

class CetRegistrationAgent {
    CetRegistrationAgent(const CetRegistrationAgent&) = delete;
    CetRegistrationAgent& operator=(const CetRegistrationAgent&) = delete;

public:
    CetRegistrationAgent(const std::string& name, ExecutorTestCaseFactory makeTest) {
        auto& entry = executorTestCaseRegistry()[name];
        if (entry) {
            LOGV2_FATAL(28713,
                        "Multiple attempts to register ExecutorTest named {executor}",
                        "Multiple attempts to register ExecutorTest",
                        "executor"_attr = name);
        }
        entry = std::move(makeTest);
    }
};

#define COMMON_EXECUTOR_TEST(TEST_NAME)                                        \
    class CET_##TEST_NAME : public CommonTaskExecutorTestFixture {             \
    public:                                                                    \
        CET_##TEST_NAME(ExecutorFactory makeExecutor)                          \
            : CommonTaskExecutorTestFixture(std::move(makeExecutor)) {}        \
                                                                               \
    private:                                                                   \
        void _doTest() override;                                               \
        static const CetRegistrationAgent _agent;                              \
    };                                                                         \
    const CetRegistrationAgent CET_##TEST_NAME::_agent(                        \
        #TEST_NAME, [](ExecutorFactory makeExecutor) {                         \
            return std::make_unique<CET_##TEST_NAME>(std::move(makeExecutor)); \
        });                                                                    \
    void CET_##TEST_NAME::_doTest()

auto makeSetStatusClosure(Status* target) {
    return [target](const TaskExecutor::CallbackArgs& cbData) {
        *target = cbData.status;
    };
}

auto makeSetStatusAndShutdownClosure(Status* target) {
    return [target](const TaskExecutor::CallbackArgs& cbData) {
        *target = cbData.status;
        if (cbData.status != ErrorCodes::CallbackCanceled) {
            cbData.executor->shutdown();
        }
    };
}

auto makeSetStatusAndTriggerEventClosure(Status* target, TaskExecutor::EventHandle event) {
    return [=](const TaskExecutor::CallbackArgs& cbData) {
        *target = cbData.status;
        if (!cbData.status.isOK())
            return;
        cbData.executor->signalEvent(event);
    };
}

auto makeScheduleSetStatusAndShutdownClosure(Status* outStatus1, Status* outStatus2) {
    return [=](const TaskExecutor::CallbackArgs& cbData) {
        if (!cbData.status.isOK()) {
            *outStatus1 = cbData.status;
            return;
        }
        *outStatus1 =
            cbData.executor->scheduleWork(makeSetStatusAndShutdownClosure(outStatus2)).getStatus();
    };
}

auto makeSetStatusOnRemoteCommandCompletionClosure(const RemoteCommandRequest* expectedRequest,
                                                   Status* outStatus) {
    return [=](const TaskExecutor::RemoteCommandCallbackArgs& cbData) {
        if (cbData.request != *expectedRequest) {
            auto desc = [](const RemoteCommandRequest& request) -> std::string {
                return str::stream()
                    << "Request(" << request.target.toString() << ", "
                    << request.dbname.toString_forTest() << ", " << request.cmdObj << ')';
            };
            *outStatus = Status(ErrorCodes::BadValue,
                                str::stream() << "Actual request: " << desc(cbData.request)
                                              << "; expected: " << desc(*expectedRequest));
            return;
        }
        *outStatus = cbData.response.status;
    };
}

auto makeSetStatusOnRemoteExhaustCommandCompletionClosure(
    const RemoteCommandRequest* expectedRequest, Status* outStatus, size_t* responseCount) {
    return [=](const TaskExecutor::RemoteCommandCallbackArgs& cbData) {
        if (cbData.request != *expectedRequest) {
            auto desc = [](const RemoteCommandRequest& request) -> std::string {
                return str::stream()
                    << "Request(" << request.target.toString() << ", "
                    << request.dbname.toString_forTest() << ", " << request.cmdObj << ')';
            };
            *outStatus = Status(ErrorCodes::BadValue,
                                str::stream() << "Actual request: " << desc(cbData.request)
                                              << "; expected: " << desc(*expectedRequest));
            return;
        }
        ++(*responseCount);
        *outStatus = cbData.response.status;
    };
}

static inline const RemoteCommandRequest kDummyRequest{HostAndPort("localhost", 27017),
                                                       DatabaseName::createDatabaseName_forTest(
                                                           boost::none, "mydb"),
                                                       BSON("whatsUp"
                                                            << "doc"),
                                                       nullptr};

COMMON_EXECUTOR_TEST(RunOne) {
    TaskExecutor& executor = getExecutor();
    Status status = getDetectableErrorStatus();
    ASSERT_OK(executor.scheduleWork(makeSetStatusAndShutdownClosure(&status)).getStatus());
    launchExecutorThread();
    joinExecutorThread();
    ASSERT_OK(status);
}

COMMON_EXECUTOR_TEST(Schedule2Cancel1) {
    TaskExecutor& executor = getExecutor();
    Status status1 = getDetectableErrorStatus();
    Status status2 = getDetectableErrorStatus();
    TaskExecutor::CallbackHandle cb =
        unittest::assertGet(executor.scheduleWork(makeSetStatusAndShutdownClosure(&status1)));
    executor.cancel(cb);
    ASSERT_OK(executor.scheduleWork(makeSetStatusAndShutdownClosure(&status2)).getStatus());
    launchExecutorThread();
    joinExecutorThread();
    ASSERT_EQUALS(status1, ErrorCodes::CallbackCanceled);
    ASSERT_OK(status2);
}

COMMON_EXECUTOR_TEST(OneSchedulesAnother) {
    TaskExecutor& executor = getExecutor();
    Status status1 = getDetectableErrorStatus();
    Status status2 = getDetectableErrorStatus();
    ASSERT_OK(executor.scheduleWork(makeScheduleSetStatusAndShutdownClosure(&status1, &status2))
                  .getStatus());
    launchExecutorThread();
    joinExecutorThread();
    ASSERT_OK(status1);
    ASSERT_OK(status2);
}

class EventChainAndWaitingTest {
    EventChainAndWaitingTest(const EventChainAndWaitingTest&) = delete;
    EventChainAndWaitingTest& operator=(const EventChainAndWaitingTest&) = delete;

public:
    EventChainAndWaitingTest(TaskExecutor* exec, NetworkInterfaceMock* network);
    ~EventChainAndWaitingTest();

    void run();
    void assertSuccess();

private:
    void onGo(const TaskExecutor::CallbackArgs& cbData);
    void onGoAfterTriggered(const TaskExecutor::CallbackArgs& cbData);

    NetworkInterfaceMock* net;
    TaskExecutor* executor;
    const TaskExecutor::EventHandle goEvent;
    const TaskExecutor::EventHandle event2;
    const TaskExecutor::EventHandle event3;
    TaskExecutor::EventHandle triggerEvent;
    TaskExecutor::CallbackFn triggered2;
    TaskExecutor::CallbackFn triggered3;
    Status status1;
    Status status2;
    Status status3;
    Status status4;
    Status status5;
    stdx::thread neverSignaledWaiter;
};

COMMON_EXECUTOR_TEST(EventChainAndWaiting) {
    launchExecutorThread();
    EventChainAndWaitingTest theTest(&getExecutor(), getNet());
    theTest.run();
    joinExecutorThread();
    theTest.assertSuccess();
}

EventChainAndWaitingTest::EventChainAndWaitingTest(TaskExecutor* exec,
                                                   NetworkInterfaceMock* network)
    : net(network),
      executor(exec),
      goEvent(unittest::assertGet(executor->makeEvent())),
      event2(unittest::assertGet(executor->makeEvent())),
      event3(unittest::assertGet(executor->makeEvent())),
      status1(ErrorCodes::InternalError, "Not mutated"),
      status2(ErrorCodes::InternalError, "Not mutated"),
      status3(ErrorCodes::InternalError, "Not mutated"),
      status4(ErrorCodes::InternalError, "Not mutated"),
      status5(ErrorCodes::InternalError, "Not mutated") {
    triggered2 = makeSetStatusAndTriggerEventClosure(&status2, event2);
    triggered3 = makeSetStatusAndTriggerEventClosure(&status3, event3);
}

EventChainAndWaitingTest::~EventChainAndWaitingTest() {
    if (neverSignaledWaiter.joinable()) {
        neverSignaledWaiter.join();
    }
}

void EventChainAndWaitingTest::run() {
    executor
        ->onEvent(goEvent, [=, this](const TaskExecutor::CallbackArgs& cbData) { onGo(cbData); })
        .status_with_transitional_ignore();
    executor->signalEvent(goEvent);
    executor->waitForEvent(goEvent);
    executor->waitForEvent(event2);
    executor->waitForEvent(event3);

    TaskExecutor::EventHandle neverSignaledEvent = unittest::assertGet(executor->makeEvent());
    auto waitForeverCallback = [this, neverSignaledEvent]() {
        executor->waitForEvent(neverSignaledEvent);
    };
    neverSignaledWaiter = stdx::thread(waitForeverCallback);
    TaskExecutor::CallbackHandle shutdownCallback =
        unittest::assertGet(executor->scheduleWork(makeSetStatusAndShutdownClosure(&status5)));
    executor->wait(shutdownCallback);
}

void EventChainAndWaitingTest::assertSuccess() {
    neverSignaledWaiter.join();
    ASSERT_OK(status1);
    ASSERT_OK(status2);
    ASSERT_OK(status3);
    ASSERT_OK(status4);
    ASSERT_OK(status5);
}

void EventChainAndWaitingTest::onGo(const TaskExecutor::CallbackArgs& cbData) {
    if (!cbData.status.isOK()) {
        status1 = cbData.status;
        return;
    }
    executor::TaskExecutor* executor = cbData.executor;
    StatusWith<TaskExecutor::EventHandle> errorOrTriggerEvent = executor->makeEvent();
    if (!errorOrTriggerEvent.isOK()) {
        status1 = errorOrTriggerEvent.getStatus();
        executor->shutdown();
        return;
    }
    triggerEvent = errorOrTriggerEvent.getValue();
    StatusWith<TaskExecutor::CallbackHandle> cbHandle =
        executor->onEvent(triggerEvent, std::move(triggered2));
    if (!cbHandle.isOK()) {
        status1 = cbHandle.getStatus();
        executor->shutdown();
        return;
    }
    cbHandle = executor->onEvent(triggerEvent, std::move(triggered3));
    if (!cbHandle.isOK()) {
        status1 = cbHandle.getStatus();
        executor->shutdown();
        return;
    }

    cbHandle = executor->onEvent(goEvent, [=, this](const TaskExecutor::CallbackArgs& cbData) {
        onGoAfterTriggered(cbData);
    });
    if (!cbHandle.isOK()) {
        status1 = cbHandle.getStatus();
        executor->shutdown();
        return;
    }
    status1 = Status::OK();
}

void EventChainAndWaitingTest::onGoAfterTriggered(const TaskExecutor::CallbackArgs& cbData) {
    status4 = cbData.status;
    if (!cbData.status.isOK()) {
        return;
    }
    cbData.executor->signalEvent(triggerEvent);
}

COMMON_EXECUTOR_TEST(EventWaitingWithTimeoutTest) {
    TaskExecutor& executor = getExecutor();
    launchExecutorThread();

    auto eventThatWillNeverBeTriggered = unittest::assertGet(executor.makeEvent());

    auto serviceContext = ServiceContext::make();

    serviceContext->setFastClockSource(std::make_unique<ClockSourceMock>());
    auto mockClock = static_cast<ClockSourceMock*>(serviceContext->getFastClockSource());

    auto client = serviceContext->makeClient("for testing");
    auto opCtx = client->makeOperationContext();

    auto deadline = mockClock->now() + Milliseconds{1};
    mockClock->advance(Milliseconds(2));
    ASSERT(stdx::cv_status::timeout ==
           executor.waitForEvent(opCtx.get(), eventThatWillNeverBeTriggered, deadline));
    executor.shutdown();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(EventSignalWithTimeoutTest) {
    TaskExecutor& executor = getExecutor();
    launchExecutorThread();

    auto eventSignalled = unittest::assertGet(executor.makeEvent());

    auto serviceContext = ServiceContext::make();

    serviceContext->setFastClockSource(std::make_unique<ClockSourceMock>());
    auto mockClock = static_cast<ClockSourceMock*>(serviceContext->getFastClockSource());

    auto client = serviceContext->makeClient("for testing");
    auto opCtx = client->makeOperationContext();

    auto deadline = mockClock->now() + Milliseconds{1};
    mockClock->advance(Milliseconds(1));

    executor.signalEvent(eventSignalled);

    ASSERT(stdx::cv_status::no_timeout ==
           executor.waitForEvent(opCtx.get(), eventSignalled, deadline));
    executor.shutdown();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(SleepUntilReturnsReadyFutureWithSuccessWhenDeadlineAlreadyPassed) {
    NetworkInterfaceMock* net = getNet();
    TaskExecutor& executor = getExecutor();
    launchExecutorThread();

    const Date_t now = net->now();

    auto alarm = executor.sleepUntil(now, CancellationToken::uncancelable());
    ASSERT(alarm.isReady());
    ASSERT_OK(alarm.getNoThrow());
    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(
    SleepUntilReturnsReadyFutureWithShutdownInProgressWhenExecutorAlreadyShutdown) {
    NetworkInterfaceMock* net = getNet();
    TaskExecutor& executor = getExecutor();
    launchExecutorThread();
    shutdownExecutorThread();
    joinExecutorThread();

    const Date_t now = net->now();
    const Milliseconds sleepDuration{1000};
    const auto deadline = now + sleepDuration;
    auto alarm = executor.sleepUntil(deadline, CancellationToken::uncancelable());

    ASSERT(alarm.isReady());
    ASSERT_EQ(alarm.getNoThrow().code(), ErrorCodes::ShutdownInProgress);
}

COMMON_EXECUTOR_TEST(SleepUntilReturnsReadyFutureWithCallbackCanceledWhenTokenAlreadyCanceled) {
    NetworkInterfaceMock* net = getNet();
    TaskExecutor& executor = getExecutor();
    const Date_t now = net->now();
    const Milliseconds sleepDuration{1000};
    const auto deadline = now + sleepDuration;
    CancellationSource cancelSource;
    cancelSource.cancel();
    auto alarm = executor.sleepUntil(deadline, cancelSource.token());

    ASSERT(alarm.isReady());
    ASSERT_EQ(alarm.getNoThrow().code(), ErrorCodes::CallbackCanceled);
}

COMMON_EXECUTOR_TEST(
    SleepUntilResolvesOutputFutureWithCallbackCanceledWhenTokenCanceledBeforeDeadline) {
    NetworkInterfaceMock* net = getNet();
    TaskExecutor& executor = getExecutor();
    launchExecutorThread();

    const Date_t now = net->now();
    const Milliseconds sleepDuration{1000};
    const auto deadline = now + sleepDuration;
    CancellationSource cancelSource;
    auto alarm = executor.sleepUntil(deadline, cancelSource.token());

    ASSERT_FALSE(alarm.isReady());

    net->enterNetwork();
    // Run almost until the deadline. This isn't really necessary for the test since we could just
    // skip running the clock at all, so just doing this for some "realism".
    net->runUntil(deadline - Milliseconds(1));
    net->exitNetwork();

    // Cancel before deadline.
    cancelSource.cancel();
    // Required to process the cancellation.
    net->enterNetwork();
    net->exitNetwork();

    ASSERT(alarm.isReady());
    ASSERT_EQ(alarm.getNoThrow().code(), ErrorCodes::CallbackCanceled);

    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(
    SleepUntilResolvesOutputFutureWithCallbackCanceledWhenExecutorShutsDownBeforeDeadline) {
    NetworkInterfaceMock* net = getNet();
    TaskExecutor& executor = getExecutor();
    launchExecutorThread();

    const Date_t now = net->now();
    const Milliseconds sleepDuration{1000};
    const auto deadline = now + sleepDuration;
    CancellationSource cancelSource;
    auto alarm = executor.sleepUntil(deadline, cancelSource.token());

    ASSERT_FALSE(alarm.isReady());

    net->enterNetwork();
    // Run almost until the deadline. This isn't really necessary for the test since we could just
    // skip running the clock at all, so just doing this for some "realism".
    net->runUntil(deadline - Milliseconds(1));
    net->exitNetwork();

    // Shut down before deadline.
    shutdownExecutorThread();
    joinExecutorThread();

    ASSERT(alarm.isReady());
    ASSERT_EQ(alarm.getNoThrow().code(), ErrorCodes::CallbackCanceled);
}

COMMON_EXECUTOR_TEST(SleepUntilResolvesOutputFutureWithSuccessWhenDeadlinePasses) {
    NetworkInterfaceMock* net = getNet();
    TaskExecutor& executor = getExecutor();
    launchExecutorThread();

    const Date_t now = net->now();
    const Milliseconds sleepDuration{1000};
    const auto deadline = now + sleepDuration;

    auto alarm = executor.sleepUntil(deadline, CancellationToken::uncancelable());
    ASSERT_FALSE(alarm.isReady());

    net->enterNetwork();
    net->runUntil(deadline);
    net->exitNetwork();

    ASSERT(alarm.isReady());
    ASSERT_OK(alarm.getNoThrow());

    executor.shutdown();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(ScheduleWorkAt) {
    NetworkInterfaceMock* net = getNet();
    TaskExecutor& executor = getExecutor();
    launchExecutorThread();
    Status status1 = getDetectableErrorStatus();
    Status status2 = getDetectableErrorStatus();
    Status status3 = getDetectableErrorStatus();
    Status status4 = getDetectableErrorStatus();

    const Date_t now = net->now();
    const TaskExecutor::CallbackHandle cb1 = unittest::assertGet(
        executor.scheduleWorkAt(now + Milliseconds(100), makeSetStatusClosure(&status1)));
    const TaskExecutor::CallbackHandle cb4 = unittest::assertGet(
        executor.scheduleWorkAt(now - Milliseconds(50), makeSetStatusClosure(&status4)));
    unittest::assertGet(
        executor.scheduleWorkAt(now + Milliseconds(5000), makeSetStatusClosure(&status3)));
    const TaskExecutor::CallbackHandle cb2 = unittest::assertGet(executor.scheduleWorkAt(
        now + Milliseconds(200), makeSetStatusAndShutdownClosure(&status2)));

    executor.wait(cb4);
    ASSERT_OK(status4);

    const Date_t startTime = net->now();
    net->enterNetwork();
    net->runUntil(startTime + Milliseconds(200));
    net->exitNetwork();
    ASSERT_EQUALS(startTime + Milliseconds(200), net->now());
    executor.wait(cb1);
    executor.wait(cb2);
    ASSERT_OK(status1);
    ASSERT_OK(status2);
    executor.shutdown();
    joinExecutorThread();
    ASSERT_EQUALS(status3, ErrorCodes::CallbackCanceled);
}

COMMON_EXECUTOR_TEST(ScheduleRemoteCommand) {
    NetworkInterfaceMock* net = getNet();
    TaskExecutor& executor = getExecutor();
    launchExecutorThread();
    Status status1 = getDetectableErrorStatus();
    TaskExecutor::CallbackHandle cbHandle = unittest::assertGet(executor.scheduleRemoteCommand(
        kDummyRequest, makeSetStatusOnRemoteCommandCompletionClosure(&kDummyRequest, &status1)));
    net->enterNetwork();
    ASSERT(net->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    net->scheduleResponse(noi, net->now(), {ErrorCodes::NoSuchKey, "I'm missing"});
    net->runReadyNetworkOperations();
    ASSERT(!net->hasReadyRequests());
    net->exitNetwork();
    executor.wait(cbHandle);
    executor.shutdown();
    joinExecutorThread();
    ASSERT_EQUALS(ErrorCodes::NoSuchKey, status1);
}

COMMON_EXECUTOR_TEST(ScheduleAndCancelRemoteCommand) {
    TaskExecutor& executor = getExecutor();
    Status status1 = getDetectableErrorStatus();
    TaskExecutor::CallbackHandle cbHandle = unittest::assertGet(executor.scheduleRemoteCommand(
        kDummyRequest, makeSetStatusOnRemoteCommandCompletionClosure(&kDummyRequest, &status1)));
    executor.cancel(cbHandle);
    launchExecutorThread();
    getNet()->enterNetwork();
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();
    executor.wait(cbHandle);
    executor.shutdown();
    joinExecutorThread();
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, status1);
}

COMMON_EXECUTOR_TEST(
    ScheduleRemoteCommandWithCancellationTokenSuccessfullyCancelsRequestIfCanceledAfterFunctionCallButBeforeProcessing) {
    TaskExecutor& executor = getExecutor();
    CancellationSource cancelSource;
    auto responseFuture = executor.scheduleRemoteCommand(kDummyRequest, cancelSource.token());

    cancelSource.cancel();

    launchExecutorThread();
    getNet()->enterNetwork();
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();

    // Wait for cancellation to happen and expect error status on future.
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, responseFuture.getNoThrow());

    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(
    ScheduleRemoteCommandWithCancellationTokenSuccessfullyCancelsRequestIfCanceledBeforeFunctionCallAndBeforeProcessing) {
    TaskExecutor& executor = getExecutor();

    CancellationSource cancelSource;
    // Cancel before calling scheduleRemoteCommand.
    cancelSource.cancel();
    auto responseFuture = executor.scheduleRemoteCommand(kDummyRequest, cancelSource.token());

    // The result should be immediately available.
    ASSERT_EQUALS(ErrorCodes::CallbackCanceled, responseFuture.getNoThrow());
}

COMMON_EXECUTOR_TEST(
    ScheduleRemoteCommandWithCancellationTokenDoesNotCancelRequestIfCanceledAfterProcessing) {
    TaskExecutor& executor = getExecutor();
    CancellationSource cancelSource;
    auto responseFuture = executor.scheduleRemoteCommand(kDummyRequest, cancelSource.token());

    launchExecutorThread();
    getNet()->enterNetwork();
    // Respond to the request.
    getNet()->scheduleSuccessfulResponse(BSONObj{});
    getNet()->runReadyNetworkOperations();
    getNet()->exitNetwork();

    // Response should be ready and okay.
    ASSERT_OK(responseFuture.getNoThrow());

    // Cancel after the response has already been processed. This shouldn't do anything or cause an
    // error.
    cancelSource.cancel();

    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(
    ScheduleRemoteCommandWithCancellationTokenReturnsShutdownInProgressIfExecutorAlreadyShutdownAndCancelNotCalled) {
    TaskExecutor& executor = getExecutor();

    launchExecutorThread();
    shutdownExecutorThread();
    joinExecutorThread();

    auto responseFuture =
        executor.scheduleRemoteCommand(kDummyRequest, CancellationToken::uncancelable());
    ASSERT_EQ(responseFuture.getNoThrow().getStatus().code(), ErrorCodes::ShutdownInProgress);
}

COMMON_EXECUTOR_TEST(
    ScheduleRemoteCommandWithCancellationTokenReturnsShutdownInProgressIfExecutorAlreadyShutdownAndCancelCalled) {
    TaskExecutor& executor = getExecutor();

    CancellationSource cancelSource;
    auto responseFuture = executor.scheduleRemoteCommand(kDummyRequest, cancelSource.token());

    launchExecutorThread();
    shutdownExecutorThread();
    joinExecutorThread();

    // Should already be ready. Returns CallbackCanceled and not ShutdownInProgress as an
    // implementation detail.
    ASSERT_EQ(responseFuture.getNoThrow().getStatus().code(), ErrorCodes::CallbackCanceled);

    // Shouldn't do anything or cause an error.
    cancelSource.cancel();
}

COMMON_EXECUTOR_TEST(RemoteCommandWithTimeout) {
    NetworkInterfaceMock* net = getNet();
    TaskExecutor& executor = getExecutor();
    Status status(ErrorCodes::InternalError, "");
    launchExecutorThread();
    const RemoteCommandRequest request(HostAndPort("lazy", 27017),
                                       DatabaseName::kAdmin,
                                       BSON("sleep" << 1),
                                       nullptr,
                                       Milliseconds(1));
    TaskExecutor::CallbackHandle cbHandle = unittest::assertGet(executor.scheduleRemoteCommand(
        request, makeSetStatusOnRemoteCommandCompletionClosure(&request, &status)));
    net->enterNetwork();
    ASSERT(net->hasReadyRequests());
    const Date_t startTime = net->now();
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    net->scheduleResponse(noi, startTime + Milliseconds(2), {});
    net->runUntil(startTime + Milliseconds(2));
    ASSERT_EQUALS(startTime + Milliseconds(2), net->now());
    net->exitNetwork();
    executor.wait(cbHandle);
    ASSERT_EQUALS(ErrorCodes::NetworkInterfaceExceededTimeLimit, status);
}

COMMON_EXECUTOR_TEST(CallbackHandleComparison) {
    TaskExecutor& executor = getExecutor();
    auto status1 = getDetectableErrorStatus();
    auto status2 = getDetectableErrorStatus();
    const RemoteCommandRequest request(
        HostAndPort("lazy", 27017), DatabaseName::kAdmin, BSON("cmd" << 1), nullptr);
    TaskExecutor::CallbackHandle cbHandle1 = unittest::assertGet(executor.scheduleRemoteCommand(
        request, makeSetStatusOnRemoteCommandCompletionClosure(&request, &status1)));
    TaskExecutor::CallbackHandle cbHandle2 = unittest::assertGet(executor.scheduleRemoteCommand(
        request, makeSetStatusOnRemoteCommandCompletionClosure(&request, &status2)));

    // test equality
    ASSERT_TRUE(cbHandle1 == cbHandle1);
    ASSERT_TRUE(cbHandle2 == cbHandle2);
    ASSERT_FALSE(cbHandle1 != cbHandle1);
    ASSERT_FALSE(cbHandle2 != cbHandle2);

    // test inequality
    ASSERT_TRUE(cbHandle1 != cbHandle2);
    ASSERT_TRUE(cbHandle2 != cbHandle1);
    ASSERT_FALSE(cbHandle1 == cbHandle2);
    ASSERT_FALSE(cbHandle2 == cbHandle1);

    TaskExecutor::CallbackHandle cbHandle1Copy = cbHandle1;
    ASSERT_TRUE(cbHandle1 == cbHandle1Copy);
    ASSERT_TRUE(cbHandle1Copy == cbHandle1);
    ASSERT_FALSE(cbHandle1Copy != cbHandle1);
    ASSERT_FALSE(cbHandle1 != cbHandle1Copy);

    std::vector<TaskExecutor::CallbackHandle> cbs;
    cbs.push_back(cbHandle1);
    cbs.push_back(cbHandle2);
    ASSERT(cbHandle1 != cbHandle2);
    std::vector<TaskExecutor::CallbackHandle>::iterator foundHandle =
        std::find(cbs.begin(), cbs.end(), cbHandle1);
    ASSERT_TRUE(cbs.end() != foundHandle);
    ASSERT_TRUE(cbHandle1 == *foundHandle);
    launchExecutorThread();
    executor.shutdown();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(ScheduleExhaustRemoteCommandIsResolvedWhenMoreToComeFlagIsFalse) {
    TaskExecutor& executor = getExecutor();

    size_t numTimesCallbackCalled = 0;
    Status status = getDetectableErrorStatus();

    TaskExecutor::CallbackHandle cbHandle =
        unittest::assertGet(executor.scheduleExhaustRemoteCommand(
            kDummyRequest,
            makeSetStatusOnRemoteExhaustCommandCompletionClosure(
                &kDummyRequest, &status, &numTimesCallbackCalled)));

    launchExecutorThread();

    auto net = getNet();

    net->enterNetwork();
    // Respond to the request.
    ASSERT(net->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const Date_t startTime = net->now();
    std::list responses = {
        std::make_pair(startTime, RemoteCommandResponse(BSONObj{}, Microseconds(), true)),
        std::make_pair(startTime + Milliseconds(2),
                       RemoteCommandResponse(BSONObj{}, Microseconds(), false))};
    for (auto& [when, response] : responses) {
        net->scheduleResponse(noi, when, response);
    }
    net->runUntil(startTime + Milliseconds(3));
    ASSERT(!net->hasReadyRequests());
    net->exitNetwork();

    // Response should be ready and okay.
    executor.wait(cbHandle);
    ASSERT_OK(status);
    ASSERT_EQUALS(numTimesCallbackCalled, 2);

    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(ScheduleExhaustRemoteCommandIsResolvedWhenCanceled) {
    TaskExecutor& executor = getExecutor();

    size_t numTimesCallbackCalled = 0;
    Status status = getDetectableErrorStatus();

    TaskExecutor::CallbackHandle cbHandle =
        unittest::assertGet(executor.scheduleExhaustRemoteCommand(
            kDummyRequest,
            makeSetStatusOnRemoteExhaustCommandCompletionClosure(
                &kDummyRequest, &status, &numTimesCallbackCalled)));

    launchExecutorThread();

    auto net = getNet();

    net->enterNetwork();
    // Respond to the request.
    ASSERT(net->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const Date_t startTime = net->now();
    std::list responses = {
        std::make_pair(startTime, RemoteCommandResponse(BSONObj{}, Microseconds(), true)),
        std::make_pair(startTime + Milliseconds(2),
                       RemoteCommandResponse(BSONObj{}, Microseconds(), false))};
    for (auto& [when, response] : responses) {
        net->scheduleResponse(noi, when, response);
    }

    net->runUntil(startTime + Milliseconds(1));
    ASSERT_EQUALS(numTimesCallbackCalled, 1);
    ASSERT_OK(status);

    net->cancelCommand(cbHandle);
    net->runReadyNetworkOperations();
    ASSERT_EQUALS(numTimesCallbackCalled, 2);
    ASSERT_EQUALS(status, ErrorCodes::CallbackCanceled);

    ASSERT(!net->hasReadyRequests());
    net->exitNetwork();

    // Response should be ready.
    executor.wait(cbHandle);

    net->enterNetwork();
    // Since we canceled before T+2, we do not observe the final response.
    net->runUntil(startTime + Milliseconds(2));
    ASSERT_EQUALS(numTimesCallbackCalled, 2);
    net->exitNetwork();

    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(ScheduleExhaustRemoteCommandSwallowsErrorsWhenMoreToComeFlagIsTrue) {
    TaskExecutor& executor = getExecutor();

    size_t numTimesCallbackCalled = 0;
    Status status = getDetectableErrorStatus();

    TaskExecutor::CallbackHandle cbHandle =
        unittest::assertGet(executor.scheduleExhaustRemoteCommand(
            kDummyRequest,
            makeSetStatusOnRemoteExhaustCommandCompletionClosure(
                &kDummyRequest, &status, &numTimesCallbackCalled)));

    launchExecutorThread();

    auto net = getNet();

    net->enterNetwork();
    // Respond to the request.
    ASSERT(net->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const Date_t startTime = net->now();
    RemoteCommandResponse error_response{ErrorCodes::NoSuchKey, "I'm missing"};
    error_response.moreToCome = true;
    std::list responses = {std::make_pair(startTime, error_response),
                           std::make_pair(startTime + Milliseconds(2),
                                          RemoteCommandResponse(BSONObj{}, Microseconds(), false))};
    for (auto& [when, response] : responses) {
        net->scheduleResponse(noi, when, response);
    }
    net->runUntil(startTime + Milliseconds(3));
    ASSERT(!net->hasReadyRequests());
    net->exitNetwork();

    // Response should be ready and okay.
    executor.wait(cbHandle);
    ASSERT_OK(status);
    ASSERT_EQUALS(numTimesCallbackCalled, 2);

    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(
    ScheduleExhaustRemoteCommandFutureIsResolvedWhenMoreToComeFlagIsFalseOnFirstResponse) {
    TaskExecutor& executor = getExecutor();
    CancellationSource cancelSource;

    size_t numTimesCallbackCalled = 0;
    auto cb = [&numTimesCallbackCalled](const TaskExecutor::RemoteCommandCallbackArgs&) {
        ++numTimesCallbackCalled;
    };
    auto responseFuture =
        executor.scheduleExhaustRemoteCommand(kDummyRequest, cb, cancelSource.token());

    launchExecutorThread();

    auto net = getNet();

    net->enterNetwork();
    // Respond to the request.
    ASSERT(net->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const Date_t startTime = net->now();
    net->scheduleResponse(noi, startTime, RemoteCommandResponse(BSONObj{}, Microseconds(), false));
    net->runUntil(startTime + Milliseconds(1));

    ASSERT_EQUALS(numTimesCallbackCalled, 1);
    ASSERT(!net->hasReadyRequests());
    net->exitNetwork();

    // Response should be ready and okay.
    ASSERT_OK(responseFuture.getNoThrow());
    ASSERT_EQUALS(numTimesCallbackCalled, 1);

    // Cancel after the response has already been processed. This shouldn't do anything or cause an
    // error.
    cancelSource.cancel();

    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(ScheduleExhaustRemoteCommandFutureIsResolvedWhenMoreToComeFlagIsFalse) {
    TaskExecutor& executor = getExecutor();
    CancellationSource cancelSource;

    size_t numTimesCallbackCalled = 0;
    auto cb = [&numTimesCallbackCalled](const TaskExecutor::RemoteCommandCallbackArgs&) {
        ++numTimesCallbackCalled;
    };
    auto responseFuture =
        executor.scheduleExhaustRemoteCommand(kDummyRequest, cb, cancelSource.token());

    launchExecutorThread();

    auto net = getNet();

    net->enterNetwork();
    // Respond to the request.
    ASSERT(net->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const Date_t startTime = net->now();
    std::list responses = {
        std::make_pair(startTime, RemoteCommandResponse(BSONObj{}, Microseconds(), true)),
        std::make_pair(startTime + Milliseconds(2),
                       RemoteCommandResponse(BSONObj{}, Microseconds(), false))};
    for (auto& [when, response] : responses) {
        net->scheduleResponse(noi, when, response);
    }
    net->runUntil(startTime + Milliseconds(1));

    ASSERT_EQUALS(numTimesCallbackCalled, 1);

    net->runUntil(startTime + Milliseconds(3));
    ASSERT(!net->hasReadyRequests());
    net->exitNetwork();

    // Response should be ready and okay.
    ASSERT_OK(responseFuture.getNoThrow());
    ASSERT_EQUALS(numTimesCallbackCalled, 2);

    // Cancel after the response has already been processed. This shouldn't do anything or cause an
    // error.
    cancelSource.cancel();

    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(ScheduleExhaustRemoteCommandFutureIsResolvedWhenErrorResponseReceived) {
    TaskExecutor& executor = getExecutor();
    CancellationSource cancelSource;

    size_t numTimesCallbackCalled = 0;
    auto cb = [&numTimesCallbackCalled](const TaskExecutor::RemoteCommandCallbackArgs&) {
        ++numTimesCallbackCalled;
    };
    auto responseFuture =
        executor.scheduleExhaustRemoteCommand(kDummyRequest, cb, cancelSource.token());

    launchExecutorThread();

    auto net = getNet();

    net->enterNetwork();
    // Respond to the request.
    ASSERT(net->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    net->scheduleResponse(noi, net->now(), {ErrorCodes::NoSuchKey, "I'm missing"});
    net->runReadyNetworkOperations();

    ASSERT_EQUALS(numTimesCallbackCalled, 1);
    ASSERT(!net->hasReadyRequests());
    net->exitNetwork();

    // Response should be ready and have an error.
    ASSERT_EQUALS(responseFuture.getNoThrow().getStatus().code(), ErrorCodes::NoSuchKey);
    ASSERT_EQUALS(numTimesCallbackCalled, 1);

    // Cancel after the response has already been processed. This shouldn't do anything or cause an
    // error.
    cancelSource.cancel();

    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(ScheduleExhaustRemoteCommandFutureSwallowsErrorsWhenMoreToCome) {
    TaskExecutor& executor = getExecutor();
    CancellationSource cancelSource;

    size_t numTimesCallbackCalled = 0;
    auto cb = [&numTimesCallbackCalled](const TaskExecutor::RemoteCommandCallbackArgs&) {
        ++numTimesCallbackCalled;
    };
    auto responseFuture =
        executor.scheduleExhaustRemoteCommand(kDummyRequest, cb, cancelSource.token());

    launchExecutorThread();

    auto net = getNet();

    net->enterNetwork();
    const Date_t startTime = net->now();

    // Respond to the request.
    ASSERT(net->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    RemoteCommandResponse error_response{ErrorCodes::NoSuchKey, "I'm missing"};
    error_response.moreToCome = true;
    auto responses = {std::make_pair(startTime, error_response),
                      std::make_pair(startTime + Milliseconds(2),
                                     RemoteCommandResponse(BSONObj{}, Microseconds(), false))};
    for (auto& [when, response] : responses) {
        net->scheduleResponse(noi, when, response);
    }
    net->runUntil(startTime + Milliseconds(1));

    ASSERT_EQUALS(numTimesCallbackCalled, 1);

    net->runUntil(startTime + Milliseconds(3));
    ASSERT(!net->hasReadyRequests());
    net->exitNetwork();

    // Response should be ready and okay, since we swallowed the first error.
    ASSERT_OK(responseFuture.getNoThrow());
    ASSERT_EQUALS(numTimesCallbackCalled, 2);

    // Cancel after the response has already been processed. This shouldn't do anything or cause an
    // error.
    cancelSource.cancel();

    shutdownExecutorThread();
    joinExecutorThread();
}

COMMON_EXECUTOR_TEST(ScheduleExhaustRemoteCommandFutureIsResolvedWithErrorOnCancellation) {
    TaskExecutor& executor = getExecutor();
    CancellationSource cancelSource;

    size_t numTimesCallbackCalled = 0;
    auto cb = [&numTimesCallbackCalled](const TaskExecutor::RemoteCommandCallbackArgs&) {
        ++numTimesCallbackCalled;
    };
    auto responseFuture =
        executor.scheduleExhaustRemoteCommand(kDummyRequest, cb, cancelSource.token());
    launchExecutorThread();

    auto net = getNet();

    net->enterNetwork();
    // Respond to the request.
    ASSERT(net->hasReadyRequests());
    NetworkInterfaceMock::NetworkOperationIterator noi = net->getNextReadyRequest();
    const Date_t startTime = net->now();
    std::list responses = {
        std::make_pair(startTime, RemoteCommandResponse(BSONObj{}, Microseconds(), true)),
        std::make_pair(startTime + Milliseconds(2),
                       RemoteCommandResponse(BSONObj{}, Microseconds(), false))};
    for (auto& [when, response] : responses) {
        net->scheduleResponse(noi, when, response);
    }
    net->runUntil(startTime + Milliseconds(1));

    cancelSource.cancel();

    net->runUntil(startTime + Milliseconds(3));
    ASSERT(!net->hasReadyRequests());
    net->exitNetwork();
    // Response should be cancelled.
    ASSERT_EQUALS(responseFuture.getNoThrow().getStatus().code(), ErrorCodes::CallbackCanceled);
    ASSERT_EQUALS(numTimesCallbackCalled, 2);

    shutdownExecutorThread();
    joinExecutorThread();
}
}  // namespace

void addTestsForExecutor(const std::string& suiteName, ExecutorFactory makeExecutor) {
    auto& suite = unittest::Suite::getSuite(suiteName);
    for (const auto& testCase : executorTestCaseRegistry()) {
        suite.add(str::stream() << suiteName << "::" << testCase.first,
                  __FILE__,
                  [testCase, makeExecutor] { testCase.second(makeExecutor)->run(); });
    }
}

}  // namespace executor
}  // namespace mongo
