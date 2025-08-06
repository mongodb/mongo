/**
 *    Copyright (C) 2025-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/unittest/temp_dir.h"

#include <filesystem>

#include <asio/io_context.hpp>
#include <asio/ip/address.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/local/stream_protocol.hpp>

/*
 * This component provides utilities for testing ASIO socket behavior.
 * It is used both by `//src/mongo/transport/asio` and by `//src/third_party/mongo/test`.
 */

namespace mongo {

#ifdef ASIO_HAS_LOCAL_SOCKETS
/*
 * The easiest way to get a connected pair of local (unix domain) sockets is to use
 * `asio::local::connect_pair`. However, newer versions of Windows (Windows 10+) that support
 * unix domain sockets do not support `asio::local::connect_pair`. Instead, we can use
 * `UnixSocketPair`.
 *
 * `UnixSocketPair` creates a unix domain socket file in a temporary directory and opens a
 * connected pair of sockets associated with the file:
 *
 * - `clientSocket()` returns a reference to the `connect`-ed socket.
 * - `serverSocket()` returns a reference to the `accept`-ed socket.
 *
 * When the `UnixSocketPair` is destroyed, the temporary directory and the contained socket file
 * are deleted.
 */
class UnixSocketPair {
    unittest::TempDir _tempDir;
    asio::io_context _io_context;
    asio::local::stream_protocol::socket _serverSocket;
    asio::local::stream_protocol::socket _clientSocket;

public:
    UnixSocketPair()
        : _tempDir("UnixSocketPair"), _serverSocket(_io_context), _clientSocket(_io_context) {
        const std::filesystem::path dirPath = _tempDir.path();
        const asio::local::stream_protocol::endpoint sockPath((dirPath / "sock").string());

        // `local::stream_protocol::acceptor` has a boolean parameter that defaults to `true`.
        // If `true`, then the `SO_REUSEADDR` option is set on the underlying listener socket.
        // On Windows, this causes the subsequent `bind` to fail.
        // `SO_REUSEADDR` is not of any use for unix domain sockets, so specify `false` here to
        // prevent it from being set.
        const bool reuse_address = false;
        asio::local::stream_protocol::acceptor acceptor(_io_context, sockPath, reuse_address);
        acceptor.listen();

        _clientSocket.connect(sockPath);
        _clientSocket.non_blocking(false);  // block by default

        acceptor.accept(_serverSocket);
        _serverSocket.non_blocking(false);  // block by default
    }

    ~UnixSocketPair() {
        _clientSocket.close();
        _serverSocket.close();
    }

    asio::local::stream_protocol::socket& serverSocket() {
        return _serverSocket;
    }

    asio::local::stream_protocol::socket& clientSocket() {
        return _clientSocket;
    }
};

#endif  // ASIO_HAS_LOCAL_SOCKETS

/*
 * `TCPSocketPair` binds a TCP socket to a local ephermeral address, and then
 * connects to it with another socket.
 *
 * - `clientSocket()` returns a reference to the `connect`-ed socket.
 * - `serverSocket()` returns a reference to the `accept`-ed socket.
 *
 * When the `TCPSocketPair` is destroyed, the sockets are closed.
 */
class TCPSocketPair {
    asio::io_context _io_context;
    asio::ip::tcp::endpoint _address;
    asio::ip::tcp::socket _serverSocket;
    asio::ip::tcp::socket _clientSocket;

public:
    TCPSocketPair()
        : _address(asio::ip::make_address("127.0.0.1"), 0),
          _serverSocket(_io_context),
          _clientSocket(_io_context, _address.protocol()) {
        // Make a local loopback connection on an arbitrary ephemeral port.
        asio::ip::tcp::acceptor acceptor(_io_context, _address.protocol());
        acceptor.bind(_address);
        acceptor.listen();

        _clientSocket.connect(acceptor.local_endpoint());
        _clientSocket.non_blocking(false);  // blocking by default

        acceptor.accept(_serverSocket);
        _serverSocket.non_blocking(false);  // blocking by default
    }

    ~TCPSocketPair() {
        _clientSocket.close();
        _serverSocket.close();
    }

    asio::ip::tcp::socket& serverSocket() {
        return _serverSocket;
    }

    asio::ip::tcp::socket& clientSocket() {
        return _clientSocket;
    }
};

}  // namespace mongo
