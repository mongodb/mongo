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

#include "mongo/transport/asio_utils.h"

#include <asio.hpp>

#include "mongo/unittest/assert_that.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/time_support.h"

namespace mongo::transport {
namespace {

using namespace unittest::match;

template <typename Stream>
void writeToSocketAndPollForResponse(Stream& writeSocket, Stream& readSocket, StringData data) {
    // Write our payload to our socket.
    asio::write(writeSocket, asio::const_buffer(data.rawData(), data.size()));

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
auto prepareUnixSocketPair(asio::io_context& io_context) {
    asio::local::stream_protocol::socket writeSocket(io_context);
    asio::local::stream_protocol::socket readSocket(io_context);
    asio::local::connect_pair(writeSocket, readSocket);
    readSocket.non_blocking(true);

    return std::pair(std::move(writeSocket), std::move(readSocket));
}

TEST(ASIOUtils, PeekAvailableBytes) {
    asio::io_context io_context;
    auto [writeSocket, readSocket] = prepareUnixSocketPair(io_context);

    peekAllSubstrings(writeSocket, readSocket, "example"_sd);
}

TEST(ASIOUtils, PeekPastAvailableBytes) {
    asio::io_context io_context;
    auto [writeSocket, readSocket] = prepareUnixSocketPair(io_context);

    peekPastBuffer(writeSocket, readSocket, "example"_sd);
}
#endif  // ASIO_HAS_LOCAL_SOCKETS

auto prepareTCPSocketPair(asio::io_context& io_context) {
    // Make a local loopback connection on an arbitrary ephemeral port.
    asio::ip::tcp::endpoint ep(asio::ip::make_address("127.0.0.1"), 0);
    asio::ip::tcp::acceptor acceptor(io_context, ep.protocol());
    {
        std::error_code ec;
        acceptor.bind(ep, ec);
        uassertStatusOK(errorCodeToStatus(ec));
    }
    acceptor.listen();

    asio::ip::tcp::socket readSocket(io_context, ep.protocol());
    readSocket.connect(acceptor.local_endpoint());
    asio::ip::tcp::socket writeSocket(io_context);
    acceptor.accept(writeSocket);
    writeSocket.non_blocking(false);
    // Set no_delay so that our output doesn't get buffered in a kernel buffer.
    writeSocket.set_option(asio::ip::tcp::no_delay(true));
    readSocket.non_blocking(true);

    return std::pair(std::move(writeSocket), std::move(readSocket));
}

TEST(ASIOUtils, PeekAvailableBytesTCP) {
    asio::io_context io_context;
    auto [writeSocket, readSocket] = prepareTCPSocketPair(io_context);

    peekAllSubstrings(writeSocket, readSocket, "example"_sd);
}

TEST(ASIOUtils, PeekPastAvailableBytesTCP) {
    asio::io_context io_context;
    auto [writeSocket, readSocket] = prepareTCPSocketPair(io_context);

    peekPastBuffer(writeSocket, readSocket, "example"_sd);
}
}  // namespace
}  // namespace mongo::transport
