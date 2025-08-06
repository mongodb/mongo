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

#include "mongo/transport/asio/asio_socket_test_util.h"
#include "mongo/unittest/join_thread.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/scopeguard.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <future>
#include <ostream>
#include <vector>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/error_code.hpp>
#include <asio/local/stream_protocol.hpp>
#include <asio/socket_base.hpp>

namespace mongo {
namespace {

/**
 * `timeoutOptionTest` is code shared among test cases that verify the behavior of timeout
 * option setting and getting. We don't actually do any timing out, but instead just set and get
 * timeout options on sockets.
 *
 * Each test case is run multiple times: on both TCP and Unix sockets, and for the recv timeout
 * option and the send timeout option.
 *
 * The actual test logic is in a lambda expression passed as a function argument. The lambda
 * accepts three arguments: a socket, a socket option, and a string that identifies the
 * socket/option combination for use in error messages. The socket option argument will be a
 * default initialized instance of either `send_timeout` or `recv_timeout` -- a test case might
 * use the value directly or just note its type and create a separate instance of the option
 * within the test.
 */
template <typename TestFunc>
void timeoutOptionTest(TestFunc&& test) {
    asio::io_context io_context;
    asio::socket_base::send_timeout send;
    asio::socket_base::recv_timeout recv;

    asio::ip::tcp::socket tcp(io_context, asio::ip::tcp::v4());
    test(tcp, send, "tcp send");
    test(tcp, recv, "tcp recv");
#ifdef ASIO_HAS_LOCAL_SOCKETS
    asio::local::stream_protocol::socket unix(io_context, asio::local::stream_protocol{});
    test(unix, send, "unix send");
    test(unix, recv, "unix recv");
#endif  // ASIO_HAS_LOCAL_SOCKETS
}

/**
 * Verify that send and recv timeouts default to zero when they have not been given values. Zero
 * means "no timeout."
 */
TEST(AsioSocketTimeoutTest, TimeoutDefaultsToZero) {
    timeoutOptionTest([](auto& socket, auto option, const char* name) {
        asio::error_code error;
        socket.get_option(option, error);
        ASSERT(!error) << "error: " << error.message() << ", name: " << name;
        const auto duration = option.value();
        ASSERT_EQ(duration, duration.zero()) << name;
    });
}

/**
 * Verify that setting a timeout option on a socket and then getting the same option results in
 * the same value.
 */
TEST(AsioSocketTimeoutTest, TimeoutSetAndGetAreConsistent) {
    timeoutOptionTest([](auto& socket, auto option, const char* name) {
        using Option = decltype(option);
        const auto duration = std::chrono::milliseconds(42);
        option = Option(duration);
        asio::error_code error;
        socket.set_option(option, error);
        ASSERT(!error) << "error: " << error.message() << ", name: " << name;
        Option after;
        socket.get_option(after, error);
        ASSERT(!error) << "error: " << error.message() << ", name: " << name;
        ASSERT_EQ(duration, after.value());
    });
}

/**
 * Verify that specifying a very small, but nonzero, value for a timeout option results in a
 * nonzero value. This test makes sure that we don't accidentally round down to zero, because
 * zero means "no timeout," which is not a small timeout.
 */
TEST(AsioSocketTimeoutTest, TimeoutTinyIsNotZero) {
    timeoutOptionTest([](auto& socket, auto option, const char* name) {
        // On POSIX the smallest representable timeout is one microsecond, on Windows it's one
        // millisecond.
        using Option = decltype(option);
        option = Option(std::chrono::nanoseconds(120));
        asio::error_code error;
        socket.set_option(option, error);
        ASSERT(!error) << "error: " << error.message() << ", name: " << name;
        socket.get_option(option, error);
        ASSERT(!error) << "error: " << error.message() << ", name: " << name;
        ASSERT_GT(option.value(), option.value().zero()) << name;
    });
}

// `IOResult` encompasses the result of a `send()` or `recv()` operation, including how long it
// took. In tests that verify that send/recv timeouts actually time out, there will be a thread
// that is sending or receiving, and it will place the result of its send/recv in a promise that
// is consumed by the main test thread.
// Then the main test thread can assert things like "it timed out," and "it took about as long
// as the configured timeout."
struct IOResult {
    std::size_t bytesTransferred;
    asio::error_code error;
    std::chrono::nanoseconds duration;
};

// for ASSERT messages, e.g.
//     IOResult{.bytesTransferred=0, .error=[Connection timed out (asio.system:110)],
//     .duration=1100022ns}
std::ostream& operator<<(std::ostream& out, const IOResult& result) {
    return out << "IOResult{.bytesTransferred=" << result.bytesTransferred << ", .error=["
               << result.error.message() << " (" << result.error
               << ")], .duration=" << result.duration << "}";
}

/*
 * `syncSendTimeoutTest` is the shared logic among all of the synchronous socket send timeout
 * tests. The tests differ only in the type of socket used (TCP vs. Unix) and the type of buffer
 * used (single contiguous buffer vs. iovec-style segmented buffer).
 *
 * A specific unit test initializes its sockets and its buffer, and then passes them to
 * `syncSendTimeoutTest`, which performs the test.
 *
 * In the test, the sender is the server, i.e. `send()` is called on the socket that was
 * returned by `accept()`. A small timeout is set on the socket before sending, and then the
 * test verifies that the send failed with a timeout error after a significant portion of the
 * configured timeout elapsed. We only `recv()` when the test is complete (to unblock the sender
 * in the case of test failure).
 *
 * There's one complication. Networking stacks will acknowledge large amounts of data without
 * requiring a receiver to have `recv()`'d the data. To work around this, `syncSendTimeoutTest`
 * first sends as much data as it can in non-blocking mode, then sets the socket back to
 * blocking mode, and then performs the blocking `send()`. Even this is not enough to prevent
 * the client from acknowledging sent data earlier than expected, though, and so callers of
 * `syncSendTimeoutTest` should specify a large `dataToSend` -- something on the order of four
 * megabytes (in my testing, I found that Linux would acknowlege ~2.5 MiB of data even after a
 * previous non-blocking send had returned EWOULDBLOCK, all without any receiver).
 */
template <typename SocketPair, typename ByteSequence>
void syncSendTimeoutTest(SocketPair& sockets, ByteSequence dataToSend) {
    auto& server = sockets.serverSocket();
    auto& client = sockets.clientSocket();

    // The server will send and the client will eventually receive.
    const auto timeout = std::chrono::milliseconds(100);
    server.set_option(asio::socket_base::send_timeout(timeout));

    std::promise<IOResult> resultPromise;
    std::future<IOResult> resultFuture = resultPromise.get_future();

    unittest::JoinThread sender{[&]() {
        const int flags = 0;
        asio::error_code error;
        // Buffering between the sender and the receiver means that we can sometimes send data
        // immediately even if there is nobody on the other end receiving.
        // Send as much data as we can without blocking, and then do a sending block to test the
        // timeout behavior.
        server.non_blocking(true);
        std::size_t bytesTransferred;
        do {
            bytesTransferred = server.send(dataToSend, flags, error);
        } while (bytesTransferred > 0 ||
                 !(error == asio::error::would_block || error == asio::error::try_again));

        server.non_blocking(false);
        const auto before = std::chrono::steady_clock::now();
        bytesTransferred = server.send(dataToSend, flags, error);
        const auto after = std::chrono::steady_clock::now();

        // Let the client know that we're done sending.
        server.shutdown(asio::ip::tcp::socket::shutdown_send);

        resultPromise.set_value(IOResult{
            .bytesTransferred = bytesTransferred, .error = error, .duration = after - before});
    }};

    ScopeGuard cleanup([&] {
        // Drain the connection, just in case `sender` is still blocking in `send`.
        std::vector<char> buf3(1024);
        std::size_t bytes;
        try {
            do {
                bytes = client.receive(asio::buffer(buf3.data(), buf3.size()));
            } while (bytes);
        } catch (const std::exception&) {
            // End of file, or an error. Either way, the sender thread is probably done.
        }
    });

    const std::future_status status = resultFuture.wait_for(timeout * 2);
    // If after twice the configured timeout the result still isn't ready, then the timeout
    // isn't working.
    ASSERT_EQ(status, std::future_status::ready) << "The sender didn't time out.";

    const IOResult result = resultFuture.get();
    // When `send` times out, it sets the error code to either "would_block" or
    // "try_again" (POSIX), or "timed_out" (Windows).
    ASSERT(result.error == asio::error::would_block || result.error == asio::error::try_again ||
           result.error == asio::error::timed_out)
        << result;
    // A timeout error occurs only if no data was transferred.
    ASSERT_EQ(result.bytesTransferred, 0) << result;
    // We expect that the sender timed out, so their `send` operation should have taken a
    // significant portion of the timeout time. Realistically, the duration will be larger than
    // the timeout, but to play it safe let's require that it was at least 75% of the timeout.
    ASSERT_GT(result.duration, 3 * timeout / 4) << result;
}

/**
 * Verify that blocking send timeouts work for TCP sockets with segmented buffers.
 */
TEST(AsioSocketTimeoutTest, SyncSendTimeoutTCP) {
    TCPSocketPair sockets;

    // Send a large amount of data. Otherwise, system level buffers might allow us to `send`
    // data successfully even after getting "try_again" or "would_block" errors. On my Linux
    // workstation, ~2.5 MB can be sent this way.

    // Use two buffers so that the vectored I/O flavor of `send` is used ("sync_send").
    std::vector<char> buf1(1024 * 1024 * 8);
    std::vector<char> buf2(1024 * 1024 * 8);
    const std::array<asio::const_buffer, 2> buffers = {
        asio::const_buffer(buf1.data(), buf1.size()), asio::const_buffer(buf2.data(), buf2.size())};

    syncSendTimeoutTest(sockets, buffers);
}

/**
 * Verify that blocking send timeouts work for TCP sockets with a contiguous buffer.
 */
TEST(AsioSocketTimeoutTest, SyncSend1TimeoutTCP) {
    TCPSocketPair sockets;

    // Use one buffer so that the non-vectored I/O flavor of `send` is used ("sync_send1").
    std::vector<char> buf(1024 * 1024 * 16);
    syncSendTimeoutTest(sockets, asio::const_buffer(buf.data(), buf.size()));
}

#ifdef ASIO_HAS_LOCAL_SOCKETS

/**
 * Verify that blocking send timeouts works for Unix domain sockets with segmented buffers.
 */
TEST(AsioSocketTimeoutTest, SyncSendTimeoutUnix) {
    UnixSocketPair sockets;

    // Use two buffers so that the vectored I/O flavor of `send` is used ("sync_send").
    std::vector<char> buf1(1024 * 1024 * 8);
    std::vector<char> buf2(1024 * 1024 * 8);
    const std::array<asio::const_buffer, 2> buffers = {
        asio::const_buffer(buf1.data(), buf1.size()), asio::const_buffer(buf2.data(), buf2.size())};

    syncSendTimeoutTest(sockets, buffers);
}

/**
 * Verify that blocking send timeouts work for Unix domain sockets with a contiguous buffer.
 */
TEST(AsioSocketTimeoutTest, SyncSend1TimeoutUnix) {
    UnixSocketPair sockets;

    // Use one buffer so that the non-vectored I/O flavor of `send` is used ("sync_send1").
    std::vector<char> buf(1024 * 1024 * 16);
    syncSendTimeoutTest(sockets, asio::const_buffer(buf.data(), buf.size()));
}

#endif  // ASIO_HAS_LOCAL_SOCKETS

/*
 * `syncReceiveTimeoutTest` is the shared logic among all of the synchronous socket receive
 * timeout tests. The tests differ only in the type of socket used (TCP vs. Unix) and the type
 * of buffer used (single contiguous buffer vs. iovec-style segmented buffer).
 *
 * A specific unit test initializes its sockets and its buffer, and then passes them to
 * `syncSendTimeoutTest`, which performs the test.
 *
 * In the test, the receiver is the client, i.e. `recv()` is called on the socket that was
 * returned by `connect()`. A small timeout is set on the socket before receiving, and then the
 * test verifies that the receive failed with a timeout error after a significant portion of the
 * configured timeout elapsed. We only `shutdown()` and `close()` the other end of the socket
 * when the test is complete (to unblock the receiver in the case of test failure).
 */
template <typename SocketPair, typename ByteSequence>
void syncReceiveTimeoutTest(SocketPair& sockets, ByteSequence destination) {
    auto& server = sockets.serverSocket();
    auto& client = sockets.clientSocket();

    // The client will recv() and the server will eventually shutdown()/close(), ideally after
    // the client has timed out.
    const auto timeout = std::chrono::milliseconds(100);
    client.set_option(asio::socket_base::recv_timeout(timeout));

    std::promise<IOResult> resultPromise;
    std::future<IOResult> resultFuture = resultPromise.get_future();

    unittest::JoinThread receiver{[&]() {
        const int flags = 0;
        IOResult result;
        const auto before = std::chrono::steady_clock::now();
        result.bytesTransferred = client.receive(destination, flags, result.error);
        const auto after = std::chrono::steady_clock::now();
        result.duration = after - before;
        resultPromise.set_value(std::move(result));
    }};

    ScopeGuard cleanup([&] {
        // Close the write end of the connection, in case the receiver is still blocked in
        // `receive`.
        asio::error_code error;
        server.shutdown(asio::socket_base::shutdown_both, error);
        ASSERT(!error) << error.message();
        server.close(error);
        ASSERT(!error) << error.message();
    });

    const std::future_status status = resultFuture.wait_for(timeout * 2);
    // If after twice the configured timeout the result still isn't ready, then the timeout
    // isn't working.
    ASSERT_EQ(status, std::future_status::ready) << "The receiver didn't time out.";

    const IOResult result = resultFuture.get();
    // When `receive` times out, it sets the error code to either "would_block"
    // or "try_again" (POSIX), or "timed_out" (Windows).
    ASSERT(result.error == asio::error::would_block || result.error == asio::error::try_again ||
           result.error == asio::error::timed_out)
        << result;
    // A timeout error occurs only if no data was transferred.
    ASSERT_EQ(result.bytesTransferred, 0) << result;
    // We expect that the receiver timed out, so their `receive` operation should have taken a
    // significant portion of the timeout time. Realistically, the duration will be larger than
    // the timeout, but to play it safe let's require that it was at least 75% of the timeout.
    ASSERT_GT(result.duration, 3 * timeout / 4) << result;
}

/**
 * Verify that blocking receive timeouts work for TCP sockets with segmented buffers.
 */
TEST(AsioSocketTimeoutTest, SyncRecvTimeoutTCP) {
    TCPSocketPair sockets;

    // Use two buffers so that the vectored I/O flavor of `send` is used ("sync_send").
    std::vector<char> buf1(512);
    std::vector<char> buf2(512);
    const std::array<asio::mutable_buffer, 2> buffers = {
        asio::mutable_buffer(buf1.data(), buf1.size()),
        asio::mutable_buffer(buf2.data(), buf2.size())};

    syncReceiveTimeoutTest(sockets, buffers);
}

/**
 * Verify that blocking receive timeouts work for TCP sockets with a contiguous buffer.
 */
TEST(AsioSocketTimeoutTest, SyncRecv1TimeoutTCP) {
    TCPSocketPair sockets;

    // Use one buffer so that the non-vectored I/O flavor of `recv` is used ("sync_recv1").
    std::vector<char> buf(1024 * 1024 * 16);
    syncReceiveTimeoutTest(sockets, asio::mutable_buffer(buf.data(), buf.size()));
}

#ifdef ASIO_HAS_LOCAL_SOCKETS

/**
 * Verify that blocking receive timeouts work for Unix domain sockets with segmented buffers.
 */
TEST(AsioSocketTimeoutTest, SyncRecvTimeoutUnix) {
    UnixSocketPair sockets;

    // Use two buffers so that the vectored I/O flavor of `send` is used ("sync_send").
    std::vector<char> buf1(512);
    std::vector<char> buf2(512);
    const std::array<asio::mutable_buffer, 2> buffers = {
        asio::mutable_buffer(buf1.data(), buf1.size()),
        asio::mutable_buffer(buf2.data(), buf2.size())};

    syncReceiveTimeoutTest(sockets, buffers);
}

/**
 * Verify that blocking receive timeouts work for Unix domain sockets with a contiguous buffer.
 */
TEST(AsioSocketTimeoutTest, SyncRecv1TimeoutUnix) {
    UnixSocketPair sockets;

    // Use one buffer so that the non-vectored I/O flavor of `recv` is used ("sync_recv1").
    std::vector<char> buf(1024 * 1024 * 16);
    syncReceiveTimeoutTest(sockets, asio::mutable_buffer(buf.data(), buf.size()));
}

#endif  // ASIO_HAS_LOCAL_SOCKETS

/**
 * Verify that connecting with a negative timeout results in the "invalid_argument" error code.
 */
TEST(AsioSocketTimeoutTest, ConnectNegativeTimeoutFails) {
    const int port = 80;  // doesn't matter
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address("0100::"), port);
    asio::io_context io_context;
    asio::ip::tcp::socket socket(io_context, endpoint.protocol());
    socket.non_blocking(false);  // blocking

