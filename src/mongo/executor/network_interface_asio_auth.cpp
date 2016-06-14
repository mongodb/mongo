/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/executor/network_interface_asio.h"

#include "mongo/bson/util/builder.h"
#include "mongo/client/authenticate.h"
#include "mongo/config.h"
#include "mongo/db/auth/authorization_manager_global.h"
#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/db/commands.h"
#include "mongo/db/server_options.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/legacy_request_builder.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/ssl_manager.h"

namespace mongo {
namespace executor {

using ResponseStatus = TaskExecutor::ResponseStatus;

void NetworkInterfaceASIO::_runIsMaster(AsyncOp* op) {
    // We use a legacy builder to create our ismaster request because we may
    // have to communicate with servers that do not support OP_COMMAND
    rpc::LegacyRequestBuilder requestBuilder{};
    requestBuilder.setDatabase("admin");
    requestBuilder.setCommandName("isMaster");

    BSONObjBuilder bob;
    bob.append("isMaster", 1);
    bob.append("hangUpOnStepDown", false);

    if (Command::testCommandsEnabled) {
        // Only include the host:port of this process in the isMaster command request if test
        // commands are enabled. mongobridge uses this field to identify the process opening a
        // connection to it.
        StringBuilder sb;
        sb << getHostName() << ':' << serverGlobalParams.port;
        bob.append("hostInfo", sb.str());
    }

    requestBuilder.setCommandArgs(bob.done());
    requestBuilder.setMetadata(rpc::makeEmptyMetadata());

    // Set current command to ismaster request and run
    auto beginStatus = op->beginCommand(
        requestBuilder.done(), AsyncCommand::CommandType::kRPC, op->request().target);
    if (!beginStatus.isOK()) {
        return _completeOperation(op, beginStatus);
    }

    // Callback to parse protocol information out of received ismaster response
    auto parseIsMaster = [this, op]() {

        auto swCommandReply = op->command()->response(rpc::Protocol::kOpQuery, now());
        if (!swCommandReply.isOK()) {
            return _completeOperation(op, swCommandReply.getStatus());
        }

        auto commandReply = std::move(swCommandReply.getValue());

        // Ensure that the isMaster response is "ok:1".
        auto commandStatus = getStatusFromCommandResult(commandReply.data);
        if (!commandStatus.isOK()) {
            return _completeOperation(op, commandStatus);
        }

        auto protocolSet = rpc::parseProtocolSetFromIsMasterReply(commandReply.data);
        if (!protocolSet.isOK())
            return _completeOperation(op, protocolSet.getStatus());

        op->connection().setServerProtocols(protocolSet.getValue());

        invariant(op->connection().clientProtocols() != rpc::supports::kNone);
        // Set the operation protocol
        auto negotiatedProtocol =
            rpc::negotiate(op->connection().serverProtocols(), op->connection().clientProtocols());

        if (!negotiatedProtocol.isOK()) {
            // Add relatively verbose logging here, since this should not happen unless we are
            // mongos and we try to connect to a node that doesn't support OP_COMMAND.
            warning() << "failed to negotiate protocol with remote host: " << op->request().target;
            warning() << "request was: " << op->request().cmdObj;
            warning() << "response was: " << commandReply.data;

            auto clientProtos = rpc::toString(op->connection().clientProtocols());
            if (clientProtos.isOK()) {
                warning() << "our (client) supported protocols: " << clientProtos.getValue();
            }
            auto serverProtos = rpc::toString(op->connection().serverProtocols());
            if (serverProtos.isOK()) {
                warning() << "remote server's supported protocols:" << serverProtos.getValue();
            }
            return _completeOperation(op, negotiatedProtocol.getStatus());
        }

        op->setOperationProtocol(negotiatedProtocol.getValue());

        if (_hook) {
            // Run the validation hook.
            auto validHost = callNoexcept(
                *_hook, &NetworkConnectionHook::validateHost, op->request().target, commandReply);
            if (!validHost.isOK()) {
                return _completeOperation(op, validHost);
            }
        }

        return _authenticate(op);

    };

    _asyncRunCommand(op, [this, op, parseIsMaster](std::error_code ec, size_t bytes) {
        _validateAndRun(op, ec, std::move(parseIsMaster));
    });
}

void NetworkInterfaceASIO::_authenticate(AsyncOp* op) {
    // There is currently no way for NetworkInterfaceASIO's users to run a command
    // without going through _authenticate(). Callers may want to run certain commands,
    // such as ismasters, pre-auth. We may want to offer this choice in the future.

    // This check is sufficient to see if auth is enabled on the system,
    // and avoids creating dependencies on deeper, less accessible auth code.
    if (!isInternalAuthSet()) {
        return _runConnectionHook(op);
    }

    // We will only have a valid clientName if SSL is enabled.
    std::string clientName;
#ifdef MONGO_CONFIG_SSL
    if (getSSLManager()) {
        clientName = getSSLManager()->getSSLConfiguration().clientSubjectName;
    }
#endif

    // authenticateClient will use this to run auth-related commands over our connection.
    auto runCommandHook = [this, op](executor::RemoteCommandRequest request,
                                     auth::AuthCompletionHandler handler) {

        // SERVER-14170: Set the metadataHook to nullptr explicitly as we cannot write metadata
        // here.
        auto beginStatus = op->beginCommand(request, nullptr);
        if (!beginStatus.isOK()) {
            return handler(beginStatus);
        }

        auto callAuthCompletionHandler = [this, op, handler]() {
            auto authResponse = op->command()->response(op->operationProtocol(), now(), nullptr);
            handler(authResponse);
        };

        _asyncRunCommand(op,
                         [this, op, callAuthCompletionHandler](std::error_code ec, size_t bytes) {
                             _validateAndRun(op, ec, callAuthCompletionHandler);
                         });
    };

    // This will be called when authentication has completed.
    auto authHook = [this, op](auth::AuthResponse response) {
        if (!response.isOK())
            return _completeOperation(op, response);
        return _runConnectionHook(op);
    };

    auto params = getInternalUserAuthParamsWithFallback();
    auth::authenticateClient(
        params, op->request().target.host(), clientName, runCommandHook, authHook);
}

}  // namespace executor
}  // namespace mongo
