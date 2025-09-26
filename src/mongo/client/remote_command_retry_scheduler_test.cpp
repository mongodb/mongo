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


// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/client/remote_command_retry_scheduler.h"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/db/baton.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/net/hostandport.h"

#include <chrono>
#include <functional>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace {

using namespace mongo;
using ResponseStatus = executor::TaskExecutor::ResponseStatus;

class CallbackResponseSaver;

class RemoteCommandRetrySchedulerTest : public executor::ThreadPoolExecutorTest {
public:
    void start(RemoteCommandRetryScheduler* scheduler);
    void checkCompletionStatus(RemoteCommandRetryScheduler* scheduler,
                               const CallbackResponseSaver& callbackResponseSaver,
                               const ResponseStatus& response);
    void processNetworkResponse(const ResponseStatus& response,
                                Milliseconds minExpectedDelay = Milliseconds{0},
                                Milliseconds maxWaitTimeout = Milliseconds{10000});

protected:
    void setUp() override;
};

class CallbackResponseSaver {
    CallbackResponseSaver(const CallbackResponseSaver&) = delete;
    CallbackResponseSaver& operator=(const CallbackResponseSaver&) = delete;

public:
    CallbackResponseSaver();

    /**
     * Use this for scheduler callback.
     */
    void operator()(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba);

    std::vector<ResponseStatus> getResponses() const;

private:
    std::vector<ResponseStatus> _responses;
};

/**
 * Task executor proxy with fail point for scheduleRemoteCommand().
 */
class TaskExecutorWithFailureInScheduleRemoteCommand : public unittest::TaskExecutorProxy {
public:
    using unittest::TaskExecutorProxy::TaskExecutorProxy;

    StatusWith<executor::TaskExecutor::CallbackHandle> scheduleRemoteCommand(
        const executor::RemoteCommandRequest& request,
        const RemoteCommandCallbackFn& cb,
        const BatonHandle& baton = nullptr) override {
        if (scheduleRemoteCommandFailPoint) {
            return Status(ErrorCodes::ShutdownInProgress,
                          "failed to send remote command - shutdown in progress");
        }
        return getExecutor()->scheduleRemoteCommand(request, cb, baton);
    }

    bool scheduleRemoteCommandFailPoint = false;
};

/**
 * A thin test wrapper over the default retry strategy that always returns a configurable retry
 * delay.
 */
class TestWithDelayRetryStrategy final : public RetryStrategy {
public:
    TestWithDelayRetryStrategy(DefaultRetryStrategy retryStrategy,
                               Milliseconds testBaseBackoffMillis)
        : _underlyingStrategy(std::move(retryStrategy)),
          _testBaseBackoffMillis(testBaseBackoffMillis) {}

    bool recordFailureAndEvaluateShouldRetry(Status s,
                                             const boost::optional<HostAndPort>& target,
                                             std::span<const std::string> errorLabels) override {
        return _underlyingStrategy.recordFailureAndEvaluateShouldRetry(s, target, errorLabels);
    }

    void recordSuccess(const boost::optional<HostAndPort>& target) override {
        _underlyingStrategy.recordSuccess(target);
    }

    Milliseconds getNextRetryDelay() const override {
        return _testBaseBackoffMillis;
    }

    const TargetingMetadata& getTargetingMetadata() const override {
        return _underlyingStrategy.getTargetingMetadata();
    }

private:
    DefaultRetryStrategy _underlyingStrategy;
    Milliseconds _testBaseBackoffMillis;
};

void RemoteCommandRetrySchedulerTest::start(RemoteCommandRetryScheduler* scheduler) {
    ASSERT_FALSE(scheduler->isActive());

    ASSERT_OK(scheduler->startup());
    ASSERT_TRUE(scheduler->isActive());

    // Starting an already active scheduler should fail.
    ASSERT_EQUALS(ErrorCodes::IllegalOperation, scheduler->startup());
    ASSERT_TRUE(scheduler->isActive());

    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    ASSERT_TRUE(net->hasReadyRequests());
}

void RemoteCommandRetrySchedulerTest::checkCompletionStatus(
    RemoteCommandRetryScheduler* scheduler,
    const CallbackResponseSaver& callbackResponseSaver,
    const ResponseStatus& response) {
    ASSERT_FALSE(scheduler->isActive());
    auto responses = callbackResponseSaver.getResponses();
    ASSERT_EQUALS(1U, responses.size());
    if (response.isOK()) {
        ASSERT_OK(responses.front().status);
        ASSERT_EQUALS(response, responses.front());
    } else {
        ASSERT_EQUALS(response.status, responses.front().status);
    }
}

