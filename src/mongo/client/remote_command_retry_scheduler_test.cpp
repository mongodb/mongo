/**
 *    Copyright 2016 MongoDB Inc.
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

#include <memory>
#include <string>
#include <vector>

#include "mongo/base/disallow_copying.h"
#include "mongo/base/status_with.h"
#include "mongo/client/remote_command_retry_scheduler.h"
#include "mongo/db/jsobj.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/task_executor_proxy.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/hostandport.h"

namespace {

using namespace mongo;

class CallbackResponseSaver;

class RemoteCommandRetrySchedulerTest : public executor::ThreadPoolExecutorTest {
public:
    void start(RemoteCommandRetryScheduler* scheduler);
    void checkCompletionStatus(RemoteCommandRetryScheduler* scheduler,
                               const CallbackResponseSaver& callbackResponseSaver,
                               const executor::TaskExecutor::ResponseStatus& response);
    void processNetworkResponse(const executor::TaskExecutor::ResponseStatus& response);
    void runReadyNetworkOperations();

protected:
    void setUp() override;
    void tearDown() override;
};

class CallbackResponseSaver {
    MONGO_DISALLOW_COPYING(CallbackResponseSaver);

public:
    CallbackResponseSaver();

    /**
     * Use this for scheduler callback.
     */
    void operator()(const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba);

    std::vector<StatusWith<executor::RemoteCommandResponse>> getResponses() const;

private:
    std::vector<StatusWith<executor::RemoteCommandResponse>> _responses;
};

/**
 * Task executor proxy with fail point for scheduleRemoteCommand().
 */
class TaskExecutorWithFailureInScheduleRemoteCommand : public unittest::TaskExecutorProxy {
public:
    TaskExecutorWithFailureInScheduleRemoteCommand(executor::TaskExecutor* executor)
        : unittest::TaskExecutorProxy(executor) {}
    virtual StatusWith<executor::TaskExecutor::CallbackHandle> scheduleRemoteCommand(
        const executor::RemoteCommandRequest& request, const RemoteCommandCallbackFn& cb) override {
        if (scheduleRemoteCommandFailPoint) {
            return Status(ErrorCodes::ShutdownInProgress,
                          "failed to send remote command - shutdown in progress");
        }
        return getExecutor()->scheduleRemoteCommand(request, cb);
    }

    bool scheduleRemoteCommandFailPoint = false;
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
    const executor::TaskExecutor::ResponseStatus& response) {
    ASSERT_FALSE(scheduler->isActive());
    auto responses = callbackResponseSaver.getResponses();
    ASSERT_EQUALS(1U, responses.size());
    if (response.isOK()) {
        ASSERT_OK(responses.front().getStatus());
        ASSERT_EQUALS(response.getValue(), responses.front().getValue());
    } else {
        ASSERT_EQUALS(response.getStatus(), responses.front().getStatus());
    }
}

void RemoteCommandRetrySchedulerTest::processNetworkResponse(
    const executor::TaskExecutor::ResponseStatus& response) {
    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    ASSERT_TRUE(net->hasReadyRequests());
    auto noi = net->getNextReadyRequest();
    net->scheduleResponse(noi, net->now(), response);
    net->runReadyNetworkOperations();
}

void RemoteCommandRetrySchedulerTest::runReadyNetworkOperations() {
    auto net = getNet();
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    net->runReadyNetworkOperations();
}

void RemoteCommandRetrySchedulerTest::setUp() {
    executor::ThreadPoolExecutorTest::setUp();
    launchExecutorThread();
}

void RemoteCommandRetrySchedulerTest::tearDown() {
    executor::ThreadPoolExecutorTest::tearDown();
}

CallbackResponseSaver::CallbackResponseSaver() = default;

void CallbackResponseSaver::operator()(
    const executor::TaskExecutor::RemoteCommandCallbackArgs& rcba) {
    _responses.push_back(rcba.response);
}

std::vector<StatusWith<executor::RemoteCommandResponse>> CallbackResponseSaver::getResponses()
    const {
    return _responses;
}

const executor::RemoteCommandRequest request(HostAndPort("h1:12345"), "db1", BSON("ping" << 1));

