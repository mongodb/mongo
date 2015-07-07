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
#include "mongo/util/log.h"

namespace mongo {
namespace executor {

namespace {
const auto kCanceledStatus = Status(ErrorCodes::CallbackCanceled, "Callback canceled");
}  // namespace

void NetworkInterfaceASIO::_asyncRunCommand(AsyncOp* op) {
    LOG(3) << "running command " << op->request().cmdObj << " against database "
           << op->request().dbname << " across network to " << op->request().target.toString();
    if (inShutdown()) {
        return;
    }

    // _connect...() will continue the state machine.
    _connectWithDBClientConnection(op);
}

void NetworkInterfaceASIO::_messageFromRequest(const RemoteCommandRequest& request,
                                               Message* toSend,
                                               bool useOpCommand) {
    BSONObj query = request.cmdObj;
    invariant(query.isValid());

    // TODO: Once OP_COMMAND work is complete,
    // look at client to see if it supports OP_COMMAND.

    // TODO: Investigate whether we can use CommandRequestBuilder here.

    BufBuilder b;
    b.appendNum(0);  // opts
    b.appendStr(request.dbname + ".$cmd");
    b.appendNum(0);  // toSkip
    b.appendNum(1);  // toReturn, don't care about responses
    query.appendSelfToBufBuilder(b);

    // TODO: If AsyncOp can own this buffer, we can avoid copying it in setData().
    toSend->setData(dbQuery, b.buf(), b.len());
    toSend->header().setId(nextMessageId());
    toSend->header().setResponseTo(0);
}

void NetworkInterfaceASIO::_asyncSendSimpleMessage(AsyncOp* op, const asio::const_buffer& buf) {
    asio::async_write(op->connection()->sock(),
                      asio::buffer(buf),
                      [this, op](std::error_code ec, std::size_t bytes) {

                          if (op->canceled()) {
                              return _completeOperation(op, kCanceledStatus);
                          }

                          if (ec) {
                              return _networkErrorCallback(op, ec);
                          }

                          _receiveResponse(op);
                      });
}

void NetworkInterfaceASIO::_beginCommunication(AsyncOp* op) {
    if (op->canceled()) {
        return _completeOperation(op, kCanceledStatus);
    }

    Message* toSend = op->toSend();
    _messageFromRequest(op->request(), toSend);

    if (toSend->empty())
        return _completedWriteCallback(op);

    // TODO: Some day we may need to support vector messages.
    fassert(28708, toSend->buf() != 0);
    asio::const_buffer buf(toSend->buf(), toSend->size());
    return _asyncSendSimpleMessage(op, buf);
}

void NetworkInterfaceASIO::_completedWriteCallback(AsyncOp* op) {
    // If we were told to send an empty message, toRecv will be empty here.

    // TODO: handle metadata SERVER-19156
    BSONObj commandReply;
    if (op->toRecv()->empty()) {
        LOG(3) << "received an empty message";
    } else {
        QueryResult::View qr = op->toRecv()->singleData().view2ptr();
        // unavoidable copy
        commandReply = BSONObj(qr.data()).getOwned();
    }
    _completeOperation(
        op, RemoteCommandResponse(std::move(commandReply), BSONObj(), now() - op->start()));
}

void NetworkInterfaceASIO::_networkErrorCallback(AsyncOp* op, const std::error_code& ec) {
    LOG(3) << "networking error occurred";
    _completeOperation(op, Status(ErrorCodes::HostUnreachable, ec.message()));
}

// NOTE: This method may only be called by ASIO threads
// (do not call from methods entered by ReplicationExecutor threads)
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

void NetworkInterfaceASIO::_recvMessageHeader(AsyncOp* op) {
    asio::async_read(op->connection()->sock(),
                     asio::buffer(reinterpret_cast<char*>(op->header()), sizeof(MSGHEADER::Value)),
                     [this, op](asio::error_code ec, size_t bytes) {

                         if (op->canceled()) {
                             return _completeOperation(op, kCanceledStatus);
                         }

                         if (ec) {
                             LOG(3) << "error receiving header";
                             return _networkErrorCallback(op, ec);
                         }
                         _recvMessageBody(op);
                     });
}

void NetworkInterfaceASIO::_recvMessageBody(AsyncOp* op) {
    // TODO: This error code should be more meaningful.
    std::error_code ec;

    // validate message length
    int len = op->header()->constView().getMessageLength();
    if (len == 542393671) {
        LOG(3) << "attempt to access MongoDB over HTTP on the native driver port.";
        return _networkErrorCallback(op, ec);
    } else if (len == -1) {
        // TODO: An endian check is run after the client connects, we should
        // set that we've received the client's handshake
        LOG(3) << "Endian check received from client";
        return _networkErrorCallback(op, ec);
    } else if (static_cast<size_t>(len) < sizeof(MSGHEADER::Value) ||
               static_cast<size_t>(len) > MaxMessageSizeBytes) {
        warning() << "recv(): message len " << len << " is invalid. "
                  << "Min " << sizeof(MSGHEADER::Value) << " Max: " << MaxMessageSizeBytes;
        return _networkErrorCallback(op, ec);
    }

    // validate response id
    uint32_t expectedId = op->toSend()->header().getId();
    uint32_t actualId = op->header()->constView().getResponseTo();
    if (actualId != expectedId) {
        LOG(3) << "got wrong response:"
               << " expected response id: " << expectedId << ", got response id: " << actualId;
        return _networkErrorCallback(op, ec);
    }

    int z = (len + 1023) & 0xfffffc00;
    invariant(z >= len);
    op->toRecv()->setData(reinterpret_cast<char*>(mongoMalloc(z)), true);
    MsgData::View mdView = op->toRecv()->buf();

    // copy header data into master buffer
    int headerLen = sizeof(MSGHEADER::Value);
    memcpy(mdView.view2ptr(), op->header(), headerLen);
    int bodyLength = len - headerLen;
    invariant(bodyLength >= 0);

    // receive remaining data into md->data
    asio::async_read(op->connection()->sock(),
                     asio::buffer(mdView.data(), bodyLength),
                     [this, op, mdView](asio::error_code ec, size_t bytes) {

                         if (op->canceled()) {
                             return _completeOperation(op, kCanceledStatus);
                         }

                         if (ec) {
                             LOG(3) << "error receiving message body";
                             return _networkErrorCallback(op, ec);
                         }

                         return _completedWriteCallback(op);
                     });
}

void NetworkInterfaceASIO::_receiveResponse(AsyncOp* op) {
    _recvMessageHeader(op);
}

}  // namespace executor
}  // namespace mongo
