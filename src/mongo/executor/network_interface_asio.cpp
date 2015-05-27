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

#include <chrono>

#include "mongo/bson/bsonobj.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context_impl.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sock.h"

namespace mongo {
namespace executor {

using ResponseStatus = TaskExecutor::ResponseStatus;
using asio::ip::tcp;
using RemoteCommandCompletionFn = stdx::function<void(const ResponseStatus&)>;

NetworkInterfaceASIO::AsyncOp::AsyncOp(const TaskExecutor::CallbackHandle& cbHandle,
                                       const RemoteCommandRequest& request,
                                       const RemoteCommandCompletionFn& onFinish,
                                       Date_t now,
                                       int id)
    : _cbHandle(cbHandle),
      _request(request),
      _onFinish(onFinish),
      _start(now),
      _state(OpState::kReady),
      _canceled(0),
      _id(id) {}

void NetworkInterfaceASIO::AsyncOp::connect(ConnectionPool* const pool,
                                            asio::io_service* service,
                                            Date_t now) {
    _conn = stdx::make_unique<ConnectionPool::ConnectionPtr>(
        pool, _request.target, now, Milliseconds(1000));

    _state = OpState::kConnectionAcquired;

    // TODO: Add a case here for unix domain sockets.
    int protocol = _conn->get()->port().localAddr().getType();
    if (protocol != AF_INET && protocol != AF_INET6) {
        throw SocketException(SocketException::CONNECT_ERROR, "Unsupported family");
    }

    _state = OpState::kConnectionVerified;

    _sock = stdx::make_unique<tcp::socket>(
        *service, protocol == AF_INET ? tcp::v4() : tcp::v6(), _conn->get()->port().psock->rawFD());

    _state = OpState::kConnected;
}

std::string NetworkInterfaceASIO::AsyncOp::toString() const {
    str::stream output;
    output << "op number: " << _id;

    output << ", state: ";
    if (_state == OpState::kReady) {
        output << "kReady";
    } else if (_state == OpState::kConnectionAcquired) {
        output << "kConnectionAcquired";
    } else if (_state == OpState::kConnectionVerified) {
        output << "kConnectionVerified";
    } else if (_state == OpState::kConnected) {
        output << "kConnected";
    } else if (_state == OpState::kCompleted) {
        output << "kCompleted";
    } else {
        MONGO_UNREACHABLE;
    }

    output << "\n";
    return output;
}

void NetworkInterfaceASIO::AsyncOp::cancel() {
    // An operation may be in mid-flight when it is canceled, so we
    // do not disconnect immediately upon cancellation.
    _canceled.store(1);
}

bool NetworkInterfaceASIO::AsyncOp::canceled() const {
    return (_canceled.load() == 1);
}

const TaskExecutor::CallbackHandle& NetworkInterfaceASIO::AsyncOp::cbHandle() const {
    return _cbHandle;
}

bool NetworkInterfaceASIO::AsyncOp::connected() const {
    return (_state == OpState::kConnected ||
            // NOTE: if we fail at kConnectionVerified,
            // ASIO will have closed the socket, don't disconnect
            _state == OpState::kConnectionAcquired);
}

void NetworkInterfaceASIO::AsyncOp::finish(const ResponseStatus& status) {
    _onFinish(status);
}

void NetworkInterfaceASIO::AsyncOp::complete(Date_t now) {
    if (canceled()) {
        finish(ResponseStatus(ErrorCodes::CallbackCanceled, "Callback canceled"));
    } else {
        finish(ResponseStatus(Response(_output, Milliseconds(now - start()))));
    }

    _state = OpState::kCompleted;
}

MSGHEADER::Value* NetworkInterfaceASIO::AsyncOp::header() {
    return &_header;
}

const RemoteCommandRequest& NetworkInterfaceASIO::AsyncOp::request() const {
    return _request;
}

void NetworkInterfaceASIO::AsyncOp::setOutput(const BSONObj& bson) {
    _output = bson;
}

tcp::socket* NetworkInterfaceASIO::AsyncOp::sock() {
    return _sock.get();
}

Date_t NetworkInterfaceASIO::AsyncOp::start() const {
    return _start;
}

Message* NetworkInterfaceASIO::AsyncOp::toRecv() {
    return &_toRecv;
}

Message* NetworkInterfaceASIO::AsyncOp::toSend() {
    return &_toSend;
}

NetworkInterfaceASIO::NetworkInterfaceASIO()
    : _io_service(), _state(State::kReady), _isExecutorRunnable(false), _numOps(0) {
    _connPool = stdx::make_unique<ConnectionPool>(kMessagingPortKeepOpen);
}

std::string NetworkInterfaceASIO::getDiagnosticString() {
    str::stream output;
    output << "NetworkInterfaceASIO";
    output << " inShutdown: " << inShutdown();
    output << " _numOps: " << _numOps.loadRelaxed();
    return output;
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
    asio::async_write(*(op->sock()),
                      asio::buffer(buf),
                      [this, op](std::error_code ec, std::size_t bytes) {
                          if (ec)
                              return _networkErrorCallback(op, ec);

                          if (op->canceled())
                              return _completeOperation(op);

                          _receiveResponse(op);
                      });
}

void NetworkInterfaceASIO::_receiveResponse(AsyncOp* op) {
    _recvMessageHeader(op);
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
    asio::async_read(*(op->sock()),
                     asio::buffer(mdView.data(), bodyLength),
                     [this, op, mdView](asio::error_code ec, size_t bytes) {
                         if (ec) {
                             LOG(3) << "error receiving message body";
                             return _networkErrorCallback(op, ec);
                         }

                         if (op->canceled())
                             return _completeOperation(op);

                         return _completedWriteCallback(op);
                     });
}

void NetworkInterfaceASIO::_recvMessageHeader(AsyncOp* op) {
    asio::async_read(*(op->sock()),
                     asio::buffer(reinterpret_cast<char*>(op->header()), sizeof(MSGHEADER::Value)),
                     [this, op](asio::error_code ec, size_t bytes) {
                         if (ec) {
                             LOG(3) << "error receiving header";
                             return _networkErrorCallback(op, ec);
                         }

                         if (op->canceled())
                             return _completeOperation(op);

                         _recvMessageBody(op);
                     });
}

void NetworkInterfaceASIO::_completedWriteCallback(AsyncOp* op) {
    // If we were told to send an empty message, toRecv will be empty here.
    if (op->toRecv()->empty()) {
        op->setOutput(BSONObj());
        LOG(3) << "received an empty message";
    } else {
        QueryResult::View qr = op->toRecv()->singleData().view2ptr();
        // unavoidable copy
        op->setOutput(BSONObj(qr.data()).getOwned());
    }
    _completeOperation(op);
}

void NetworkInterfaceASIO::_networkErrorCallback(AsyncOp* op, const std::error_code& ec) {
    LOG(3) << "networking error occurred";
    // TODO: Perhaps we should set this to a specific 'network error' value.
    op->setOutput(BSONObj());
    _completeOperation(op);
}

// NOTE: This method may only be called by ASIO threads
// (do not call from methods entered by ReplicationExecutor threads)
void NetworkInterfaceASIO::_completeOperation(AsyncOp* op) {
    op->complete(now());

    {
        // NOTE: op will be deleted in the call to erase() below.
        // It is invalid to reference op after this point.
        stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
        _inProgress.erase(op);
    }

    signalWorkAvailable();
}

void NetworkInterfaceASIO::startup() {
    _serviceRunner = std::thread([this]() {
        asio::io_service::work work(_io_service);
        _io_service.run();
    });
    _state.store(State::kRunning);
}

bool NetworkInterfaceASIO::inShutdown() const {
    return (_state.load() == State::kShutdown);
}

void NetworkInterfaceASIO::shutdown() {
    _state.store(State::kShutdown);
    _io_service.stop();
    _serviceRunner.join();
}

void NetworkInterfaceASIO::signalWorkAvailable() {
    stdx::unique_lock<stdx::mutex> lk(_executorMutex);
    _signalWorkAvailable_inlock();
}

void NetworkInterfaceASIO::_signalWorkAvailable_inlock() {
    if (!_isExecutorRunnable) {
        _isExecutorRunnable = true;
        _isExecutorRunnableCondition.notify_one();
    }
}

void NetworkInterfaceASIO::waitForWork() {
    stdx::unique_lock<stdx::mutex> lk(_executorMutex);
    // TODO: This can be restructured with a lambda.
    while (!_isExecutorRunnable) {
        _isExecutorRunnableCondition.wait(lk);
    }
    _isExecutorRunnable = false;
}

void NetworkInterfaceASIO::waitForWorkUntil(Date_t when) {
    stdx::unique_lock<stdx::mutex> lk(_executorMutex);
    // TODO: This can be restructured with a lambda.
    while (!_isExecutorRunnable) {
        const Milliseconds waitTime(when - now());
        if (waitTime <= Milliseconds(0)) {
            break;
        }
        _isExecutorRunnableCondition.wait_for(lk, waitTime);
    }
    _isExecutorRunnable = false;
}

Date_t NetworkInterfaceASIO::now() {
    return Date_t::now();
}

void NetworkInterfaceASIO::startCommand(const TaskExecutor::CallbackHandle& cbHandle,
                                        const RemoteCommandRequest& request,
                                        const RemoteCommandCompletionFn& onFinish) {
    LOG(3) << "running command " << request.cmdObj << " against database " << request.dbname
           << " across network to " << request.target.toString();

    if (inShutdown()) {
        return;
    }

    auto ownedOp =
        stdx::make_unique<AsyncOp>(cbHandle, request, onFinish, now(), _numOps.fetchAndAdd(1));

    AsyncOp* op = ownedOp.get();

    {
        stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
        _inProgress.emplace(op, std::move(ownedOp));
    }

    // connect in a separate thread to avoid blocking the rest of the system
    std::thread t([this, op]() {
        try {
            // The call to connect() will throw if:
            // - we cannot get a new connection from the pool
            // - we get a connection from the pool, but cannot use it
            // - we fail to transfer the connection's socket to an ASIO wrapper
            op->connect(_connPool.get(), &_io_service, now());
        } catch (...) {
            LOG(3) << "failed to connect, posting mock completion";

            if (inShutdown()) {
                return;
            }

            ResponseStatus status(exceptionToStatus());
            asio::post(_io_service, [this, op, status]() { return _completeOperation(op); });
            return;
        }

        // send control back to main thread(pool)
        asio::post(_io_service,
                   [this, op]() {
                       Message* toSend = op->toSend();
                       _messageFromRequest(op->request(), toSend);
                       if (toSend->empty()) {
                           _completedWriteCallback(op);
                       } else if (op->canceled()) {
                           return _completeOperation(op);
                       } else {
                           // TODO: Some day we may need to support vector messages.
                           fassert(28708, toSend->buf() != 0);
                           asio::const_buffer buf(toSend->buf(), toSend->size());
                           return _asyncSendSimpleMessage(op, buf);
                       }
                   });
    });
    t.detach();
}

void NetworkInterfaceASIO::cancelCommand(const TaskExecutor::CallbackHandle& cbHandle) {
    stdx::lock_guard<stdx::mutex> lk(_inProgressMutex);
    for (auto iter = _inProgress.begin(); iter != _inProgress.end(); ++iter) {
        if (iter->first->cbHandle() == cbHandle) {
            iter->first->cancel();
            break;
        }
    }
}

}  // namespace executor
}  // namespace mongo
