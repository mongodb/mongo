/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/bson/oid.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/remote_command_retry_policy.h"
#include "mongo/executor/remote_command_runner.h"
#include "mongo/executor/remote_command_runner_error_info.h"
#include "mongo/executor/remote_command_runner_test_fixture.h"
#include "mongo/executor/remote_command_targeter.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include <memory>

namespace mongo {
namespace executor {
namespace remote_command_runner {
namespace {
/*
 * Mock a successful network response to hello command.
 */
TEST_F(RemoteCommandRunnerTestFixture, SuccessfulHello) {
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<RemoteCommandLocalHostTargeter>();
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    auto opCtxHolder = makeOperationContext();
    SemiFuture<RemoteCommandRunnerResponse<HelloCommandReply>> resultFuture = doRequest(
        helloCmd, opCtxHolder.get(), std::move(targeter), getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return helloReply.toBSON();
    });

    RemoteCommandRunnerResponse res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), res.targetUsed);
}

/*
 * Tests that 'doRequest' will appropriately retry multiple times under the conditions defined by
 * the retry policy.
 */
TEST_F(RemoteCommandRunnerTestFixture, RetryOnSuccessfulHelloAdditionalAttempts) {
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<RemoteCommandLocalHostTargeter>();
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    // Define a retry policy that simply decides to always retry a command three additional times.
    std::shared_ptr<RemoteCommandTestRetryPolicy> testPolicy =
        std::make_shared<RemoteCommandTestRetryPolicy>();
    const auto maxNumRetries = 3;
    const auto retryDelay = Milliseconds(100);
    testPolicy->setMaxNumRetries(maxNumRetries);
    testPolicy->setRetryDelay(retryDelay);

    auto opCtxHolder = makeOperationContext();
    SemiFuture<RemoteCommandRunnerResponse<HelloCommandReply>> resultFuture =
        doRequest(helloCmd,
                  opCtxHolder.get(),
                  std::move(targeter),
                  getExecutorPtr(),
                  _cancellationToken,
                  testPolicy);

    const auto onCommandFunc = [&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return helloReply.toBSON();
    };
    // Schedule 1 request as the initial attempt, and then three following retries to satisfy the
    // condition for the runner to stop retrying.
    for (auto i = 0; i <= maxNumRetries; i++) {
        scheduleRequestAndAdvanceClockForRetry(testPolicy, onCommandFunc);
    }
    RemoteCommandRunnerResponse res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), res.targetUsed);
    ASSERT_EQ(maxNumRetries, testPolicy->getNumRetriesPerformed());
}

/*
 * Tests that 'doRequest' will not retry when the retry policy indicates accordingly.
 */
TEST_F(RemoteCommandRunnerTestFixture, DoNotRetryOnErrorAccordingToPolicy) {
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<RemoteCommandLocalHostTargeter>();
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    std::shared_ptr<RemoteCommandTestRetryPolicy> testPolicy =
        std::make_shared<RemoteCommandTestRetryPolicy>();
    const auto zeroRetries = 0;
    testPolicy->setMaxNumRetries(zeroRetries);

    auto opCtxHolder = makeOperationContext();
    SemiFuture<RemoteCommandRunnerResponse<HelloCommandReply>> resultFuture =
        doRequest(helloCmd,
                  opCtxHolder.get(),
                  std::move(targeter),
                  getExecutorPtr(),
                  _cancellationToken,
                  testPolicy);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return Status(ErrorCodes::NetworkTimeout, "mock");
    });

    auto error = resultFuture.getNoThrow().getStatus();

    // The error returned by our API should always be RemoteCommandExecutionError
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    ASSERT_EQ(zeroRetries, testPolicy->getNumRetriesPerformed());
}

/*
 * Mock error on local host side.
 */
TEST_F(RemoteCommandRunnerTestFixture, LocalError) {
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<RemoteCommandLocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    auto opCtxHolder = makeOperationContext();
    auto resultFuture = doRequest(
        helloCmd, opCtxHolder.get(), std::move(targeter), getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return Status(ErrorCodes::NetworkTimeout, "mock");
    });

    auto error = resultFuture.getNoThrow().getStatus();

    // The error returned by our API should always be RemoteCommandExecutionError
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    // Make sure we can extract the extra error info
    auto extraInfo = error.extraInfo<RemoteCommandExecutionErrorInfo>();
    ASSERT(extraInfo);
    // Make sure the extra info indicates the error was local, and that the
    // local error (which is just a Status) has the correct code.
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal().code(), ErrorCodes::NetworkTimeout);
}

