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

#include "mongo/client/async_remote_command_targeter_adapter.h"
#include "mongo/client/read_preference.h"
#include "mongo/client/remote_command_targeter.h"
#include "mongo/client/remote_command_targeter_factory_mock.h"
#include "mongo/client/remote_command_targeter_mock.h"
#include "mongo/client/remote_command_targeter_rs.h"
#include "mongo/db/database_name.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/cursor_response_gen.h"
#include "mongo/db/query/find_command_gen.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/async_rpc_test_fixture.h"
#include "mongo/executor/hedged_async_rpc.h"
#include "mongo/executor/hedging_metrics.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/s/mongos_server_parameters_gen.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace async_rpc {
namespace {
using executor::RemoteCommandResponse;

class HedgedAsyncRPCTest : public AsyncRPCTestFixture {
public:
    const std::vector<HostAndPort> kTwoHosts{HostAndPort("FakeHost1", 12345),
                                             HostAndPort("FakeHost2", 12345)};
    using TwoHostCallback =
        std::function<void(NetworkInterfaceMock::NetworkOperationIterator authoritative,
                           NetworkInterfaceMock::NetworkOperationIterator hedged)>;

    NetworkInterface::Counters getNetworkInterfaceCounters() {
        auto counters = getNetworkInterfaceMock()->getCounters();
        return counters;
    }

    void setUp() override {
        AsyncRPCTestFixture::setUp();
        hm = HedgingMetrics::get(_opCtx->getServiceContext());
    }

    /**
     * Retrieves authoritative and hedged NOIs, then performs specified behavior callback.
     */
    void performAuthoritativeHedgeBehavior(NetworkInterfaceMock* network,
                                           TwoHostCallback mockBehaviorFn) {
        NetworkInterfaceMock::NetworkOperationIterator noi1 = network->getNextReadyRequest();
        NetworkInterfaceMock::NetworkOperationIterator noi2 = network->getNextReadyRequest();

        auto firstRequest = (*noi1).getRequestOnAny();
        auto secondRequest = (*noi2).getRequestOnAny();

        bool firstRequestAuthoritative = firstRequest.target[0] == kTwoHosts[0];

        auto authoritative = firstRequestAuthoritative ? noi1 : noi2;
        auto hedged = firstRequestAuthoritative ? noi2 : noi1;

        mockBehaviorFn(authoritative, hedged);
    }

    /**
     * Testing wrapper to perform common set up, then call sendHedgedCommand. Only safe to call once
     * per test fixture as to not create multiple OpCtx.
     */
    template <typename CommandType>
    SemiFuture<AsyncRPCResponse<typename CommandType::Reply>> sendHedgedCommandWithHosts(
        CommandType cmd,
        std::vector<HostAndPort> hosts,
        std::shared_ptr<RetryPolicy> retryPolicy = std::make_shared<NeverRetryPolicy>(),
        GenericArgs genericArgs = GenericArgs(),
        BatonHandle bh = nullptr) {
        // Use a readPreference that's elgible for hedging.
        ReadPreferenceSetting readPref(ReadPreference::Nearest);
        readPref.hedgingMode = HedgingMode();

        auto factory = RemoteCommandTargeterFactoryMock();
        std::shared_ptr<RemoteCommandTargeter> t;
        t = factory.create(ConnectionString::forStandalones(hosts));
        auto targeterMock = RemoteCommandTargeterMock::get(t);
        targeterMock->setFindHostsReturnValue(hosts);

        std::unique_ptr<Targeter> targeter =
            std::make_unique<AsyncRemoteCommandTargeterAdapter>(readPref, t);

        return sendHedgedCommand(cmd,
                                 _opCtx.get(),
                                 std::move(targeter),
                                 getExecutorPtr(),
                                 CancellationToken::uncancelable(),
                                 retryPolicy,
                                 readPref,
                                 genericArgs,
                                 bh);
    }

    const NamespaceString testNS = NamespaceString("testdb", "testcoll");
    const FindCommandRequest testFindCmd = FindCommandRequest(testNS);
    const BSONObj testFirstBatch = BSON("x" << 1);

