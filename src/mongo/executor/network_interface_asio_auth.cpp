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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kExecutor

#include "mongo/platform/basic.h"

#include "mongo/executor/network_interface_asio.h"

#include "mongo/rpc/reply_interface.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/legacy_request_builder.h"

namespace mongo {
namespace executor {

void NetworkInterfaceASIO::_runIsMaster(AsyncOp* op) {
    // We use a legacy builder to create our ismaster request because we may
    // have to communicate with servers that do not support OP_COMMAND
    rpc::LegacyRequestBuilder requestBuilder{};
    requestBuilder.setDatabase("admin");
    requestBuilder.setCommandName("ismaster");
    requestBuilder.setMetadata(rpc::makeEmptyMetadata());
    requestBuilder.setCommandArgs(BSON("ismaster" << 1));

    // Set current command to ismaster request and run
    auto& cmd = op->beginCommand(std::move(*(requestBuilder.done())));
    cmd.toSend().header().setResponseTo(0);

    // Callback to parse protocol information out of received ismaster response
    auto parseIsMaster = [this, op]() {
        try {
            auto commandReply = rpc::makeReply(&(op->command().toRecv()));
            BSONObj isMasterReply = commandReply->getCommandReply();

            auto protocolSet = rpc::parseProtocolSetFromIsMasterReply(isMasterReply);
            if (!protocolSet.isOK())
                return _completeOperation(op, protocolSet.getStatus());

            op->connection().setServerProtocols(protocolSet.getValue());

            // Advance the state machine
            _authenticate(op);

        } catch (...) {
            // makeReply will throw if the rely was invalid.
            _completeOperation(op, exceptionToStatus());
        }
    };

    _asyncRunCommand(&cmd,
                     [this, op, parseIsMaster](std::error_code ec, size_t bytes) {
                         _validateAndRun(op, ec, std::move(parseIsMaster));
                     });
}

void NetworkInterfaceASIO::_authenticate(AsyncOp* op) {
    // TODO: Implement asynchronous authentication, SERVER-19155
    asio::post(_io_service, [this, op]() { _beginCommunication(op); });
}

}  // namespace executor
}  // namespace mongo
