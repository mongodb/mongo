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

#include "mongo/executor/network_interface_mock.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/executor/network_connection_hook.h"
#include "mongo/executor/network_interface_mock_test_fixture.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/test_network_connection_hook.h"
#include "mongo/rpc/metadata.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/notification.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <list>
#include <memory>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {
namespace executor {
namespace {

TEST_F(NetworkInterfaceMockTest, ConnectionHook) {
    bool validateCalled = false;
    bool hostCorrectForValidate = false;
    bool replyCorrectForValidate;

    bool makeRequestCalled = false;
    bool hostCorrectForRequest = false;

    bool handleReplyCalled = false;
    bool gotExpectedReply = false;

    RemoteCommandRequest expectedRequest{
        testHost(),
        DatabaseName::createDatabaseName_forTest(boost::none, "test"),
        BSON("1" << 2),
        BSON("some" << "stuff"),
        nullptr};

    RemoteCommandResponse expectedResponse =
        RemoteCommandResponse::make_forTest(BSON("foo" << "bar"
                                                       << "baz"
                                                       << "garply"
                                                       << "bar"
                                                       << "baz"),
                                            Milliseconds(30));

    // need to copy as it will be moved
    auto helloReplyData = BSON("iamyour" << "father");

    RemoteCommandResponse helloReply =
        RemoteCommandResponse::make_forTest(helloReplyData.copy(), Milliseconds(20));

    net().setHandshakeReplyForHost(testHost(), std::move(helloReply));

    // Since the contract of these methods is that they do not throw, we run the ASSERTs in
    // the test scope.
    net().setConnectionHook(makeTestHook(
        [&](const HostAndPort& remoteHost,
            const BSONObj&,
            const RemoteCommandResponse& helloReply) {
            validateCalled = true;
            hostCorrectForValidate = (remoteHost == testHost());
            replyCorrectForValidate =
                SimpleBSONObjComparator::kInstance.evaluate(helloReply.data == helloReplyData);
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

    RemoteCommandRequest actualCommandExpected{
        {testHost()},
        DatabaseName::createDatabaseName_forTest(boost::none, "testDB"),
        BSON("test" << 1),
        rpc::makeEmptyMetadata(),
        nullptr};
    RemoteCommandResponse actualResponseExpected{
        testHost(), BSON("1212121212" << "12121212121212"), Milliseconds(0)};

    net()
        .startCommand(cb, actualCommandExpected)
        .unsafeToInlineFuture()
        .getAsync([&](StatusWith<RemoteCommandResponse> swRcr) {
            if (!swRcr.getStatus().isOK()) {
                return;
            }
            commandFinished = true;
            auto resp = swRcr.getValue();
            if (resp.isOK()) {
                gotCorrectCommandReply = (actualResponseExpected == resp);
            }
        });

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
        ASSERT_EQ(net().getNumReadyRequests(), 1);
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
        ASSERT_EQ(net().getNumReadyRequests(), 1);
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
        [&](const HostAndPort& remoteHost, const BSONObj&, const RemoteCommandResponse& helloReply)
            -> Status {
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

    RemoteCommandRequest request{kUnimportantRequest};
    net()
        .startCommand(cb, request)
        .unsafeToInlineFuture()
        .getAsync([&](StatusWith<RemoteCommandResponse> swRcr) {
            if (!swRcr.getStatus().isOK()) {
                return;
            }
            commandFinished = true;
            auto resp = swRcr.getValue();
            statusPropagated = resp.status.code() == ErrorCodes::ConflictingOperationInProgress;
        });


    {
        net().enterNetwork();
        // We should have short-circuited the network and immediately called the callback.
        // If we change "hello" replies to go through the normal network mechanism,
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
        [&](const HostAndPort& remoteHost, const BSONObj&, const RemoteCommandResponse& helloReply)
            -> Status { return Status::OK(); },
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

    RemoteCommandRequest request{kUnimportantRequest};
    net()
        .startCommand(cb, request)
        .unsafeToInlineFuture()
        .getAsync([&](StatusWith<RemoteCommandResponse> swRcr) {
            if (!swRcr.getStatus().isOK()) {
                return;
            }
            commandFinished = true;
        });

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
        [&](const HostAndPort& remoteHost, const BSONObj&, const RemoteCommandResponse& helloReply)
            -> Status { return Status::OK(); },
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

    RemoteCommandRequest request{kUnimportantRequest};
    net()
        .startCommand(cb, request)
        .unsafeToInlineFuture()
        .getAsync([&](StatusWith<RemoteCommandResponse> swRcr) {
            if (!swRcr.getStatus().isOK()) {
                return;
            }
            commandFinished = true;
            auto resp = swRcr.getValue();
            errorPropagated = resp.status.code() == ErrorCodes::InvalidSyncSource;
        });


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
        [&](const HostAndPort& remoteHost, const BSONObj&, const RemoteCommandResponse& helloReply)
            -> Status { return Status::OK(); },
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

    RemoteCommandRequest request{kUnimportantRequest};
    net()
        .startCommand(cb, request)
        .unsafeToInlineFuture()
        .getAsync([&](StatusWith<RemoteCommandResponse> swRcr) {
            if (!swRcr.getStatus().isOK()) {
                return;
            }
            commandFinished = true;
            auto resp = swRcr.getValue();
            errorPropagated = resp.status.code() == ErrorCodes::CappedPositionLost;
        });


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
    net().setAlarm(deadline).unsafeToInlineFuture().getAsync([&](Status status) {
        if (!status.isOK()) {
            return;
        }
        alarmHasFired = true;
    });
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
    CancellationSource source;
    auto future = net().setAlarm(deadline, source.token()).unsafeToInlineFuture().then([&]() {
        alarmHasFired = true;
    });
    ASSERT_FALSE(alarmHasFired);

    source.cancel();

    {
        executor::NetworkInterfaceMock::InNetworkGuard guard(&net());

        net().advanceTime(deadline);
        ASSERT_FALSE(alarmHasFired);
    }

    // The future won't be ready until we actually advance the clock due to how cancellation works
    // in NetworkInterfaceMock
    ASSERT_THROWS_CODE(future.get(), DBException, ErrorCodes::CallbackCanceled);
}

TEST_F(NetworkInterfaceMockTest, CancelTokenBeforeSchedulingCommand) {
    startNetwork();

    TaskExecutor::CallbackHandle cb{};
    RemoteCommandRequest request{kUnimportantRequest};
    CancellationSource source;

    source.cancel();
    auto deferred = net().startCommand(cb, request, nullptr, source.token());

    net().enterNetwork();
    ASSERT(!net().hasReadyRequests());

    net().runReadyNetworkOperations();
    ASSERT_EQ(deferred.get().status, ErrorCodes::CallbackCanceled);
}

TEST_F(NetworkInterfaceMockTest, CancelCommandBeforeResponse) {
    startNetwork();

    TaskExecutor::CallbackHandle cb{};
    RemoteCommandRequest request{kUnimportantRequest};
    CancellationSource source;

    auto deferred = net().startCommand(cb, request, nullptr, source.token());

    net().enterNetwork();
    ASSERT(net().hasReadyRequests());

    source.cancel();
    ASSERT(!net().hasReadyRequests());

    net().runReadyNetworkOperations();
    net().exitNetwork();

    ASSERT_EQ(deferred.get().status, ErrorCodes::CallbackCanceled);
}

TEST_F(NetworkInterfaceMockTest, DestroyCancellationSourceBeforeRunning) {
    startNetwork();

    TaskExecutor::CallbackHandle cb{};
    RemoteCommandRequest request{kUnimportantRequest};
    RemoteCommandResponse resp =
        RemoteCommandResponse::make_forTest(BSON("foo" << "bar"), Milliseconds(30));
    CancellationSource source;

    auto deferred = net().startCommand(cb, request, nullptr, source.token());

    source = {};
    net().enterNetwork();
    ASSERT(net().hasReadyRequests());

    net().scheduleSuccessfulResponse(resp);
    net().runReadyNetworkOperations();
    net().exitNetwork();

    ASSERT_OK(deferred.get().status);
}

TEST_F(NetworkInterfaceMockTest, CancelCommandAfterResponse) {
    startNetwork();

    TaskExecutor::CallbackHandle cb{};
    RemoteCommandRequest request{kUnimportantRequest};
    RemoteCommandResponse resp =
        RemoteCommandResponse::make_forTest(BSON("foo" << "bar"), Milliseconds(30));
    CancellationSource source;

    auto deferred = net().startCommand(cb, request, nullptr, source.token());

    net().enterNetwork();
    ASSERT(net().hasReadyRequests());

    auto req = net().getNextReadyRequest();
    net().scheduleResponse(req, net().now(), resp);
    net().runReadyNetworkOperations();

    ASSERT_TRUE(req->isFinished());

    auto before = net().getNumResponses();
    source.cancel();
    auto after = net().getNumResponses();

    ASSERT_EQ(before, after);
    ASSERT_OK(deferred.get().status);
}

TEST_F(NetworkInterfaceMockTest, DrainUnfinishedNetworkOperations) {
    auto client = getGlobalServiceContext()->getService()->makeClient("NetworkInterfaceMockTest");
    startNetwork();

    TaskExecutor::CallbackHandle cb{};
    RemoteCommandRequest request{kUnimportantRequest};
    RemoteCommandResponse resp =
        RemoteCommandResponse::make_forTest(BSON("foo" << "bar"), Milliseconds(30));

    auto deferred = net().startCommand(cb, request, nullptr, CancellationToken::uncancelable());

    {
        NetworkInterfaceMock::InNetworkGuard guard(&net());
        ASSERT_TRUE(net().hasReadyRequests());
        auto req = net().getNextReadyRequest();
        ASSERT_FALSE(req->isFinished());
        // Schedule a response to be processed by drainUnfinishedNetworkOperations.
        net().scheduleResponse(req, net().now(), resp);

        // Check that drainUnfinishedNetworkOperations processes the response.
        net().drainUnfinishedNetworkOperations();

        ASSERT_TRUE(req->isFinished());
        ASSERT_FALSE(net().hasReadyRequests());
    }

    // Make sure that the request is fulfilled.
    auto opCtx = client->makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Seconds(30), ErrorCodes::ExceededTimeLimit);
    ASSERT_OK(deferred.get(opCtx.get()).status);
}

TEST_F(NetworkInterfaceMockTest, TestNumReadyRequests) {
    startNetwork();

    TaskExecutor::CallbackHandle cb1{}, cb2{};
    RemoteCommandRequest request1{kUnimportantRequest}, request2{kUnimportantRequest};

    net().enterNetwork();
    ASSERT_EQ(net().getNumReadyRequests(), 0);

    auto deferred1 = net().startCommand(cb1, request1);
    ASSERT_EQ(net().getNumReadyRequests(), 1);
    auto deferred2 = net().startCommand(cb2, request2);
    ASSERT_EQ(net().getNumReadyRequests(), 2);

    TaskExecutor::CallbackHandle exhaustCb{};
    RemoteCommandRequest exhaustRequest{kUnimportantRequest};
    auto reader = net().startExhaustCommand(exhaustCb, exhaustRequest).get();
    ASSERT_EQ(net().getNumReadyRequests(), 2);

    auto deferredExhaust1 = reader->next();
    auto deferredExhaust2 = reader->next();

    ASSERT_EQ(net().getNumReadyRequests(), 4);


    RemoteCommandResponse resp =
        RemoteCommandResponse::make_forTest(BSON("foo" << "bar"), Milliseconds(30));
    auto req = net().getNextReadyRequest();
    net().scheduleResponse(req, net().now(), resp);
    net().runReadyNetworkOperations();

    ASSERT_EQ(net().getNumReadyRequests(), 3);

    net().exitNetwork();
}

TEST_F(NetworkInterfaceMockTest, InShutdown) {
    startNetwork();
    ASSERT_FALSE(net().inShutdown());
    tearDown();
    ASSERT(net().inShutdown());
}

TEST_F(NetworkInterfaceMockTest, StartCommandThrowsIfShutdownHasStarted) {
    startNetwork();
    tearDown();

    TaskExecutor::CallbackHandle cb{};
    RemoteCommandRequest request{kUnimportantRequest};
    ASSERT_THROWS_CODE(net().startCommand(cb, request).unsafeToInlineFuture().getNoThrow(),
                       DBException,
                       ErrorCodes::ShutdownInProgress);
}

TEST_F(NetworkInterfaceMockTest, SetAlarmReturnsNotOKIfShutdownHasStarted) {
    startNetwork();
    tearDown();
    ASSERT_THROWS_CODE(net().setAlarm(net().now() + Milliseconds(100)).get(),
                       DBException,
                       ErrorCodes::ShutdownInProgress);
}

TEST_F(NetworkInterfaceMockTest, CommandTimeout) {
    startNetwork();

    TaskExecutor::CallbackHandle cb;
    RemoteCommandRequest request{kUnimportantRequest};
    request.timeout = Milliseconds(2000);

    ErrorCodes::Error statusPropagated = ErrorCodes::OK;
    auto finishFn = [&](const RemoteCommandResponse& resp) {
        statusPropagated = resp.status.code();
    };

    //
    // Command times out.
    //
    net()
        .startCommand(cb, request)
        .unsafeToInlineFuture()
        .getAsync([&](StatusWith<RemoteCommandResponse> swRcr) {
            if (!swRcr.getStatus().isOK()) {
                return;
            }
            finishFn(swRcr.getValue());
        });
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

    net()
        .startCommand(cb, request)
        .unsafeToInlineFuture()
        .getAsync([&](StatusWith<RemoteCommandResponse> swRcr) {
            if (!swRcr.getStatus().isOK()) {
                return;
            }
            finishFn(swRcr.getValue());
        });
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
