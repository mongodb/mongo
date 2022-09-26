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
#include "mongo/executor/mock_remote_command_runner.h"

#include <memory>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/repl/hello_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_runner.h"
#include "mongo/executor/remote_command_runner_test_fixture.h"
#include "mongo/executor/remote_command_targeter.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/debugger.h"
#include "mongo/util/optional_util.h"

namespace mongo::executor::remote_command_runner {
namespace {

/**
 * This test fixture is used to test the functionality of the mocks, rather than test any facilities
 * or usage of the RemoteCommandRunner implementation.
 */
template <typename MockType>
class MockRemoteCommandRunnerTestFixture : public RemoteCommandRunnerTestFixture {
public:
    void setUp() override {
        RemoteCommandRunnerTestFixture::setUp();
        auto uniqueMock = std::make_unique<MockType>();
        _mock = uniqueMock.get();
        detail::RemoteCommandRunner::set(getServiceContext(), std::move(uniqueMock));
    }

    void tearDown() override {
        detail::RemoteCommandRunner::set(getServiceContext(), nullptr);
        RemoteCommandRunnerTestFixture::tearDown();
    }

    MockType& getMockRunner() {
        return *_mock;
    }


    SemiFuture<RemoteCommandRunnerResponse<HelloCommandReply>> sendHelloCommandToHostAndPort(
        HostAndPort target) {
        HelloCommand hello;
        initializeCommand(hello);
        auto opCtxHolder = makeOperationContext();
        return doRequest(hello,
                         opCtxHolder.get(),
                         std::make_unique<RemoteCommandFixedTargeter>(target),
                         getExecutorPtr(),
                         _cancellationToken);
    }

