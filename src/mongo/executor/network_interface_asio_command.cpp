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

#include <utility>

#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/request_builder_interface.h"
#include "mongo/util/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace executor {

/**
 * The following send - receive utility functions are "stateless" in that they exist
 * apart from the AsyncOp state machine.
 */

namespace {

using asio::ip::tcp;
using NetworkOpHandler = stdx::function<void(std::error_code, size_t)>;

// TODO: Consider templatizing on handler here to avoid using stdx::functions.
void asyncSendMessage(tcp::socket& sock, Message* m, NetworkOpHandler handler) {
    // TODO: Some day we may need to support vector messages.
    fassert(28708, m->buf() != 0);
    asio::const_buffer buf(m->buf(), m->size());
    asio::async_write(sock, asio::buffer(buf), handler);
}

void asyncRecvMessageHeader(tcp::socket& sock, MSGHEADER::Value* header, NetworkOpHandler handler) {
    asio::async_read(
        sock, asio::buffer(header->view().view2ptr(), sizeof(MSGHEADER::Value)), handler);
}

void asyncRecvMessageBody(tcp::socket& sock,
                          MSGHEADER::Value* header,
                          Message* m,
                          NetworkOpHandler handler) {
    // TODO: This error code should be more meaningful.
    std::error_code ec;

    // validate message length
    int len = header->constView().getMessageLength();
    if (len == 542393671) {
        LOG(3) << "attempt to access MongoDB over HTTP on the native driver port.";
        return handler(ec, 0);
    } else if (static_cast<size_t>(len) < sizeof(MSGHEADER::Value) ||
               static_cast<size_t>(len) > MaxMessageSizeBytes) {
        warning() << "recv(): message len " << len << " is invalid. "
                  << "Min " << sizeof(MSGHEADER::Value) << " Max: " << MaxMessageSizeBytes;
        return handler(ec, 0);
    }

    int z = (len + 1023) & 0xfffffc00;
    invariant(z >= len);
    m->setData(reinterpret_cast<char*>(mongoMalloc(z)), true);
    MsgData::View mdView = m->buf();

    // copy header data into master buffer
    int headerLen = sizeof(MSGHEADER::Value);
    memcpy(mdView.view2ptr(), header, headerLen);
    int bodyLength = len - headerLen;
    invariant(bodyLength >= 0);

    // receive remaining data into md->data
    asio::async_read(sock, asio::buffer(mdView.data(), bodyLength), handler);
}

}  // namespace

NetworkInterfaceASIO::AsyncCommand::AsyncCommand(AsyncConnection* conn) : _conn(conn) {}

void NetworkInterfaceASIO::AsyncCommand::reset() {
    // TODO: Optimize reuse of Messages to be more space-efficient.
    _toSend.reset();
    _toRecv.reset();
}

NetworkInterfaceASIO::AsyncConnection& NetworkInterfaceASIO::AsyncCommand::conn() {
    return *_conn;
}

Message& NetworkInterfaceASIO::AsyncCommand::toSend() {
    return _toSend;
}

void NetworkInterfaceASIO::AsyncCommand::setToSend(Message&& message) {
    _toSend = std::move(message);
}

Message& NetworkInterfaceASIO::AsyncCommand::toRecv() {
    return _toRecv;
}

MSGHEADER::Value& NetworkInterfaceASIO::AsyncCommand::header() {
    return _header;
}

void NetworkInterfaceASIO::_startCommand(AsyncOp* op) {
    LOG(3) << "running command " << op->request().cmdObj << " against database "
           << op->request().dbname << " across network to " << op->request().target.toString();
    if (inShutdown()) {
        return;
    }

    // _connect...() will continue the state machine.
    _connectWithDBClientConnection(op);
}

std::unique_ptr<Message> NetworkInterfaceASIO::_messageFromRequest(
    const RemoteCommandRequest& request, rpc::Protocol protocol) {
    BSONObj query = request.cmdObj;
    auto requestBuilder = rpc::makeRequestBuilder(protocol);

    // TODO: handle metadata writers
    auto toSend = rpc::makeRequestBuilder(protocol)
                      ->setDatabase(request.dbname)
                      .setCommandName(request.cmdObj.firstElementFieldName())
                      .setMetadata(request.metadata)
                      .setCommandArgs(request.cmdObj)
                      .done();

    toSend->header().setId(nextMessageId());
    toSend->header().setResponseTo(0);

    return toSend;
}

