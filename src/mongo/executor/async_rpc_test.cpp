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

#include "mongo/executor/async_rpc.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/async_remote_command_targeter_adapter.h"
#include "mongo/client/connection_string.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/db/database_name.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/client_cursor/cursor_response_gen.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/repl/hello/hello_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/executor/async_rpc_error_info.h"
#include "mongo/executor/async_rpc_retry_policy.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/async_rpc_test_fixture.h"
#include "mongo/executor/async_rpc_util.h"
#include "mongo/executor/async_transaction_rpc.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/task_executor.h"
#include "mongo/idl/generic_argument_gen.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/s/transaction_router.h"
#include "mongo/stdx/thread.h"
#include "mongo/unittest/thread_assertion_monitor.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

#include <absl/container/node_hash_map.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

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

template <typename CommandType>
auto sendCommandAndWaitUntilRequestIsReady(std::shared_ptr<AsyncRPCOptions<CommandType>> options,
                                           OperationContext* opCtx,
                                           std::unique_ptr<Targeter> targeter,
                                           NetworkInterfaceMock* net) {
    auto getNumReady = [net] {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        return net->getNumReadyRequests();
    };
    const auto numBefore = getNumReady();
    auto future = sendCommand(options, opCtx, std::move(targeter));
    for (auto i = 0; i < 5000; ++i) {  // Wait for at most 5 seconds
        sleepFor(Milliseconds(1));
        if (getNumReady() == numBefore + 1) {
            return future;
        }
    }
    iasserted(ErrorCodes::ExceededTimeLimit, "Timed out while waiting for NetworkInterfaceMock!");
}

void shutdownExecutor(TaskExecutor* executor, NetworkInterfaceMock* net) {
    executor::NetworkInterfaceMock::InNetworkGuard guard(net);
    executor->shutdown();
    net->runReadyNetworkOperations();
}

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
        getExecutorPtr(), _cancellationToken, helloCmd);
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
        getExecutorPtr(), _cancellationToken, helloCmd);
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
        getExecutorPtr(), _cancellationToken, helloCmd);
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

    // Populate generic arguments to be passed along when the command is converted
    // to BSON.
    helloCmd.setAutocommit(false);
    const UUID clientOpKey = UUID::gen();
    helloCmd.setClientOperationKey(clientOpKey);

    // Populate struct for generic reply fields that are expected to be parsed from the
    // response object.
    GenericReplyFields genericReplyFields;
    genericReplyFields.setOk(1);
    genericReplyFields.setDollarConfigTime(Timestamp(1, 1));
    auto clusterTime = ClusterTime();
    clusterTime.setClusterTime(Timestamp(2, 3));
    clusterTime.setSignature(ClusterTimeSignature(std::vector<std::uint8_t>(), 0));
    genericReplyFields.setDollarClusterTime(clusterTime);
    auto configTime = Timestamp(1, 1);
    helloCmd.setDollarConfigTime(configTime);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        getExecutorPtr(), _cancellationToken, helloCmd, std::make_shared<NeverRetryPolicy>());
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
        reply.append("$clusterTime", clusterTime.toBSON());

        return reply.obj();
    });

    AsyncRPCResponse res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), res.targetUsed);
    ASSERT_BSONOBJ_EQ(genericReplyFields.toBSON(), res.genericReplyFields.toBSON());
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
        getExecutorPtr(), _cancellationToken, helloCmd, testPolicy);
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
        getExecutorPtr(), _cancellationToken, helloCmd, testPolicy);
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
        getExecutorPtr(), _cancellationToken, helloCmd, testPolicy);
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
        getExecutorPtr(), _cancellationToken, helloCmd);
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
        getExecutorPtr(), _cancellationToken, helloCmd);
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
        getExecutorPtr(), _cancellationToken, helloCmd);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    GenericReplyFields genericReplyFields;
    auto clusterTime = ClusterTime();
    clusterTime.setClusterTime(Timestamp(2, 3));
    clusterTime.setSignature(ClusterTimeSignature(std::vector<std::uint8_t>(), 0));
    genericReplyFields.setDollarClusterTime(clusterTime);
    genericReplyFields.setDollarConfigTime(Timestamp(1, 1));
    genericReplyFields.setOk(false);

    onCommand([&, genericReplyFields](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), request.target);
        auto remoteErrorBson = createErrorResponse(Status(ErrorCodes::BadValue, "mock"));
        return remoteErrorBson.addFields(genericReplyFields.toBSON());
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
    ASSERT_BSONOBJ_EQ(genericReplyFields.toBSON(), replyFields.toBSON());
}

