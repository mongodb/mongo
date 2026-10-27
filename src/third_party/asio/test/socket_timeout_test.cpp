// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/unittest/unittest.h"

#include <chrono>
#include <cstdlib>

#include <asio/buffer.hpp>
#include <asio/error.hpp>
#include <asio/error_code.hpp>
#include <asio/ip/tcp.hpp>
#include <asio/local/stream_protocol.hpp>
#include <asio/socket_base.hpp>

namespace mongo {
namespace {

// The worker thread deadline is intentionally larger than the socket timeout. It includes thread
// scheduling and sanitizer overhead in addition to the blocking socket operation itself.
constexpr auto kWorkerCompletionDeadline = std::chrono::seconds(5);

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
        // Some kernels store SO_SNDTIMEO/SO_RCVTIMEO in tick units, so sub-second
        // values can be rounded when read back with getsockopt().
        const auto duration = std::chrono::seconds(1);
        option = Option(duration);
        asio::error_code error;
        socket.set_option(option, error);
        ASSERT(!error) << "error: " << error.message() << ", name: " << name;
        Option after;
        socket.get_option(after, error);
        ASSERT(!error) << "error: " << error.message() << ", name: " << name;
        ASSERT_EQ(duration, after.value()) << name;
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

}  // namespace
}  // namespace mongo
