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
#include <boost/move/utility_core.hpp>
#include <boost/smart_ptr.hpp>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/client/connection_string.h"
#include "mongo/db/service_context.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/metadata/metadata_hook.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kASIO


namespace mongo {
namespace executor {

MONGO_FAIL_POINT_DEFINE(networkInterfaceFixtureHangOnCompletion);

void NetworkInterfaceIntegrationFixture::createNet(
    std::unique_ptr<NetworkConnectionHook> connectHook, ConnectionPool::Options options) {
    options.minConnections = 0u;

#ifdef _WIN32
    // Connections won't queue on widnows, so attempting to open too many connections
    // concurrently will result in refused connections and test failure.
    options.maxConnections = 16u;
#else
    options.maxConnections = 256u;
#endif
    _net = makeNetworkInterface(
        "NetworkInterfaceIntegrationFixture", std::move(connectHook), nullptr, std::move(options));
}

void NetworkInterfaceIntegrationFixture::startNet(
    std::unique_ptr<NetworkConnectionHook> connectHook) {
    createNet(std::move(connectHook));
    net().startup();
}

void NetworkInterfaceIntegrationFixture::tearDown() {
    // Network interface will only shutdown once because of an internal shutdown guard
    _net->shutdown();

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

void NetworkInterfaceIntegrationFixture::startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                                      RemoteCommandRequest& request,
                                                      StartCommandCB onFinish) {
    RemoteCommandRequestOnAny rcroa{request};

    auto cb = [onFinish = std::move(onFinish)](const TaskExecutor::ResponseOnAnyStatus& rs) {
        onFinish(rs);
    };
    invariant(net().startCommand(cbHandle, rcroa, std::move(cb)));
}

Future<RemoteCommandResponse> NetworkInterfaceIntegrationFixture::runCommand(
    const TaskExecutor::CallbackHandle& cbHandle, RemoteCommandRequestOnAny rcroa) {

    _onSchedulingCommand();

    return net()
        .startCommand(cbHandle, rcroa)
        .then([](TaskExecutor::ResponseOnAnyStatus roa) {
            auto res = RemoteCommandResponse(roa);
            if (res.isOK()) {
                LOGV2(4820500,
                      "Got command result: {response}",
                      "Got command result",
                      "response"_attr = res.toString());
            } else {
                LOGV2(4820501,
                      "Command failed: {error}",
                      "Command failed",
                      "error"_attr = res.status);
            }
            return res;
        })
        .onCompletion([this](StatusOrStatusWith<RemoteCommandResponse> status) {
            _onCompletingCommand();
            return status;
        });
}

Future<RemoteCommandOnAnyResponse> NetworkInterfaceIntegrationFixture::runCommandOnAny(
    const TaskExecutor::CallbackHandle& cbHandle, RemoteCommandRequestOnAny request) {
    RemoteCommandRequestOnAny rcroa{request};

    _onSchedulingCommand();

    return net()
        .startCommand(cbHandle, rcroa)
        .then([](TaskExecutor::ResponseOnAnyStatus roa) {
            if (roa.isOK()) {
                LOGV2(4820502,
                      "Got command result: {response}",
                      "Got command result",
                      "response"_attr = roa.toString());
            } else {
                LOGV2(4820503,
                      "Command failed: {error}",
                      "Command failed",
                      "error"_attr = roa.status);
            }
            return roa;
        })
        .onCompletion([this](StatusOrStatusWith<TaskExecutor::ResponseOnAnyStatus> status) {
            _onCompletingCommand();
            return status;
        });
}

Future<void> NetworkInterfaceIntegrationFixture::startExhaustCommand(
    const TaskExecutor::CallbackHandle& cbHandle,
    RemoteCommandRequest request,
    std::function<void(const RemoteCommandResponse&)> exhaustUtilCB,
    const BatonHandle& baton) {
    RemoteCommandRequestOnAny rcroa{request};
    auto pf = makePromiseFuture<void>();

    auto status = net().startExhaustCommand(
        cbHandle,
        rcroa,
        [p = std::move(pf.promise), exhaustUtilCB = std::move(exhaustUtilCB)](
            const TaskExecutor::ResponseOnAnyStatus& rs) mutable {
            exhaustUtilCB(rs);

            if (!rs.status.isOK()) {
                invariant(!rs.moreToCome);
                p.setError(rs.status);
                return;
            }

            if (!rs.moreToCome) {
                p.emplaceValue();
            }
        },
        baton);

    if (!status.isOK()) {
        return status;
    }
    return std::move(pf.future);
}

RemoteCommandResponse NetworkInterfaceIntegrationFixture::runCommandSync(
    RemoteCommandRequest& request) {
    auto deferred = runCommand(makeCallbackHandle(), request);
    auto& res = deferred.get();
    return res;
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