TEST_F(AsyncRPCTestFixture, SuccessfulFind) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    auto opCtxHolder = makeOperationContext();
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        getExecutorPtr(), _cancellationToken, findCmd);
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

    const BSONObj writeConcernError = BSON("code" << ErrorCodes::WriteConcernTimeout << "errmsg"
                                                  << "mock");
    BSONObj resWithWriteConcernError = BSON("ok" << 1 << "writeConcernError" << writeConcernError);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        getExecutorPtr(), _cancellationToken, helloCmd);
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
              Status(ErrorCodes::WriteConcernTimeout, "mock"));

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
    BSONObj resWithWriteError = BSON("ok" << 1 << "writeErrors" << BSON_ARRAY(writeError));
    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        getExecutorPtr(), _cancellationToken, helloCmd);
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
        getExecutorPtr(), _cancellationToken, helloCmd);
    auto resultFuture = sendCommandAndWaitUntilRequestIsReady(
        options, opCtxHolder.get(), std::move(targeter), getNetworkInterfaceMock());
    shutdownExecutor(getExecutorPtr().get(), getNetworkInterfaceMock());
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
        getExecutorPtr(), _cancellationToken, helloCmd);
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
        getExecutorPtr(), _cancellationToken, helloCmd);
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
        getExecutorPtr(), _cancellationToken, helloCmd);
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
        getExecutorPtr(), _cancellationToken, helloCmd);
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
        getExecutorPtr(), opCtxHolder.get(), shardId, readPref, testHost);
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        getExecutorPtr(), _cancellationToken, findCmd);
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
        getExecutorPtr(), getOpCtx(), shardId, readPref, testHost);
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
        getExecutorPtr(), _cancellationToken, findCmd);
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
        getExecutorPtr(), _cancellationToken, secondFindCmd);
    auto secondTargeter = std::make_unique<ShardIdTargeterForTest>(
        getExecutorPtr(), getOpCtx(), shardId, readPref, testHost);
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
        getExecutorPtr(), getOpCtx(), shardId, readPref, testHost);
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
        getExecutorPtr(), _cancellationToken, findCmd);

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
        getExecutorPtr(), _cancellationToken, secondFindCmd);
    auto secondTargeter = std::make_unique<ShardIdTargeterForTest>(
        getExecutorPtr(), getOpCtx(), shardId, readPref, testHost);
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
        getExecutorPtr(), getOpCtx(), shardId, readPref, testHost);
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
        getExecutorPtr(), _cancellationToken, findCmd);

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
        getExecutorPtr(), _cancellationToken, secondFindCmd);
    auto secondTargeter = std::make_unique<ShardIdTargeterForTest>(
        getExecutorPtr(), getOpCtx(), shardId, readPref, testHost);
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

