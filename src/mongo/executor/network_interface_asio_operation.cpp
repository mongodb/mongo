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
#include "mongo/executor/async_stream_interface.h"

namespace mongo {
namespace executor {

using asio::ip::tcp;

NetworkInterfaceASIO::AsyncOp::AsyncOp(const TaskExecutor::CallbackHandle& cbHandle,
                                       const RemoteCommandRequest& request,
                                       const RemoteCommandCompletionFn& onFinish,
                                       Date_t now)
    : _cbHandle(cbHandle), _request(request), _onFinish(onFinish), _start(now), _canceled(0) {}

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

NetworkInterfaceASIO::AsyncConnection& NetworkInterfaceASIO::AsyncOp::connection() {
    invariant(_connection.is_initialized());
    return *_connection;
}

void NetworkInterfaceASIO::AsyncOp::setConnection(AsyncConnection&& conn) {
    invariant(!_connection.is_initialized());
    _connection = std::move(conn);
}

NetworkInterfaceASIO::AsyncCommand& NetworkInterfaceASIO::AsyncOp::beginCommand(
    Message&& newCommand) {
    // NOTE: We operate based on the assumption that AsyncOp's
    // AsyncConnection does not change over its lifetime.
    invariant(_connection.is_initialized());
    if (_command.is_initialized()) {
        // We can just reset our state if initialized.
        _command->reset();
    } else {
        _command.emplace(_connection.get_ptr());
    }
    _command->setToSend(std::move(newCommand));
    return _command.get();
}

NetworkInterfaceASIO::AsyncCommand& NetworkInterfaceASIO::AsyncOp::command() {
    invariant(_command.is_initialized());
    return _command.get();
}

void NetworkInterfaceASIO::AsyncOp::finish(const ResponseStatus& status) {
    _onFinish(status);
}

const RemoteCommandRequest& NetworkInterfaceASIO::AsyncOp::request() const {
    return _request;
}

Date_t NetworkInterfaceASIO::AsyncOp::start() const {
    return _start;
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
