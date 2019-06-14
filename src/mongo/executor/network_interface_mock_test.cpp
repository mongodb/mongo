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

#include "mongo/platform/basic.h"

#include <iostream>
#include <memory>
#include <utility>

#include "mongo/base/status.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/test_network_connection_hook.h"
#include "mongo/executor/thread_pool_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace executor {
namespace {

class NetworkInterfaceMockTest : public mongo::unittest::Test {
public:
    NetworkInterfaceMockTest()
        : _net{}, _executor(&_net, 1, ThreadPoolMock::Options()), _tearDownCalled(false) {}

    NetworkInterfaceMock& net() {
        return _net;
    }

    ThreadPoolMock& executor() {
        return _executor;
    }

    HostAndPort testHost() {
        return {"localHost", 27017};
    }

    // intentionally not done in setUp as some methods need to be called prior to starting
    // the network.
    void startNetwork() {
        net().startup();
        executor().startup();
    }

    virtual void setUp() override {
        _tearDownCalled = false;
    }

    virtual void tearDown() override {
        // We're calling tearDown() manually in some tests so
        // we can check post-conditions.
        if (_tearDownCalled) {
            return;
        }
        _tearDownCalled = true;

        net().exitNetwork();
        executor().shutdown();
        // Wake up sleeping executor threads so they clean up.
        net().signalWorkAvailable();
        executor().join();
        net().shutdown();
    }

    RemoteCommandRequestOnAny kUnimportantRequest{
        {testHost()}, "testDB", BSON("test" << 1), rpc::makeEmptyMetadata(), nullptr};

private:
    NetworkInterfaceMock _net;
    ThreadPoolMock _executor;
    bool _tearDownCalled;
};

TEST_F(NetworkInterfaceMockTest, ConnectionHook) {
    bool validateCalled = false;
    bool hostCorrectForValidate = false;
    bool replyCorrectForValidate;

    bool makeRequestCalled = false;
    bool hostCorrectForRequest = false;

    bool handleReplyCalled = false;
    bool gotExpectedReply = false;

    RemoteCommandRequest expectedRequest{testHost(),
                                         "test",
                                         BSON("1" << 2),
                                         BSON("some"
                                              << "stuff"),
                                         nullptr};

    RemoteCommandResponse expectedResponse{BSON("foo"
                                                << "bar"
                                                << "baz"
                                                << "garply"
                                                << "bar"
                                                << "baz"),
                                           Milliseconds(30)};

    // need to copy as it will be moved
    auto isMasterReplyData = BSON("iamyour"
                                  << "father");

    RemoteCommandResponse isMasterReply{isMasterReplyData.copy(), Milliseconds(20)};

    net().setHandshakeReplyForHost(testHost(), std::move(isMasterReply));

    // Since the contract of these methods is that they do not throw, we run the ASSERTs in
    // the test scope.
    net().setConnectionHook(makeTestHook(
        [&](const HostAndPort& remoteHost,
            const BSONObj&,
            const RemoteCommandResponse& isMasterReply) {
            validateCalled = true;
            hostCorrectForValidate = (remoteHost == testHost());
            replyCorrectForValidate = SimpleBSONObjComparator::kInstance.evaluate(
                isMasterReply.data == isMasterReplyData);
            return Status::OK();
        },
        [&](const HostAndPort& remoteHost) {
            makeRequestCalled = true;
            hostCorrectForRequest = (remoteHost == testHost());
            return boost::make_optional(expectedRequest);
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) {
            handleReplyCalled = true;
            hostCorrectForRequest = (remoteHost == testHost());
            gotExpectedReply = SimpleBSONObjComparator::kInstance.evaluate(
                expectedResponse.data == response.data);  // Don't bother checking all fields.
            return Status::OK();
        }));

    startNetwork();

    TaskExecutor::CallbackHandle cb{};

    bool commandFinished = false;
    bool gotCorrectCommandReply = false;

    RemoteCommandRequestOnAny actualCommandExpected{
        {testHost()}, "testDB", BSON("test" << 1), rpc::makeEmptyMetadata(), nullptr};
    RemoteCommandOnAnyResponse actualResponseExpected{testHost(),
                                                      BSON("1212121212"
                                                           << "12121212121212"),
                                                      Milliseconds(0)};

    ASSERT_OK(
        net().startCommand(cb, actualCommandExpected, [&](const RemoteCommandOnAnyResponse& resp) {
            commandFinished = true;
            if (resp.isOK()) {
                gotCorrectCommandReply = (actualResponseExpected == resp);
            }
        }));

    // At this point validate and makeRequest should have been called.
    ASSERT(validateCalled);
    ASSERT(hostCorrectForValidate);
    ASSERT(replyCorrectForValidate);
    ASSERT(makeRequestCalled);
    ASSERT(hostCorrectForRequest);

    // handleReply should not have been called as we haven't responded to the reply yet.
    ASSERT(!handleReplyCalled);
    // we haven't gotten to the actual command yet
    ASSERT(!commandFinished);

    {
        net().enterNetwork();
        ASSERT(net().hasReadyRequests());
        auto req = net().getNextReadyRequest();
        ASSERT_BSONOBJ_EQ(req->getRequest().cmdObj, expectedRequest.cmdObj);
        net().scheduleResponse(req, net().now(), expectedResponse);
        net().runReadyNetworkOperations();
        net().exitNetwork();
    }

    // We should have responded to the post connect command.
    ASSERT(handleReplyCalled);
    ASSERT(gotExpectedReply);

    // We should not have responsed to the actual command.
    ASSERT(!commandFinished);

    {
        net().enterNetwork();
        ASSERT(net().hasReadyRequests());
        auto actualCommand = net().getNextReadyRequest();
        ASSERT_BSONOBJ_EQ(actualCommand->getRequest().cmdObj, actualCommandExpected.cmdObj);
        net().scheduleResponse(actualCommand, net().now(), actualResponseExpected);
        net().runReadyNetworkOperations();
        net().exitNetwork();
    }

    ASSERT(commandFinished);
    ASSERT(gotCorrectCommandReply);
}

TEST_F(NetworkInterfaceMockTest, ConnectionHookFailedValidation) {
    net().setConnectionHook(makeTestHook(
        [&](const HostAndPort& remoteHost,
            const BSONObj&,
            const RemoteCommandResponse& isMasterReply) -> Status {
            // We just need some obscure non-OK code.
            return {ErrorCodes::ConflictingOperationInProgress, "blah"};
        },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            MONGO_UNREACHABLE;
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) -> Status {
            MONGO_UNREACHABLE;
        }));