/*
 * Mock error on remote host.
 */
TEST_F(RemoteCommandRunnerTestFixture, RemoteError) {
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<RemoteCommandLocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    auto opCtxHolder = makeOperationContext();
    auto resultFuture = doRequest(
        helloCmd, opCtxHolder.get(), std::move(targeter), getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return createErrorResponse(Status(ErrorCodes::BadValue, "mock"));
    });

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<RemoteCommandExecutionErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), Status(ErrorCodes::BadValue, "mock"));

    // No write concern or write errors expected
    ASSERT_EQ(remoteError.getRemoteCommandWriteConcernError(), Status::OK());
    ASSERT_EQ(remoteError.getRemoteCommandFirstWriteError(), Status::OK());
}

TEST_F(RemoteCommandRunnerTestFixture, SuccessfulFind) {
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<RemoteCommandLocalHostTargeter>();
    auto opCtxHolder = makeOperationContext();
    DatabaseName testDbName = DatabaseName("testdb", boost::none);
    NamespaceString nss(testDbName);

    FindCommandRequest findCmd(nss);
    auto resultFuture = doRequest(
        findCmd, opCtxHolder.get(), std::move(targeter), getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        // The BSON documents in this cursor response are created here.
        // When the remote_command_runner parses the response, it participates
        // in ownership of the underlying data, so it will participate in
        // owning the data in the cursor response.
        return CursorResponse(nss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    CursorInitialReply res = std::move(resultFuture).get().response;

    ASSERT_BSONOBJ_EQ(res.getCursor()->getFirstBatch()[0], BSON("x" << 1));
}

/*
 * Mock write concern error on remote host.
 */
TEST_F(RemoteCommandRunnerTestFixture, WriteConcernError) {
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<RemoteCommandLocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    const BSONObj writeConcernError = BSON("code" << ErrorCodes::WriteConcernFailed << "errmsg"
                                                  << "mock");
    const BSONObj resWithWriteConcernError =
        BSON("ok" << 1 << "writeConcernError" << writeConcernError);

    auto opCtxHolder = makeOperationContext();
    SemiFuture<RemoteCommandRunnerResponse<HelloCommandReply>> resultFuture = doRequest(
        helloCmd, opCtxHolder.get(), std::move(targeter), getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return resWithWriteConcernError;
    });

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<RemoteCommandExecutionErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandWriteConcernError(),
              Status(ErrorCodes::WriteConcernFailed, "mock"));

    // No top-level command or write errors expected
    ASSERT_EQ(remoteError.getRemoteCommandFirstWriteError(), Status::OK());
    ASSERT_EQ(remoteError.getRemoteCommandResult(), Status::OK());
}

/*
 * Mock write error on remote host.
 */
TEST_F(RemoteCommandRunnerTestFixture, WriteError) {
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<RemoteCommandLocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    const BSONObj writeErrorExtraInfo = BSON("failingDocumentId" << OID::gen());
    const BSONObj writeError = BSON("code" << ErrorCodes::DocumentValidationFailure << "errInfo"
                                           << writeErrorExtraInfo << "errmsg"
                                           << "Document failed validation");
    const BSONObj resWithWriteError = BSON("ok" << 1 << "writeErrors" << BSON_ARRAY(writeError));
    auto opCtxHolder = makeOperationContext();
    SemiFuture<RemoteCommandRunnerResponse<HelloCommandReply>> resultFuture = doRequest(
        helloCmd, opCtxHolder.get(), std::move(targeter), getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);

        return resWithWriteError;
    });
    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<RemoteCommandExecutionErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandFirstWriteError(),
              Status(ErrorCodes::DocumentValidationFailure,
                     "Document failed validation",
                     BSON("errInfo" << writeErrorExtraInfo)));

    // No top-level command or write errors expected
    ASSERT_EQ(remoteError.getRemoteCommandWriteConcernError(), Status::OK());
    ASSERT_EQ(remoteError.getRemoteCommandResult(), Status::OK());
}

