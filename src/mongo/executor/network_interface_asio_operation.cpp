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

namespace mongo {
namespace executor {

using asio::ip::tcp;

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

void NetworkInterfaceASIO::AsyncOp::connect(ConnectionPool* const pool,
                                            asio::io_service* service,
                                            Date_t now) {
    // TODO(amidvidy): why is this hardcoded to 1 second? That seems too low.
    ConnectionPool::ConnectionPtr conn(pool, _request.target, now, Milliseconds(1000));

    _state = OpState::kConnectionAcquired;

    // TODO: Add a case here for unix domain sockets.
    int protocol = conn.get()->port().localAddr().getType();
    if (protocol != AF_INET && protocol != AF_INET6) {
        throw SocketException(SocketException::CONNECT_ERROR, "Unsupported family");
    }

    _state = OpState::kConnectionVerified;

    tcp::socket sock{
        *service, protocol == AF_INET ? tcp::v4() : tcp::v6(), conn.get()->port().psock->rawFD()};

    _connection.emplace(std::move(sock), conn.get()->getServerRPCProtocols(), std::move(conn));

    _state = OpState::kConnected;
}

NetworkInterfaceASIO::AsyncConnection* NetworkInterfaceASIO::AsyncOp::connection() {
    invariant(_connection.is_initialized());
    return _connection.get_ptr();
}

void NetworkInterfaceASIO::AsyncOp::setConnection(AsyncConnection&& conn) {
    invariant(!_connection.is_initialized());
    _connection = std::move(conn);
    _state = OpState::kConnected;
}

bool NetworkInterfaceASIO::AsyncOp::connected() const {
    return (_state == OpState::kConnected ||
            // NOTE: if we fail at kConnectionVerified,
            // ASIO will have closed the socket, don't disconnect
            _state == OpState::kConnectionAcquired);
}

void NetworkInterfaceASIO::AsyncOp::finish(const ResponseStatus& status) {
    _onFinish(status);
    _state = OpState::kCompleted;
}

MSGHEADER::Value* NetworkInterfaceASIO::AsyncOp::header() {
    return &_header;
}

const RemoteCommandRequest& NetworkInterfaceASIO::AsyncOp::request() const {
    return _request;
}

Date_t NetworkInterfaceASIO::AsyncOp::start() const {
    return _start;
}

Message* NetworkInterfaceASIO::AsyncOp::toSend() {
    invariant(_toSend.is_initialized());
    return _toSend.get_ptr();
}

void NetworkInterfaceASIO::AsyncOp::setToSend(Message&& message) {
    invariant(!_toSend.is_initialized());
    _toSend = std::move(message);
}

Message* NetworkInterfaceASIO::AsyncOp::toRecv() {
    return &_toRecv;
}

rpc::Protocol NetworkInterfaceASIO::AsyncOp::operationProtocol() const {
    invariant(_operationProtocol.is_initialized());
    return *_operationProtocol;
}

void NetworkInterfaceASIO::AsyncOp::setOperationProtocol(rpc::Protocol proto) {
    invariant(!_operationProtocol.is_initialized());
    _operationProtocol = proto;
}

}  // namespace executor
}  // namespace mongo