    startNetwork();

    TaskExecutor::CallbackHandle cb{};

    bool commandFinished = false;
    bool statusPropagated = false;

    RemoteCommandRequestOnAny request{kUnimportantRequest};
    ASSERT_OK(net().startCommand(cb, request, [&](const RemoteCommandOnAnyResponse& resp) {
        commandFinished = true;

        statusPropagated = resp.status.code() == ErrorCodes::ConflictingOperationInProgress;
    }));

    {
        net().enterNetwork();
        // We should have short-circuited the network and immediately called the callback.
        // If we change isMaster replies to go through the normal network mechanism,
        // this test will need to change.
        ASSERT(!net().hasReadyRequests());
        net().exitNetwork();
    }

    ASSERT(commandFinished);
    ASSERT(statusPropagated);
}

TEST_F(NetworkInterfaceMockTest, ConnectionHookNoRequest) {
    bool makeRequestCalled = false;
    net().setConnectionHook(makeTestHook(
        [&](const HostAndPort& remoteHost,
            const BSONObj&,
            const RemoteCommandResponse& isMasterReply) -> Status { return Status::OK(); },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            makeRequestCalled = true;
            return {boost::none};
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) -> Status {
            MONGO_UNREACHABLE;
        }));

    startNetwork();

    TaskExecutor::CallbackHandle cb{};

    bool commandFinished = false;

    RemoteCommandRequestOnAny request{kUnimportantRequest};
    ASSERT_OK(net().startCommand(
        cb, request, [&](const RemoteCommandOnAnyResponse& resp) { commandFinished = true; }));

    {
        net().enterNetwork();
        ASSERT(net().hasReadyRequests());
        auto req = net().getNextReadyRequest();
        net().scheduleResponse(req, net().now(), RemoteCommandResponse{});
        net().runReadyNetworkOperations();
        net().exitNetwork();
    }

    ASSERT(commandFinished);
}

TEST_F(NetworkInterfaceMockTest, ConnectionHookMakeRequestFails) {
    bool makeRequestCalled = false;
    net().setConnectionHook(makeTestHook(
        [&](const HostAndPort& remoteHost,
            const BSONObj&,
            const RemoteCommandResponse& isMasterReply) -> Status { return Status::OK(); },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            makeRequestCalled = true;
            return {ErrorCodes::InvalidSyncSource, "blah"};
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) -> Status {
            MONGO_UNREACHABLE;
        }));

    startNetwork();

    TaskExecutor::CallbackHandle cb{};

    bool commandFinished = false;
    bool errorPropagated = false;

    RemoteCommandRequestOnAny request{kUnimportantRequest};
    ASSERT_OK(net().startCommand(cb, request, [&](const RemoteCommandOnAnyResponse& resp) {
        commandFinished = true;
        errorPropagated = resp.status.code() == ErrorCodes::InvalidSyncSource;
    }));

    {
        net().enterNetwork();
        ASSERT(!net().hasReadyRequests());
        net().exitNetwork();
    }

    ASSERT(commandFinished);
    ASSERT(errorPropagated);
}

