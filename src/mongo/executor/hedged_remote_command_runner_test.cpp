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

#include "mongo/executor/hedged_remote_command_runner.h"

#include "mongo/client/async_remote_command_targeter.h"
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
#include "mongo/executor/network_interface.h"
#include "mongo/executor/network_interface_mock.h"
#include "mongo/executor/network_test_env.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/remote_command_runner.h"
#include "mongo/executor/remote_command_runner_test_fixture.h"
#include "mongo/executor/remote_command_targeter.h"
#include "mongo/s/mongos_server_parameters_gen.h"
#include "mongo/unittest/barrier.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/time_support.h"
#include <memory>
#include <vector>


namespace mongo {
namespace executor {
using namespace remote_command_runner;
namespace {

class HedgedCommandRunnerTest : public RemoteCommandRunnerTestFixture {
public:
    void setUp() {
        RemoteCommandRunnerTestFixture::setUp();
        ReadPreferenceSetting readPref;

        auto factory = RemoteCommandTargeterFactoryMock();
        _targeter_two_hosts = factory.create(ConnectionString::forStandalones(kTwoHosts));
        _targeter_three_hosts = factory.create(ConnectionString::forStandalones(kTwoHosts));
        _emptyTargeter = factory.create(ConnectionString::forStandalones(kTwoHosts));

        auto targeterMock = RemoteCommandTargeterMock::get(_targeter_two_hosts);
        targeterMock->setFindHostsReturnValue(kTwoHosts);

        auto threeTargeterMock = RemoteCommandTargeterMock::get(_targeter_three_hosts);
        threeTargeterMock->setFindHostsReturnValue(kThreeHosts);

        auto emptyTargeterMock = RemoteCommandTargeterMock::get(_emptyTargeter);
        emptyTargeterMock->setFindHostsReturnValue(std::vector<HostAndPort>{});
    }

    NetworkInterface::Counters getNetworkInterfaceCounters() {
        auto counters = getNetworkInterfaceMock()->getCounters();
        return counters;
    }

    std::shared_ptr<RemoteCommandTargeter> getTwoHostsTargeter() {
        return _targeter_two_hosts;
    }

    std::shared_ptr<RemoteCommandTargeter> getThreeHostsTargeter() {
        return _targeter_three_hosts;
    }

    std::shared_ptr<RemoteCommandTargeter> getEmptyTargeter() {
        return _emptyTargeter;
    }

    const std::vector<HostAndPort> kTwoHosts{HostAndPort("FakeHost1", 12345),
                                             HostAndPort("FakeHost2", 12345)};
    const std::vector<HostAndPort> kThreeHosts{HostAndPort("FakeHost1", 12345),
                                               HostAndPort("FakeHost2", 12345),
                                               HostAndPort("FakeHost3", 12345)};

private:
    std::shared_ptr<RemoteCommandTargeter> _targeter_two_hosts;
    std::shared_ptr<RemoteCommandTargeter> _targeter_three_hosts;
    std::shared_ptr<RemoteCommandTargeter> _emptyTargeter;
};

// TODO SERVER-68709: write more comprehensive test cases with the mock implementation.

/**
 * When we send a find command to the doHedgedRequest function, it sends out two requests and
 * cancels the second one once the first has responsed.
 */
TEST_F(HedgedCommandRunnerTest, FindHedgeRequestTwoHosts) {
    ReadPreferenceSetting readPref;
    std::shared_ptr<RemoteCommandTargeter> t = getTwoHostsTargeter();
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<mongo::remote_command_runner::AsyncRemoteCommandTargeter>(readPref, t);
    FindCommandRequest findCmd(NamespaceString("testdb", "testcoll"));

    auto h = makeOperationContext();
    auto resultFuture = doHedgedRequest(
        findCmd, h.get(), std::move(targeter), getExecutorPtr(), CancellationToken::uncancelable());

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(NamespaceString("testdb", "testcoll"), 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    RemoteCommandRunnerResponse<CursorInitialReply> res = resultFuture.get();

    auto counters = getNetworkInterfaceCounters();
    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 1);


    ASSERT_EQ(res.response.getCursor()->getNs(), NamespaceString("testdb", "testcoll"));
}

TEST_F(HedgedCommandRunnerTest, FindHedgeRequestThreeHosts) {
    ReadPreferenceSetting readPref;
    std::shared_ptr<RemoteCommandTargeter> t = getThreeHostsTargeter();
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<mongo::remote_command_runner::AsyncRemoteCommandTargeter>(readPref, t);
    FindCommandRequest findCmd(NamespaceString("testdb", "testcoll"));

    auto opCtxHolder = makeOperationContext();
    auto resultFuture = doHedgedRequest(findCmd,
                                        opCtxHolder.get(),
                                        std::move(targeter),
                                        getExecutorPtr(),
                                        CancellationToken::uncancelable());

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return CursorResponse(NamespaceString("testdb", "testcoll"), 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse);
    });

    RemoteCommandRunnerResponse<CursorInitialReply> res = resultFuture.get();

    auto counters = getNetworkInterfaceCounters();
    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 2);