void RemoteCommandRetrySchedulerTest::processNetworkResponse(const ResponseStatus& response,
                                                             const Milliseconds minExpectedDelay,
                                                             const Milliseconds maxWaitTimeout) {
    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    if (minExpectedDelay > Milliseconds(0)) {
        auto totalDelay = Milliseconds(0);
        while (!net->hasReadyRequests() && totalDelay < maxWaitTimeout) {
            net->advanceTime(net->now() + minExpectedDelay);
            totalDelay += minExpectedDelay;
            stdx::this_thread::sleep_for(stdx::chrono::milliseconds(1));
        }
        // Request should not become ready too quickly - verify delay is working.
        ASSERT_GTE(totalDelay, minExpectedDelay);
        // Ensure we didn't timeout waiting for the request to become ready.
        ASSERT_LT(totalDelay, maxWaitTimeout);
    }
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    net->scheduleResponse(noi, net->now(), response);
    net->runReadyNetworkOperations();
}

void RemoteCommandRetrySchedulerTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();
    launchExecutorThread();
}

CallbackResponseSaver::CallbackResponseSaver() = default;

void CallbackResponseSaver::operator()(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba) {
    _responses.push_back(rcba.response);
}

std::vector<ResponseStatus> CallbackResponseSaver::getResponses() const {
    return _responses;
}

executor::RemoteCommandRequest makeRemoteCommandRequest() {
    return executor::RemoteCommandRequest{
        HostAndPort("h1:12345"),
        DatabaseName::createDatabaseName_forTest(boost::none, "db1"),
        BSON("ping" << 1),
        nullptr};
}

TEST_F(RemoteCommandRetrySchedulerTest, MakeSingleShotRetryStrategy) {
    auto strategy = NoRetryStrategy();
    // Doesn't matter what "shouldRetryOnError()" returns since we won't be retrying the remote
    // command.
    for (int i = 0; i < int(ErrorCodes::MaxError); ++i) {
        const auto error = ErrorCodes::Error(i);
        if (ErrorCodes::mustHaveExtraInfo(error)) {
            continue;
        }
        const auto status = Status(error, ""_sd);
        ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(status, boost::none, {}));
    }
}

TEST_F(RemoteCommandRetrySchedulerTest, MakeRetryStrategyMaxResponse) {
    auto strategy = DefaultRetryStrategy(15U);
    size_t errorCounter = 0;
    for (int i = 0; i < int(ErrorCodes::MaxError); ++i) {
        auto error = ErrorCodes::Error(i);
        if (ErrorCodes::mustHaveExtraInfo(error)) {
            continue;
        }
        auto status = Status(error, "test");
        if (ErrorCodes::isA<ErrorCategory::RetriableError>(error)) {
            errorCounter++;
            if (errorCounter <= 15) {
                ASSERT_TRUE(strategy.recordFailureAndEvaluateShouldRetry(status, boost::none, {}));
            } else {
                ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(status, boost::none, {}));
            }
            continue;
        }
        ASSERT_FALSE(strategy.recordFailureAndEvaluateShouldRetry(status, boost::none, {}));
    }
}

TEST_F(RemoteCommandRetrySchedulerTest, MakeRetryStrategy) {
    auto strategy = std::make_unique<DefaultRetryStrategy>(ErrorCodes::MaxError);
    for (int i = 0; i < int(ErrorCodes::MaxError); ++i) {
        auto error = ErrorCodes::Error(i);
        if (ErrorCodes::mustHaveExtraInfo(error)) {
            continue;
        }
        auto status = Status(error, "test");
        if (ErrorCodes::isA<ErrorCategory::RetriableError>(error)) {
            ASSERT_TRUE(strategy->recordFailureAndEvaluateShouldRetry(status, boost::none, {}));
            strategy->recordSuccess(boost::none);
            continue;
        }
        ASSERT_FALSE(strategy->recordFailureAndEvaluateShouldRetry(status, boost::none, {}));
    }
}