TEST_F(NetworkInterfaceMockTest, ConnectionHookHandleReplyFails) {
    bool handleReplyCalled = false;
    net().setConnectionHook(makeTestHook(
        [&](const HostAndPort& remoteHost,
            const BSONObj&,
            const RemoteCommandResponse& isMasterReply) -> Status { return Status::OK(); },
        [&](const HostAndPort& remoteHost) -> StatusWith<boost::optional<RemoteCommandRequest>> {
            return boost::make_optional<RemoteCommandRequest>({});
        },
        [&](const HostAndPort& remoteHost, RemoteCommandResponse&& response) -> Status {
            handleReplyCalled = true;
            return {ErrorCodes::CappedPositionLost, "woot"};
        }));

    startNetwork();

    TaskExecutor::CallbackHandle cb{};

    bool commandFinished = false;
    bool errorPropagated = false;

    RemoteCommandRequestOnAny request{kUnimportantRequest};
    ASSERT_OK(net().startCommand(cb, request, [&](const RemoteCommandOnAnyResponse& resp) {
        commandFinished = true;
        errorPropagated = resp.status.code() == ErrorCodes::CappedPositionLost;
    }));

    ASSERT(!handleReplyCalled);

    {
        net().enterNetwork();
        ASSERT(net().hasReadyRequests());
        auto req = net().getNextReadyRequest();
        net().scheduleResponse(req, net().now(), RemoteCommandResponse{});
        net().runReadyNetworkOperations();
        net().exitNetwork();
    }

    ASSERT(handleReplyCalled);
    ASSERT(commandFinished);
    ASSERT(errorPropagated);
}

TEST_F(NetworkInterfaceMockTest, SetAlarmFires) {
    startNetwork();

    TaskExecutor::CallbackHandle cb{};
    bool alarmHasFired = false;

    const auto deadline = net().now() + Milliseconds(100);
    ASSERT_OK(net().setAlarm(cb, deadline, [&](Status) { alarmHasFired = true; }));
    ASSERT_FALSE(alarmHasFired);

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(&net());

        net().advanceTime(deadline);
        ASSERT_TRUE(alarmHasFired);
    }
}

TEST_F(NetworkInterfaceMockTest, SetAlarmCanceled) {
    startNetwork();

    TaskExecutor::CallbackHandle cb{};
    bool alarmHasFired = false;

    const auto deadline = net().now() + Milliseconds(100);
    ASSERT_OK(net().setAlarm(cb, deadline, [&](Status) { alarmHasFired = true; }));
    ASSERT_FALSE(alarmHasFired);

    net().cancelAlarm(cb);

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(&net());

        net().advanceTime(deadline);
        ASSERT_FALSE(alarmHasFired);
    }
}

TEST_F(NetworkInterfaceMockTest, InShutdown) {
    startNetwork();
    ASSERT_FALSE(net().inShutdown());
    tearDown();
    ASSERT(net().inShutdown());
}

TEST_F(NetworkInterfaceMockTest, StartCommandReturnsNotOKIfShutdownHasStarted) {
    startNetwork();
    tearDown();

    TaskExecutor::CallbackHandle cb{};
    RemoteCommandRequestOnAny request{kUnimportantRequest};
    ASSERT_NOT_OK(net().startCommand(cb, request, [](const RemoteCommandOnAnyResponse& resp) {}));
}

TEST_F(NetworkInterfaceMockTest, SetAlarmReturnsNotOKIfShutdownHasStarted) {
    startNetwork();
    tearDown();
    ASSERT_NOT_OK(net().setAlarm(
        TaskExecutor::CallbackHandle(), net().now() + Milliseconds(100), [](Status) {}));
}

TEST_F(NetworkInterfaceMockTest, CommandTimeout) {
    startNetwork();

    TaskExecutor::CallbackHandle cb;
    RemoteCommandRequestOnAny request{kUnimportantRequest};
    request.timeout = Milliseconds(2000);

    ErrorCodes::Error statusPropagated = ErrorCodes::OK;
    auto finishFn = [&](const RemoteCommandOnAnyResponse& resp) {
        statusPropagated = resp.status.code();
    };

    //
    // Command times out.
    //
    ASSERT_OK(net().startCommand(cb, request, finishFn));
    net().enterNetwork();
    ASSERT(net().hasReadyRequests());
    net().blackHole(net().getNextReadyRequest());
    net().runUntil(net().now() + Milliseconds(2010));
    net().exitNetwork();
    ASSERT_NOT_EQUALS(ErrorCodes::OK, statusPropagated);

    //
    // Command finishes before timeout.
    //
    Date_t start = net().now();

    ASSERT_OK(net().startCommand(cb, request, finishFn));
    net().enterNetwork();
    // Consume the request. We'll schedule a successful response later.
    ASSERT(net().hasReadyRequests());
    auto noi = net().getNextReadyRequest();

    // Assert the command hasn't timed out after 1000ms.
    net().runUntil(start + Milliseconds(1000));
    ASSERT_EQUALS(start + Milliseconds(1000), net().now());
    ASSERT_NOT_EQUALS(ErrorCodes::OK, statusPropagated);
    // Reply with a successful response.
    net().scheduleResponse(noi, net().now(), {});
    net().runReadyNetworkOperations();
    net().exitNetwork();
    ASSERT_EQUALS(ErrorCodes::OK, statusPropagated);
    ASSERT_EQUALS(start + Milliseconds(1000), net().now());
}


}  // namespace
}  // namespace executor
}  // namespace mongo
