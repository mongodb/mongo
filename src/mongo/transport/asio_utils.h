/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#pragma once

#include "mongo/base/status.h"
#include "mongo/base/system_error.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"

#ifndef _WIN32
#include <sys/poll.h>
#endif  // ndef _WIN32

#include <asio.hpp>

namespace mongo {
namespace transport {

inline SockAddr endpointToSockAddr(const asio::generic::stream_protocol::endpoint& endPoint) {
    struct sockaddr_storage sa = {};
    memcpy(&sa, endPoint.data(), endPoint.size());
    SockAddr wrappedAddr(sa, endPoint.size());
    return wrappedAddr;
}

// Utility function to turn an ASIO endpoint into a mongo HostAndPort
inline HostAndPort endpointToHostAndPort(const asio::generic::stream_protocol::endpoint& endPoint) {
    return HostAndPort(endpointToSockAddr(endPoint));
}

inline Status errorCodeToStatus(const std::error_code& ec) {
    if (!ec)
        return Status::OK();

    if (ec == asio::error::operation_aborted) {
        return {ErrorCodes::CallbackCanceled, "Callback was canceled"};
    }

#ifdef _WIN32
    if (ec == asio::error::timed_out) {
#else
    if (ec == asio::error::try_again || ec == asio::error::would_block) {
#endif
        return {ErrorCodes::NetworkTimeout, "Socket operation timed out"};
    } else if (ec == asio::error::eof || ec == asio::error::connection_reset ||
               ec == asio::error::network_reset) {
        return {ErrorCodes::HostUnreachable, "Connection was closed"};
    }

    // If the ec.category() is a mongoErrorCategory() then this error was propogated from
    // mongodb code and we should just pass the error cdoe along as-is.
    ErrorCodes::Error errorCode = (ec.category() == mongoErrorCategory())
        ? ErrorCodes::Error(ec.value())
        // Otherwise it's an error code from the network and we should pass it along as a
        // SocketException
        : ErrorCodes::SocketException;
    // Either way, include the error message.
    return {errorCode, ec.message()};
}

/*
 * The ASIO implementation of poll (i.e. socket.wait()) cannot poll for a mask of events, and
 * doesn't support timeouts.
 *
 * This wraps up ::select/::poll for Windows/POSIX for a single socket and handles EINTR on POSIX
 *
 * - On timeout: it returns Status(ErrorCodes::NetworkTimeout)
 * - On poll returning with an event: it returns the EventsMask for the socket, the caller must
 * check whether it matches the expected events mask.
 * - On error: it returns a Status(ErrorCodes::InternalError)
 */
template <typename Socket, typename EventsMask>
StatusWith<EventsMask> pollASIOSocket(Socket& socket, EventsMask mask, Milliseconds timeout) {
#ifdef _WIN32
    fd_set readfds;
    fd_set writefds;
    fd_set errfds;

    FD_ZERO(&readfds);
    FD_ZERO(&writefds);
    FD_ZERO(&errfds);

    auto fd = socket.native_handle();
    if (mask & POLLIN) {
        FD_SET(fd, &readfds);
    }
    if (mask & POLLOUT) {
        FD_SET(fd, &writefds);
    }
    FD_SET(fd, &errfds);

    timeval timeoutTv{};
    auto timeoutUs = duration_cast<Microseconds>(timeout);
    if (timeoutUs >= Seconds{1}) {
        auto timeoutSec = duration_cast<Seconds>(timeoutUs);
        timeoutTv.tv_sec = timeoutSec.count();
        timeoutUs -= timeoutSec;
    }
    timeoutTv.tv_usec = timeoutUs.count();
    int result = ::select(1, &readfds, &writefds, &errfds, &timeoutTv);
    if (result == SOCKET_ERROR) {
        auto errDesc = errnoWithDescription(WSAGetLastError());
        return {ErrorCodes::InternalError, errDesc};
    }
    int revents = (FD_ISSET(fd, &readfds) ? POLLIN : 0) | (FD_ISSET(fd, &writefds) ? POLLOUT : 0) |
        (FD_ISSET(fd, &errfds) ? POLLERR : 0);
#else
    pollfd pollItem = {};
    pollItem.fd = socket.native_handle();
    pollItem.events = mask;

    int result;
    boost::optional<Date_t> expiration;
    if (timeout.count() > 0) {
        expiration = Date_t::now() + timeout;
    }
    do {
        Milliseconds curTimeout;
        if (expiration) {
            curTimeout = *expiration - Date_t::now();
            if (curTimeout.count() <= 0) {
                result = 0;
                break;
            }
        } else {
            curTimeout = timeout;
        }
        result = ::poll(&pollItem, 1, curTimeout.count());
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
        int errCode = errno;
        return {ErrorCodes::InternalError, errnoWithDescription(errCode)};
    }
    int revents = pollItem.revents;
#endif

    if (result == 0) {
        return {ErrorCodes::NetworkTimeout, "Timed out waiting for poll"};
    } else {
        return revents;
    }
}

/**
 * Pass this to asio functions in place of a callback to have them return a Future<T>. This behaves
 * similarly to asio::use_future_t, however it returns a mongo::Future<T> rather than a
 * std::future<T>.
 *
 * The type of the Future will be determined by the arguments that the callback would have if one
 * was used. If the arguments start with std::error_code, it will be used to set the Status of the
 * Future and will not affect the Future's type. For the remaining arguments:
 *  - if none: Future<void>
 *  - if one: Future<T>
 *  - more than one: Future<std::tuple<A, B, ...>>
 *
 * Example:
 *    Future<size_t> future = my_socket.async_read_some(my_buffer, UseFuture{});
 */
struct UseFuture {};

namespace use_future_details {

template <typename... Args>
struct AsyncHandlerHelper {
    using Result = std::tuple<Args...>;
    static void complete(Promise<Result>* promise, Args... args) {
        promise->emplaceValue(args...);
    }
};

template <>
struct AsyncHandlerHelper<> {
    using Result = void;
    static void complete(SharedPromise<Result>* promise) {
        promise->emplaceValue();
    }
};

template <typename Arg>
struct AsyncHandlerHelper<Arg> {
    using Result = Arg;
    static void complete(SharedPromise<Result>* promise, Arg arg) {
        promise->emplaceValue(arg);
    }
};

template <typename... Args>
struct AsyncHandlerHelper<std::error_code, Args...> {
    using Helper = AsyncHandlerHelper<Args...>;
    using Result = typename Helper::Result;

    template <typename... Args2>
    static void complete(SharedPromise<Result>* promise, std::error_code ec, Args2&&... args) {
        if (ec) {
            promise->setError(errorCodeToStatus(ec));
        } else {
            Helper::complete(promise, std::forward<Args2>(args)...);
        }
    }
};

template <>
struct AsyncHandlerHelper<std::error_code> {
    using Result = void;
    static void complete(SharedPromise<Result>* promise, std::error_code ec) {
        if (ec) {
            promise->setError(errorCodeToStatus(ec));
        } else {
            promise->emplaceValue();
        }
    }
};

template <typename... Args>
struct AsyncHandler {
    using Helper = AsyncHandlerHelper<Args...>;
    using Result = typename Helper::Result;

    explicit AsyncHandler(UseFuture) {}

    template <typename... Args2>
    void operator()(Args2&&... args) {
        Helper::complete(&promise, std::forward<Args2>(args)...);
    }

    SharedPromise<Result> promise;
};

template <typename... Args>
struct AsyncResult {
    using completion_handler_type = AsyncHandler<Args...>;
    using RealResult = typename AsyncHandler<Args...>::Result;
    using return_type = Future<RealResult>;

    explicit AsyncResult(completion_handler_type& handler) {
        Promise<RealResult> promise;
        fut = promise.getFuture();
        handler.promise = promise.share();
    }

    auto get() {
        return std::move(fut);
    }

    Future<RealResult> fut;
};

}  // namespace use_future_details
}  // namespace transport
}  // namespace mongo

namespace asio {
template <typename Comp, typename Sig>
class async_result;

template <typename Result, typename... Args>
class async_result<::mongo::transport::UseFuture, Result(Args...)>
    : public ::mongo::transport::use_future_details::AsyncResult<Args...> {
    using ::mongo::transport::use_future_details::AsyncResult<Args...>::AsyncResult;
};
}  // namespace asio