    // TODO SERVER-68767: ASSERT on actual BSONObj from getFirstBatch()
    ASSERT_EQ(res.response.getCursor()->getNs(), NamespaceString("testdb", "testcoll"));
}

/**
 * When we send a hello command to the doHedgedRequest function, it does not hedge and only sends
 * one request.
 */
TEST_F(HedgedCommandRunnerTest, HelloHedgeRequest) {
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    ReadPreferenceSetting readPref;
    std::shared_ptr<RemoteCommandTargeter> t = getTwoHostsTargeter();
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<mongo::remote_command_runner::AsyncRemoteCommandTargeter>(readPref, t);

    auto opCtxHolder = makeOperationContext();
    auto resultFuture = doHedgedRequest(helloCmd,
                                        opCtxHolder.get(),
                                        std::move(targeter),
                                        getExecutorPtr(),
                                        CancellationToken::uncancelable());

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["hello"]);
        return helloReply.toBSON();
    });

    auto counters = getNetworkInterfaceCounters();
    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 0);

    RemoteCommandRunnerResponse<HelloCommandReply> res = resultFuture.get();

    ASSERT_BSONOBJ_EQ(res.response.toBSON(), helloReply.toBSON());
}

/**
 * When the targeter returns no hosts, we get a HostNotFound error.
 */
TEST_F(HedgedCommandRunnerTest, NoShardsFound) {
    HelloCommandReply helloReply = HelloCommandReply(TopologyVersion(OID::gen(), 0));
    HelloCommand helloCmd;
    initializeCommand(helloCmd);

    ReadPreferenceSetting readPref;
    std::shared_ptr<RemoteCommandTargeter> t = getEmptyTargeter();
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<mongo::remote_command_runner::AsyncRemoteCommandTargeter>(readPref, t);


    auto opCtxHolder = makeOperationContext();
    auto resultFuture = doHedgedRequest(helloCmd,
                                        opCtxHolder.get(),
                                        std::move(targeter),
                                        getExecutorPtr(),
                                        CancellationToken::uncancelable());

    ASSERT_THROWS_CODE(resultFuture.get(), DBException, ErrorCodes::HostNotFound);
}

/**
 * When a hedged command is sent and one request resolves with a non-ignorable error, we propagate
 * that error upwards and cancel the other requests.
 */
TEST_F(HedgedCommandRunnerTest, FirstCommandFailsWithSignificantError) {
    FindCommandRequest findCmd(NamespaceString("testdb", "testcoll"));

    ReadPreferenceSetting readPref;
    std::shared_ptr<RemoteCommandTargeter> t = getTwoHostsTargeter();
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<mongo::remote_command_runner::AsyncRemoteCommandTargeter>(readPref, t);


    auto opCtxHolder = makeOperationContext();
    auto resultFuture = doHedgedRequest(findCmd,
                                        opCtxHolder.get(),
                                        std::move(targeter),
                                        getExecutorPtr(),
                                        CancellationToken::uncancelable());

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return Status(ErrorCodes::NetworkTimeout, "mock");
    });

    auto counters = getNetworkInterfaceCounters();
    ASSERT_EQ(counters.failed, 1);
    ASSERT_EQ(counters.canceled, 1);

    ASSERT_THROWS_CODE(resultFuture.get(), DBException, ErrorCodes::NetworkTimeout);
}

/**
 * When a hedged command is sent and all requests fail with an "ignorable" error, that error
 * propagates upwards.
 */
TEST_F(HedgedCommandRunnerTest, BothCommandsFailWithSkippableError) {
    FindCommandRequest findCmd(NamespaceString("testdb", "testcoll"));

    ReadPreferenceSetting readPref;
    std::shared_ptr<RemoteCommandTargeter> t = getTwoHostsTargeter();
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<mongo::remote_command_runner::AsyncRemoteCommandTargeter>(readPref, t);


    auto opCtxHolder = makeOperationContext();
    auto resultFuture = doHedgedRequest(findCmd,
                                        opCtxHolder.get(),
                                        std::move(targeter),
                                        getExecutorPtr(),
                                        CancellationToken::uncancelable());

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return Status(ErrorCodes::MaxTimeMSExpired, "mock");
    });

    onCommand([&](const auto& request) {
        ASSERT(request.cmdObj["find"]);
        return Status(ErrorCodes::MaxTimeMSExpired, "mock");
    });

    auto counters = getNetworkInterfaceCounters();
    ASSERT_EQ(counters.failed, 2);
    ASSERT_EQ(counters.canceled, 0);

    ASSERT_THROWS_CODE(resultFuture.get(), DBException, ErrorCodes::MaxTimeMSExpired);
}

