// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/executor/mock_async_rpc.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/oid.h"
#include "mongo/client/retry_strategy.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/commands.h"
#include "mongo/db/repl/hello/hello_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_rpc.h"
#include "mongo/executor/async_rpc_targeter.h"
#include "mongo/executor/async_rpc_test_fixture.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <string_view>

#include <absl/container/flat_hash_set.h>
#include <boost/move/utility_core.hpp>

namespace mongo::async_rpc {
namespace {
using namespace std::literals::string_view_literals;

/**
 * This test fixture is used to test the functionality of the mocks, rather than test any facilities
 * or usage of the AsyncRPCRunner implementation.
 */
template <typename MockType>
class MockAsyncRPCRunnerTestFixture : public AsyncRPCTestFixture {
public:
    void setUp() override {
        AsyncRPCTestFixture::setUp();
        auto uniqueMock = std::make_unique<MockType>();
        _mock = uniqueMock.get();
        detail::AsyncRPCRunner::set(getServiceContext(), std::move(uniqueMock));
    }

    void tearDown() override {
        detail::AsyncRPCRunner::set(getServiceContext(), nullptr);
        AsyncRPCTestFixture::tearDown();
    }

    MockType& getMockRunner() {
        return *_mock;
    }


    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> sendHelloCommandToHostAndPort(
        HostAndPort target,
        std::shared_ptr<mongo::RetryStrategy> retryStrategy = std::make_shared<NoRetryStrategy>()) {
        HelloCommand hello;
        initializeCommand(hello);
        auto options = std::make_shared<AsyncRPCOptions<HelloCommand>>(
            getExecutorPtr(), _cancellationToken, hello, retryStrategy);
        return sendCommand(options, _opCtx.get(), std::make_unique<FixedTargeter>(target));
    }

    ExecutorFuture<AsyncRPCResponse<HelloCommandReply>> sendHelloCommandToLocalHost() {
        return sendHelloCommandToHostAndPort(getLocalHost());
    }

