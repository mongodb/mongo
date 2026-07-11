// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/asio/asio_utils.h"

#include "mongo/transport/asio/asio_socket_test_util.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

#include <string_view>

#include <asio.hpp>

namespace mongo::transport {
namespace {
using namespace std::literals::string_view_literals;

using namespace unittest::match;

template <typename Stream>
void writeToSocketAndPollForResponse(Stream& writeSocket,
                                     Stream& readSocket,
                                     std::string_view data) {
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
void peekAllSubstrings(Stream& writeSocket, Stream& readSocket, std::string_view data) {
    writeToSocketAndPollForResponse(writeSocket, readSocket, data);

    // Peek from the socket for all substrings up to and including the full payload size.
    // We should never block here.
    for (size_t bufferSize = 0; bufferSize <= data.size(); ++bufferSize) {
        auto inBuffer = std::make_unique<char[]>(bufferSize);
        const auto bytesRead =
            peekASIOStream(readSocket, asio::mutable_buffer(inBuffer.get(), bufferSize));
        ASSERT_THAT(std::string_view(inBuffer.get(), bytesRead), Eq(data.substr(0, bufferSize)));
    }
}

template <typename Stream>
void peekPastBuffer(Stream& writeSocket, Stream& readSocket, std::string_view data) {
    writeToSocketAndPollForResponse(writeSocket, readSocket, data);

    // Peek from the socket more than is available. We should just get what is available.
    const auto bufferSize = data.size() + 1;
    for (size_t attemptCount = 0; attemptCount < 3; ++attemptCount) {
        auto inBuffer = std::make_unique<char[]>(bufferSize);
        const auto bytesRead =
            peekASIOStream(readSocket, asio::mutable_buffer(inBuffer.get(), bufferSize));
        ASSERT_THAT(std::string_view(inBuffer.get(), bytesRead), Eq(data));
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

    peekAllSubstrings(writeSocket, readSocket, "example"sv);
}

TEST(ASIOUtils, PeekAvailableBytesUnixNonBlocking) {
    UnixSocketPair socks;
    auto& writeSocket = socks.serverSocket();
    auto& readSocket = socks.clientSocket();
    readSocket.non_blocking(true);

    peekAllSubstrings(writeSocket, readSocket, "example"sv);
}

TEST(ASIOUtils, PeekPastAvailableBytesUnixBlocking) {
    UnixSocketPair socks;
    auto& writeSocket = socks.serverSocket();
    auto& readSocket = socks.clientSocket();

    peekPastBuffer(writeSocket, readSocket, "example"sv);
}

TEST(ASIOUtils, PeekPastAvailableBytesUnixNonBlocking) {
    UnixSocketPair socks;
    auto& writeSocket = socks.serverSocket();
    auto& readSocket = socks.clientSocket();
    readSocket.non_blocking(true);

    peekPastBuffer(writeSocket, readSocket, "example"sv);
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
    peekAllSubstrings(sockets.serverSocket(), sockets.clientSocket(), "example"sv);
}

TEST(ASIOUtils, PeekAvailableBytesTCPNonBlocking) {
    TCPSocketPair sockets;
    sockets.clientSocket().non_blocking(true);
    peekAllSubstrings(sockets.serverSocket(), sockets.clientSocket(), "example"sv);
}

TEST(ASIOUtils, PeekPastAvailableBytesTCPBlocking) {
    TCPSocketPair sockets;
    peekPastBuffer(sockets.serverSocket(), sockets.clientSocket(), "example"sv);
}

TEST(ASIOUtils, PeekPastAvailableBytesTCPNonBlocking) {
    TCPSocketPair sockets;
    sockets.clientSocket().non_blocking(true);
    peekPastBuffer(sockets.serverSocket(), sockets.clientSocket(), "example"sv);
}
}  // namespace
}  // namespace mongo::transport
