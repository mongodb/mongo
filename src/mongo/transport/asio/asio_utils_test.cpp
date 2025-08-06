/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/transport/asio/asio_utils.h"

#include "mongo/transport/asio/asio_socket_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <asio.hpp>

namespace mongo::transport {
namespace {

using namespace unittest::match;

template <typename Stream>
void writeToSocketAndPollForResponse(Stream& writeSocket, Stream& readSocket, StringData data) {
    // Write our payload to our socket.
    asio::write(writeSocket, asio::const_buffer(data.data(), data.size()));

    // Poll the other end of the connection for data before returning. Wait up to a second for data
    // to appear.
    const auto start = Date_t::now();
    while (!readSocket.available()) {
        if (Date_t::now() - start >= Seconds(1)) {
            FAIL("Data was not successfully transmitted across socket pair in time");
        }
    }
}

template <typename Stream>
void peekEmptySocket(Stream& readSocket) {
    const auto bufferSize = 10;
    auto inBuffer = std::make_unique<char[]>(bufferSize);
    const auto bytesRead =
        peekASIOStream(readSocket, asio::mutable_buffer(inBuffer.get(), bufferSize));
    ASSERT_EQ(bytesRead, 0);
}

template <typename Stream>
void peekAllSubstrings(Stream& writeSocket, Stream& readSocket, StringData data) {
    writeToSocketAndPollForResponse(writeSocket, readSocket, data);

    // Peek from the socket for all substrings up to and including the full payload size.
    // We should never block here.
    for (size_t bufferSize = 0; bufferSize <= data.size(); ++bufferSize) {
        auto inBuffer = std::make_unique<char[]>(bufferSize);
        const auto bytesRead =
            peekASIOStream(readSocket, asio::mutable_buffer(inBuffer.get(), bufferSize));
        ASSERT_THAT(StringData(inBuffer.get(), bytesRead), Eq(data.substr(0, bufferSize)));
    }
}

template <typename Stream>
void peekPastBuffer(Stream& writeSocket, Stream& readSocket, StringData data) {
    writeToSocketAndPollForResponse(writeSocket, readSocket, data);

    // Peek from the socket more than is available. We should just get what is available.
    const auto bufferSize = data.size() + 1;
    for (size_t attemptCount = 0; attemptCount < 3; ++attemptCount) {
        auto inBuffer = std::make_unique<char[]>(bufferSize);
        const auto bytesRead =
            peekASIOStream(readSocket, asio::mutable_buffer(inBuffer.get(), bufferSize));
        ASSERT_THAT(StringData(inBuffer.get(), bytesRead), Eq(data));
    }
}

#ifdef ASIO_HAS_LOCAL_SOCKETS

TEST(ASIOUtils, PeekEmptySocketUnixBlocking) {
    UnixSocketPair socks;
    auto& readSocket = socks.clientSocket();

    peekEmptySocket(readSocket);
}

TEST(ASIOUtils, PeekEmptySocketUnixNonBlocking) {
    UnixSocketPair socks;
    auto& readSocket = socks.clientSocket();
    readSocket.non_blocking(true);

    peekEmptySocket(readSocket);
}

TEST(ASIOUtils, PeekAvailableBytesUnixBlocking) {
    UnixSocketPair socks;
    auto& writeSocket = socks.serverSocket();
    auto& readSocket = socks.clientSocket();

    peekAllSubstrings(writeSocket, readSocket, "example"_sd);
}

TEST(ASIOUtils, PeekAvailableBytesUnixNonBlocking) {
    UnixSocketPair socks;
    auto& writeSocket = socks.serverSocket();
    auto& readSocket = socks.clientSocket();
    readSocket.non_blocking(true);

    peekAllSubstrings(writeSocket, readSocket, "example"_sd);
}

TEST(ASIOUtils, PeekPastAvailableBytesUnixBlocking) {
    UnixSocketPair socks;
    auto& writeSocket = socks.serverSocket();
    auto& readSocket = socks.clientSocket();

    peekPastBuffer(writeSocket, readSocket, "example"_sd);
}

TEST(ASIOUtils, PeekPastAvailableBytesUnixNonBlocking) {
    UnixSocketPair socks;
    auto& writeSocket = socks.serverSocket();
    auto& readSocket = socks.clientSocket();
    readSocket.non_blocking(true);

    peekPastBuffer(writeSocket, readSocket, "example"_sd);
}
#endif  // ASIO_HAS_LOCAL_SOCKETS

TEST(ASIOUtils, PeekEmptySocketTCPBlocking) {
    TCPSocketPair sockets;
    peekEmptySocket(sockets.clientSocket());
}

TEST(ASIOUtils, PeekEmptySocketTCPNonBlocking) {
    TCPSocketPair sockets;
    sockets.clientSocket().non_blocking(true);
    peekEmptySocket(sockets.clientSocket());
}

TEST(ASIOUtils, PeekAvailableBytesTCPBlocking) {
    TCPSocketPair sockets;
    peekAllSubstrings(sockets.serverSocket(), sockets.clientSocket(), "example"_sd);
}

TEST(ASIOUtils, PeekAvailableBytesTCPNonBlocking) {
    TCPSocketPair sockets;
    sockets.clientSocket().non_blocking(true);
    peekAllSubstrings(sockets.serverSocket(), sockets.clientSocket(), "example"_sd);
}

TEST(ASIOUtils, PeekPastAvailableBytesTCPBlocking) {
    TCPSocketPair sockets;
    peekPastBuffer(sockets.serverSocket(), sockets.clientSocket(), "example"_sd);
}

TEST(ASIOUtils, PeekPastAvailableBytesTCPNonBlocking) {
    TCPSocketPair sockets;
    sockets.clientSocket().non_blocking(true);
    peekPastBuffer(sockets.serverSocket(), sockets.clientSocket(), "example"_sd);
}
}  // namespace
}  // namespace mongo::transport