    const Status ignorableMaxTimeMSExpiredStatus{Status(ErrorCodes::MaxTimeMSExpired, "mock")};
    const Status ignorableNetworkTimeoutStatus{Status(ErrorCodes::NetworkTimeout, "mock")};
    const Status fatalInternalErrorStatus{Status(ErrorCodes::InternalError, "mock")};

    const RemoteCommandResponse testSuccessResponse{
        CursorResponse(testNS, 0LL, {testFirstBatch})
            .toBSON(CursorResponse::ResponseType::InitialResponse),
        Milliseconds::zero()};
    const RemoteCommandResponse testFatalErrorResponse{
        createErrorResponse(fatalInternalErrorStatus), Milliseconds(1)};
    const RemoteCommandResponse testIgnorableErrorResponse{
        createErrorResponse(ignorableMaxTimeMSExpiredStatus), Milliseconds(1)};
    const RemoteCommandResponse testAlternateIgnorableErrorResponse{
        createErrorResponse(ignorableNetworkTimeoutStatus), Milliseconds(1)};

    HedgingMetrics* hm;
    auto getOpCtx() {
        return _opCtx.get();
    }

private:
    // This OpCtx is used by sendHedgedCommandWithHosts and is destroyed during fixture destruction.
    ServiceContext::UniqueOperationContext _opCtx{makeOperationContext()};
};

/**
 * When we send a find command to the sendHedgedCommand function, it sends out two requests and
 * cancels the second one once the first has responsed.
 */
TEST_F(HedgedAsyncRPCTest, FindHedgeRequestTwoHosts) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(testNS, 0LL, {testFirstBatch})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    auto network = getNetworkInterfaceMock();
    network->enterNetwork();
    network->runReadyNetworkOperations();
    network->exitNetwork();

    auto counters = getNetworkInterfaceCounters();
    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 1);

    auto resCursor = resultFuture.get().response.getCursor();
    ASSERT_EQ(resCursor->getNs(), testNS);
    ASSERT_BSONOBJ_EQ(resCursor->getFirstBatch()[0], testFirstBatch);
}

/**
 * When we send a hello command to the sendHedgedCommand function, it does not hedge and only sends
 * one request.
 */
TEST_F(HedgedAsyncRPCTest, HelloHedgeRequest) {
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    auto resultFuture = sendHedgedCommandWithHosts(helloCmd, kTwoHosts);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        return helloReply.toBSON();
    });

    auto counters = getNetworkInterfaceCounters();
    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 0);

    auto response = resultFuture.get().response;

    ASSERT_BSONOBJ_EQ(response.toBSON(), helloReply.toBSON());
}

/**
 * Test that generic args are passed in.
 */
TEST_F(HedgedAsyncRPCTest, HelloHedgeRequestWithGenericArgs) {
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    // Populate structs for generic arguments to be passed along when the command is converted
    // to BSON.
    GenericArgsAPIV1 genericArgsApiV1;
    GenericArgsAPIV1Unstable genericArgsUnstable;
    const UUID clientOpKey = UUID::gen();
    genericArgsApiV1.setClientOperationKey(clientOpKey);
    auto configTime = Timestamp(1, 1);
    genericArgsUnstable.setDollarConfigTime(configTime);

    auto resultFuture =
        sendHedgedCommandWithHosts(helloCmd,
                                   kTwoHosts,
                                   std::make_shared<NeverRetryPolicy>(),
                                   GenericArgs(genericArgsApiV1, genericArgsUnstable));

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        // Confirm that the generic arguments are present in the BSON command object.
        ASSERT_EQ(UUID::fromCDR(request.cmdObj["clientOperationKey"].uuid()), clientOpKey);
        ASSERT_EQ(request.cmdObj["$configTime"].timestamp(), configTime);
        return helloReply.toBSON();
    });

    auto counters = getNetworkInterfaceCounters();
    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 0);

    auto response = resultFuture.get().response;

    ASSERT_BSONOBJ_EQ(response.toBSON(), helloReply.toBSON());
}

/*
 * Test that remote errors with generic reply fields are properly parsed.
 */