    HostAndPort getLocalHost() {
        return {"localhost", serverGlobalParams.port};
    }

private:
    MockType* _mock;
    ServiceContext::UniqueOperationContext _opCtx = makeOperationContext();
};

auto extractUUID(const BSONElement& element) {
    return UUID::fromCDR(element.uuid());
}

auto getOpKeyFromCommand(const BSONObj& cmdObj) {
    return extractUUID(cmdObj["clientOperationKey"]);
}

using SyncMockAsyncRPCRunnerTestFixture = MockAsyncRPCRunnerTestFixture<SyncMockAsyncRPCRunner>;
using AsyncMockAsyncRPCRunnerTestFixture = MockAsyncRPCRunnerTestFixture<AsyncMockAsyncRPCRunner>;

// A simple test showing that an arbitrary mock result can be set for a command scheduled through
// the AsyncRPCRunner.
TEST_F(SyncMockAsyncRPCRunnerTestFixture, RemoteSuccess) {
    auto responseFuture = sendHelloCommandToLocalHost();

    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    BSONObjBuilder result(helloReply.toBSON());
    CommandHelpers::appendCommandStatusNoThrow(result, Status::OK());
    auto expectedResultObj = result.obj();

    auto& request = getMockRunner().getNextRequest();
    ASSERT_FALSE(responseFuture.isReady());

    request.respondWith(expectedResultObj);
    auto actualResult = responseFuture.get();
    HostAndPort localhost = HostAndPort("localhost", serverGlobalParams.port);
    ASSERT_EQ(actualResult.targetUsed, localhost);
    ASSERT_BSONOBJ_EQ(actualResult.response.toBSON(), helloReply.toBSON());
}

TEST_F(SyncMockAsyncRPCRunnerTestFixture, RemoteError) {
    std::string_view exampleErrMsg{"example error message"};
    auto exampleErrCode = ErrorCodes::ShutdownInProgress;
    ErrorReply errorReply;
    errorReply.setOk(0);
    errorReply.setCode(exampleErrCode);
    errorReply.setCodeName(ErrorCodes::errorString(exampleErrCode));
    errorReply.setErrmsg(exampleErrMsg);

    auto responseFuture = sendHelloCommandToLocalHost();

    auto& request = getMockRunner().getNextRequest();
    request.respondWith(errorReply.toBSON());

    auto check = [&](const DBException& ex) {
        ASSERT_EQ(ex.code(), ErrorCodes::RemoteCommandExecutionError) << ex.toString();
        auto extraInfo = ex.extraInfo<AsyncRPCErrorInfo>();
        ASSERT(extraInfo);

        ASSERT(extraInfo->isRemote());
        auto remoteError = extraInfo->asRemote();
        ASSERT_EQ(remoteError.getRemoteCommandResult().code(), exampleErrCode);
        ASSERT_EQ(remoteError.getRemoteCommandResult().reason(), exampleErrMsg);

        // Ensure the targetAttempted/used portions of the error is populated correctly.
        auto targetAttempted = extraInfo->getTargetAttempted();
        ASSERT(targetAttempted);
        ASSERT_EQ(*targetAttempted, getLocalHost());
        ASSERT_EQ(remoteError.getTargetUsed(), getLocalHost());
    };
    // Ensure we fail to parse the reply due to the unknown fields.
    ASSERT_THROWS_WITH_CHECK(responseFuture.get(), DBException, check);
}

TEST_F(SyncMockAsyncRPCRunnerTestFixture, LocalError) {
    auto responseFuture = sendHelloCommandToLocalHost();
    auto& request = getMockRunner().getNextRequest();
    ASSERT_FALSE(responseFuture.isReady());
    auto exampleLocalErr = Status{ErrorCodes::InterruptedAtShutdown, "example local error"};
    request.respondWith(exampleLocalErr);
    auto error = responseFuture.getNoThrow().getStatus();
    // The error returned by our API should always be RemoteCommandExecutionError.
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    // Make sure we can extract the extra error info.
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    // Make sure the extra info indicates the error was local, and that the
    // local error (which is just a Status) is the one we provided.
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal(), exampleLocalErr);
    // Ensure the targetAttempted portion of the error is populated correctly.
    auto targetAttempted = extraInfo->getTargetAttempted();
    ASSERT(targetAttempted);
    ASSERT_EQ(*targetAttempted, getLocalHost());
}

TEST_F(SyncMockAsyncRPCRunnerTestFixture, MultipleResponses) {
    auto responseOneFut = sendHelloCommandToLocalHost();
    ASSERT_FALSE(responseOneFut.isReady());
    auto& request = getMockRunner().getNextRequest();
    auto responseTwoFut = sendHelloCommandToLocalHost();
    ASSERT_FALSE(responseTwoFut.isReady());

    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    BSONObjBuilder result(helloReply.toBSON());
    CommandHelpers::appendCommandStatusNoThrow(result, Status::OK());
    auto expectedResultObj = result.obj();

    request.respondWith(expectedResultObj);
    auto responseOne = responseOneFut.get();
    HostAndPort localhost = HostAndPort("localhost", serverGlobalParams.port);
    ASSERT_EQ(responseOne.targetUsed, localhost);
    ASSERT_BSONOBJ_EQ(responseOne.response.toBSON(), helloReply.toBSON());

    auto& requestTwo = getMockRunner().getNextRequest();
    ASSERT_FALSE(responseTwoFut.isReady());
    auto exampleLocalErr = Status{ErrorCodes::InterruptedAtShutdown, "example local error"};
    requestTwo.respondWith(exampleLocalErr);
    auto error = responseTwoFut.getNoThrow().getStatus();
    ASSERT_EQ(error.code(), ErrorCodes::RemoteCommandExecutionError);
    auto extraInfo = error.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(extraInfo);
    ASSERT(extraInfo->isLocal());
    ASSERT_EQ(extraInfo->asLocal(), exampleLocalErr);
}

TEST_F(SyncMockAsyncRPCRunnerTestFixture, OnCommand) {
    auto responseFut = sendHelloCommandToLocalHost();

    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    BSONObjBuilder result(helloReply.toBSON());
    CommandHelpers::appendCommandStatusNoThrow(result, Status::OK());
    auto expectedResultObj = result.obj();

    HelloCommand hello;
    initializeCommand(hello);

    getMockRunner().onCommand([&](const RequestInfo& ri) {
        // OperationKey not provided, so internally created OperationKey must be extracted to make
        // this assertion valid
        auto expected = hello;
        hello.setClientOperationKey(getOpKeyFromCommand(ri._cmd));
        ASSERT_BSONOBJ_EQ(hello.toBSON(), ri._cmd);
        return expectedResultObj;
    });
    ASSERT_BSONOBJ_EQ(responseFut.get().response.toBSON(), helloReply.toBSON());
}

TEST_F(SyncMockAsyncRPCRunnerTestFixture, SyncMockAsyncRPCRunnerWithRetryStrategy) {
    const auto retryStrategy = std::make_shared<TestRetryStrategy>();
    const auto maxNumRetries = 1;
    const auto retryDelay = Milliseconds(100);
    retryStrategy->setMaxNumRetries(maxNumRetries);
    retryStrategy->pushRetryDelay(retryDelay);
    auto responseFut =
        sendHelloCommandToHostAndPort({"localhost", serverGlobalParams.port}, retryStrategy);

    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    BSONObjBuilder resultError(helloReply.toBSON());
    BSONObjBuilder resultOK(helloReply.toBSON());
    CommandHelpers::appendCommandStatusNoThrow(
        resultError, Status(ErrorCodes::Overflow, "test error code for retry"));
    CommandHelpers::appendCommandStatusNoThrow(resultOK, Status::OK());
    auto expectedResultErrorObj = resultError.obj();
    auto expectedResultOKObj = resultOK.obj();

    HelloCommand hello;
    initializeCommand(hello);

    getMockRunner().onCommand([&](const RequestInfo& ri) {
        auto expected = hello;
        hello.setClientOperationKey(getOpKeyFromCommand(ri._cmd));
        ASSERT_BSONOBJ_EQ(hello.toBSON(), ri._cmd);
        return expectedResultErrorObj;
    });
    auto net = getNetworkInterfaceMock();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->advanceTime(net->now() + retryDelay);
    }