TEST_F(AsyncRPCTxnTestFixture, SendTxnCommandWithGenericArguments) {
    ShardId shardId("shard");
    ReadPreferenceSetting readPref;
    std::vector<HostAndPort> testHost = {kTestShardHosts[0]};
    // Use a mock ShardIdTargeter to avoid calling into the ShardRegistry to get a target shard.
    auto targeter = std::make_unique<ShardIdTargeterForTest>(
        getExecutorPtr(), getOpCtx(), shardId, readPref, testHost);
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    // Set up the transaction metadata.
    TxnNumber txnNum{3};
    getOpCtx()->setTxnNumber(txnNum);
    auto txnRouter = TransactionRouter::get(getOpCtx());
    txnRouter.beginOrContinueTxn(getOpCtx(), txnNum, TransactionRouter::TransactionActions::kStart);
    txnRouter.setDefaultAtClusterTime(getOpCtx());

    FindCommandRequest findCmd(nss);

    // Populate generic arguments to be passed along when the command is converted
    // to BSON. This is simply to test that generic args are passed properly and they should not
    // contribute to any other behaviors of this test.
    const UUID clientOpKey = UUID::gen();
    findCmd.setClientOperationKey(clientOpKey);
    auto configTime = Timestamp(1, 1);
    findCmd.setDollarConfigTime(configTime);
    auto expectedShardVersion = ShardVersion();
    findCmd.setShardVersion(expectedShardVersion);

    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        getExecutorPtr(), _cancellationToken, findCmd, std::make_shared<NeverRetryPolicy>());
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
        getExecutorPtr(), getOpCtx(), shardId, readPref, testHost);
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
        getExecutorPtr(), _cancellationToken, findCmd);
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
        getExecutorPtr(), getOpCtx(), shardId, readPref, testHost);
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
        getExecutorPtr(), _cancellationToken, findCmd);
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
        getExecutorPtr(), _cancellationToken, helloCmd);
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
    auto targetAttempted = extraInfo->getTargetAttempted();
    ASSERT(targetAttempted);
    ASSERT_EQ(*targetAttempted, target);
}

TEST_F(AsyncRPCTestFixture, NoAttemptedTargetIfTargetingFails) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    Status resolveErr{ErrorCodes::BadValue, "Failing resolve for test!"};
    auto targeter = std::make_unique<FailingTargeter>(resolveErr);


    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        getExecutorPtr(), _cancellationToken, helloCmd);
    auto resultFuture = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal(), resolveErr);
    auto targetAttempted = extraInfo->getTargetAttempted();
    ASSERT_FALSE(targetAttempted);
}

TEST_F(AsyncRPCTestFixture, RemoteErrorAttemptedTargetMatchesActual) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    HostAndPort target("FakeHost1", 12345);
    auto targeter = std::make_unique<FixedTargeter>(target);

    auto opCtxHolder = makeOperationContext();
    auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
        getExecutorPtr(), _cancellationToken, helloCmd);
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
    auto targetAttempted = extraInfo->getTargetAttempted();
    ASSERT(targetAttempted);
    auto targetHeardFrom = remoteErr.getTargetUsed();
    ASSERT_EQ(targetAttempted, targetHeardFrom);
    ASSERT_EQ(target, targetHeardFrom);
}

auto extractUUID(const BSONElement& element) {
    return UUID::fromCDR(element.uuid());
}

auto getOpKeyFromCommand(const BSONObj& cmdObj) {
    return extractUUID(cmdObj["clientOperationKey"]);
}