TEST_F(HedgedAsyncRPCTest, HelloHedgeRemoteErrorWithGenericReplyFields) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = network->now();
    network->enterNetwork();

    GenericReplyFieldsWithTypesV1 stableFields;
    stableFields.setDollarClusterTime(LogicalTime(Timestamp(2, 3)));
    GenericReplyFieldsWithTypesUnstableV1 unstableFields;
    unstableFields.setDollarConfigTime(Timestamp(1, 1));
    unstableFields.setOk(false);

    // Send "ignorable" error responses for both requests.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            auto remoteErrorBson = createErrorResponse(ignorableMaxTimeMSExpiredStatus);
            remoteErrorBson = remoteErrorBson.addFields(stableFields.toBSON());
            remoteErrorBson = remoteErrorBson.addFields(unstableFields.toBSON());
            const auto rcr = RemoteCommandResponse(remoteErrorBson, Milliseconds(1));
            network->scheduleResponse(hedged, now, rcr);
            network->scheduleSuccessfulResponse(authoritative, now + Milliseconds(1000), rcr);
        });

    network->runUntil(now + Milliseconds(1500));
    network->exitNetwork();

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), ignorableMaxTimeMSExpiredStatus);

    // Check generic reply fields.
    auto replyFields = remoteError.getGenericReplyFields();
    ASSERT_BSONOBJ_EQ(stableFields.toBSON(), replyFields.stable.toBSON());
    ASSERT_BSONOBJ_EQ(unstableFields.toBSON(), replyFields.unstable.toBSON());
}

TEST_F(HedgedAsyncRPCTest, HedgedAsyncRPCWithRetryPolicy) {
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

    auto resultFuture = sendHedgedCommandWithHosts(helloCmd, kTwoHosts, testPolicy);

    const auto onCommandFunc = [&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        return helloReply.toBSON();
    };
    // Schedule 1 request as the initial attempt, and then three following retries to satisfy the
    // condition for the runner to stop retrying.
    for (auto i = 0; i <= maxNumRetries; i++) {
        scheduleRequestAndAdvanceClockForRetry(testPolicy, onCommandFunc, retryDelay);
    }

    auto counters = getNetworkInterfaceCounters();
    ASSERT_EQ(counters.succeeded, 4);
    ASSERT_EQ(counters.canceled, 0);

    AsyncRPCResponse<HelloCommandReply> res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(maxNumRetries, testPolicy->getNumRetriesPerformed());
}

/**
 * When the targeter returns an error, ensure we rewrite it correctly.
 */
TEST_F(HedgedAsyncRPCTest, FailedTargeting) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    auto targeterFailStatus = Status{ErrorCodes::InternalError, "Fake targeter failure"};
    auto targeter = std::make_unique<FailingTargeter>(targeterFailStatus);

    auto resultFuture = sendHedgedCommand(helloCmd,
                                          getOpCtx(),
                                          std::move(targeter),
                                          getExecutorPtr(),
                                          CancellationToken::uncancelable());

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isLocal());
    auto localError = extraInfo->asLocal();
    ASSERT_EQ(localError, targeterFailStatus);

    // Test metrics where error occurs before hedged command dispatch
    ASSERT_EQ(hm->getNumTotalOperations(), 0);
    ASSERT_EQ(hm->getNumTotalHedgedOperations(), 0);
    ASSERT_EQ(hm->getNumAdvantageouslyHedgedOperations(), 0);
}

// Ensure that the sendHedgedCommand correctly returns RemoteCommandExecutionError when the executor
// is shutdown mid-remote-invocation, and that the executor shutdown error is contained
// in the error's ExtraInfo.
TEST_F(HedgedAsyncRPCTest, ExecutorShutdown) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);
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

/**
 * When a hedged command is sent and one request resolves with a non-ignorable error, we propagate
 * that error upwards and cancel the other requests.
 */
TEST_F(HedgedAsyncRPCTest, FirstCommandFailsWithSignificantError) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return fatalInternalErrorStatus;
    });

    auto network = getNetworkInterfaceMock();
    network->enterNetwork();
    network->runReadyNetworkOperations();
    network->exitNetwork();

    auto counters = getNetworkInterfaceCounters();
    ASSERT_EQ(counters.failed, 1);
    ASSERT_EQ(counters.canceled, 1);

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isLocal());
    auto localError = extraInfo->asLocal();
    ASSERT_EQ(localError, fatalInternalErrorStatus);
}