    getMockRunner().onCommand([&](const RequestInfo& ri) {
        auto expected = hello;
        hello.setClientOperationKey(getOpKeyFromCommand(ri._cmd));
        ASSERT_BSONOBJ_EQ(hello.toBSON(), ri._cmd);
        return expectedResultOKObj;
    });
    ASSERT_BSONOBJ_EQ(responseFut.get().response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(maxNumRetries, retryStrategy->getNumRetriesPerformed());
}

// A simple test showing that we can asynchronously register an expectation
// that a request will eventually be scheduled with the mock before a request
// actually arrives. Then, once the request is scheduled, we are asynchronously
// notified of the request and can schedule a response to it.
TEST_F(AsyncMockAsyncRPCRunnerTestFixture, Expectation) {
    // We expect that some code will use the runner to send a hello
    // to localhost on "testdb".
    auto matcher = [](const AsyncMockAsyncRPCRunner::Request& req) {
        bool isHello = req.cmdBSON.firstElementFieldName() == "hello"sv;
        bool isRightTarget = req.target == HostAndPort("localhost", serverGlobalParams.port);
        return isHello && isRightTarget;
    };
    // Register our expectation and ensure it isn't yet met.
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    BSONObjBuilder result(helloReply.toBSON());
    CommandHelpers::appendCommandStatusNoThrow(result, Status::OK());

    auto expectation = getMockRunner().expect(matcher, result.obj(), "example expectation");
    ASSERT_FALSE(expectation.isReady());

    // Allow a request to be scheduled on the mock.
    auto response = sendHelloCommandToLocalHost();

    // Now, our expectation should be met, and the response to it provided.
    auto reply = response.get();
    expectation.get();
    ASSERT_BSONOBJ_EQ(reply.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), reply.targetUsed);
}

