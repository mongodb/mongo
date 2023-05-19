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
#include "mongo/client/connection_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_error_info.h"
#include "mongo/executor/async_rpc_retry_policy.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/async_rpc_test_fixture.h"
#include "mongo/executor/async_transaction_rpc.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_test_fixture.h"
#include "mongo/executor/thread_pool_task_executor.h"
#include "mongo/executor/thread_pool_task_executor_test_fixture.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include <memory>

#include "mongo/logv2/log.h"
#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo {
const HostAndPort kTestConfigShardHost = HostAndPort("FakeConfigHost", 12345);
const std::vector<ShardId> kTestShardIds = {
    ShardId("FakeShard1"), ShardId("FakeShard2"), ShardId("FakeShard3")};
const std::vector<HostAndPort> kTestShardHosts = {HostAndPort("FakeShard1Host", 12345),
                                                  HostAndPort("FakeShard2Host", 12345),
                                                  HostAndPort("FakeShard3Host", 12345)};
const HostAndPort kTestStandaloneHost = {HostAndPort("FakeStandalone1Host", 12345)};

namespace async_rpc {
namespace {
/*
 * Mock a successful network response to hello command.
 */
TEST_F(AsyncRPCTestFixture, SuccessfulHello) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> resultFuture =
        sendCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return helloReply.toBSON();
    });

    AsyncRPCResponse res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), res.targetUsed);
}

/*
 * Test URI overload version of 'sendCommand'.
 */
TEST_F(AsyncRPCTestFixture, URIOverload) {
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    MongoURI uri = MongoURI::parse("mongodb://" + kTestStandaloneHost.toString()).getValue();

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> resultFuture =
        sendCommand(options, opCtxHolder.get(), uri);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(kTestStandaloneHost, request.target);
        return helloReply.toBSON();
    });

    AsyncRPCResponse res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(kTestStandaloneHost, res.targetUsed);
}

/*
 * Test ConnectionString overload version of 'sendCommand'.
 */
TEST_F(AsyncRPCTestFixture, ConnectionStringOverload) {
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    std::vector<HostAndPort> hosts;
    hosts.push_back(kTestStandaloneHost);
    ConnectionString cstr = ConnectionString::forStandalones(hosts);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> resultFuture =
        sendCommand(options, opCtxHolder.get(), cstr);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(kTestStandaloneHost, request.target);
        return helloReply.toBSON();
    });

    AsyncRPCResponse res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(kTestStandaloneHost, res.targetUsed);
}

/*
 * Verify that generic command arguments are correctly serialized into the BSON builder of a
 * command, and that generic reply fields are correctly parsed from the network response.
 */
TEST_F(AsyncRPCTestFixture, SuccessfulHelloWithGenericFields) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    // Populate structs for generic arguments to be passed along when the command is converted
    // to BSON.
    GenericArgsAPIV1 genericArgsApiV1;
    GenericArgsAPIV1Unstable genericArgsUnstable;

    genericArgsApiV1.setAutocommit(false);
    const UUID clientOpKey = UUID::gen();
    genericArgsApiV1.setClientOperationKey(clientOpKey);

    // Populate structs for generic reply fields that are expected to be parsed from the
    // response object.
    GenericReplyFieldsWithTypesV1 genericReplyApiV1;
    GenericReplyFieldsWithTypesUnstableV1 genericReplyUnstable;
    genericReplyUnstable.setOk(1);
    genericReplyUnstable.setDollarConfigTime(Timestamp(1, 1));
    const LogicalTime clusterTime = LogicalTime(Timestamp(2, 3));
    genericReplyApiV1.setDollarClusterTime(clusterTime);
    auto configTime = Timestamp(1, 1);
    genericArgsUnstable.setDollarConfigTime(configTime);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd,
        getExecutorPtr(),
        _cancellationToken,
        std::make_shared<NeverRetryPolicy>(),
        GenericArgs(genericArgsApiV1, genericArgsUnstable));
    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> resultFuture =
        sendCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);

        // Confirm that the generic arguments are present in the BSON command object.
        ASSERT_EQ(request.cmdObj["autocommit"].booleanSafe(), false);
        ASSERT_EQ(UUID::fromCDR(request.cmdObj["clientOperationKey"].uuid()), clientOpKey);
        ASSERT_EQ(request.cmdObj["$configTime"].timestamp(), configTime);

        // Append generic reply fields to the reply object.
        BSONObjBuilder reply = BSONObjBuilder(helloReply.toBSON());
        reply.append("ok", 1);
        reply.append("$configTime", Timestamp(1, 1));
        clusterTime.serializeToBSON("$clusterTime", &reply);

        return reply.obj();
    });

    AsyncRPCResponse res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), res.targetUsed);
    ASSERT_BSONOBJ_EQ(genericReplyApiV1.toBSON(), res.genericReplyFields.stable.toBSON());
    ASSERT_BSONOBJ_EQ(genericReplyUnstable.toBSON(), res.genericReplyFields.unstable.toBSON());
}