TEST_F(RemoteCommandRetrySchedulerTest, InvalidConstruction) {
    auto callback = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {
    };
    auto makeRetryStrategy = [] {
        return std::make_unique<NoRetryStrategy>();
    };
    auto request = makeRemoteCommandRequest();

    // Null executor.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(nullptr, request, callback, makeRetryStrategy()),
        AssertionException,
        ErrorCodes::BadValue,
        "task executor cannot be null");

    // Empty source in remote command request.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            executor::RemoteCommandRequest(HostAndPort(), request.dbname, request.cmdObj, nullptr),
            callback,
            makeRetryStrategy()),
        AssertionException,
        ErrorCodes::BadValue,
        "source in remote command request cannot be empty");

    // Empty source in remote command request.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            executor::RemoteCommandRequest(
                request.target, DatabaseName::kEmpty, request.cmdObj, nullptr),
            callback,
            makeRetryStrategy()),
        AssertionException,
        ErrorCodes::BadValue,
        "database name in remote command request cannot be empty");

    // Empty command object in remote command request.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            executor::RemoteCommandRequest(request.target, request.dbname, BSONObj(), nullptr),
            callback,
            makeRetryStrategy()),
        AssertionException,
        ErrorCodes::BadValue,
        "command object in remote command request cannot be empty");

    // Null remote command callback function.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(&getExecutor(),
                                    request,
                                    executor::TaskExecutor::RemoteCommandCallbackFn(),
                                    makeRetryStrategy()),
        AssertionException,
        ErrorCodes::BadValue,
        "remote command callback function cannot be null");

    // Null retry strategy.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(), request, callback, std::unique_ptr<DefaultRetryStrategy>()),
        AssertionException,
        ErrorCodes::BadValue,
        "retry strategy cannot be null");
}

TEST_F(RemoteCommandRetrySchedulerTest, StartupFailsWhenExecutorIsShutDown) {
    auto callback = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {
    };
    auto strategy = std::make_unique<NoRetryStrategy>();
    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(&getExecutor(), request, callback, std::move(strategy));
    ASSERT_FALSE(scheduler.isActive());

    getExecutor().shutdown();

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, scheduler.startup());
    ASSERT_FALSE(scheduler.isActive());
}

TEST_F(RemoteCommandRetrySchedulerTest, StartupFailsWhenSchedulerIsShutDown) {
    auto callback = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {
    };
    auto strategy = std::make_unique<NoRetryStrategy>();
    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(&getExecutor(), request, callback, std::move(strategy));
    ASSERT_FALSE(scheduler.isActive());

    scheduler.shutdown();

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, scheduler.startup());
    ASSERT_FALSE(scheduler.isActive());
}

TEST_F(RemoteCommandRetrySchedulerTest,
       ShuttingDownExecutorAfterSchedulerStartupInvokesCallbackWithCallbackCanceledError) {
    CallbackResponseSaver callback;
    auto strategy = std::make_unique<DefaultRetryStrategy>(10U);
    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, std::ref(callback), std::move(strategy));
    start(&scheduler);

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_EQUALS(request, net->getNextReadyRequest()->getRequest());
    }

    getExecutor().shutdown();

    runReadyNetworkOperations();
    checkCompletionStatus(
        &scheduler,
        callback,
        ResponseStatus::make_forTest(Status(ErrorCodes::CallbackCanceled, "executor shutdown")));
}