    asio::error_code error;
    (void)socket.connect(endpoint, std::chrono::milliseconds(-1), error);
    ASSERT_EQ(error, asio::error::invalid_argument) << error.message();
}


/**
 * Verify that connecting a socket to an endpoint can time out.
 */
TEST(AsioSocketTimeoutTest, ConnectTimeoutTCP) {
    // RFC 6666 designates the IPv6 address prefix 0100::/64 as the "discard prefix." Networks
    // are encouraged to ignore packets whose destination address has that prefix.
    // So, assuming we're on a network that implements RFC 6666, and if our connect timeout is
    // short enough (to precede any timeouts built-in to the local network stack, e.g. "host
    // unreachable"), then we can test connect timeouts by trying to connect to an address
    // having the 0100::/64 prefix, e.g. "0100::" (all zeroes after the "::").
    // It's a valid address that is unlikely to respond to your SYN.
    const int port = 80;  // doesn't matter
    asio::ip::tcp::endpoint endpoint(asio::ip::make_address("0100::"), port);
    asio::io_context io_context;
    asio::ip::tcp::socket socket(io_context, endpoint.protocol());
    // This is a quirk of how we implemented timeouts for `connect()`. The socket must be in
    // non-blocking mode. If it is in blocking mode, then there is no way to prevent the initial
    // call to `::connect()` from blocking.
    socket.non_blocking(true);

    std::promise<IOResult> resultPromise;
    std::shared_future<IOResult> resultFuture = resultPromise.get_future().share();
    const auto timeout = std::chrono::milliseconds(100);

    stdx::thread connector([&]() {
        IOResult result;
        result.bytesTransferred = 0;
        const auto before = std::chrono::steady_clock::now();
        (void)socket.connect(endpoint, timeout, result.error);
        const auto after = std::chrono::steady_clock::now();
        result.duration = after - before;
        resultPromise.set_value(std::move(result));
    });

    ScopeGuard cleanup([&] {
        // If the timeout happened, then the connector thread is done and we can `.join()` it.
        // If the timeout didn't happen, then the connector thread is still blocked in a call to
        // `socket.connect`. There's no portable way to wake up that thread.
        // Instead, in that case, detach the thread and let the system clean it up when the
        // process terminates.
        const std::future_status status = resultFuture.wait_for(std::chrono::seconds(0));
        if (status == std::future_status::ready) {
            connector.join();
        } else {
            connector.detach();
        }
    });

    const std::future_status status = resultFuture.wait_for(timeout * 2);
    // If after twice the configured timeout the result still isn't ready, then the timeout
    // isn't working.
    ASSERT_EQ(status, std::future_status::ready) << "The connector didn't time out.";

    const IOResult result = resultFuture.get();
    ASSERT(result.error == asio::error::timed_out) << result;
    // We expect that the connector timed out, so their `connect` operation should have taken a
    // significant portion of the timeout time. Realistically, the duration will be larger than
    // the timeout, but to play it safe let's require that it was at least 75% of the timeout.
    ASSERT_GT(result.duration, 3 * timeout / 4) << result;
}

}  // namespace
}  // namespace mongo
