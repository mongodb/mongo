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
#include "mongo/executor/async_stream_factory.h"
#include "mongo/executor/async_stream_interface.h"
#include "mongo/executor/async_timer_asio.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/executor/network_interface_asio_integration_fixture.h"
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
void NetworkInterfaceASIOIntegrationFixture::startNet(NetworkInterfaceASIO::Options options) {
    options.streamFactory = stdx::make_unique<AsyncStreamFactory>();
    options.timerFactory = stdx::make_unique<AsyncTimerFactoryASIO>();
#ifdef _WIN32
    // Connections won't queue on widnows, so attempting to open too many connections
    // concurrently will result in refused connections and test failure.
    options.connectionPoolOptions.maxConnections = 16u;
#else
    options.connectionPoolOptions.maxConnections = 256u;
#endif
    _net = stdx::make_unique<NetworkInterfaceASIO>(std::move(options));
    _net->startup();
}

void NetworkInterfaceASIOIntegrationFixture::tearDown() {
    if (!_net->inShutdown()) {
        _net->shutdown();
    }
}

NetworkInterfaceASIO& NetworkInterfaceASIOIntegrationFixture::net() {
    return *_net;
}

ConnectionString NetworkInterfaceASIOIntegrationFixture::fixture() {
    return unittest::getFixtureConnectionString();
}

void NetworkInterfaceASIOIntegrationFixture::setRandomNumberGenerator(PseudoRandom* generator) {
    _rng = generator;
}

PseudoRandom* NetworkInterfaceASIOIntegrationFixture::getRandomNumberGenerator() {
    return _rng;
}

void NetworkInterfaceASIOIntegrationFixture::startCommand(
    const TaskExecutor::CallbackHandle& cbHandle,
    RemoteCommandRequest& request,
    StartCommandCB onFinish) {
    net().startCommand(cbHandle, request, onFinish).transitional_ignore();
}

Deferred<RemoteCommandResponse> NetworkInterfaceASIOIntegrationFixture::runCommand(
    const TaskExecutor::CallbackHandle& cbHandle, RemoteCommandRequest& request) {
    Deferred<RemoteCommandResponse> deferred;
    net()
        .startCommand(
            cbHandle,
            request,
            [deferred](RemoteCommandResponse resp) mutable { deferred.emplace(std::move(resp)); })
        .transitional_ignore();
    return deferred;
}

RemoteCommandResponse NetworkInterfaceASIOIntegrationFixture::runCommandSync(
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

void NetworkInterfaceASIOIntegrationFixture::assertCommandOK(StringData db,
                                                             const BSONObj& cmd,
                                                             Milliseconds timeoutMillis) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db.toString(), cmd, BSONObj(), nullptr, timeoutMillis};
    auto res = runCommandSync(request);
    ASSERT_OK(res.status);
    ASSERT_OK(getStatusFromCommandResult(res.data));
    ASSERT(!res.data["writeErrors"]);
}

void NetworkInterfaceASIOIntegrationFixture::assertCommandFailsOnClient(
    StringData db, const BSONObj& cmd, ErrorCodes::Error reason, Milliseconds timeoutMillis) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db.toString(), cmd, BSONObj(), nullptr, timeoutMillis};
    auto res = runCommandSync(request);
    ASSERT_EQ(reason, res.status.code());
}

void NetworkInterfaceASIOIntegrationFixture::assertCommandFailsOnServer(
    StringData db, const BSONObj& cmd, ErrorCodes::Error reason, Milliseconds timeoutMillis) {
    RemoteCommandRequest request{
        fixture().getServers()[0], db.toString(), cmd, BSONObj(), nullptr, timeoutMillis};
    auto res = runCommandSync(request);
    ASSERT_OK(res.status);
    auto serverStatus = getStatusFromCommandResult(res.data);
    ASSERT_EQ(reason, serverStatus);
}

void NetworkInterfaceASIOIntegrationFixture::assertWriteError(StringData db,
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
