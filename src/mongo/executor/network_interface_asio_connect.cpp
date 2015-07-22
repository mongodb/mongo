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

#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"

namespace mongo {
namespace executor {

using asio::ip::tcp;

NetworkInterfaceASIO::AsyncConnection::AsyncConnection(asio::ip::tcp::socket&& sock,
                                                       rpc::ProtocolSet protocols)
    : AsyncConnection(std::move(sock), protocols, boost::none) {}

NetworkInterfaceASIO::AsyncConnection::AsyncConnection(
    asio::ip::tcp::socket&& sock,
    rpc::ProtocolSet protocols,
    boost::optional<ConnectionPool::ConnectionPtr>&& bootstrapConn)
    : _sock(std::move(sock)),
      _serverProtocols(protocols),
      _bootstrapConn(std::move(bootstrapConn)) {}

#if defined(_MSC_VER) && _MSC_VER < 1900
NetworkInterfaceASIO::AsyncConnection::AsyncConnection(AsyncConnection&& other)
    : _stream(std::move(other._stream)),
      _serverProtocols(other._serverProtocols),
      _clientProtocols(other._clientProtocols) {}

NetworkInterfaceASIO::AsyncConnection& NetworkInterfaceASIO::AsyncConnection::operator=(
    AsyncConnection&& other) {
    _stream = std::move(other._stream);
    _serverProtocols = other._serverProtocols;
    _clientProtocols = other._clientProtocols;
    return *this;
}
#endif

asio::ip::tcp::socket& NetworkInterfaceASIO::AsyncConnection::sock() {
    return _sock;
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

void NetworkInterfaceASIO::_connectASIO(AsyncOp* op) {
    tcp::resolver::query query(op->request().target.host(),
                               std::to_string(op->request().target.port()));
    // TODO: Investigate how we might hint or use shortcuts to resolve when possible.
    _resolver.async_resolve(
        query,
        [this, op](std::error_code ec, asio::ip::basic_resolver_iterator<tcp> endpoints) {
            _validateAndRun(op, ec, [this, op, endpoints]() { _setupSocket(op, endpoints); });
        });
}

void NetworkInterfaceASIO::_connectWithDBClientConnection(AsyncOp* op) {
    // connect in a separate thread to avoid blocking the rest of the system
    stdx::thread t([this, op]() {
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

            auto status = exceptionToStatus();

            asio::post(_io_service,
                       [this, op, status]() { return _completeOperation(op, status); });
            return;
        }

        // send control back to main thread(pool)
        asio::post(_io_service, [this, op]() { _beginCommunication(op); });
    });

    t.detach();
}

void NetworkInterfaceASIO::_setupSocket(AsyncOp* op, const tcp::resolver::iterator& endpoints) {
    tcp::socket sock(_io_service);
    AsyncConnection conn(std::move(sock), rpc::supports::kOpQueryOnly);

    // TODO: Consider moving this call to post-auth so we only assign completed connections.
    op->setConnection(std::move(conn));

    asio::async_connect(op->connection()->sock(),
                        std::move(endpoints),
                        [this, op](std::error_code ec, tcp::resolver::iterator iter) {
                            _validateAndRun(op, ec, [this, op]() { _sslHandshake(op); });
                        });
}

}  // namespace executor
}  // namespace mongo
