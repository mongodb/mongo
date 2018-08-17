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
#include "mongo/config.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/ssl_manager.h"

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

#ifdef MONGO_CONFIG_SSL
/**
 * Peeks at a fragment of a client issued TLS handshake packet. Returns a TLS alert
 * packet if the client has selected a protocol which has been disabled by the server.
 */
template <typename Buffer>
boost::optional<std::array<std::uint8_t, 7>> checkTLSRequest(const Buffer& buffers) {
    // This method's caller should have read in at least one MSGHEADER::Value's worth of data.
    // The fragment we are about to examine must be strictly smaller.
    static const size_t sizeOfTLSFragmentToRead = 11;
    invariant(asio::buffer_size(buffers) >= sizeOfTLSFragmentToRead);

    static_assert(sizeOfTLSFragmentToRead < sizeof(MSGHEADER::Value),
                  "checkTLSRequest's caller read a MSGHEADER::Value, which must be larger than "
                  "message containing the TLS version");

    /**
     * The fragment we are to examine is a record, containing a handshake, containing a
     * ClientHello. We wish to examine the advertised protocol version in the ClientHello.
     * The following roughly describes the contents of these structures. Note that we do not
     * need, or wish to, examine the entire ClientHello, we're looking exclusively for the
     * client_version.
     *
     * Below is a rough description of the payload we will be examining. We shall perform some
     * basic checks to ensure the payload matches these expectations. If it does not, we should
     * bail out, and not emit protocol version alerts.
     *
     * enum {alert(21), handshake(22)} ContentType;
     * TLSPlaintext {
     *   ContentType type = handshake(22),
     *   ProtocolVersion version; // Irrelevant. Clients send the real version in ClientHello.
     *   uint16 length;
     *   fragment, see Handshake stuct for contents
     * ...
     * }
     *
     * enum {client_hello(1)} HandshakeType;
     * Handshake {
     *   HandshakeType msg_type = client_hello(1);
     *   uint24_t length;
     *   ClientHello body;
     * }
     *
     * ClientHello {
     *   ProtocolVersion client_version; // <- This is the value we want to extract.
     * }
     */

    static const std::uint8_t ContentType_handshake = 22;
    static const std::uint8_t HandshakeType_client_hello = 1;

    using ProtocolVersion = std::array<std::uint8_t, 2>;
    static const ProtocolVersion tls10VersionBytes{3, 1};
    static const ProtocolVersion tls11VersionBytes{3, 2};

    auto request = asio::buffer_cast<const char*>(buffers);
    auto cdr = ConstDataRangeCursor(request, request + asio::buffer_size(buffers));

    // Parse the record header.
    // Extract the ContentType from the header, and ensure it is a handshake.
    StatusWith<std::uint8_t> record_ContentType = cdr.readAndAdvance<std::uint8_t>();
    if (!record_ContentType.isOK() || record_ContentType.getValue() != ContentType_handshake) {
        return boost::none;
    }
    // Skip the record's ProtocolVersion. Clients tend to send TLS 1.0 in
    // the record, but then their real protocol version in the enclosed ClientHello.
    StatusWith<ProtocolVersion> record_protocol_version = cdr.readAndAdvance<ProtocolVersion>();
    if (!record_protocol_version.isOK()) {
        return boost::none;
    }
    // Parse the record length. It should be be larger than the remaining expected payload.
    auto record_length = cdr.readAndAdvance<BigEndian<std::uint16_t>>();
    if (!record_length.isOK() || record_length.getValue() < cdr.length()) {
        return boost::none;
    }

    // Parse the handshake header.
    // Extract the HandshakeType, and ensure it is a ClientHello.
    StatusWith<std::uint8_t> handshake_type = cdr.readAndAdvance<std::uint8_t>();
    if (!handshake_type.isOK() || handshake_type.getValue() != HandshakeType_client_hello) {
        return boost::none;
    }
    // Extract the handshake length, and ensure it is larger than the remaining expected
    // payload. This requires a little work because the packet represents it with a uint24_t.
    StatusWith<std::array<std::uint8_t, 3>> handshake_length_bytes =
        cdr.readAndAdvance<std::array<std::uint8_t, 3>>();
    if (!handshake_length_bytes.isOK()) {
        return boost::none;
    }
    std::uint32_t handshake_length = 0;
    for (std::uint8_t handshake_byte : handshake_length_bytes.getValue()) {
        handshake_length <<= 8;
        handshake_length |= handshake_byte;
    }
    if (handshake_length < cdr.length()) {
        return boost::none;
    }
    StatusWith<ProtocolVersion> client_version = cdr.readAndAdvance<ProtocolVersion>();
    if (!client_version.isOK()) {
        return boost::none;
    }

    // Invariant: We read exactly as much data as expected.
    invariant((cdr.data() - request) == sizeOfTLSFragmentToRead);

    auto isProtocolDisabled = [](SSLParams::Protocols protocol) {
        const auto& params = getSSLGlobalParams();
        return std::find(params.sslDisabledProtocols.begin(),
                         params.sslDisabledProtocols.end(),
                         protocol) != params.sslDisabledProtocols.end();
    };

    auto makeTLSProtocolVersionAlert =
        [](const std::array<std::uint8_t, 2>& versionBytes) -> std::array<std::uint8_t, 7> {
        /**
         * The structure for this alert packet is as follows:
         * TLSPlaintext {
         *   ContentType type = alert(21);
         *   ProtocolVersion = versionBytes;
         *   uint16_t length = 2
         *   fragment = AlertDescription {
         *     AlertLevel level = fatal(2);
         *     AlertDescription = protocol_version(70);
         *   }
         *
         */
        return std::array<std::uint8_t, 7>{
            0x15, versionBytes[0], versionBytes[1], 0x00, 0x02, 0x02, 0x46};
    };

    ProtocolVersion version = client_version.getValue();
    if (version == tls10VersionBytes && isProtocolDisabled(SSLParams::Protocols::TLS1_0)) {
        return makeTLSProtocolVersionAlert(version);
    } else if (client_version == tls11VersionBytes &&
               isProtocolDisabled(SSLParams::Protocols::TLS1_1)) {
        return makeTLSProtocolVersionAlert(version);
    }
    // TLS1.2 cannot be distinguished from TLS1.3, just by looking at the ProtocolVersion bytes.
    // TLS 1.3 compatible clients advertise a "supported_versions" extension, which we would
    // have to extract here.
    // Hopefully by the time this matters, OpenSSL will properly emit protocol_version alerts.

    return boost::none;
}
#endif

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
        auto pf = makePromiseFuture<RealResult>();
        fut = std::move(pf.future);
        handler.promise = pf.promise.share();
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
