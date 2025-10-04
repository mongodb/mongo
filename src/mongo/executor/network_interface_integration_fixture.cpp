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


// IWYU pragma: no_include "cxxabi.h"
#include "mongo/executor/network_interface_integration_fixture.h"

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"

#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace executor {

MONGO_FAIL_POINT_DEFINE(networkInterfaceFixtureHangOnCompletion);

std::unique_ptr<NetworkInterface> NetworkInterfaceIntegrationFixture::_makeNet(
    std::string instanceName, transport::TransportProtocol protocol) {

    auto opts = _opts ? *_opts : makeDefaultConnectionPoolOptions();

    switch (protocol) {
        case transport::TransportProtocol::MongoRPC:
            return makeNetworkInterface(instanceName, nullptr, nullptr, opts);
        case transport::TransportProtocol::GRPC:
#ifdef MONGO_CONFIG_GRPC
            return makeNetworkInterfaceGRPC(instanceName);
#else
            MONGO_UNREACHABLE;
#endif
    }
    MONGO_UNREACHABLE;
}

ConnectionPool::Options NetworkInterfaceIntegrationFixture::makeDefaultConnectionPoolOptions() {
    ConnectionPool::Options options{};
    options.minConnections = 0u;

#ifdef _WIN32
    // Connections won't queue on widnows, so attempting to open too many connections
    // concurrently will result in refused connections and test failure.
    options.maxConnections = 16u;
#else
    options.maxConnections = 256u;
#endif
    return options;
}

void NetworkInterfaceIntegrationFixture::createNet() {
    auto protocol = unittest::shouldUseGRPCEgress() ? transport::TransportProtocol::GRPC
                                                    : transport::TransportProtocol::MongoRPC;
    _net = _makeNet("NetworkInterfaceIntegrationFixture", protocol);

    switch (protocol) {
        case transport::TransportProtocol::MongoRPC:
            _fixtureNet =
                makeNetworkInterface("FixtureNet", nullptr, nullptr, ConnectionPool::Options());
            break;
        case transport::TransportProtocol::GRPC:
#ifdef MONGO_CONFIG_GRPC
            _fixtureNet = makeNetworkInterfaceGRPC("FixtureNet");
#else
            MONGO_UNREACHABLE;
#endif
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

void NetworkInterfaceIntegrationFixture::startNet() {
    createNet();
    net().startup();
    _fixtureNet->startup();
}

void NetworkInterfaceIntegrationFixture::tearDown() {
    // Network interface will only shutdown once because of an internal shutdown guard
    if (_net) {
        _net->shutdown();
    }

    if (_fixtureNet) {
        _fixtureNet->shutdown();
    }

    auto lk = stdx::unique_lock(_mutex);
    auto checkIdle = [&]() {
        return _workInProgress == 0;
    };
    _fixtureIsIdle.wait(lk, checkIdle);
}

NetworkInterface& NetworkInterfaceIntegrationFixture::net() {
    return *_net;
}

ConnectionString NetworkInterfaceIntegrationFixture::fixture() {
    return unittest::getFixtureConnectionString();
}

void NetworkInterfaceIntegrationFixture::setRandomNumberGenerator(PseudoRandom* generator) {
    _rng = generator;
}

PseudoRandom* NetworkInterfaceIntegrationFixture::getRandomNumberGenerator() {
    return _rng;
}

void NetworkInterfaceIntegrationFixture::resetIsInternalClient(bool isInternalClient) {
    WireSpec::Specification newSpec = *WireSpec::getWireSpec(getGlobalServiceContext()).get();
    newSpec.isInternalClient = isInternalClient;
    WireSpec::getWireSpec(getGlobalServiceContext()).reset(std::move(newSpec));
}

Future<RemoteCommandResponse> NetworkInterfaceIntegrationFixture::runCommand(
    const TaskExecutor::CallbackHandle& cbHandle,
    RemoteCommandRequest request,
    const CancellationToken& token) {
    auto fut = net().startCommand(cbHandle, request, baton(), token);
    _onSchedulingCommand();
    return std::move(fut)
        .unsafeToInlineFuture()
        .then([request](TaskExecutor::ResponseStatus resStatus) {
            if (resStatus.isOK()) {
                LOGV2(4820500,
                      "Got command result",
                      "request"_attr = request.toString(),
                      "response"_attr = resStatus.toString());
            } else {
                LOGV2(4820501,
                      "Command failed",
                      "request"_attr = request.toString(),
                      "error"_attr = resStatus.status);
            }
            return resStatus;
        })
        .onCompletion([this](StatusOrStatusWith<RemoteCommandResponse> status) {
            _onCompletingCommand();
            return status;
        });
}

void NetworkInterfaceIntegrationFixture::cancelCommand(
    const TaskExecutor::CallbackHandle& cbHandle) {
    net().cancelCommand(cbHandle, baton());
}

BSONObj NetworkInterfaceIntegrationFixture::runSetupCommandSync(const DatabaseName& db,
                                                                BSONObj cmdObj) {
    RemoteCommandRequest request(
        fixture().getServers()[0], db, std::move(cmdObj), BSONObj(), nullptr, Minutes(1));
    request.sslMode = transport::kGlobalSSLMode;

    auto res = _fixtureNet->startCommand(makeCallbackHandle(), request).get();
    ASSERT_OK(res.status);
    ASSERT_OK(getStatusFromCommandResult(res.data));
    return res.data;
}

RemoteCommandResponse NetworkInterfaceIntegrationFixture::runCommandSync(
    RemoteCommandRequest& request) {
    return runCommand(makeCallbackHandle(), request).get(interruptible());
}

void NetworkInterfaceIntegrationFixture::assertCommandOK(const DatabaseName& db,
                                                         const BSONObj& cmd,
                                                         Milliseconds timeoutMillis,
                                                         transport::ConnectSSLMode sslMode) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db, cmd, BSONObj(), nullptr, timeoutMillis};
    request.sslMode = sslMode;
    auto res = runCommandSync(request);
    ASSERT_OK(res.status);
    ASSERT_OK(getStatusFromCommandResult(res.data));
    ASSERT(!res.data["writeErrors"]);
}