/*
 * Tests that 'sendCommand' will appropriately retry multiple times under the conditions defined by
 * the retry policy.
 */
TEST_F(AsyncRPCTestFixture, RetryOnSuccessfulHelloAdditionalAttempts) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    // Define a retry policy that simply decides to always retry a command three additional times.
    std::shared_ptr<TestRetryPolicy> testPolicy = std::make_shared<TestRetryPolicy>();
    const auto maxNumRetries = 3;
    const auto retryDelay = Milliseconds(100);
    testPolicy->setMaxNumRetries(maxNumRetries);
    testPolicy->pushRetryDelay(retryDelay);
    testPolicy->pushRetryDelay(retryDelay);
    testPolicy->pushRetryDelay(retryDelay);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken, testPolicy);
    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> resultFuture =
        sendCommand(options, opCtxHolder.get(), std::move(targeter));

    const auto onCommandFunc = [&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return helloReply.toBSON();
    };
    // Schedule 1 request as the initial attempt, and then three following retries to satisfy the
    // condition for the runner to stop retrying.
    for (auto i = 0; i <= maxNumRetries; i++) {
        scheduleRequestAndAdvanceClockForRetry(testPolicy, onCommandFunc, retryDelay);
    }
    AsyncRPCResponse res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), res.targetUsed);
    ASSERT_EQ(maxNumRetries, testPolicy->getNumRetriesPerformed());
}

/*
 * Tests that 'sendCommand' will appropriately retry multiple times under the conditions defined by
 * the retry policy, with a dynmically changing wait-time between retries.
 */
TEST_F(AsyncRPCTestFixture, DynamicDelayBetweenRetries) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    // Define a retry policy that simply decides to always retry a command three additional times.
    std::shared_ptr<TestRetryPolicy> testPolicy = std::make_shared<TestRetryPolicy>();
    const auto maxNumRetries = 3;
    const std::array<Milliseconds, maxNumRetries> retryDelays{
        Milliseconds(100), Milliseconds(50), Milliseconds(10)};
    testPolicy->setMaxNumRetries(maxNumRetries);
    testPolicy->pushRetryDelay(retryDelays[0]);
    testPolicy->pushRetryDelay(retryDelays[1]);
    testPolicy->pushRetryDelay(retryDelays[2]);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken, testPolicy);
    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> resultFuture =
        sendCommand(options, opCtxHolder.get(), std::move(targeter));

    const auto onCommandFunc = [&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return helloReply.toBSON();
    };

    // Schedule 1 response to the initial attempt, and then two for the following retries.
    // Advance the clock appropriately based on each retry delay.
    for (auto i = 0; i < maxNumRetries; i++) {
        scheduleRequestAndAdvanceClockForRetry(testPolicy, onCommandFunc, retryDelays[i]);
    }
    // Schedule a response to the final retry. No need to advance clock since no more
    // retries should be attemped after this third one.
    onCommand(onCommandFunc);

    AsyncRPCResponse res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), res.targetUsed);
    ASSERT_EQ(maxNumRetries, testPolicy->getNumRetriesPerformed());
}

/*
 * Tests that 'sendCommand' will not retry when the retry policy indicates accordingly.
 */
TEST_F(AsyncRPCTestFixture, DoNotRetryOnErrorAccordingToPolicy) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    std::shared_ptr<TestRetryPolicy> testPolicy = std::make_shared<TestRetryPolicy>();
    const auto zeroRetries = 0;
    testPolicy->setMaxNumRetries(zeroRetries);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken, testPolicy);
    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> resultFuture =
        sendCommand(options, opCtxHolder.get(), std::move(targeter));

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
TEST_F(AsyncRPCTestFixture, LocalError) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return Status(ErrorCodes::NetworkTimeout, "mock");
    });

    auto error = resultFuture.getNoThrow().getStatus();

    // The error returned by our API should always be RemoteCommandExecutionError
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    // Make sure we can extract the extra error info
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    // Make sure the extra info indicates the error was local, and that the
    // local error (which is just a Status) has the correct code.
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal().code(), ErrorCodes::NetworkTimeout);
}