TEST_F(AsyncRPCTestFixture, OperationKeyIsSetByDefault) {
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    auto opCtxHolder = makeOperationContext();
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        getExecutorPtr(), _cancellationToken, findCmd);
    auto future = sendCommand(options, opCtxHolder.get(), std::move(targeter));
    ASSERT_DOES_NOT_THROW([&] {
        onCommand([&](const auto& request) {
            (void)getOpKeyFromCommand(request.cmdObj);
            return CursorResponse(nss, 0LL, {BSON("x" << 1)})
                .toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    }());
    auto network = getNetworkInterfaceMock();
    network->enterNetwork();
    network->runReadyNetworkOperations();
    network->exitNetwork();

    future.get();
}

TEST_F(AsyncRPCTestFixture, UseOperationKeyWhenProvided) {
    const auto opKey = UUID::gen();

    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    auto opCtxHolder = makeOperationContext();
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    FindCommandRequest findCmd(nss);
    findCmd.setClientOperationKey(opKey);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        getExecutorPtr(), _cancellationToken, findCmd);
    auto future = sendCommand(options, opCtxHolder.get(), std::move(targeter));
    onCommand([&](const auto& request) {
        ASSERT_EQ(getOpKeyFromCommand(request.cmdObj), opKey);
        return CursorResponse(nss, 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });
    future.get();
}

/**
 * Checks that if cancellation occurs after TaskExecutor receives a network response, the
 * cancellation fails and the network response fulfills the final response.
 */
TEST_F(AsyncRPCTestFixture, CancelAfterNetworkResponse) {
    auto pauseAfterNetworkResponseFailPoint =
        globalFailPointRegistry().find("pauseAsyncRPCAfterNetworkResponse");
    pauseAfterNetworkResponseFailPoint->setMode(FailPoint::alwaysOn);
    std::unique_ptr<Targeter> targeter = std::make_unique<LocalHostTargeter>();
    auto opCtxHolder = makeOperationContext();
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    CancellationSource source;
    CancellationToken token = source.token();
    FindCommandRequest findCmd(nss);
    auto options =
        std::make_shared<AsyncRPCOptions<FindCommandRequest>>(getExecutorPtr(), token, findCmd);
    auto future = sendCommand(options, opCtxHolder.get(), std::move(targeter));

    // Will pause processing response after network interface.
    stdx::thread worker([&] {
        onCommand([&](const auto& request) {
            return CursorResponse(nss, 0LL, {BSON("x" << 1)})
                .toBSON(CursorResponse::ResponseType::InitialResponse);
        });
    });

    // Cancel after network response received in the TaskExecutor.
    pauseAfterNetworkResponseFailPoint->waitForTimesEntered(1);
    source.cancel();
    pauseAfterNetworkResponseFailPoint->setMode(FailPoint::off);

    // Canceling after network response received does not change the final response and
    // does not send killOperation.
    CursorInitialReply res = std::move(future).get().response;
    ASSERT_BSONOBJ_EQ(res.getCursor()->getFirstBatch()[0], BSON("x" << 1));

    worker.join();
}

/**
 * Tests that targeter->onRemoteCommandError is called for errors attributed to a remote
 * host.
 */
TEST_F(AsyncRPCTestFixture, TargeterOnRemoteCommandError) {
    const HostAndPort testHost = HostAndPort("Host1", 1);
    const std::vector<HostAndPort> hosts{testHost};
    auto factory = RemoteCommandTargeterFactoryMock();
    std::shared_ptr<RemoteCommandTargeter> t;
    t = factory.create(ConnectionString::forStandalones(hosts));
    auto targeterMock = RemoteCommandTargeterMock::get(t);
    targeterMock->setFindHostsReturnValue(hosts);

    ReadPreferenceSetting readPref;
    std::unique_ptr<Targeter> targeter =
        std::make_unique<AsyncRemoteCommandTargeterAdapter>(readPref, t);
    auto opCtxHolder = makeOperationContext();
    DatabaseName testDbName = DatabaseName::createDatabaseName_forTest(boost::none, "testdb");
    NamespaceString nss = NamespaceString::createNamespaceString_forTest(testDbName);

    FindCommandRequest findCmd(nss);
    auto options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        getExecutorPtr(), CancellationToken::uncancelable(), findCmd);
    auto future = sendCommandAndWaitUntilRequestIsReady(
        options, opCtxHolder.get(), std::move(targeter), getNetworkInterfaceMock());

    onCommand([&](const auto& request) {
        return createErrorResponse(Status(ErrorCodes::ShutdownInProgress, "test"));
    });

    future.wait();

    // Check for call to onRemoteCommandError.
    auto downHosts = targeterMock->getAndClearMarkedDownHosts();
    ASSERT_TRUE(downHosts.find(testHost) != downHosts.end());

    // Run another command, but this time, simulate a local error and check that targeter does
    // not update with the testHost.
    targeter = std::make_unique<AsyncRemoteCommandTargeterAdapter>(readPref, t);
    options = std::make_shared<AsyncRPCOptions<FindCommandRequest>>(
        getExecutorPtr(), CancellationToken::uncancelable(), findCmd);
    future = sendCommandAndWaitUntilRequestIsReady(
        options, opCtxHolder.get(), std::move(targeter), getNetworkInterfaceMock());
    shutdownExecutor(getExecutorPtr().get(), getNetworkInterfaceMock());
    future.wait();

    // onRemoteCommandError not called, error not from remote host.
    downHosts = targeterMock->getAndClearMarkedDownHosts();
    ASSERT_FALSE(downHosts.find(testHost) != downHosts.end());
}

}  // namespace
}  // namespace async_rpc
}  // namespace mongo