TEST_F(RemoteCommandRetrySchedulerTest,
       ShuttingDownSchedulerAfterSchedulerStartupInvokesCallbackWithCallbackCanceledError) {
    CallbackResponseSaver callback;
    auto strategy = std::make_unique<DefaultRetryStrategy>(10U);
    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, std::ref(callback), std::move(strategy));
    start(&scheduler);

    scheduler.shutdown();

    runReadyNetworkOperations();
    checkCompletionStatus(
        &scheduler,
        callback,
        ResponseStatus::make_forTest(Status(ErrorCodes::CallbackCanceled, "scheduler shutdown")));
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerInvokesCallbackOnFirstSuccessfulResponse) {
    CallbackResponseSaver callback;
    auto strategy = std::make_unique<DefaultRetryStrategy>(10U);
    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, std::ref(callback), std::move(strategy));
    start(&scheduler);

    // Elapsed time in response is ignored on successful responses.
    ResponseStatus response = ResponseStatus::make_forTest(
        BSON("ok" << 1 << "x" << 123 << "z" << 456), Milliseconds(100));

    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);

    // Scheduler cannot be restarted once it has run to completion.
    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, scheduler.startup());
    ASSERT_FALSE(scheduler.isActive());
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerIgnoresEmbeddedErrorInSuccessfulResponse) {
    CallbackResponseSaver callback;
    auto strategy = std::make_unique<DefaultRetryStrategy>(10U);
    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, std::ref(callback), std::move(strategy));
    start(&scheduler);

    // Scheduler does not parse document in a successful response for embedded errors.
    // This is the case with some commands (e.g. find) which do not always return errors using the
    // wire protocol.
    ResponseStatus response = ResponseStatus::make_forTest(
        BSON("ok" << 0 << "code" << int(ErrorCodes::FailedToParse) << "errmsg"
                  << "injected error"
                  << "z" << 456),
        Milliseconds(100));

    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerInvokesCallbackWithErrorFromExecutorIfScheduleRemoteCommandFailsOnRetry) {
    CallbackResponseSaver callback;
    auto strategy = std::make_unique<DefaultRetryStrategy>(3U);
    auto request = makeRemoteCommandRequest();
    auto badExecutor =
        std::make_shared<TaskExecutorWithFailureInScheduleRemoteCommand>(&getExecutor());
    RemoteCommandRetryScheduler scheduler(
        badExecutor.get(), request, std::ref(callback), std::move(strategy));
    start(&scheduler);

    processNetworkResponse(
        ResponseStatus::make_forTest(Status(ErrorCodes::HostNotFound, "first"), Milliseconds(0)));

    // scheduleRemoteCommand() will fail with ErrorCodes::ShutdownInProgress when trying to send
    // third remote command request after processing second failed response.
    badExecutor->scheduleRemoteCommandFailPoint = true;
    processNetworkResponse(
        ResponseStatus::make_forTest(Status(ErrorCodes::HostNotFound, "second"), Milliseconds(0)));

    checkCompletionStatus(
        &scheduler,
        callback,
        ResponseStatus::make_forTest(Status(ErrorCodes::ShutdownInProgress, ""), Milliseconds(0)));
}