/*
 * Mock error on remote host.
 */
TEST_F(AsyncRPCTestFixture, RemoteError) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return createErrorResponse(Status(ErrorCodes::BadValue, "mock"));
    });

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), Status(ErrorCodes::BadValue, "mock"));

    // No write concern or write errors expected
    ASSERT_EQ(remoteError.getRemoteCommandWriteConcernError(), Status::OK());
    ASSERT_EQ(remoteError.getRemoteCommandFirstWriteError(), Status::OK());
}

/*
 * Test that remote errors with generic reply fields are properly parsed.
 */
TEST_F(AsyncRPCTestFixture, RemoteErrorWithGenericReplyFields) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    GenericReplyFieldsWithTypesV1 stableFields;
    stableFields.setDollarClusterTime(LogicalTime(Timestamp(2, 3)));
    GenericReplyFieldsWithTypesUnstableV1 unstableFields;
    unstableFields.setDollarConfigTime(Timestamp(1, 1));
    unstableFields.setOk(false);

    onCommand([&, stableFields, unstableFields](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        auto remoteErrorBson = createErrorResponse(Status(ErrorCodes::BadValue, "mock"));
        auto ret = remoteErrorBson.addFields(stableFields.toBSON());
        return ret.addFields(unstableFields.toBSON());
    });

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), Status(ErrorCodes::BadValue, "mock"));

    // No write concern or write errors expected
    ASSERT_EQ(Status::OK(), remoteError.getRemoteCommandWriteConcernError());
    ASSERT_EQ(Status::OK(), remoteError.getRemoteCommandFirstWriteError());

    // Check generic reply fields.
    auto replyFields = remoteError.getGenericReplyFields();
    ASSERT_BSONOBJ_EQ(stableFields.toBSON(), replyFields.stable.toBSON());
    ASSERT_BSONOBJ_EQ(unstableFields.toBSON(), replyFields.unstable.toBSON());
}

TEST_F(AsyncRPCTestFixture, SuccessfulFind) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    auto opCtxHolder = makeOperationContext();
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        findCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT(!request.cmdObj["startTransaction"]);
        ASSERT(!request.cmdObj["coordinator"]);
        ASSERT(!request.cmdObj["autocommit"]);
        ASSERT(!request.cmdObj["txnNumber"]);
        // The BSON documents in this cursor response are created here.
        // When async_rpc::sendCommand parses the response, it participates
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
TEST_F(AsyncRPCTestFixture, WriteConcernError) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    const BSONObj writeConcernError = BSON("code" << ErrorCodes::WriteConcernFailed << "errmsg"
                                                  << "mock");
    const BSONObj resWithWriteConcernError =
        BSON("ok" << 1 << "writeConcernError" << writeConcernError);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> resultFuture =
        sendCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return resWithWriteConcernError;
    });

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
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
TEST_F(AsyncRPCTestFixture, WriteError) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    const BSONObj writeErrorExtraInfo = BSON("failingDocumentId" << OID::gen());
    const BSONObj writeError = BSON("code" << ErrorCodes::DocumentValidationFailure << "errInfo"
                                           << writeErrorExtraInfo << "errmsg"
                                           << "Document failed validation");
    const BSONObj resWithWriteError = BSON("ok" << 1 << "writeErrors" << BSON_ARRAY(writeError));
    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> resultFuture =
        sendCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);

        return resWithWriteError;
    });
    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
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
TEST_F(AsyncRPCTestFixture, ExecutorShutdown) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));
    getExecutorPtr()->shutdown();
    auto error = resultFuture.getNoThrow().getStatus();
    // The error returned by our API should always be RemoteCommandExecutionError
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    // Make sure we can extract the extra error info
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    // Make sure the extra info indicates the error was local, and that the
    // local error (which is just a Status) has the correct code.
    ASSERT(extraInfo->isLocal());
    ASSERT(ErrorCodes::isA<ErrorCategory::CancellationError>(extraInfo->asLocal()));
}

