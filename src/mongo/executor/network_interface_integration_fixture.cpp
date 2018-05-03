/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include "mongo/client/connection_string.h"
#include "mongo/executor/network_interface_factory.h"
#include "mongo/executor/network_interface_integration_fixture.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/integration_test.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace executor {

void NetworkInterfaceIntegrationFixture::startNet(
    std::unique_ptr<NetworkConnectionHook> connectHook) {
    ConnectionPool::Options options;
#ifdef _WIN32
    // Connections won't queue on widnows, so attempting to open too many connections
    // concurrently will result in refused connections and test failure.
    options.maxConnections = 16u;
#else
    options.maxConnections = 256u;
#endif
    _net = makeNetworkInterface(
        "NetworkInterfaceIntegrationFixture", std::move(connectHook), nullptr, std::move(options));

    _net->startup();
}

void NetworkInterfaceIntegrationFixture::tearDown() {
    if (!_net->inShutdown()) {
        _net->shutdown();
    }
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
    net().startCommand(cbHandle, request, onFinish).transitional_ignore();
}

Future<RemoteCommandResponse> NetworkInterfaceIntegrationFixture::runCommand(
    const TaskExecutor::CallbackHandle& cbHandle, RemoteCommandRequest request) {
    return net().startCommand(cbHandle, request);
}

RemoteCommandResponse NetworkInterfaceIntegrationFixture::runCommandSync(
    RemoteCommandRequest& request) {
    auto deferred = runCommand(makeCallbackHandle(), request);
    auto& res = deferred.get();
    if (res.isOK()) {
        log() << "got command result: " << res.toString();
    } else {
        log() << "command failed: " << res.status;
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