TEST_F(HedgedCommandRunnerTest, AllCommandsFailWithSkippableError) {
    FindCommandRequest findCmd(NamespaceString("testdb", "testcoll"));

    ReadPreferenceSetting readPref;
    std::shared_ptr<RemoteCommandTargeter> t = getThreeHostsTargeter();
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<mongo::remote_command_runner::AsyncRemoteCommandTargeter>(readPref, t);


    auto opCtxHolder = makeOperationContext();
    auto resultFuture = doHedgedRequest(findCmd,
                                        opCtxHolder.get(),
                                        std::move(targeter),
                                        getExecutorPtr(),
                                        CancellationToken::uncancelable());

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();
    network->enterNetwork();

    NetworkInterfaceMock::NetworkOperationIterator noi1 = network->getNextReadyRequest();
    NetworkInterfaceMock::NetworkOperationIterator noi2 = network->getNextReadyRequest();
    NetworkInterfaceMock::NetworkOperationIterator noi3 = network->getNextReadyRequest();

    auto firstRequest = (*noi1).getRequestOnAny();
    auto secondRequest = (*noi2).getRequestOnAny();
    auto thirdRequest = (*noi3).getRequestOnAny();

    if (firstRequest.target[0] == kThreeHosts[0]) {
        network->scheduleErrorResponse(
            noi1, now + Milliseconds(1000), Status(ErrorCodes::MaxTimeMSExpired, "mock"));
    } else {
        network->scheduleErrorResponse(noi1, Status(ErrorCodes::MaxTimeMSExpired, "mock"));
    }

    if (secondRequest.target[0] == kThreeHosts[0]) {
        network->scheduleErrorResponse(
            noi2, now + Milliseconds(1000), Status(ErrorCodes::MaxTimeMSExpired, "mock"));
    } else {
        network->scheduleErrorResponse(noi2, Status(ErrorCodes::MaxTimeMSExpired, "mock"));
    }

    if (thirdRequest.target[0] == kThreeHosts[0]) {
        network->scheduleErrorResponse(
            noi3, now + Milliseconds(1000), Status(ErrorCodes::MaxTimeMSExpired, "mock"));
    } else {
        network->scheduleErrorResponse(noi3, Status(ErrorCodes::MaxTimeMSExpired, "mock"));
    }

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    ASSERT_EQ(counters.failed, 3);
    ASSERT_EQ(counters.canceled, 0);

    ASSERT_THROWS_CODE(resultFuture.get(), DBException, ErrorCodes::MaxTimeMSExpired);
}

/**
 * When a hedged command is sent and the first (hedged) request fails with an ignorable error and
 * the second (authoritative request) succeeds, we get the success result.
 */
TEST_F(HedgedCommandRunnerTest, FirstCommandFailsWithSkippableErrorNextSucceeds) {
    FindCommandRequest findCmd(NamespaceString("testdb", "testcoll"));

    ReadPreferenceSetting readPref;
    std::shared_ptr<RemoteCommandTargeter> t = getTwoHostsTargeter();
    std::unique_ptr<RemoteCommandHostTargeter> targeter =
        std::make_unique<mongo::remote_command_runner::AsyncRemoteCommandTargeter>(readPref, t);

    auto opCtxHolder = makeOperationContext();
    auto resultFuture = doHedgedRequest(findCmd,
                                        opCtxHolder.get(),
                                        std::move(targeter),
                                        getExecutorPtr(),
                                        CancellationToken::uncancelable());

    auto network = getNetworkInterfaceMock();
    auto now = getNetworkInterfaceMock()->now();

    RemoteCommandResponse successResponse{
        CursorResponse(NamespaceString("testdb", "testcoll"), 0LL, {BSON("x" << 1)})
            .toBSON(CursorResponse::ResponseType::InitialResponse),
        Milliseconds::zero()};

    network->enterNetwork();

    NetworkInterfaceMock::NetworkOperationIterator noi1 = network->getNextReadyRequest();
    NetworkInterfaceMock::NetworkOperationIterator noi2 = network->getNextReadyRequest();

    auto firstRequest = (*noi1).getRequestOnAny();
    auto secondRequest = (*noi2).getRequestOnAny();

    // if the first request is the authoritative one, send a delayed success response
    // otherwise send an ignorable error
    if (firstRequest.target[0] == kThreeHosts[0]) {
        network->scheduleSuccessfulResponse(noi1, now + Milliseconds(1000), successResponse);
    } else {
        network->scheduleErrorResponse(noi1, Status(ErrorCodes::MaxTimeMSExpired, "mock"));
    }

    // if the second request is the authoritative one, send a delayed success response
    // otherwise send an ignorable error
    if (secondRequest.target[0] == kThreeHosts[0]) {
        network->scheduleSuccessfulResponse(noi2, now + Milliseconds(1000), successResponse);
    } else {
        network->scheduleErrorResponse(noi2, Status(ErrorCodes::MaxTimeMSExpired, "mock"));
    }

    network->runUntil(now + Milliseconds(1500));

    auto counters = network->getCounters();
    network->exitNetwork();

    ASSERT_EQ(counters.failed, 1);
    ASSERT_EQ(counters.succeeded, 1);
    ASSERT_EQ(counters.canceled, 0);

    ASSERT_EQ(resultFuture.get().response.getCursor()->getNs(),
              NamespaceString("testdb", "testcoll"));
}

}  // namespace
}  // namespace executor
}  // namespace mongo
