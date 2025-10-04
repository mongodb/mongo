/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#pragma once

#ifndef _WIN32
#include <sys/poll.h>
#endif

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/base/system_error.h"
#include "mongo/config.h"
#include "mongo/logv2/log_severity.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/future.h"
#include "mongo/util/hex.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

#include <asio.hpp>

namespace mongo::transport {

/**
 * Generic wrapper for making an ASIO socket get_option or set_option call
 * having a payload of type `T`, which is usually just `int` so it's the default.
 * Can be value-initializd with a `T`. A reference to the payload is available
 * using the dereferencing operators.
 *
 * Provides a `toBSON` method for logging purposes.
 *
 * Models Asio GettableSocketOption and SettableSocketOption.
 * https://www.boost.org/doc/libs/1_80_0/doc/html/boost_asio/reference/GettableSocketOption.html
 * https://www.boost.org/doc/libs/1_80_0/doc/html/boost_asio/reference/SettableSocketOption.html
 *
 * The Asio-required accessors must accept a `Protocol` argument, which we ignore.
 * The kinds of options we use don't need it.
 * https://www.boost.org/doc/libs/1_80_0/doc/html/boost_asio/reference/Protocol.html
 *
 * Example:
 *     using TcpInfoOption = SocketOption<IPPROTO_TCP, TCP_INFO, tcp_info>;
 *     ...
 *     TcpInfoOption tcpiOption;
 *     socket.get_option(tcpiOption);
 *     tcp_info& infoOut = *tcpiOption;
 */
template <int optLevel, int optName, typename T = int>
class SocketOption {
public:
    SocketOption() = default;
    explicit SocketOption(T d) : _data{std::move(d)} {}
    template <typename Protocol>
    int level(const Protocol&) const {
        return optLevel;
    }
    template <typename Protocol>
    int name(const Protocol&) const {
        return optName;
    }
    template <typename Protocol>
    T* data(const Protocol&) {
        return &**this;
    }
    template <typename Protocol>
    const T* data(const Protocol&) const {
        return &**this;
    }
    template <typename Protocol>
    size_t size(const Protocol&) const {
        return sizeof(_data);
    }
    template <typename Protocol>
    void resize(const Protocol&, size_t) const {}

    T& operator*() {
        return _data;
    }
    const T& operator*() const {
        return _data;
    }

    T* operator->() {
        return &**this;
    }
    const T* operator->() const {
        return &**this;
    }