TEST_F(AsyncRPCTestFixture, BatonTest) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    auto retryPolicy = std::make_shared<NeverRetryPolicy>();
    HelloCommand helloCmd;
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    initializeCommand(helloCmd);
    auto opCtxHolder = makeOperationContext();
    auto baton = opCtxHolder->getBaton();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    options->baton = baton;
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    Notification<void> seenNetworkRequest;
    unittest::ThreadAssertionMonitor monitor;
    // This thread will respond to the request we sent via sendCommand above.
    auto networkResponder = monitor.spawn([&] {
        onCommand([&](const auto& request) {
            ASSERT(request.cmdObj["hello"]);
            seenNetworkRequest.set();
            monitor.notifyDone();
            return helloReply.toBSON();
        });
    });
    // Wait on the opCtx until networkResponder has observed the network request.
    // While we block on the opCtx, the current thread should run jobs scheduled
    // on the baton, including enqueuing the network request via `sendCommand` above.
    seenNetworkRequest.get(opCtxHolder.get());

    networkResponder.join();
    // Wait on the opCtx again to allow the current thread, via the baton, to propogate
    // the network response up into the resultFuture.
    AsyncRPCResponse res = resultFuture.get(opCtxHolder.get());

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), res.targetUsed);
}

/*
 * Basic Targeter that returns the host that invoked it.
 */
TEST_F(AsyncRPCTestFixture, LocalTargeter) {
    LocalHostTargeter t;
    auto targetFuture = t.resolve(_cancellationToken);
    auto target = targetFuture.get();

    ASSERT_EQ(target.size(), 1);
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), target[0]);
}

/*
 * Basic Targeter that wraps a single HostAndPort.
 */
TEST_F(AsyncRPCTestFixture, HostAndPortTargeter) {
    FixedTargeter t{HostAndPort("FakeHost1", 12345)};
    auto targetFuture = t.resolve(_cancellationToken);
    auto target = targetFuture.get();

    ASSERT_EQ(target.size(), 1);
    ASSERT_EQ(HostAndPort("FakeHost1", 12345), target[0]);
}

/*
 * Basic RetryPolicy that never retries.
 */
TEST_F(AsyncRPCTestFixture, NoRetry) {
    NeverRetryPolicy p;

    ASSERT_FALSE(p.recordAndEvaluateRetry(Status(ErrorCodes::BadValue, "mock")));
    ASSERT_EQUALS(p.getNextRetryDelay(), Milliseconds::zero());
}

TEST_F(AsyncRPCTestFixture, ParseAndSeralizeNoop) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        return Status(ErrorCodes::NetworkTimeout, "mock");
    });

    // Check that AsyncRPCErrorInfo::serialize() works safely (when converting a
    // Status to string), instead of crashing the server.
    try {
        auto error = resultFuture.get();
    } catch (const DBException& ex) {
        ASSERT_EQ(ex.toStatus(), ErrorCodes::RemoteCommandExecutionError);
        ASSERT_FALSE(ex.toString().empty());
    }

    // Check that AsyncRPCErrorInfo::parse() safely creates a dummy ErrorExtraInfo
    // (when a Status is constructed), instead of crashing the server.
    const auto status = Status(ErrorCodes::RemoteCommandExecutionError, "", fromjson("{foo: 123}"));
    ASSERT_EQ(status, ErrorCodes::RemoteCommandExecutionError);
    ASSERT(status.extraInfo());
    ASSERT(status.extraInfo<AsyncRPCErrorInfo>());
    ASSERT_EQ(status.extraInfo<AsyncRPCErrorInfo>()->asLocal(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.extraInfo<AsyncRPCErrorInfo>()->asLocal().toString(),
                           "RemoteCommandExectionError illegally parsed from bson");
}

/**
 * When the targeter returns an error, ensure we rewrite it correctly.
 */
TEST_F(AsyncRPCTestFixture, FailedTargeting) {
    auto targeterFailStatus = Status{ErrorCodes::InternalError, "Fake targeter failure"};
    auto targeter = std::make_unique<FailingTargeter>(targeterFailStatus);
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    auto error = resultFuture.getNoThrow().getStatus();
    // The error returned by our API should always be RemoteCommandExecutionError
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    // Make sure we can extract the extra error info
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    // Make sure the extra info indicates the error was local, and that the
    // local error (which is just a Status) has the correct code.
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal(), targeterFailStatus);
}
TEST_F(AsyncRPCTestFixture, BatonShutdownExecutorAlive) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    auto retryPolicy = std::make_shared<TestRetryPolicy>();
    const auto maxNumRetries = 5;
    const auto retryDelay = Milliseconds(10);
    retryPolicy->setMaxNumRetries(maxNumRetries);
    for (int i = 0; i < maxNumRetries; ++i)
        retryPolicy->pushRetryDelay(retryDelay);
    HelloCommand helloCmd;
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    initializeCommand(helloCmd);
    auto opCtxHolder = makeOperationContext();
    auto subBaton = opCtxHolder->getBaton()->makeSubBaton();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    options->baton = *subBaton;
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    subBaton.shutdown();

    auto error = resultFuture.getNoThrow().getStatus();
    auto expectedDetachError = Status(ErrorCodes::ShutdownInProgress, "Baton detached");
    auto expectedOuterReason = "Remote command execution failed due to executor shutdown";

    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    ASSERT_EQ(error.reason(), expectedOuterReason);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isLocal());
    auto localError = extraInfo->asLocal();
    ASSERT_EQ(localError, expectedDetachError);

    ASSERT_EQ(0, retryPolicy->getNumRetriesPerformed());
}