    SemiFuture<RemoteCommandRunnerResponse<HelloCommandReply>> sendHelloCommandToLocalHost() {
        return sendHelloCommandToHostAndPort({"localhost", serverGlobalParams.port});
    }

private:
    MockType* _mock;
};

using SyncMockRemoteCommandRunnerTestFixture =
    MockRemoteCommandRunnerTestFixture<SyncMockRemoteCommandRunner>;
using AsyncMockRemoteCommandRunnerTestFixture =
    MockRemoteCommandRunnerTestFixture<AsyncMockRemoteCommandRunner>;

// A simple test showing that an arbitrary mock result can be set for a command scheduled through
// the RemoteCommandRunner.
TEST_F(SyncMockRemoteCommandRunnerTestFixture, RemoteSuccess) {
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

TEST_F(SyncMockRemoteCommandRunnerTestFixture, RemoteError) {
    StringData exampleErrMsg{"example error message"};
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
        auto extraInfo = ex.extraInfo<RemoteCommandExecutionErrorInfo>();
        ASSERT(extraInfo);

        ASSERT(extraInfo->isRemote());
        auto remoteError = extraInfo->asRemote();
        ASSERT_EQ(remoteError.getRemoteCommandResult().code(), exampleErrCode);
        ASSERT_EQ(remoteError.getRemoteCommandResult().reason(), exampleErrMsg);
    };
    // Ensure we fail to parse the reply due to the unknown fields.
    ASSERT_THROWS_WITH_CHECK(responseFuture.get(), DBException, check);
}

TEST_F(SyncMockRemoteCommandRunnerTestFixture, LocalError) {
    auto responseFuture = sendHelloCommandToLocalHost();
    auto& request = getMockRunner().getNextRequest();
    ASSERT_FALSE(responseFuture.isReady());
    auto exampleLocalErr = Status{ErrorCodes::InterruptedAtShutdown, "example local error"};
    request.respondWith(exampleLocalErr);
    ASSERT_EQ(responseFuture.getNoThrow().getStatus(), exampleLocalErr);
}

TEST_F(SyncMockRemoteCommandRunnerTestFixture, MultipleResponses) {
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
    ASSERT_EQ(responseTwoFut.getNoThrow().getStatus(), exampleLocalErr);
}

TEST_F(SyncMockRemoteCommandRunnerTestFixture, OnCommand) {
    auto responseFut = sendHelloCommandToLocalHost();

    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    BSONObjBuilder result(helloReply.toBSON());
    CommandHelpers::appendCommandStatusNoThrow(result, Status::OK());
    auto expectedResultObj = result.obj();

    HelloCommand hello;
    initializeCommand(hello);

    getMockRunner().onCommand([&](const RequestInfo& ri) {
        ASSERT_BSONOBJ_EQ(hello.toBSON({}), ri._cmd);
        return expectedResultObj;
    });
    ASSERT_BSONOBJ_EQ(responseFut.get().response.toBSON(), helloReply.toBSON());
}

// A simple test showing that we can asynchronously register an expectation
// that a request will eventually be scheduled with the mock before a request
// actually arrives. Then, once the request is scheduled, we are asynchronously
// notified of the request and can schedule a response to it.
TEST_F(AsyncMockRemoteCommandRunnerTestFixture, Expectation) {
    // We expect that some code will use the runner to send a hello
    // to localhost on "testdb"
    auto matcher = [](const AsyncMockRemoteCommandRunner::Request& req) {
        bool isHello = req.cmdBSON.firstElementFieldName() == "hello"_sd;
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

// A more complicated test that registers several expectations, and then
// schedules the requests that match them and their responses out-of-order.
// Demonstrates how we can register expectations on the mock for events in an
// unordered way.
TEST_F(AsyncMockRemoteCommandRunnerTestFixture, SeveralExpectations) {
    HostAndPort targetOne("FakeHost1", 12345);
    HostAndPort targetTwo("FakeHost2", 12345);
    HostAndPort targetThree("FakeHost3", 12345);

    auto matcherOne = [&](const AsyncMockRemoteCommandRunner::Request& req) {
        return (req.cmdBSON.firstElementFieldName() == "hello"_sd) && (req.target == targetOne);
    };
    auto matcherTwo = [&](const AsyncMockRemoteCommandRunner::Request& req) {
        return (req.cmdBSON.firstElementFieldName() == "hello"_sd) && (req.target == targetTwo);
    };
    auto matcherThree = [&](const AsyncMockRemoteCommandRunner::Request& req) {
        return (req.cmdBSON.firstElementFieldName() == "hello"_sd) && (req.target == targetThree);
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
    auto assertResponseMatches = [&](RemoteCommandRunnerResponse<HelloCommandReply> reply,
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

TEST_F(AsyncMockRemoteCommandRunnerTestFixture, UnexpectedRequests) {
    auto responseFut = sendHelloCommandToLocalHost();
    ASSERT_EQ(responseFut.getNoThrow()
                  .getStatus()
                  .extraInfo<RemoteCommandExecutionErrorInfo>()
                  ->asLocal(),
              Status(ErrorCodes::InternalErrorNotSupported, "Unexpected request"));
    ASSERT(getMockRunner().hadUnexpectedRequests());
    auto unexpectedRequests = getMockRunner().getUnexpectedRequests();
    ASSERT_EQ(unexpectedRequests.size(), 1);
    HelloCommand hello;
    initializeCommand(hello);
    ASSERT_BSONOBJ_EQ(unexpectedRequests[0].cmdBSON, hello.toBSON({}));
    ASSERT_EQ(unexpectedRequests[0].dbName, "testdb"_sd);
    HostAndPort localhost = HostAndPort("localhost", serverGlobalParams.port);
    ASSERT_EQ(unexpectedRequests[0].target, localhost);
    // Note that unexpected requests are BSON-convertable and can be printed as extended JSON.
    // For example, if you wanted to fail the test if any unexpected requests were found, and
    // print out the first such offending request, you could simply do:
    // ASSERT(!getMockRunner().hadUnexpectedRequests())
    //      << "but found: " << optional_io::Extension{getMockRunner().getFirstUnexpectedRequest()};
    // (This is a live example, feel free to uncomment and try it).
}

TEST_F(AsyncMockRemoteCommandRunnerTestFixture, UnmetExpectations) {
    HostAndPort theTarget("FakeHost1", 12345);
    auto matcher = [&](const AsyncMockRemoteCommandRunner::Request& req) {
        return (req.cmdBSON.firstElementFieldName() == "hello"_sd) && (req.target == theTarget);
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
}  // namespace mongo::executor::remote_command_runner