TEST_F(
    RemoteCommandRetrySchedulerTest,
    SchedulerEnforcesStrategyMaximumAttemptsAndReturnsErrorOfLastFailedRequestErrorLabelNoDelay) {
    CallbackResponseSaver callback;

    auto strategy = std::make_unique<DefaultRetryStrategy>(2U);
    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, std::ref(callback), std::move(strategy));
    start(&scheduler);

    // RemoteCommandRetryScheduler checks a local error code for a retry and ignores the error code
    // and error labels in the body.
    BSONObj responseObj = BSON("code" << static_cast<int>(ErrorCodes::Overflow) << "errorLabels"
                                      << BSON_ARRAY("RetryableWriteError") << "codeName"
                                      << "Overflow");

    auto response1 = ResponseStatus::make_forTest(responseObj, Milliseconds(0));
    response1.status = Status(ErrorCodes::HostUnreachable, "first");
    processNetworkResponse(response1);

    auto response2 = ResponseStatus::make_forTest(responseObj, Milliseconds(0));
    response2.status = Status(ErrorCodes::HostUnreachable, "second");
    processNetworkResponse(response2);

    auto response3 = ResponseStatus::make_forTest(responseObj, Milliseconds(0));
    response3.status = Status(ErrorCodes::HostUnreachable, "last");
    processNetworkResponse(response3);

    checkCompletionStatus(&scheduler, callback, response3);
}

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerEnforcesStrategyMaximumAttemptsAndReturnsErrorOfLastFailedRequestErrorLabel) {
    CallbackResponseSaver callback;

    const int testBaseBackoffMillis = 200;
    auto strategy = std::make_unique<TestWithDelayRetryStrategy>(
        DefaultRetryStrategy(2U), Milliseconds(testBaseBackoffMillis));

    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, std::ref(callback), std::move(strategy));
    start(&scheduler);

    // RemoteCommandRetryScheduler checks a local error code for a retry and ignores the error code
    // and error labels in the body.
    BSONObj responseObj =
        BSON("code" << static_cast<int>(ErrorCodes::Overflow) << "errorLabels"
                    << BSON_ARRAY("SystemOverloadedError" << "RetryableWriteError") << "codeName"
                    << "Overflow");

    auto response1 = ResponseStatus::make_forTest(responseObj, Milliseconds(0));
    response1.status = Status(ErrorCodes::HostUnreachable, "first");
    processNetworkResponse(response1);

    auto response2 = ResponseStatus::make_forTest(responseObj, Milliseconds(0));
    response2.status = Status(ErrorCodes::HostUnreachable, "");
    processNetworkResponse(response2, Milliseconds(testBaseBackoffMillis));

    auto response3 = ResponseStatus::make_forTest(responseObj, Milliseconds(0));
    response3.status = Status(ErrorCodes::HostUnreachable, "");
    processNetworkResponse(response3, Milliseconds(testBaseBackoffMillis));

    checkCompletionStatus(&scheduler, callback, response3);
}

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerEnforcesStrategyMaximumAttemptsAndReturnsErrorOfLastFailedRequest) {
    CallbackResponseSaver callback;
    auto strategy = std::make_unique<DefaultRetryStrategy>(2U);
    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, std::ref(callback), std::move(strategy));
    start(&scheduler);

    processNetworkResponse(
        ResponseStatus::make_forTest(Status(ErrorCodes::HostNotFound, "first"), Milliseconds(0)));
    processNetworkResponse(ResponseStatus::make_forTest(
        Status(ErrorCodes::HostUnreachable, "second"), Milliseconds(0)));

    ResponseStatus response =
        ResponseStatus::make_forTest(Status(ErrorCodes::NetworkTimeout, "last"), Milliseconds(0));
    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerShouldRetryUntilSuccessfulResponseIsReceived) {
    CallbackResponseSaver callback;
    auto strategy = std::make_unique<DefaultRetryStrategy>(3U);
    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, std::ref(callback), std::move(strategy));
    start(&scheduler);

    processNetworkResponse(
        ResponseStatus::make_forTest(Status(ErrorCodes::HostNotFound, "first"), Milliseconds(0)));

    ResponseStatus response = ResponseStatus::make_forTest(
        BSON("ok" << 1 << "x" << 123 << "z" << 456), Milliseconds(100));
    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerReturnsCallbackCanceledIfShutdownBeforeSendingRetryCommand) {
    CallbackResponseSaver callback;
    auto strategy = std::make_unique<DefaultRetryStrategy>(
        [](Status, std::span<const std::string>) { return true; },
        DefaultRetryStrategy::getRetryParametersFromServerParameters());
    auto request = makeRemoteCommandRequest();
    auto badExecutor =
        std::make_shared<TaskExecutorWithFailureInScheduleRemoteCommand>(&getExecutor());
    RemoteCommandRetryScheduler scheduler(
        badExecutor.get(), request, std::ref(callback), std::move(strategy));
    start(&scheduler);

    FailPointEnableBlock fp{"shutdownBeforeSendingRetryCommand"};

    processNetworkResponse(
        ResponseStatus::make_forTest(Status(ErrorCodes::HostNotFound, "first"), Milliseconds(0)));

    checkCompletionStatus(
        &scheduler,
        callback,
        ResponseStatus::make_forTest(
            Status(ErrorCodes::CallbackCanceled, "scheduler was shut down before retrying command"),
            Milliseconds(0)));
}

bool sharedCallbackStateDestroyed = false;
class SharedCallbackState {
    SharedCallbackState(const SharedCallbackState&) = delete;
    SharedCallbackState& operator=(const SharedCallbackState&) = delete;

public:
    SharedCallbackState() {}
    ~SharedCallbackState() {
        sharedCallbackStateDestroyed = true;
    }
};

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerResetsOnCompletionCallbackFunctionAfterCompletion) {
    sharedCallbackStateDestroyed = false;
    auto sharedCallbackData = std::make_shared<SharedCallbackState>();

    Status result = getDetectableErrorStatus();
    auto strategy = std::make_unique<NoRetryStrategy>();
    auto request = makeRemoteCommandRequest();

    RemoteCommandRetryScheduler scheduler(
        &getExecutor(),
        request,
        [&result,
         sharedCallbackData](const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba) {
            LOGV2(20156, "Setting result", "result"_attr = rcba.response.status);
            result = rcba.response.status;
        },
        std::move(strategy));
    start(&scheduler);

    sharedCallbackData.reset();
    ASSERT_FALSE(sharedCallbackStateDestroyed);

    processNetworkResponse(ResponseStatus::make_forTest(
        Status(ErrorCodes::OperationFailed, "command failed"), Milliseconds(0)));

    scheduler.join();
    ASSERT_EQUALS(ErrorCodes::OperationFailed, result);
    ASSERT_TRUE(sharedCallbackStateDestroyed);
}

}  // namespace