TEST_F(AsyncRPCTestFixture, SendTxnCommandWithoutTxnRouterAppendsNoTxnFields) {
    ShardId shardId("shard");
    ReadPreferenceSetting readPref;
    std::vector<HostAndPort> testHost = {kTestShardHosts[0]};
    // Use a mock ShardIdTargeter to avoid calling into the ShardRegistry to get a target shard.
    auto opCtxHolder = makeOperationContext();
    auto targeter = std::make_unique<ShardIdTargeterForTest>(
        shardId, opCtxHolder.get(), readPref, getExecutorPtr(), testHost);
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        findCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendTxnCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT(!request.cmdObj["startTransaction"]);
        ASSERT(!request.cmdObj["coordinator"]);
        ASSERT(!request.cmdObj["autocommit"]);
        ASSERT(!request.cmdObj["txnNumber"]);
        // The BSON documents in this cursor response are created here.
        // When async_rpc::sendCommand parses the response, it participates
        // in ownership of the underlying data, so it will participate in
        // owning the data in the cursor response.
        return CursorResponse(nss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    CursorInitialReply res = std::move(resultFuture).get().response;
    ASSERT_BSONOBJ_EQ(res.getCursor()->getFirstBatch()[0], BSON("x" << 1));
}

TEST_F(AsyncRPCTxnTestFixture, MultipleSendTxnCommand) {
    ShardId shardId("shard");
    ReadPreferenceSetting readPref;
    std::vector<HostAndPort> testHost = {kTestShardHosts[0]};
    // Use a mock ShardIdTargeter to avoid calling into the ShardRegistry to get a target shard.
    auto targeter = std::make_unique<ShardIdTargeterForTest>(
        shardId, getOpCtx(), readPref, getExecutorPtr(), testHost);
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    // Set up the transaction metadata.
    TxnNumber txnNum{3};
    getOpCtx()->setTxnNumber(txnNum);
    auto txnRouter = TransactionRouter::get(getOpCtx());
    txnRouter.beginOrContinueTxn(getOpCtx(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(getOpCtx());

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        findCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendTxnCommand(options, getOpCtx(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT(request.cmdObj["startTransaction"].Bool());
        ASSERT(request.cmdObj["coordinator"].Bool());
        ASSERT(!request.cmdObj["autocommit"].Bool());
        ASSERT_EQUALS(request.cmdObj["txnNumber"].numberLong(), 3LL);
        // The BSON documents in this cursor response are created here.
        // When async_rpc::sendCommand parses the response, it participates
        // in ownership of the underlying data, so it will participate in
        // owning the data in the cursor response.
        return CursorResponse(nss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse)
            .addFields(BSON("readOnly" << true));
    });

    CursorInitialReply res = std::move(resultFuture).get().response;
    ASSERT_BSONOBJ_EQ(res.getCursor()->getFirstBatch()[0], BSON("x" << 1));

    // // Issue a follow-up find command in the same transaction.
    FindCommandRequest secondFindCmd(nss);
    auto secondCmdOptions = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        secondFindCmd, getExecutorPtr(), _cancellationToken);
    auto secondTargeter = std::make_unique<ShardIdTargeterForTest>(
        shardId, getOpCtx(), readPref, getExecutorPtr(), testHost);
    auto secondResultFuture =
        sendTxnCommand(secondCmdOptions, getOpCtx(), std::move(secondTargeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT(!request.cmdObj["startTransaction"]);
        ASSERT(request.cmdObj["coordinator"].Bool());
        ASSERT(!request.cmdObj["autocommit"].Bool());
        ASSERT_EQUALS(request.cmdObj["txnNumber"].numberLong(), 3LL);
        return CursorResponse(nss, 0LL, {BSON("x" << 2)})
            .toBSON(CursorResponse::ResponseType::InitialResponse)
            .addFields(BSON("readOnly" << false));
    });

    CursorInitialReply secondRes = std::move(secondResultFuture).get().response;
    ASSERT_BSONOBJ_EQ(secondRes.getCursor()->getFirstBatch()[0], BSON("x" << 2));
}

// We test side-effects of calling `processParticipantResponse` with different values for `readOnly`
// in the response to ensure it is being invoked correctly by the sendTxnCommand wrapper.
TEST_F(AsyncRPCTxnTestFixture, EnsureProcessParticipantCalledCorrectlyOnSuccess) {
    ShardId shardId("shard");
    ReadPreferenceSetting readPref;
    std::vector<HostAndPort> testHost = {kTestShardHosts[0]};
    // Use a mock ShardIdTargeter to avoid calling into the ShardRegistry to get a target shard.
    auto targeter = std::make_unique<ShardIdTargeterForTest>(
        shardId, getOpCtx(), readPref, getExecutorPtr(), testHost);
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    // Set up the transaction metadata.
    TxnNumber txnNum{3};
    getOpCtx()->setTxnNumber(txnNum);
    auto txnRouter = TransactionRouter::get(getOpCtx());
    txnRouter.beginOrContinueTxn(getOpCtx(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(getOpCtx());

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        findCmd, getExecutorPtr(), _cancellationToken);

    // There should be no recovery shard to start with.
    ASSERT(!txnRouter.getRecoveryShardId());
    auto resultFuture = sendTxnCommand(options, getOpCtx(), std::move(targeter));

    // Set "readOnly: true" in the reply.
    onCommand([&](const auto& request) {
        return CursorResponse(nss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse)
            .addFields(BSON("readOnly" << true));
    });

    CursorInitialReply res = std::move(resultFuture).get().response;
    ASSERT_BSONOBJ_EQ(res.getCursor()->getFirstBatch()[0], BSON("x" << 1));
    // First statement was read-only. If processed correctly by the router, we shouldn't have set a
    // recovery shard.
    ASSERT_FALSE(txnRouter.getRecoveryShardId());

    // // Issue a follow-up find command in the same transaction.
    FindCommandRequest secondFindCmd(nss);
    auto secondCmdOptions = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        secondFindCmd, getExecutorPtr(), _cancellationToken);
    auto secondTargeter = std::make_unique<ShardIdTargeterForTest>(
        shardId, getOpCtx(), readPref, getExecutorPtr(), testHost);
    auto secondResultFuture =
        sendTxnCommand(secondCmdOptions, getOpCtx(), std::move(secondTargeter));

    // Set "readOnly: false" in this response. If processed correctly by the router, we _will_ set a
    // recovery shard.
    onCommand([&](const auto& request) {
        return CursorResponse(nss, 0LL, {BSON("x" << 2)})
            .toBSON(CursorResponse::ResponseType::InitialResponse)
            .addFields(BSON("readOnly" << false));
    });

    CursorInitialReply secondRes = std::move(secondResultFuture).get().response;
    ASSERT_BSONOBJ_EQ(secondRes.getCursor()->getFirstBatch()[0], BSON("x" << 2));

    // We should have set a recovery shard, if `TxnRouter::processParticipantResponse` was invoked
    // correctly.
    ASSERT(txnRouter.getRecoveryShardId());
    ASSERT_EQ(*txnRouter.getRecoveryShardId(), shardId);
}
TEST_F(AsyncRPCTxnTestFixture, EnsureProcessParticipantCalledCorrectlyOnRemoteError) {
    ShardId shardId("shard");
    ReadPreferenceSetting readPref;
    std::vector<HostAndPort> testHost = {kTestShardHosts[0]};
    // Use a mock ShardIdTargeter to avoid calling into the ShardRegistry to get a target shard.
    auto targeter = std::make_unique<ShardIdTargeterForTest>(
        shardId, getOpCtx(), readPref, getExecutorPtr(), testHost);
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    // Set up the transaction metadata.
    TxnNumber txnNum{3};
    getOpCtx()->setTxnNumber(txnNum);
    auto txnRouter = TransactionRouter::get(getOpCtx());
    txnRouter.beginOrContinueTxn(getOpCtx(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(getOpCtx());

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        findCmd, getExecutorPtr(), _cancellationToken);

    // There should be no recovery shard to start with.
    ASSERT(!txnRouter.getRecoveryShardId());
    auto resultFuture = sendTxnCommand(options, getOpCtx(), std::move(targeter));

    // Set "readOnly: false" in the reply.
    onCommand([&](const auto& request) {
        return createErrorResponse({ErrorCodes::BadValue, "test"})
            .addFields(BSON("readOnly" << false));
    });
    // Because the router ignores error-responses that aren't "ErrorCodes::WouldChangeOwningShard",
    // expect no change to the TransactionRouter state.
    std::move(resultFuture).getNoThrow().getStatus().ignore();
    ASSERT_FALSE(txnRouter.getRecoveryShardId());

    // // Issue a follow-up find command in the same transaction.
    FindCommandRequest secondFindCmd(nss);
    auto secondCmdOptions = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        secondFindCmd, getExecutorPtr(), _cancellationToken);
    auto secondTargeter = std::make_unique<ShardIdTargeterForTest>(
        shardId, getOpCtx(), readPref, getExecutorPtr(), testHost);
    auto secondResultFuture =
        sendTxnCommand(secondCmdOptions, getOpCtx(), std::move(secondTargeter));

    // Use WouldChangeOwningShard error this time.
    onCommand([&](const auto& request) -> BSONObj {
        auto code = ErrorCodes::WouldChangeOwningShard;
        auto err = BSON("ok" << false << "code" << code << "codeName"
                             << ErrorCodes::errorString(code) << "errmsg"
                             << "test"
                             << "preImage" << BSON("x" << 1) << "postImage" << BSON("x" << 2)
                             << "shouldUpsert" << true);
        return err.addFields(BSON("readOnly" << false));
    });

    std::move(secondResultFuture).getNoThrow().getStatus().ignore();
    // We should have set a recovery shard, if `TxnRouter::processParticipantResponse` was invoked
    // correctly.
    ASSERT(txnRouter.getRecoveryShardId());
    ASSERT_EQ(*txnRouter.getRecoveryShardId(), shardId);
}

TEST_F(AsyncRPCTxnTestFixture, SendTxnCommandWithGenericArgs) {
    ShardId shardId("shard");
    ReadPreferenceSetting readPref;
    std::vector<HostAndPort> testHost = {kTestShardHosts[0]};
    // Use a mock ShardIdTargeter to avoid calling into the ShardRegistry to get a target shard.
    auto targeter = std::make_unique<ShardIdTargeterForTest>(
        shardId, getOpCtx(), readPref, getExecutorPtr(), testHost);
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    // Set up the transaction metadata.
    TxnNumber txnNum{3};
    getOpCtx()->setTxnNumber(txnNum);
    auto txnRouter = TransactionRouter::get(getOpCtx());
    txnRouter.beginOrContinueTxn(getOpCtx(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(getOpCtx());

    FindCommandRequest findCmd(nss);

    // Populate structs for generic arguments to be passed along when the command is converted
    // to BSON. This is simply to test that generic args are passed properly and they should not
    // contribute to any other behaviors of this test.
    GenericArgsAPIV1 genericArgsApiV1;
    GenericArgsAPIV1Unstable genericArgsUnstable;
    const UUID clientOpKey = UUID::gen();
    genericArgsApiV1.setClientOperationKey(clientOpKey);
    auto configTime = Timestamp(1, 1);
    genericArgsUnstable.setDollarConfigTime(configTime);
    auto expectedShardVersion = ShardVersion();
    genericArgsUnstable.setShardVersion(expectedShardVersion);

    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        findCmd,
        getExecutorPtr(),
        _cancellationToken,
        std::make_shared<NeverRetryPolicy>(),
        GenericArgs(genericArgsApiV1, genericArgsUnstable));
    auto resultFuture = sendTxnCommand(options, getOpCtx(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT(request.cmdObj["startTransaction"].Bool());
        ASSERT(request.cmdObj["coordinator"].Bool());
        ASSERT(!request.cmdObj["autocommit"].Bool());

        ASSERT(request.cmdObj["shardVersion"]);
        auto shardVersion = ShardVersion::parse(request.cmdObj["shardVersion"]);
        ASSERT_EQUALS(expectedShardVersion, shardVersion);
        ASSERT_EQUALS(request.cmdObj["txnNumber"].numberLong(), 3LL);
        // Confirm that the generic arguments are present in the BSON command object.
        ASSERT_EQ(UUID::fromCDR(request.cmdObj["clientOperationKey"].uuid()), clientOpKey);
        ASSERT_EQ(request.cmdObj["$configTime"].timestamp(), configTime);

        // The BSON documents in this cursor response are created here.
        // When async_rpc::sendCommand parses the response, it participates
        // in ownership of the underlying data, so it will participate in
        // owning the data in the cursor response.
        return CursorResponse(nss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse)
            .addFields(BSON("readOnly" << false));
    });

    CursorInitialReply res = std::move(resultFuture).get().response;
    ASSERT_BSONOBJ_EQ(res.getCursor()->getFirstBatch()[0], BSON("x" << 1));
}

TEST_F(AsyncRPCTxnTestFixture, SendTxnCommandReturnsRemoteError) {
    ShardId shardId("shard");
    ReadPreferenceSetting readPref;
    std::vector<HostAndPort> testHost = {kTestShardHosts[0]};
    // Use a mock ShardIdTargeter to avoid calling into the ShardRegistry to get a target shard.
    auto targeter = std::make_unique<ShardIdTargeterForTest>(
        shardId, getOpCtx(), readPref, getExecutorPtr(), testHost);
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    // Set up the transaction metadata.
    TxnNumber txnNum{3};
    getOpCtx()->setTxnNumber(txnNum);
    auto txnRouter = TransactionRouter::get(getOpCtx());
    txnRouter.beginOrContinueTxn(getOpCtx(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(getOpCtx());

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        findCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendTxnCommand(options, getOpCtx(), std::move(targeter));
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT(request.cmdObj["startTransaction"].Bool());
        ASSERT(request.cmdObj["coordinator"].Bool());
        ASSERT(!request.cmdObj["autocommit"].Bool());
        ASSERT_EQUALS(request.cmdObj["txnNumber"].numberLong(), 3LL);
        return createErrorResponse(Status(ErrorCodes::BadValue, "mock"));
    });

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    ASSERT(extraInfo->isRemote());
}

TEST_F(AsyncRPCTxnTestFixture, SendTxnCommandReturnsLocalError) {
    ShardId shardId("shard");
    ReadPreferenceSetting readPref;
    std::vector<HostAndPort> testHost = {kTestShardHosts[0]};
    // Use a mock ShardIdTargeter to avoid calling into the ShardRegistry to get a target shard.
    auto targeter = std::make_unique<ShardIdTargeterForTest>(
        shardId, getOpCtx(), readPref, getExecutorPtr(), testHost);
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    // Set up the transaction metadata.
    TxnNumber txnNum{3};
    getOpCtx()->setTxnNumber(txnNum);
    auto txnRouter = TransactionRouter::get(getOpCtx());
    txnRouter.beginOrContinueTxn(getOpCtx(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(getOpCtx());

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        findCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendTxnCommand(options, getOpCtx(), std::move(targeter));
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        ASSERT(request.cmdObj["startTransaction"].Bool());
        ASSERT(request.cmdObj["coordinator"].Bool());
        ASSERT(!request.cmdObj["autocommit"].Bool());
        ASSERT_EQUALS(request.cmdObj["txnNumber"].numberLong(), 3LL);
        return Status(ErrorCodes::NetworkTimeout, "mock");
    });

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    ASSERT(extraInfo->isLocal());
}

TEST_F(AsyncRPCTestFixture, AttemptedTargetCorrectlyPropogatedWithLocalError) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    HostAndPort target("FakeHost1", 12345);
    auto targeter = std::make_unique<FixedTargeter>(target);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(request.target, target);
        return Status(ErrorCodes::NetworkTimeout, "mock");
    });

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    ASSERT(extraInfo->isLocal());
    auto targetsAttempted = extraInfo->getTargetsAttempted();
    ASSERT_EQ(targetsAttempted.size(), 1);
    ASSERT_EQ(targetsAttempted[0], target);
}