/**
 * When a hedged command is sent and all requests fail with an "ignorable" error, that error
 * propagates upwards.
 */
TEST_F(HedgedAsyncRPCTest, BothCommandsFailWithIgnorableError) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = network->now();
    network->enterNetwork();

    // Send "ignorable" error responses for both requests.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleResponse(hedged, now, testIgnorableErrorResponse);
            network->scheduleSuccessfulResponse(
                authoritative, now + Milliseconds(1000), testIgnorableErrorResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    ASSERT_EQ(counters.sent, 2);
    ASSERT_EQ(counters.canceled, 0);

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), ignorableMaxTimeMSExpiredStatus);
}

TEST_F(HedgedAsyncRPCTest, AllCommandsFailWithIgnorableError) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = network->now();
    network->enterNetwork();

    // Send "ignorable" responses for both requests.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged1) {
            network->scheduleResponse(
                authoritative, now + Milliseconds(1000), testIgnorableErrorResponse);
            network->scheduleResponse(hedged1, now, testIgnorableErrorResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    ASSERT_EQ(counters.succeeded, 2);
    ASSERT_EQ(counters.canceled, 0);

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), ignorableMaxTimeMSExpiredStatus);
}

/**
 * When a hedged command is sent and the first request, which is hedged, fails with an
 * ignorable error and the second request, which is authoritative, succeeds, we get
 * the success result.
 */
TEST_F(HedgedAsyncRPCTest, HedgedFailsWithIgnorableErrorAuthoritativeSucceeds) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    // If the request is the authoritative one, send a delayed success response
    // otherwise send an "ignorable" error.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleResponse(hedged, now, testIgnorableErrorResponse);
            network->scheduleSuccessfulResponse(
                authoritative, now + Milliseconds(1000), testSuccessResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    ASSERT_EQ(counters.succeeded, 2);
    ASSERT_EQ(counters.canceled, 0);

    auto res = std::move(resultFuture).get().response;
    ASSERT_EQ(res.getCursor()->getNs(), testNS);
    ASSERT_BSONOBJ_EQ(res.getCursor()->getFirstBatch()[0], testFirstBatch);

    // Test metrics where hedged fails with ignorable error
    ASSERT_EQ(hm->getNumTotalOperations(), 1);
    ASSERT_EQ(hm->getNumTotalHedgedOperations(), 1);
    ASSERT_EQ(hm->getNumAdvantageouslyHedgedOperations(), 0);
}

/**
 * When a hedged command is sent and the first request, which is authoritative, fails
 * with an ignorable error and the second request, which is hedged, cancels, we get
 * the ignorable error.
 */
TEST_F(HedgedAsyncRPCTest, AuthoritativeFailsWithIgnorableErrorHedgedCancelled) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    // If the request is the authoritative one, send an "ignorable" error response,
    // otherwise send a delayed success response.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleResponse(authoritative, now, testIgnorableErrorResponse);
            network->scheduleSuccessfulResponse(
                hedged, now + Milliseconds(1000), testSuccessResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 1);

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), ignorableMaxTimeMSExpiredStatus);
}

/**
 * When a hedged command is sent and the first request, which is authoritative, fails
 * with a fatal error and the second request, which is hedged, cancels, we get
 * the fatal error.
 */
TEST_F(HedgedAsyncRPCTest, AuthoritativeFailsWithFatalErrorHedgedCancelled) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    // If the request is the authoritative one, send a "fatal" error response,
    // otherwise send a delayed success response.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleResponse(authoritative, now, testFatalErrorResponse);
            network->scheduleSuccessfulResponse(
                hedged, now + Milliseconds(1000), testSuccessResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 1);

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), fatalInternalErrorStatus);
}

/**
 * When a hedged command is sent and the first request, which is authoritative, succeeds
 * and the second request, which is hedged, cancels, we get the success result.
 */
