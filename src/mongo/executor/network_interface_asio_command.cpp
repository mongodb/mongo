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

#include <type_traits>
#include <utility>

#include "mongo/db/dbmessage.h"
#include "mongo/db/jsobj.h"
#include "mongo/executor/async_stream_interface.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/protocol.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {
namespace executor {

/**
 * The following send - receive utility functions are "stateless" in that they exist
 * apart from the AsyncOp state machine.
 */

namespace {

using asio::ip::tcp;
using ResponseStatus = TaskExecutor::ResponseStatus;

// A type conforms to the NetworkHandler concept if it is a callable type that takes a
// std::error_code and std::size_t and returns void. The std::error_code parameter is used
// to inform the handler if the asynchronous operation it was waiting on succeeded, and the size_t
// parameter conveys how many bytes were read or written.
template <typename FunctionLike>
using IsNetworkHandler =
    std::is_convertible<FunctionLike, stdx::function<void(std::error_code, std::size_t)>>;

template <typename Handler>
void asyncSendMessage(AsyncStreamInterface& stream, Message* m, Handler&& handler) {
    static_assert(IsNetworkHandler<Handler>::value,
                  "Handler passed to asyncSendMessage does not conform to NetworkHandler concept");
    // TODO: Some day we may need to support vector messages.
    fassert(28708, m->buf() != 0);
    stream.write(asio::buffer(m->buf(), m->size()), std::forward<Handler>(handler));
}

template <typename Handler>
void asyncRecvMessageHeader(AsyncStreamInterface& stream,
                            MSGHEADER::Value* header,
                            Handler&& handler) {
    static_assert(
        IsNetworkHandler<Handler>::value,
        "Handler passed to asyncRecvMessageHeader does not conform to NetworkHandler concept");
    stream.read(asio::buffer(header->view().view2ptr(), sizeof(decltype(*header))),
                std::forward<Handler>(handler));
}

template <typename Handler>
void asyncRecvMessageBody(AsyncStreamInterface& stream,
                          MSGHEADER::Value* header,
                          Message* m,
                          Handler&& handler) {
    static_assert(
        IsNetworkHandler<Handler>::value,
        "Handler passed to asyncRecvMessageBody does not conform to NetworkHandler concept");
    // validate message length
    int len = header->constView().getMessageLength();
    if (len == 542393671) {
        LOG(3) << "attempt to access MongoDB over HTTP on the native driver port.";
        return handler(make_error_code(ErrorCodes::ProtocolError), 0);
    } else if (static_cast<size_t>(len) < sizeof(MSGHEADER::Value) ||
               static_cast<size_t>(len) > MaxMessageSizeBytes) {
        warning() << "recv(): message len " << len << " is invalid. "
                  << "Min " << sizeof(MSGHEADER::Value) << " Max: " << MaxMessageSizeBytes;
        return handler(make_error_code(ErrorCodes::InvalidLength), 0);
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
    stream.read(asio::buffer(mdView.data(), bodyLength), std::forward<Handler>(handler));
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

    // _connect() will continue the state machine.
    _connect(op);
}

ResponseStatus NetworkInterfaceASIO::_responseFromMessage(const Message& received,
                                                          rpc::Protocol protocol) {
    try {
        // TODO: elapsed isn't going to be correct here, SERVER-19697
        auto start = now();
        auto reply = rpc::makeReply(&received);

        if (reply->getProtocol() != protocol) {
            auto requestProtocol = rpc::toString(static_cast<rpc::ProtocolSet>(protocol));
            if (!requestProtocol.isOK())
                return requestProtocol.getStatus();

            return Status(ErrorCodes::RPCProtocolNegotiationFailed,
                          str::stream() << "Mismatched RPC protocols - request was '"
                                        << requestProtocol.getValue().toString() << "' '"
                                        << " but reply was '" << opToString(received.operation())
                                        << "'");
        }

        // unavoidable copy
        auto ownedCommandReply = reply->getCommandReply().getOwned();
        auto ownedReplyMetadata = reply->getMetadata().getOwned();
        return ResponseStatus(
            RemoteCommandResponse(ownedCommandReply, ownedReplyMetadata, now() - start));
    } catch (...) {
        // makeReply can throw if the reply was invalid.
        return exceptionToStatus();
    }
}

void NetworkInterfaceASIO::_beginCommunication(AsyncOp* op) {
    auto& cmd = op->beginCommand(op->request(), op->operationProtocol());

    _asyncRunCommand(&cmd,
                     [this, op](std::error_code ec, size_t bytes) {
                         _validateAndRun(op, ec, [this, op]() { _completedOpCallback(op); });
                     });
}

void NetworkInterfaceASIO::_completedOpCallback(AsyncOp* op) {
    // TODO: handle metadata readers.
    auto response = _responseFromMessage(op->command().toRecv(), op->operationProtocol());
    _completeOperation(op, response);
}

void NetworkInterfaceASIO::_networkErrorCallback(AsyncOp* op, const std::error_code& ec) {
    LOG(3) << "networking error occurred";
    if (ec.category() == mongoErrorCategory()) {
        // If we get a Mongo error code, we can preserve it.
        _completeOperation(op, Status(ErrorCodes::fromInt(ec.value()), ec.message()));
    } else {
        // If we get an asio or system error, we just convert it to a network error.
        _completeOperation(op, Status(ErrorCodes::HostUnreachable, ec.message()));
    }
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
        if (ec != ErrorCodes::OK)
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
            cmd->conn().stream(), &cmd->header(), &cmd->toRecv(), std::move(recvMessageCallback));
    };

    // Step 2
    auto sendMessageCallback = [this, cmd, handler, recvHeaderCallback](std::error_code ec,
                                                                        size_t bytes) {
        if (ec != ErrorCodes::OK)
            return handler(ec, bytes);

        asyncRecvMessageHeader(cmd->conn().stream(), &cmd->header(), std::move(recvHeaderCallback));
    };

    // Step 1
    asyncSendMessage(cmd->conn().stream(), &cmd->toSend(), std::move(sendMessageCallback));
}

}  // namespace executor
}  // namespace mongo