// Ensure that the RCR correctly returns RemoteCommandExecutionError when the executor
// is shutdown mid-remote-invocation, and that the executor shutdown error is contained
// in the error's ExtraInfo.
TEST_F(RemoteCommandRunnerTestFixture, ExecutorShutdown) {
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<RemoteCommandLocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    auto opCtxHolder = makeOperationContext();
    SemiFuture<RemoteCommandRunnerResponse<HelloCommandReply>> resultFuture = doRequest(
        helloCmd, opCtxHolder.get(), std::move(targeter), getExecutorPtr(), _cancellationToken);
    getExecutorPtr()->shutdown();
    auto error = resultFuture.getNoThrow().getStatus();
    // The error returned by our API should always be RemoteCommandExecutionError
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    // Make sure we can extract the extra error info
    auto extraInfo = error.extraInfo<RemoteCommandExecutionErrorInfo>();
    ASSERT(extraInfo);
    // Make sure the extra info indicates the error was local, and that the
    // local error (which is just a Status) has the correct code.
    ASSERT(extraInfo->isLocal());
    ASSERT(ErrorCodes::isA<ErrorCategory::CancellationError>(extraInfo->asLocal()));
}

/*
 * Basic Targeter that returns the host that invoked it.
 */
TEST_F(RemoteCommandRunnerTestFixture, LocalTargeter) {
    RemoteCommandLocalHostTargeter t;
    auto targetFuture = t.resolve(_cancellationToken);
    auto target = targetFuture.get();

    ASSERT_EQ(target.size(), 1);
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), target[0]);
}

/*
 * Basic Targeter that wraps a single HostAndPort.
 */
TEST_F(RemoteCommandRunnerTestFixture, HostAndPortTargeter) {
    RemoteCommandFixedTargeter t{HostAndPort("FakeHost1", 12345)};
    auto targetFuture = t.resolve(_cancellationToken);
    auto target = targetFuture.get();

    ASSERT_EQ(target.size(), 1);
    ASSERT_EQ(HostAndPort("FakeHost1", 12345), target[0]);
}

/*
 * Basic RetryPolicy that never retries.
 */
TEST_F(RemoteCommandRunnerTestFixture, NoRetry) {
    RemoteCommandNoRetryPolicy p;

    ASSERT_FALSE(p.recordAndEvaluateRetry(Status(ErrorCodes::BadValue, "mock")));
    ASSERT_EQUALS(p.getNextRetryDelay(), Milliseconds::zero());
}

// TODO SERVER-69634: Remove this test.
TEST_F(RemoteCommandRunnerTestFixture, ParseAndSeralizeNoop) {
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<RemoteCommandLocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    auto opCtxHolder = makeOperationContext();
    auto resultFuture = doRequest(
        helloCmd, opCtxHolder.get(), std::move(targeter), getExecutorPtr(), _cancellationToken);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return Status(ErrorCodes::NetworkTimeout, "mock");
    });

    // Check that RemoteCommandExecutionErrorInfo::serialize() works safely (when converting a
    // Status to string), instead of crashing the server.
    try {
        auto error = resultFuture.get();
    } catch (const DBException& ex) {
        ASSERT_EQ(ex.toStatus(), ErrorCodes::RemoteCommandExecutionError);
        ASSERT_FALSE(ex.toString().empty());
    }

    // Check that RemoteCommandExecutionErrorInfo::parse() safely creates a dummy ErrorExtraInfo
    // (when a Status is constructed), instead of crashing the server.
    const auto status = Status(ErrorCodes::RemoteCommandExecutionError, "", fromjson("{foo: 123}"));
    ASSERT_EQ(status, ErrorCodes::RemoteCommandExecutionError);
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<RemoteCommandExecutionErrorInfo>());
    ASSERT_EQ(status.extraInfo<RemoteCommandExecutionErrorInfo>()->asLocal(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(
        status.extraInfo<RemoteCommandExecutionErrorInfo>()->asLocal().toString(),
        "RemoteCommandExectionError illegally parsed from bson");
}

}  // namespace
}  // namespace remote_command_runner
}  // namespace executor
}  // namespace mongo