TEST_F(RemoteCommandRetrySchedulerTest, MakeSingleShotRetryPolicy) {
    auto policy = RemoteCommandRetryScheduler::makeNoRetryPolicy();
    ASSERT_TRUE(policy);
    ASSERT_EQUALS(1U, policy->getMaximumAttempts());
    ASSERT_EQUALS(executor::RemoteCommandRequest::kNoTimeout,
                  policy->getMaximumResponseElapsedTotal());
    // Doesn't matter what "shouldRetryOnError()" returns since we won't be retrying the remote
    // command.
    for (int i = 0; i < int(ErrorCodes::MaxError); ++i) {
        auto error = ErrorCodes::fromInt(i);
        ASSERT_FALSE(policy->shouldRetryOnError(error));
    }
}

TEST_F(RemoteCommandRetrySchedulerTest, MakeRetryPolicy) {
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        5U,
        Milliseconds(100),
        {ErrorCodes::FailedToParse, ErrorCodes::InvalidNamespace, ErrorCodes::InternalError});
    ASSERT_EQUALS(5U, policy->getMaximumAttempts());
    ASSERT_EQUALS(Milliseconds(100), policy->getMaximumResponseElapsedTotal());
    for (int i = 0; i < int(ErrorCodes::MaxError); ++i) {
        auto error = ErrorCodes::fromInt(i);
        if (error == ErrorCodes::InternalError || error == ErrorCodes::FailedToParse ||
            error == ErrorCodes::InvalidNamespace) {
            ASSERT_TRUE(policy->shouldRetryOnError(error));
            continue;
        }
        ASSERT_FALSE(policy->shouldRetryOnError(error));
    }
}

TEST_F(RemoteCommandRetrySchedulerTest, InvalidConstruction) {
    auto callback = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {};
    auto makeRetryPolicy = [] { return RemoteCommandRetryScheduler::makeNoRetryPolicy(); };

    // Null executor.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(nullptr, request, callback, makeRetryPolicy()),
        UserException,
        ErrorCodes::BadValue,
        "task executor cannot be null");

    // Empty source in remote command request.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            executor::RemoteCommandRequest(HostAndPort(), request.dbname, request.cmdObj),
            callback,
            makeRetryPolicy()),
        UserException,
        ErrorCodes::BadValue,
        "source in remote command request cannot be empty");

    // Empty source in remote command request.
    ASSERT_THROWS_CODE_AND_WHAT(RemoteCommandRetryScheduler(&getExecutor(),
                                                            executor::RemoteCommandRequest(
                                                                request.target, "", request.cmdObj),
                                                            callback,
                                                            makeRetryPolicy()),
                                UserException,
                                ErrorCodes::BadValue,
                                "database name in remote command request cannot be empty");

    // Empty command object in remote command request.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            executor::RemoteCommandRequest(request.target, request.dbname, BSONObj()),
            callback,
            makeRetryPolicy()),
        UserException,
        ErrorCodes::BadValue,
        "command object in remote command request cannot be empty");

    // Null remote command callback function.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(&getExecutor(),
                                    request,
                                    executor::TaskExecutor::RemoteCommandCallbackFn(),
                                    makeRetryPolicy()),
        UserException,
        ErrorCodes::BadValue,
        "remote command callback function cannot be null");

    // Null retry policy.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(&getExecutor(),
                                    request,
                                    callback,
                                    std::unique_ptr<RemoteCommandRetryScheduler::RetryPolicy>()),
        UserException,
        ErrorCodes::BadValue,
        "retry policy cannot be null");

    // Policy max attempts should be positive.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            request,
            callback,
            RemoteCommandRetryScheduler::makeRetryPolicy(0, Milliseconds(100), {})),
        UserException,
        ErrorCodes::BadValue,
        "policy max attempts cannot be zero");

    // Policy max response elapsed total cannot be negative.
    ASSERT_THROWS_CODE_AND_WHAT(
        RemoteCommandRetryScheduler(
            &getExecutor(),
            request,
            callback,
            RemoteCommandRetryScheduler::makeRetryPolicy(1U, Milliseconds(-100), {})),
        UserException,
        ErrorCodes::BadValue,
        "policy max response elapsed total cannot be negative");
}

TEST_F(RemoteCommandRetrySchedulerTest, StartupFailsWhenExecutorIsShutDown) {
    auto callback = [](const executor::TaskExecutor::RemoteCommandCallbackArgs&) {};
    auto policy = RemoteCommandRetryScheduler::makeNoRetryPolicy();

    RemoteCommandRetryScheduler scheduler(&getExecutor(), request, callback, std::move(policy));
    ASSERT_FALSE(scheduler.isActive());

    getExecutor().shutdown();

    ASSERT_EQUALS(ErrorCodes::ShutdownInProgress, scheduler.startup());
    ASSERT_FALSE(scheduler.isActive());
}