TEST_F(AsyncMockAsyncRPCRunnerTestFixture, ExpectLocalError) {
    // We expect that some code will use the runner to send a hello
    // to localhost on "testdb".
    auto matcher = [](const AsyncMockAsyncRPCRunner::Request& req) {
        bool isHello = req.cmdBSON.firstElementFieldName() == "hello"sv;
        bool isRightTarget = req.target == HostAndPort("localhost", serverGlobalParams.port);
        return isHello && isRightTarget;
    };

    auto exampleLocalErr = Status{ErrorCodes::InterruptedAtShutdown, "example local error"};
    auto expectation = getMockRunner().expect(matcher, exampleLocalErr, "example expectation");
    ASSERT_FALSE(expectation.isReady());

    // Allow a request to be scheduled on the mock.
    auto response = sendHelloCommandToLocalHost();

    // Now, our expectation should be met, and the response to it provided.
    auto reply = response.getNoThrow();
    expectation.get();
    auto err = reply.getStatus();
    auto info = err.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(info->isLocal());
    ASSERT_EQ(info->asLocal(), exampleLocalErr);
    // Ensure the targetAttempted portion of the error is populated correctly.
    auto targetAttempted = info->getTargetAttempted();
    ASSERT(targetAttempted);
    ASSERT_EQ(*targetAttempted, getLocalHost());
}

TEST_F(AsyncMockAsyncRPCRunnerTestFixture, ExpectRemoteError) {
    std::string_view exampleErrMsg{"example error message"};
    auto exampleErrCode = ErrorCodes::ShutdownInProgress;
    ErrorReply errorReply;
    errorReply.setOk(0);
    errorReply.setCode(exampleErrCode);
    errorReply.setCodeName(ErrorCodes::errorString(exampleErrCode));
    errorReply.setErrmsg(exampleErrMsg);
    // We expect that some code will use the runner to send a hello
    // to localhost on "testdb".
    auto matcher = [](const AsyncMockAsyncRPCRunner::Request& req) {
        bool isHello = req.cmdBSON.firstElementFieldName() == "hello"sv;
        bool isRightTarget = req.target == HostAndPort("localhost", serverGlobalParams.port);
        return isHello && isRightTarget;
    };

    auto expectation = getMockRunner().expect(matcher, errorReply.toBSON(), "example expectation");
    ASSERT_FALSE(expectation.isReady());

    // Allow a request to be scheduled on the mock.
    auto response = sendHelloCommandToLocalHost();

    // Now, our expectation should be met, and the response to it provided.
    auto reply = response.getNoThrow();
    expectation.get();
    auto err = reply.getStatus();
    auto info = err.extraInfo<AsyncRPCErrorInfo>();
    ASSERT(info->isRemote());
    auto remoteErr = info->asRemote();
    ASSERT_BSONOBJ_EQ(remoteErr.getResponseObj(), errorReply.toBSON());
    // Ensure the targetAttempted portion of the error is populated correctly.
    auto targetAttempted = info->getTargetAttempted();
    ASSERT(targetAttempted);
    ASSERT_EQ(*targetAttempted, getLocalHost());
    ASSERT_EQ(remoteErr.getTargetUsed(), getLocalHost());
}

