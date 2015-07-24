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

#include "mongo/config.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/net/sock.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#endif

namespace mongo {
namespace executor {

using asio::ip::tcp;

class NetworkInterfaceASIO::AsyncStream final : public AsyncStreamInterface {
public:
    // TODO: after we get rid of the bootstrap connection path, change this constructor
    // to take the io_service instead to more closely match AsyncSecureStream
    AsyncStream(tcp::socket&& stream) : _stream(std::move(stream)) {}

    void connect(tcp::resolver::iterator iter, ConnectHandler&& connectHandler) override {
        asio::async_connect(
            _stream,
            std::move(iter),
            // We need to wrap this with a lambda of the right signature so it compiles, even
            // if we don't actually use the resolver iterator.
            [this, connectHandler](std::error_code ec, tcp::resolver::iterator) {
                return connectHandler(ec);
            });
    }

    void write(asio::const_buffer buffer, StreamHandler&& streamHandler) override {
        asio::async_write(_stream, asio::buffer(buffer), std::move(streamHandler));
    }

    void read(asio::mutable_buffer buffer, StreamHandler&& streamHandler) override {
        asio::async_read(_stream, asio::buffer(buffer), std::move(streamHandler));
    }

private:
    tcp::socket _stream;
};

NetworkInterfaceASIO::AsyncConnection::AsyncConnection(std::unique_ptr<AsyncStreamInterface> stream,
                                                       rpc::ProtocolSet protocols)
    : _stream(std::move(stream)), _serverProtocols(protocols) {}

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

AsyncStreamInterface& NetworkInterfaceASIO::AsyncConnection::stream() {
    return *_stream;
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
    tcp::resolver::query query(op->request().target.host(),
                               std::to_string(op->request().target.port()));
    // TODO: Investigate how we might hint or use shortcuts to resolve when possible.
    const auto thenConnect = [this, op](std::error_code ec, tcp::resolver::iterator endpoints) {
        _validateAndRun(op,
                        ec,
                        [this, op, endpoints]() {

#ifdef MONGO_CONFIG_SSL
            int sslModeVal = getSSLGlobalParams().sslMode.load();
            if (sslModeVal == SSLParams::SSLMode_preferSSL ||
                sslModeVal == SSLParams::SSLMode_requireSSL) {
                invariant(_sslContext.is_initialized());
                return _setupSecureSocket(op, std::move(endpoints));
            }
#endif
            _setupSocket(op, std::move(endpoints));

                        });
    };
    _resolver.async_resolve(query, std::move(thenConnect));
}

void NetworkInterfaceASIO::_setupSocket(AsyncOp* op, tcp::resolver::iterator endpoints) {
    // TODO: Consider moving this call to post-auth so we only assign completed connections.
    op->setConnection(AsyncConnection(stdx::make_unique<AsyncStream>(tcp::socket{_io_service}),
                                      rpc::supports::kOpQueryOnly));

    auto& stream = op->connection().stream();

    stream.connect(std::move(endpoints),
                   [this, op](std::error_code ec) {
                       _validateAndRun(op, ec, [this, op]() { _authenticate(op); });
                   });
}

}  // namespace executor
}  // namespace mongo
