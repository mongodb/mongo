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

#include <utility>

#include "mongo/base/system_error.h"
#include "mongo/config.h"
#include "mongo/db/wire_version.h"
#include "mongo/executor/async_stream.h"
#include "mongo/executor/async_stream_factory.h"
#include "mongo/executor/async_stream_interface.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {
namespace executor {

using asio::ip::tcp;

NetworkInterfaceASIO::AsyncConnection::AsyncConnection(std::unique_ptr<AsyncStreamInterface> stream,
                                                       rpc::ProtocolSet protocols)
    : _stream(std::move(stream)),
      _serverProtocols(protocols),
      _clientProtocols(rpc::computeProtocolSet(WireSpec::instance().minWireVersionOutgoing,
                                               WireSpec::instance().maxWireVersionOutgoing)) {}

AsyncStreamInterface& NetworkInterfaceASIO::AsyncConnection::stream() {
    return *_stream;
}

void NetworkInterfaceASIO::AsyncConnection::cancel() {
    _stream->cancel();
}

rpc::ProtocolSet NetworkInterfaceASIO::AsyncConnection::serverProtocols() const {
    return _serverProtocols;
}

rpc::ProtocolSet NetworkInterfaceASIO::AsyncConnection::clientProtocols() const {
    return _clientProtocols;
}

void NetworkInterfaceASIO::AsyncConnection::setServerProtocols(rpc::ProtocolSet protocols) {
    _serverProtocols = protocols;
}

void NetworkInterfaceASIO::_connect(AsyncOp* op) {
    LOG(1) << "Connecting to " << op->request().target.toString();

    tcp::resolver::query query(op->request().target.host(),
                               std::to_string(op->request().target.port()));
    // TODO: Investigate how we might hint or use shortcuts to resolve when possible.
    const auto thenConnect = [this, op](std::error_code ec, tcp::resolver::iterator endpoints) {
        if (endpoints == tcp::resolver::iterator()) {
            // Workaround a bug in ASIO returning an invalid resolver iterator (with a non-error
            // std::error_code) when file descriptors are exhausted.
            ec = make_error_code(ErrorCodes::HostUnreachable);
        }
        _validateAndRun(
            op, ec, [this, op, endpoints]() { _setupSocket(op, std::move(endpoints)); });
    };
    op->resolver().async_resolve(query, op->_strand.wrap(std::move(thenConnect)));
}

void NetworkInterfaceASIO::_setupSocket(AsyncOp* op, tcp::resolver::iterator endpoints) {
    // TODO: Consider moving this call to post-auth so we only assign completed connections.
    {
        auto stream = _streamFactory->makeStream(&op->strand(), op->request().target);
        op->setConnection({std::move(stream), rpc::supports::kOpQueryOnly});
    }

    auto& stream = op->connection().stream();

    stream.connect(std::move(endpoints), [this, op](std::error_code ec) {
        _validateAndRun(op, ec, [this, op]() { _runIsMaster(op); });
    });
}

}  // namespace executor
}  // namespace mongo
