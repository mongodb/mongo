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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/executor/task_executor_test_common.h"

#include <memory>

#include "mongo/db/operation_context.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/thread.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/log.h"
#include "mongo/util/str.h"

namespace mongo {
namespace executor {
namespace {

using ExecutorFactory =
    std::function<std::unique_ptr<TaskExecutor>(std::unique_ptr<NetworkInterfaceMock>)>;

class CommonTaskExecutorTestFixture : public TaskExecutorTest {
public:
    CommonTaskExecutorTestFixture(ExecutorFactory makeExecutor)
        : _makeExecutor(std::move(makeExecutor)) {}

private:
    std::unique_ptr<TaskExecutor> makeTaskExecutor(
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
            severe() << "Multiple attempts to register ExecutorTest named " << name;
            fassertFailed(28713);
        }
        entry = std::move(makeTest);
    }
};

#define COMMON_EXECUTOR_TEST(TEST_NAME)                                         \
    class CET_##TEST_NAME : public CommonTaskExecutorTestFixture {              \
    public:                                                                     \
        CET_##TEST_NAME(ExecutorFactory makeExecutor)                           \
            : CommonTaskExecutorTestFixture(std::move(makeExecutor)) {}         \
                                                                                \
    private:                                                                    \
        void _doTest() override;                                                \
        static const CetRegistrationAgent _agent;                               \
    };                                                                          \
    const CetRegistrationAgent CET_##TEST_NAME::_agent(                         \
        #TEST_NAME, [](ExecutorFactory makeExecutor) {                          \
            return stdx::make_unique<CET_##TEST_NAME>(std::move(makeExecutor)); \
        });                                                                     \
    void CET_##TEST_NAME::_doTest()

auto makeSetStatusClosure(Status* target) {
    return [target](const TaskExecutor::CallbackArgs& cbData) { *target = cbData.status; };
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
                return str::stream() << "Request(" << request.target.toString() << ", "
                                     << request.dbname << ", " << request.cmdObj << ')';
            };
            *outStatus =
                Status(ErrorCodes::BadValue,
                       str::stream() << "Actual request: " << desc(cbData.request) << "; expected: "
                                     << desc(*expectedRequest));
            return;
        }
        *outStatus = cbData.response.status;
    };
}

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
    executor->onEvent(goEvent, [=](const TaskExecutor::CallbackArgs& cbData) { onGo(cbData); })
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

    cbHandle = executor->onEvent(
        goEvent, [=](const TaskExecutor::CallbackArgs& cbData) { onGoAfterTriggered(cbData); });
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

    serviceContext->setFastClockSource(stdx::make_unique<ClockSourceMock>());
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

    serviceContext->setFastClockSource(stdx::make_unique<ClockSourceMock>());
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
    const RemoteCommandRequest request(HostAndPort("localhost", 27017),
                                       "mydb",
                                       BSON("whatsUp"
                                            << "doc"),
                                       nullptr);
    TaskExecutor::CallbackHandle cbHandle = unittest::assertGet(executor.scheduleRemoteCommand(
        request, makeSetStatusOnRemoteCommandCompletionClosure(&request, &status1)));
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
    const RemoteCommandRequest request(HostAndPort("localhost", 27017),
                                       "mydb",
                                       BSON("whatsUp"
                                            << "doc"),
                                       nullptr);
    TaskExecutor::CallbackHandle cbHandle = unittest::assertGet(executor.scheduleRemoteCommand(
        request, makeSetStatusOnRemoteCommandCompletionClosure(&request, &status1)));
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


COMMON_EXECUTOR_TEST(RemoteCommandWithTimeout) {
    NetworkInterfaceMock* net = getNet();
    TaskExecutor& executor = getExecutor();
    Status status(ErrorCodes::InternalError, "");
    launchExecutorThread();
    const RemoteCommandRequest request(
        HostAndPort("lazy", 27017), "admin", BSON("sleep" << 1), nullptr, Milliseconds(1));
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
        HostAndPort("lazy", 27017), "admin", BSON("cmd" << 1), nullptr);
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

}  // namespace

void addTestsForExecutor(const std::string& suiteName, ExecutorFactory makeExecutor) {
    auto suite = unittest::Suite::getSuite(suiteName);
    for (auto testCase : executorTestCaseRegistry()) {
        suite->add(str::stream() << suiteName << "::" << testCase.first,
                   [testCase, makeExecutor] { testCase.second(makeExecutor)->run(); });
    }
}

}  // namespace executor
}  // namespace mongo