void NetworkInterfaceIntegrationFixture::assertCommandFailsOnClient(const DatabaseName& db,
                                                                    const BSONObj& cmd,
                                                                    ErrorCodes::Error reason,
                                                                    Milliseconds timeoutMillis) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db, cmd, BSONObj(), nullptr, timeoutMillis};
    auto res = runCommandSync(request);
    ASSERT_EQ(reason, res.status.code());
}

void NetworkInterfaceIntegrationFixture::assertCommandFailsOnServer(const DatabaseName& db,
                                                                    const BSONObj& cmd,
                                                                    ErrorCodes::Error reason,
                                                                    Milliseconds timeoutMillis) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db, cmd, BSONObj(), nullptr, timeoutMillis};
    auto res = runCommandSync(request);
    ASSERT_OK(res.status);
    auto serverStatus = getStatusFromCommandResult(res.data);
    ASSERT_EQ(reason, serverStatus);
}

void NetworkInterfaceIntegrationFixture::assertWriteError(const DatabaseName& db,
                                                          const BSONObj& cmd,
                                                          ErrorCodes::Error reason,
                                                          Milliseconds timeoutMillis) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db, cmd, BSONObj(), nullptr, timeoutMillis};
    auto res = runCommandSync(request);
    ASSERT_OK(res.status);
    ASSERT_OK(getStatusFromCommandResult(res.data));
    ASSERT(res.data["writeErrors"]);
    auto firstWriteError = res.data["writeErrors"].embeddedObject().firstElement().embeddedObject();
    Status writeErrorStatus(ErrorCodes::Error(firstWriteError.getIntField("code")),
                            firstWriteError.getStringField("errmsg"));
    ASSERT_EQ(reason, writeErrorStatus);
}

void NetworkInterfaceIntegrationFixture::_onSchedulingCommand() {
    auto lk = stdx::lock_guard(_mutex);
    _workInProgress++;
}

void NetworkInterfaceIntegrationFixture::_onCompletingCommand() {
    networkInterfaceFixtureHangOnCompletion.pauseWhileSet();
    auto lk = stdx::lock_guard(_mutex);
    if (--_workInProgress == 0) {
        _fixtureIsIdle.notify_all();
    }
}

}  // namespace executor
}  // namespace mongo
