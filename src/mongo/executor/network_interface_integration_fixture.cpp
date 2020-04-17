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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/client/connection_string.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/future.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace executor {

void NetworkInterfaceIntegrationFixture::createNet(
    std::unique_ptr<NetworkConnectionHook> connectHook) {
    ConnectionPool::Options options;

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
    const TaskExecutor::CallbackHandle& cbHandle, RemoteCommandRequest request) {
    RemoteCommandRequestOnAny rcroa{request};

    return net().startCommand(cbHandle, rcroa).then([](TaskExecutor::ResponseOnAnyStatus roa) {
        return RemoteCommandResponse(roa);
    });
}

Future<RemoteCommandOnAnyResponse> NetworkInterfaceIntegrationFixture::runCommandOnAny(
    const TaskExecutor::CallbackHandle& cbHandle, RemoteCommandRequestOnAny request) {
    RemoteCommandRequestOnAny rcroa{request};

    return net().startCommand(cbHandle, rcroa).then([](TaskExecutor::ResponseOnAnyStatus roa) {
        return roa;
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
    if (res.isOK()) {
        LOGV2(22586,
              "Got command result: {response}",
              "Got command result",
              "response"_attr = res.toString());
    } else {
        LOGV2(22587, "Command failed: {error}", "Command failed", "error"_attr = res.status);
    }
    return res;
}

void NetworkInterfaceIntegrationFixture::assertCommandOK(StringData db,
                                                         const BSONObj& cmd,
                                                         Milliseconds timeoutMillis) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db.toString(), cmd, BSONObj(), nullptr, timeoutMillis};
    auto res = runCommandSync(request);
    ASSERT_OK(res.status);
    ASSERT_OK(getStatusFromCommandResult(res.data));
    ASSERT(!res.data["writeErrors"]);
}

void NetworkInterfaceIntegrationFixture::assertCommandFailsOnClient(StringData db,
                                                                    const BSONObj& cmd,
                                                                    ErrorCodes::Error reason,
                                                                    Milliseconds timeoutMillis) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db.toString(), cmd, BSONObj(), nullptr, timeoutMillis};
    auto res = runCommandSync(request);
    ASSERT_EQ(reason, res.status.code());
}

void NetworkInterfaceIntegrationFixture::assertCommandFailsOnServer(StringData db,
                                                                    const BSONObj& cmd,
                                                                    ErrorCodes::Error reason,
                                                                    Milliseconds timeoutMillis) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db.toString(), cmd, BSONObj(), nullptr, timeoutMillis};
    auto res = runCommandSync(request);
    ASSERT_OK(res.status);
    auto serverStatus = getStatusFromCommandResult(res.data);
    ASSERT_EQ(reason, serverStatus);
}

void NetworkInterfaceIntegrationFixture::assertWriteError(StringData db,
                                                          const BSONObj& cmd,
                                                          ErrorCodes::Error reason,
                                                          Milliseconds timeoutMillis) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db.toString(), cmd, BSONObj(), nullptr, timeoutMillis};
    auto res = runCommandSync(request);
    ASSERT_OK(res.status);
    ASSERT_OK(getStatusFromCommandResult(res.data));
    ASSERT(res.data["writeErrors"]);
    auto firstWriteError = res.data["writeErrors"].embeddedObject().firstElement().embeddedObject();
    Status writeErrorStatus(ErrorCodes::Error(firstWriteError.getIntField("code")),
                            firstWriteError.getStringField("errmsg"));
    ASSERT_EQ(reason, writeErrorStatus);
}

}  // namespace executor
}  // namespace mongo