TEST_F(RemoteCommandRetrySchedulerTest,
       ShuttingDownExecutorAfterSchedulerStartupInvokesCallbackWithCallbackCanceledError) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        10U, Milliseconds(1), {ErrorCodes::HostNotFound});
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    auto net = getNet();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        ASSERT_EQUALS(request, net->getNextReadyRequest()->getRequest());
    }

    getExecutor().shutdown();

    runReadyNetworkOperations();
    checkCompletionStatus(
        &scheduler, callback, {ErrorCodes::CallbackCanceled, "executor shutdown"});
}


TEST_F(RemoteCommandRetrySchedulerTest,
       ShuttingDownSchedulerAfterSchedulerStartupInvokesCallbackWithCallbackCanceledError) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        10U, Milliseconds(1), {ErrorCodes::HostNotFound});
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    scheduler.shutdown();

    runReadyNetworkOperations();
    checkCompletionStatus(
        &scheduler, callback, {ErrorCodes::CallbackCanceled, "scheduler shutdown"});
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerInvokesCallbackOnNonRetryableErrorInResponse) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        10U, Milliseconds(1), RemoteCommandRetryScheduler::kNotMasterErrors);
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    // This should match one of the non-retryable error codes in the policy.
    Status response(ErrorCodes::OperationFailed, "injected error");

    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerInvokesCallbackOnFirstSuccessfulResponse) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        10U, Milliseconds(1), {ErrorCodes::HostNotFound});
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    // Elapsed time in response is ignored on successful responses.
    executor::RemoteCommandResponse response(
        BSON("ok" << 1 << "x" << 123), BSON("z" << 456), Milliseconds(100));

    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerIgnoresEmbeddedErrorInSuccessfulResponse) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        10U, Milliseconds(1), {ErrorCodes::HostNotFound});
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    // Scheduler does not parse document in a successful response for embedded errors.
    // This is the case with some commands (e.g. find) which do not always return errors using the
    // wire protocol.
    executor::RemoteCommandResponse response(
        BSON("ok" << 0 << "code" << int(ErrorCodes::FailedToParse) << "errmsg"
                  << "injected error"),
        BSON("z" << 456),
        Milliseconds(100));

    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerInvokesCallbackWithErrorFromExecutorIfScheduleRemoteCommandFailsOnRetry) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        3U, executor::RemoteCommandRequest::kNoTimeout, {ErrorCodes::HostNotFound});
    TaskExecutorWithFailureInScheduleRemoteCommand badExecutor(&getExecutor());
    RemoteCommandRetryScheduler scheduler(
        &badExecutor, request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    processNetworkResponse({ErrorCodes::HostNotFound, "first"});

    // scheduleRemoteCommand() will fail with ErrorCodes::ShutdownInProgress when trying to send
    // third remote command request after processing second failed response.
    badExecutor.scheduleRemoteCommandFailPoint = true;
    processNetworkResponse({ErrorCodes::HostNotFound, "second"});

    checkCompletionStatus(&scheduler, callback, {ErrorCodes::ShutdownInProgress, ""});
}

TEST_F(RemoteCommandRetrySchedulerTest,
       SchedulerEnforcesPolicyMaximumAttemptsAndReturnsErrorOfLastFailedRequest) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        3U,
        executor::RemoteCommandRequest::kNoTimeout,
        RemoteCommandRetryScheduler::kAllRetriableErrors);
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    processNetworkResponse({ErrorCodes::HostNotFound, "first"});
    processNetworkResponse({ErrorCodes::HostUnreachable, "second"});

    Status response(ErrorCodes::NetworkTimeout, "last");
    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

TEST_F(RemoteCommandRetrySchedulerTest, SchedulerShouldRetryUntilSuccessfulResponseIsReceived) {
    CallbackResponseSaver callback;
    auto policy = RemoteCommandRetryScheduler::makeRetryPolicy(
        3U, executor::RemoteCommandRequest::kNoTimeout, {ErrorCodes::HostNotFound});
    RemoteCommandRetryScheduler scheduler(
        &getExecutor(), request, stdx::ref(callback), std::move(policy));
    start(&scheduler);

    processNetworkResponse({ErrorCodes::HostNotFound, "first"});

    executor::RemoteCommandResponse response(
        BSON("ok" << 1 << "x" << 123), BSON("z" << 456), Milliseconds(100));
    processNetworkResponse(response);
    checkCompletionStatus(&scheduler, callback, response);
}

}  // namespace