    BSONObj toBSON() const {
        return BSONObjBuilder{}
            .append("level", optLevel)
            .append("name", optName)
            .append("data", hexdump(&**this, sizeof(_data)))
            .obj();
    }

private:
    T _data{};
};

inline SockAddr endpointToSockAddr(const asio::generic::stream_protocol::endpoint& endPoint) {
    SockAddr wrappedAddr(endPoint.data(), endPoint.size());
    return wrappedAddr;
}

// Utility function to turn an ASIO endpoint into a mongo HostAndPort
inline HostAndPort endpointToHostAndPort(const asio::generic::stream_protocol::endpoint& endPoint) {
    return HostAndPort(endpointToSockAddr(endPoint).toString(true));
}

Status errorCodeToStatus(const std::error_code& ec);
Status errorCodeToStatus(const std::error_code& ec, StringData context);

/**
 * Wrappers around asio socket's `local_endpoint` and `remote_endpoint` methods (which in turn wrap
 * `getsockname` and `getpeername`) with error logging. These functions re-throw any exception
 * coming from the underlying socket method, but they produce log messages indicating the error.
 */
asio::generic::stream_protocol::endpoint getLocalEndpoint(
    asio::generic::stream_protocol::socket& sock, StringData errorLogNote, logv2::LogSeverity sev);
asio::generic::stream_protocol::endpoint getRemoteEndpoint(
    asio::generic::stream_protocol::socket& sock, StringData errorLogNote, logv2::LogSeverity sev);

/**
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
StatusWith<unsigned> pollASIOSocket(asio::generic::stream_protocol::socket& socket,
                                    unsigned mask,
                                    Milliseconds timeout);

/**
 * Attempts to fill up the passed in buffer sequence with bytes from the underlying stream
 * without blocking. Returns the number of bytes we were actually able to fill in. Throws
 * on failure to read socket for reasons other than blocking.
 */
template <typename Stream, typename MutableBufferSequence>
size_t peekASIOStream(Stream& stream, const MutableBufferSequence& buffers) {
    // Check that the socket has bytes available to read so that receive does not block.
    asio::socket_base::bytes_readable command;
    stream.io_control(command);
    std::size_t bytes_readable = command.get();

    if (bytes_readable == 0) {
        return 0;
    }

    std::error_code ec;
    size_t bytesRead;
    do {
        bytesRead = stream.receive(buffers, stream.message_peek, ec);
    } while (ec == asio::error::interrupted);

    // On a completely empty socket, receive returns 0 bytes read and sets
    // the error code to either would_block or try_again. Since this isn't
    // actually an error condition for our purposes, we ignore these two
    // errors.
    if (ec != asio::error::would_block && ec != asio::error::try_again) {
        uassertStatusOK(errorCodeToStatus(ec, "peekASIOStream"));
    }

    return bytesRead;
}

/**
 * setSocketOption failed. Log the error.
 * This is in the .cpp file just to keep LOGV2 out of this header.
 */
void failedSetSocketOption(const std::system_error& ex,
                           StringData errorLogNote,
                           BSONObj optionDescription,
                           logv2::LogSeverity errorLogSeverity);

/**
 * Calls Asio `socket.set_option(opt)` with better failure diagnostics.
 * To be used instead of Asio `socket.set_option``, because errors are hard to diagnose.
 * Emits a log message about what option was attempted and what went wrong with
 * it. The `errorLogNote` string should uniquely identify the source of the call, and the type of
 * `opt` should model asio's `SettableSocketOption` and expose a `toBSON()` method. The
 * `SocketOption` template meets these requirements.
 *
 * Two overloads are provided matching the Asio `socket.set_option` overloads, with an additional
 * parameter to indicate the level at which the failure diagnostics should logged.
 *
 *     setSocketOption(socket, opt, errorLogNote, errorLogSeverity)
 *     setSocketOption(socket, opt, errorLogNote, errorLogSeverity, ec)
 *
 * If an `ec` is provided, errors are reported by mutating it.
 * Otherwise, the Asio `std::system_error` exception is rethrown.
 */
template <typename Socket, typename Option>
void setSocketOption(Socket& socket,
                     const Option& opt,
                     StringData errorLogNote,
                     logv2::LogSeverity errorLogSeverity) {
    try {
        socket.set_option(opt);
    } catch (const std::system_error& ex) {
        failedSetSocketOption(ex, errorLogNote, opt.toBSON(), errorLogSeverity);
        throw;
    }
}

template <typename Socket, typename Option>
void setSocketOption(Socket& socket,
                     const Option& opt,
                     StringData errorLogNote,
                     logv2::LogSeverity errorLogSeverity,
                     std::error_code& ec) {
    try {
        setSocketOption(socket, opt, errorLogNote, errorLogSeverity);
    } catch (const std::system_error& ex) {
        ec = ex.code();
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
struct UseFuture {
    template <typename... Args>
    class Adapter;
};

template <typename... ArgsFromAsio>
class UseFuture::Adapter {
private:
    template <typename Dum, typename... Ts>
    struct ArgPack : stdx::type_identity<std::tuple<Ts...>> {};
    template <typename Dum>
    struct ArgPack<Dum> : stdx::type_identity<void> {};
    template <typename Dum, typename T>
    struct ArgPack<Dum, T> : stdx::type_identity<T> {};

    /**
     * If an Asio callback takes a leading error_code, it's stripped from
     * the Future's value_type. Any errors reported by Asio will instead
     * be delivered by setting the Future's error Status.
     */
    template <typename Dum, typename... Ts>
    struct StripError : ArgPack<Dum, Ts...> {};
    template <typename Dum, typename... Ts>
    struct StripError<Dum, std::error_code, Ts...> : ArgPack<Dum, Ts...> {};

    using Result = typename StripError<void, ArgsFromAsio...>::type;

    struct Handler {
    private:
        template <typename... As>
        void _onSuccess(As&&... args) {
            promise.emplaceValue(std::forward<As>(args)...);
        }
        template <typename... As>
        void _onInvoke(As&&... args) {
            _onSuccess(std::forward<As>(args)...);
        }
        template <typename... As>
        void _onInvoke(std::error_code ec, As&&... args) {
            if (ec) {
                promise.setError(errorCodeToStatus(ec, "onInvoke"));
                return;
            }
            _onSuccess(std::forward<As>(args)...);
        }

    public:
        explicit Handler(const UseFuture&) {}

        template <typename... As>
        void operator()(As&&... args) {
            static_assert((std::is_same_v<std::decay_t<As>, std::decay_t<ArgsFromAsio>> && ...),
                          "Unexpected argument list from Asio async result callback.");
            _onInvoke(std::forward<As>(args)...);
        }

        Promise<Result> promise;
    };

public:
    using return_type = Future<Result>;
    using completion_handler_type = Handler;

    explicit Adapter(Handler& handler) {
        auto&& [p, f] = makePromiseFuture<Result>();
        _fut = std::move(f);
        handler.promise = std::move(p);
    }

    return_type get() {
        return std::move(_fut);
    }

private:
    Future<Result> _fut;
};

}  // namespace mongo::transport

namespace asio {
template <typename... Args>
class async_result<mongo::transport::UseFuture, void(Args...)>
    : public mongo::transport::UseFuture::Adapter<Args...> {
    using mongo::transport::UseFuture::Adapter<Args...>::Adapter;
};
}  // namespace asio