TEST_F(AsyncRPCTestFixture, NoAttemptedTargetIfTargetingFails) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    Status resolveErr{ErrorCodes::BadValue, "Failing resolve for test!"};
    auto targeter = std::make_unique<FailingTargeter>(resolveErr);


    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal(), resolveErr);
    auto targetsAttempted = extraInfo->getTargetsAttempted();
    ASSERT_EQ(targetsAttempted.size(), 0);
}

TEST_F(AsyncRPCTestFixture, RemoteErrorAttemptedTargetMatchesActual) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    HostAndPort target("FakeHost1", 12345);
    auto targeter = std::make_unique<FixedTargeter>(target);


    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        helloCmd, getExecutorPtr(), _cancellationToken);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(request.target, target);
        return createErrorResponse(Status(ErrorCodes::BadValue, "mock"));
    });

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    ASSERT(extraInfo->isRemote());

    auto remoteErr = extraInfo->asRemote();
    auto targetsAttempted = extraInfo->getTargetsAttempted();
    ASSERT_EQ(targetsAttempted.size(), 1);
    auto targetAttempted = targetsAttempted[0];
    auto targetHeardFrom = remoteErr.getTargetUsed();
    ASSERT_EQ(targetAttempted, targetHeardFrom);
    ASSERT_EQ(target, targetHeardFrom);
}

}  // namespace
}  // namespace async_rpc
}  // namespace mongo