TEST_F(HedgedAsyncRPCTest, AuthoritativeSucceedsHedgedCancelled) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    // If the request is the authoritative one, send a success response,
    // otherwise send a delayed "fatal" error response.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleSuccessfulResponse(authoritative, now, testSuccessResponse);
            network->scheduleSuccessfulResponse(
                hedged, now + Milliseconds(1000), testFatalErrorResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 1);

    auto res = std::move(resultFuture).get().response;
    ASSERT_EQ(res.getCursor()->getNs(), testNS);
    ASSERT_BSONOBJ_EQ(res.getCursor()->getFirstBatch()[0], testFirstBatch);
}

/**
 * When a hedged command is sent and the first request, which is hedged, succeeds
 * and the second request, which is authoritative, cancels, we get the success result.
 */
TEST_F(HedgedAsyncRPCTest, HedgedSucceedsAuthoritativeCancelled) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    // If the request is the authoritative one, send a delayed "fatal" error response,
    // otherwise send a success response.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleSuccessfulResponse(
                authoritative, now + Milliseconds(1000), testFatalErrorResponse);
            network->scheduleSuccessfulResponse(hedged, now, testSuccessResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 1);

    auto res = std::move(resultFuture).get().response;
    ASSERT_EQ(res.getCursor()->getNs(), testNS);
    ASSERT_BSONOBJ_EQ(res.getCursor()->getFirstBatch()[0], testFirstBatch);

    // Test metrics where hedged succeeds
    ASSERT_EQ(hm->getNumTotalOperations(), 1);
    ASSERT_EQ(hm->getNumTotalHedgedOperations(), 1);
    ASSERT_EQ(hm->getNumAdvantageouslyHedgedOperations(), 1);
}

/**
 * When a hedged command is sent and the first request, which is hedged, fails with an ignorable
 * error and the second request, which is authoritative, also fails with an ignorable error, we get
 * the ignorable error from the authoritative.
 */
TEST_F(HedgedAsyncRPCTest, HedgedThenAuthoritativeFailsWithIgnorableError) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    // Send an "ignorable" error response for both requests, but delay the response if the request
    // is the authoritative one. Different "ignorable" responses are used in order to verify that
    // the returned response corresponds to the authoritative request.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleResponse(
                authoritative, now + Milliseconds(1000), testIgnorableErrorResponse);
            network->scheduleResponse(hedged, now, testAlternateIgnorableErrorResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    // Any response received from a "remote" node, whether it contains the result of a successful
    // operation or otherwise resulting error, is considered a "success" by the network interface.
    ASSERT_EQ(counters.succeeded, 2);
    ASSERT_EQ(counters.canceled, 0);

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), ignorableMaxTimeMSExpiredStatus);
}

/**
 * When a hedged command is sent and the first request, which is hedged, fails with an ignorable
 * error and the second request, which is authoritative, fails with a fatal error, we get the fatal
 * error.
 */
TEST_F(HedgedAsyncRPCTest, HedgedFailsWithIgnorableErrorAuthoritativeFailsWithFatalError) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    // Schedules an ignorable error response for the hedged request, and then a delayed fatal error
    // response for the authoritative request.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleResponse(
                authoritative, now + Milliseconds(1000), testFatalErrorResponse);
            network->scheduleResponse(hedged, now, testIgnorableErrorResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    // Any response received from a "remote" node, whether it contains the result of a successful
    // operation or otherwise resulting error, is considered a "success" by the network interface.
    ASSERT_EQ(counters.succeeded, 2);
    ASSERT_EQ(counters.canceled, 0);

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), fatalInternalErrorStatus);
}

/**
 * When a hedged command is sent and the first request, which is hedged, fails with an ignorable
 * error and the second request, which is authoritative, succeeds, we get the success response.
 */
TEST_F(HedgedAsyncRPCTest, AuthoritativeSucceedsHedgedFailsWithIgnorableError) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    // Schedules an ignorable error response for the hedged request, and then a delayed success
    // response for the authoritative request.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleResponse(authoritative, now + Milliseconds(1000), testSuccessResponse);
            network->scheduleResponse(hedged, now, testIgnorableErrorResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    // Any response received from a "remote" node, whether it contains the result of a successful
    // operation or otherwise resulting error, is considered a "success" by the network interface.
    ASSERT_EQ(counters.succeeded, 2);
    ASSERT_EQ(counters.canceled, 0);

    auto res = std::move(resultFuture).get().response;
    ASSERT_EQ(res.getCursor()->getNs(), testNS);
    ASSERT_BSONOBJ_EQ(res.getCursor()->getFirstBatch()[0], testFirstBatch);
}