void NetworkInterfaceASIO::_beginCommunication(AsyncOp* op) {
    auto negotiatedProtocol =
        rpc::negotiate(op->connection()->serverProtocols(), op->connection()->clientProtocols());

    if (!negotiatedProtocol.isOK()) {
        return _completeOperation(op, negotiatedProtocol.getStatus());
    }

    op->setOperationProtocol(negotiatedProtocol.getValue());

    auto& cmd = op->beginCommand(
        std::move(*_messageFromRequest(op->request(), negotiatedProtocol.getValue())));

    _asyncRunCommand(&cmd,
                     [this, op](std::error_code ec, size_t bytes) {
                         _validateAndRun(op, ec, [this, op]() { _completedOpCallback(op); });
                     });
}

void NetworkInterfaceASIO::_completedOpCallback(AsyncOp* op) {
    // If we were told to send an empty message, toRecv will be empty here.

    // TODO: handle metadata readers
    const auto elapsed = [this, op]() { return now() - op->start(); };

    if (op->command().toRecv().empty()) {
        LOG(3) << "received an empty message";
        return _completeOperation(op, RemoteCommandResponse(BSONObj(), BSONObj(), elapsed()));
    }

    try {
        auto reply = rpc::makeReply(&(op->command().toRecv()));

        if (reply->getProtocol() != op->operationProtocol()) {
            return _completeOperation(
                op,
                Status(ErrorCodes::RPCProtocolNegotiationFailed,
                       str::stream() << "Mismatched RPC protocols - request was '"
                                     << opToString(op->command().toSend().operation()) << "' '"
                                     << " but reply was '"
                                     << opToString(op->command().toRecv().operation()) << "'"));
        }

        _completeOperation(op,
                           // unavoidable copy
                           RemoteCommandResponse(reply->getCommandReply().getOwned(),
                                                 reply->getMetadata().getOwned(),
                                                 elapsed()));
    } catch (...) {
        // makeReply can throw if the reply was invalid.
        _completeOperation(op, exceptionToStatus());
    }
}

void NetworkInterfaceASIO::_networkErrorCallback(AsyncOp* op, const std::error_code& ec) {
    LOG(3) << "networking error occurred";
    _completeOperation(op, Status(ErrorCodes::HostUnreachable, ec.message()));
}

// NOTE: This method may only be called by ASIO threads
// (do not call from methods entered by TaskExecutor threads)
void NetworkInterfaceASIO::_completeOperation(AsyncOp* op, const ResponseStatus& resp) {
    op->finish(resp);

    {
        // NOTE: op will be deleted in the call to erase() below.
        // It is invalid to reference op after this point.
        stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
        _inProgress.erase(op);
    }

    signalWorkAvailable();
}

void NetworkInterfaceASIO::_asyncRunCommand(AsyncCommand* cmd, NetworkOpHandler handler) {
    // We invert the following steps below to run a command:
    // 1 - send the given command
    // 2 - receive a header for the response
    // 3 - validate and receive response body
    // 4 - advance the state machine by calling handler()

    // Step 4
    auto recvMessageCallback =
        [this, cmd, handler](std::error_code ec, size_t bytes) { handler(ec, bytes); };

    // Step 3
    auto recvHeaderCallback = [this, cmd, handler, recvMessageCallback](std::error_code ec,
                                                                        size_t bytes) {
        if (ec)
            return handler(ec, bytes);

        // validate response id
        uint32_t expectedId = cmd->toSend().header().getId();
        uint32_t actualId = cmd->header().constView().getResponseTo();
        if (actualId != expectedId) {
            LOG(3) << "got wrong response:"
                   << " expected response id: " << expectedId << ", got response id: " << actualId;
            // TODO: This error code should be more meaningful.
            return handler(ec, bytes);
        }

        asyncRecvMessageBody(
            cmd->conn().sock(), &cmd->header(), &cmd->toRecv(), std::move(recvMessageCallback));
    };

    // Step 2
    auto sendMessageCallback = [this, cmd, handler, recvHeaderCallback](std::error_code ec,
                                                                        size_t bytes) {
        if (ec)
            return handler(ec, bytes);

        asyncRecvMessageHeader(cmd->conn().sock(), &cmd->header(), std::move(recvHeaderCallback));
    };

    // Step 1
    asyncSendMessage(cmd->conn().sock(), &cmd->toSend(), std::move(sendMessageCallback));
}

}  // namespace executor
}  // namespace mongo
