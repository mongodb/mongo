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
#include "mongo/executor/network_interface.h"
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
    const std::vector<HostAndPort> kEmptyHosts{};
    const std::vector<HostAndPort> kTwoHosts{HostAndPort("FakeHost1", 12345),
                                             HostAndPort("FakeHost2", 12345)};
    const std::vector<HostAndPort> kThreeHosts{HostAndPort("FakeHost1", 12345),
                                               HostAndPort("FakeHost2", 12345),
                                               HostAndPort("FakeHost3", 12345)};

    using TwoHostCallback =
        std::function<void(NetworkInterfaceMock::NetworkOperationIterator authoritative,
                           NetworkInterfaceMock::NetworkOperationIterator hedged)>;
    using ThreeHostCallback =
        std::function<void(NetworkInterfaceMock::NetworkOperationIterator authoritative,
                           NetworkInterfaceMock::NetworkOperationIterator hedged1,
                           NetworkInterfaceMock::NetworkOperationIterator hedged2)>;

    NetworkInterface::Counters getNetworkInterfaceCounters() {
        auto counters = getNetworkInterfaceMock()->getCounters();
        return counters;
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

    void performAuthoritativeHedgeBehavior(NetworkInterfaceMock* network,
                                           ThreeHostCallback mockBehaviorFn) {
        std::vector<NetworkInterfaceMock::NetworkOperationIterator> nois{
            network->getNextReadyRequest(),
            network->getNextReadyRequest(),
            network->getNextReadyRequest()};

        NetworkInterfaceMock::NetworkOperationIterator authoritative;
        std::vector<NetworkInterfaceMock::NetworkOperationIterator> hedged;
        // Find authoritative and hedged NOIs.
        std::for_each(
            nois.begin(), nois.end(), [&](NetworkInterfaceMock::NetworkOperationIterator& noi) {
                if (noi->getRequestOnAny().target[0] == kThreeHosts[0]) {
                    authoritative = noi;
                } else {
                    hedged.push_back(noi);
                }
            });

        auto hedged1 = hedged.back();
        hedged.pop_back();
        auto hedged2 = hedged.back();

        mockBehaviorFn(authoritative, hedged1, hedged2);
    }

    /**
     * Testing wrapper to perform common set up, then call sendHedgedCommand. Only safe to call once
     * per test fixture as to not create multiple OpCtx.
     */
    template <typename CommandType>
    SemiFuture<AsyncRPCResponse<typename CommandType::Reply>> sendHedgedCommandWithHosts(
        CommandType cmd,
        std::vector<HostAndPort> hosts,
        std::shared_ptr<RetryPolicy> retryPolicy = std::make_shared<NeverRetryPolicy>()) {
        ReadPreferenceSetting readPref;

        auto factory = RemoteCommandTargeterFactoryMock();
        std::shared_ptr<RemoteCommandTargeter> t;
        t = factory.create(ConnectionString::forStandalones(hosts));
        auto targeterMock = RemoteCommandTargeterMock::get(t);
        targeterMock->setFindHostsReturnValue(hosts);

        std::unique_ptr<Targeter> targeter =
            std::make_unique<AsyncRemoteCommandTargeterAdapter>(readPref, t);

        _opCtx = makeOperationContext();
        return sendHedgedCommand(cmd,
                                 _opCtx.get(),
                                 std::move(targeter),
                                 getExecutorPtr(),
                                 CancellationToken::uncancelable(),
                                 retryPolicy);
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

private:
    // This OpCtx is used by sendHedgedCommandWithHosts and is initialized when the function is
    // first invoked and destroyed during fixture destruction.
    ServiceContext::UniqueOperationContext _opCtx;
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

TEST_F(HedgedAsyncRPCTest, FindHedgeRequestThreeHosts) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kThreeHosts);

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
    ASSERT_EQ(counters.canceled, 2);

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
 * When the targeter returns no hosts, we get a HostNotFound error.
 */
TEST_F(HedgedAsyncRPCTest, NoShardsFound) {
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kEmptyHosts);

    ASSERT_THROWS_CODE(resultFuture.get(), DBException, ErrorCodes::HostNotFound);
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
TEST_F(HedgedAsyncRPCTest, BothCommandsFailWithSkippableError) {
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

TEST_F(HedgedAsyncRPCTest, AllCommandsFailWithSkippableError) {
    auto resultFuture = sendHedgedCommandWithHosts(testFindCmd, kThreeHosts);

    auto network = getNetworkInterfaceMock();
    auto now = network->now();
    network->enterNetwork();

    // Send "ignorable" responses for all three requests.
    performAuthoritativeHedgeBehavior(
        network,
        [&](NetworkInterfaceMock::NetworkOperationIterator authoritative,
            NetworkInterfaceMock::NetworkOperationIterator hedged1,
            NetworkInterfaceMock::NetworkOperationIterator hedged2) {
            network->scheduleResponse(
                authoritative, now + Milliseconds(1000), testIgnorableErrorResponse);
            network->scheduleResponse(hedged1, now, testIgnorableErrorResponse);
            network->scheduleResponse(hedged2, now, testIgnorableErrorResponse);
        });

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    ASSERT_EQ(counters.succeeded, 3);
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
TEST_F(HedgedAsyncRPCTest, HedgedFailsWithSkippableErrorAuthoritativeSucceeds) {
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
TEST_F(HedgedAsyncRPCTest, FirstCommandFailsWithSkippableErrorNextSucceeds) {
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
TEST_F(HedgedAsyncRPCTest, HedgedFailsWithIgnorableErrorAuthoritativeSucceeds) {
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
}

}  // namespace
}  // namespace async_rpc
}  // namespace mongo