/**
 * When a hedged command is sent and the first request, which is hedged, fails with a fatal
 * error and the second request, which is authoritative, cancels, we get the fatal error.
 */
TEST_F(HedgedAsyncRPCTest, HedgedFailsWithFatalErrorAuthoritativeCanceled) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    // Schedules a fatal error response for the hedged request, and then a delayed success response
    // for the authoritative request.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleResponse(authoritative, now + Milliseconds(1000), testSuccessResponse);
            network->scheduleResponse(hedged, now, testFatalErrorResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    // Any response received from a "remote" node, whether it contains the result of a successful
    // operation or otherwise resulting error, is considered a "success" by the network interface.
    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 1);

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);

    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);

    ASSERT(extraInfo->isRemote());
    auto remoteError = extraInfo->asRemote();
    ASSERT_EQ(remoteError.getRemoteCommandResult(), fatalInternalErrorStatus);

    // Test metrics where hedged fails with fatal error
    ASSERT_EQ(hm->getNumTotalOperations(), 1);
    ASSERT_EQ(hm->getNumTotalHedgedOperations(), 1);
    ASSERT_EQ(hm->getNumAdvantageouslyHedgedOperations(), 1);
}

/*
 * Tests that 'sendHedgedCommand' will appropriately retry multiple times under the conditions
 * defined by the retry policy, with a dynmically changing wait-time between retries.
 */
TEST_F(HedgedAsyncRPCTest, DynamicDelayBetweenRetries) {
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    // Define a retry policy that simply decides to always retry a command three additional times,
    // with a different delay between each retry.
    std::shared_ptr<TestRetryPolicy> testPolicy = std::make_shared<TestRetryPolicy>();
    const auto maxNumRetries = 3;
    const std::array<Milliseconds, maxNumRetries> retryDelays{
        Milliseconds(100), Milliseconds(50), Milliseconds(10)};
    testPolicy->setMaxNumRetries(maxNumRetries);
    testPolicy->pushRetryDelay(retryDelays[0]);
    testPolicy->pushRetryDelay(retryDelays[1]);
    testPolicy->pushRetryDelay(retryDelays[2]);

    auto resultFuture = sendHedgedCommandWithHosts(helloCmd, kTwoHosts, testPolicy);

    const auto onCommandFunc = [&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
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

    // Wait until the RPC attempt is done, including all retries. Ignore the result.
    resultFuture.wait();

    ASSERT_EQ(maxNumRetries, testPolicy->getNumRetriesPerformed());
}

namespace m = unittest::match;
TEST_F(HedgedAsyncRPCTest, AttemptedTargetsPropogatedWithLocalErrors) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    // Respond to one of the RPCs composing the hedged command with a fatal local error.
    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return fatalInternalErrorStatus;
    });

    // Ensure that both of the hosts are recorded as attempted targets.
    auto error = resultFuture.getNoThrow().getStatus();
    auto info = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(info);
    ASSERT(info->isLocal());

    auto eitherHostMatcher = m::AnyOf(m::Eq(kTwoHosts[0]), m::Eq(kTwoHosts[1]));
    ASSERT_THAT(info->getTargetsAttempted(), m::ElementsAre(eitherHostMatcher, eitherHostMatcher));
}

TEST_F(HedgedAsyncRPCTest, NoAttemptedTargetIfTargetingFails) {
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    Status resolveErr{ErrorCodes::BadValue, "Failing resolve for test!"};
    auto targeter = std::make_unique<FailingTargeter>(resolveErr);


    auto resultFuture = sendHedgedCommand(
        helloCmd, getOpCtx(), std::move(targeter), getExecutorPtr(), _cancellationToken);

    auto error = resultFuture.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal(), resolveErr);
    auto targetsAttempted = extraInfo->getTargetsAttempted();
    ASSERT_EQ(targetsAttempted.size(), 0);
}