TEST_F(AsyncMockAsyncRPCRunnerTestFixture, AsyncMockAsyncRPCRunnerWithRetryStrategy) {
    // We expect that some code will use the runner to send a hello
    // to localhost on "testdb".
    auto matcher = [](const AsyncMockAsyncRPCRunner::Request& req) {
        bool isHello = req.cmdBSON.firstElementFieldName() == "hello"sv;
        bool isRightTarget = req.target == HostAndPort("localhost", serverGlobalParams.port);
        return isHello && isRightTarget;
    };

    // Register our expectation and ensure it isn't yet met.
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    BSONObjBuilder firstResult(helloReply.toBSON());
    CommandHelpers::appendCommandStatusNoThrow(
        firstResult, Status(ErrorCodes::Overflow, "test error code for retry"));
    BSONObjBuilder secondResult(helloReply.toBSON());
    CommandHelpers::appendCommandStatusNoThrow(secondResult, Status::OK());

    auto firstExpectation =
        getMockRunner().expect(matcher, firstResult.obj(), "example expectation 1");
    auto secondExpectation =
        getMockRunner().expect(matcher, secondResult.obj(), "example expectation 2");
    ASSERT_FALSE(firstExpectation.isReady());
    ASSERT_FALSE(secondExpectation.isReady());

    const auto retryStrategy = std::make_shared<TestRetryStrategy>();
    const auto maxNumRetries = 1;
    const auto retryDelay = Milliseconds(100);
    retryStrategy->setMaxNumRetries(maxNumRetries);
    retryStrategy->pushRetryDelay(retryDelay);
    // Allow a request to be scheduled on the mock.
    auto response =
        sendHelloCommandToHostAndPort({"localhost", serverGlobalParams.port}, retryStrategy);

    // Now, our first expectation should be met.
    firstExpectation.get();

    // Advance the network clock to simulate the delay between retries so the second expectation
    // will be met.
    auto net = getNetworkInterfaceMock();
    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(net);
        net->advanceTime(net->now() + retryDelay);
    }
    auto reply = response.get();
    // The second expectation should be met.
    secondExpectation.get();
    ASSERT_BSONOBJ_EQ(reply.response.toBSON(), helloReply.toBSON());
    ASSERT_EQ(HostAndPort("localhost", serverGlobalParams.port), reply.targetUsed);
    ASSERT_EQ(maxNumRetries, retryStrategy->getNumRetriesPerformed());
}

// A more complicated test that registers several expectations, and then
// schedules the requests that match them and their responses out-of-order.
// Demonstrates how we can register expectations on the mock for events in an
// unordered way.
TEST_F(AsyncMockAsyncRPCRunnerTestFixture, SeveralExpectations) {
    HostAndPort targetOne("FakeHost1", 12345);
    HostAndPort targetTwo("FakeHost2", 12345);
    HostAndPort targetThree("FakeHost3", 12345);

    auto matcherOne = [&](const AsyncMockAsyncRPCRunner::Request& req) {
        return (req.cmdBSON.firstElementFieldName() == "hello"sv) && (req.target == targetOne);
    };
    auto matcherTwo = [&](const AsyncMockAsyncRPCRunner::Request& req) {
        return (req.cmdBSON.firstElementFieldName() == "hello"sv) && (req.target == targetTwo);
    };
    auto matcherThree = [&](const AsyncMockAsyncRPCRunner::Request& req) {
        return (req.cmdBSON.firstElementFieldName() == "hello"sv) && (req.target == targetThree);
    };

    // Create three expectations
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    BSONObjBuilder result(helloReply.toBSON());
    CommandHelpers::appendCommandStatusNoThrow(result, Status::OK());
    auto resultObj = result.obj();
    auto e1 = getMockRunner().expect(matcherOne, resultObj, "expectation one");
    auto e2 = getMockRunner().expect(matcherTwo, resultObj, "expectation two");
    auto e3 = getMockRunner().expect(matcherThree, resultObj, "expectation three");

    ASSERT_FALSE(e1.isReady());
    ASSERT_FALSE(e2.isReady());
    ASSERT_FALSE(e3.isReady());

    // Send requests corresponding to expectations `e3` and `e2`, but not as`e1`.
    auto r3 = sendHelloCommandToHostAndPort(targetThree);
    auto r2 = sendHelloCommandToHostAndPort(targetTwo);
    e3.get();
    e2.get();
    ASSERT_FALSE(e1.isReady());
    // Make sure the correct responses were sent.
    auto assertResponseMatches = [&](AsyncRPCResponse<HelloCommandReply> reply,
                                     const HostAndPort& correctTarget) {
        ASSERT_EQ(correctTarget, reply.targetUsed);
        ASSERT_BSONOBJ_EQ(reply.response.toBSON(), helloReply.toBSON());
    };
    assertResponseMatches(r3.get(), targetThree);
    assertResponseMatches(r2.get(), targetTwo);

    // Now, send a request matching `e1` as well.
    auto r1 = sendHelloCommandToHostAndPort(targetOne);
    assertResponseMatches(r1.get(), targetOne);
    e1.get();
}