TEST_F(HedgedAsyncRPCTest, RemoteErrorAttemptedTargetsContainActual) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kTwoHosts);

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    // Schedules a fatal error response for the hedged request, and then a delayed success response
    // for the authoritative request.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged) {
            network->scheduleResponse(authoritative, now + Milliseconds(1000), testSuccessResponse);
            network->scheduleResponse(hedged, now, testFatalErrorResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    network->exitNetwork();

    // Ensure that both of the hosts are recorded as attempted targets.
    auto error = resultFuture.getNoThrow().getStatus();
    auto info = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(info);
    ASSERT(info->isRemote());
    auto remoteErr = info->asRemote();

    auto eitherHostMatcher = m::AnyOf(m::Eq(kTwoHosts[0]), m::Eq(kTwoHosts[1]));
    ASSERT_THAT(info->getTargetsAttempted(), m::ElementsAre(eitherHostMatcher, eitherHostMatcher));

    // Ensure that the actual host from the remote error is one of the attempted ones.
    ASSERT_THAT(remoteErr.getTargetUsed(), eitherHostMatcher);
}

TEST_F(HedgedAsyncRPCTest, BatonTest) {
    std::shared_ptr<RetryPolicy> retryPolicy = std::make_shared<NeverRetryPolicy>();
    auto resultFuture = sendHedgedCommandWithHosts(
        testFindCmd, kTwoHosts, retryPolicy, GenericArgs(), getOpCtx()->getBaton());

    Notification<void> seenNetworkRequests;
    // This thread will respond to the requests we sent via sendHedgedCommandWithHosts above.
    stdx::thread networkResponder([&] {
        auto network = getNetworkInterfaceMock();
        network->enterNetwork();
        auto now = network->now();
        performAuthoritativeHedgeBehavior(
            network,
            [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
                NetworkInterfaceMock::NetworkOperationIterator hedged) {
                network->scheduleResponse(hedged, now, testIgnorableErrorResponse);
                network->scheduleSuccessfulResponse(authoritative, now, testSuccessResponse);
                seenNetworkRequests.set();
            });
        network->runReadyNetworkOperations();
        network->exitNetwork();
    });
    // Wait on the opCtx until networkResponder has observed the network requests.
    // While we block on the opCtx, the current thread should run jobs scheduled
    // on the baton, including enqueuing the network requests via `sendHedgedCommand` above.
    seenNetworkRequests.get(getOpCtx());

    networkResponder.join();
    // Wait on the opCtx again to allow the current thread, via the baton, to propogate
    // the network responses up into the resultFuture.
    AsyncRPCResponse res = resultFuture.get(getOpCtx());

    ASSERT_EQ(res.response.getCursor()->getNs(), testNS);
    ASSERT_BSONOBJ_EQ(res.response.getCursor()->getFirstBatch()[0], testFirstBatch);
    namespace m = unittest::match;
    ASSERT_THAT(res.targetUsed, m::AnyOf(m::Eq(kTwoHosts[0]), m::Eq(kTwoHosts[1])));
}
TEST_F(HedgedAsyncRPCTest, BatonShutdownExecutorAlive) {
    auto retryPolicy = std::make_shared<TestRetryPolicy>();
    const auto maxNumRetries = 5;
    const auto retryDelay = Milliseconds(10);
    retryPolicy->setMaxNumRetries(maxNumRetries);
    for (int i = 0; i < maxNumRetries; ++i)
        retryPolicy->pushRetryDelay(retryDelay);
    auto subBaton = getOpCtx()->getBaton()->makeSubBaton();
    auto resultFuture =
        sendHedgedCommandWithHosts(testFindCmd, kTwoHosts, retryPolicy, GenericArgs(), *subBaton);

    subBaton.shutdown();
    auto net = getNetworkInterfaceMock();
    for (auto i = 0; i <= maxNumRetries; i++) {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->advanceTime(net->now() + retryDelay);
    }

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
}  // namespace
}  // namespace async_rpc
}  // namespace mongo