TEST_F(AsyncMockAsyncRPCRunnerTestFixture, UnexpectedRequests) {
    auto responseFut = sendHelloCommandToLocalHost();
    ASSERT_EQ(responseFut.getNoThrow().getStatus().extraInfo<AsyncRPCErrorInfo>()->asLocal(),
              Status(ErrorCodes::InternalErrorNotSupported, "Unexpected request"));
    ASSERT(getMockRunner().hadUnexpectedRequests());
    auto unexpectedRequests = getMockRunner().getUnexpectedRequests();
    ASSERT_EQ(unexpectedRequests.size(), 1);
    HelloCommand hello;
    initializeCommand(hello);
    hello.setClientOperationKey(getOpKeyFromCommand(unexpectedRequests[0].cmdBSON));
    ASSERT_BSONOBJ_EQ(unexpectedRequests[0].cmdBSON, hello.toBSON());
    ASSERT_EQ(unexpectedRequests[0].dbName, "testdb"sv);
    HostAndPort localhost = HostAndPort("localhost", serverGlobalParams.port);
    ASSERT_EQ(unexpectedRequests[0].target, localhost);
    // Note that unexpected requests are BSON-convertable and can be printed as extended JSON.
    // For example, if you wanted to fail the test if any unexpected requests were found, and
    // print out the first such offending request, you could simply do:
    // ASSERT(!getMockRunner().hadUnexpectedRequests())
    //      << "but found: " << optional_io::Extension{getMockRunner().getFirstUnexpectedRequest()};
    // (This is a live example, feel free to uncomment and try it).
}

TEST_F(AsyncMockAsyncRPCRunnerTestFixture, UnmetExpectations) {
    HostAndPort theTarget("FakeHost1", 12345);
    auto matcher = [&](const AsyncMockAsyncRPCRunner::Request& req) {
        return (req.cmdBSON.firstElementFieldName() == "hello"sv) && (req.target == theTarget);
    };
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    BSONObjBuilder result(helloReply.toBSON());
    CommandHelpers::appendCommandStatusNoThrow(result, Status::OK());
    auto resultObj = result.obj();
    auto expectation = getMockRunner().expect(matcher, resultObj, "unmet expectation");

    ASSERT(getMockRunner().hasUnmetExpectations());
    auto unmetExpectations = getMockRunner().getUnmetExpectations();
    ASSERT_EQ(unmetExpectations.size(), 1);
    ASSERT(unmetExpectations.contains("unmet expectation"));
    // Note that unmet expectations all have string names and can be printed.
    // For example, if you wanted to fail the test if any unmet expectations were found, and
    // print out the first such offending expectation , you could simply do:
    // ASSERT(!getMockRunner().hasUnmetExpectations())
    //      << optional_io::Extension{getMockRunner().getFirstUnmetExpectation()};
    // (This is a live example, feel free to uncomment and try it).
}
}  // namespace
}  // namespace mongo::async_rpc
