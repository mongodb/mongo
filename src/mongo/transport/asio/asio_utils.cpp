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

#include "mongo/config.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::transport {

Status errorCodeToStatus(const std::error_code& ec) {
    return errorCodeToStatus(ec, {});
}

Status errorCodeToStatus(const std::error_code& ec, StringData context) {
    if (!ec)
        return Status::OK();

    // Add additional context string to Status reason if included.
    auto makeStatus = [&](ErrorCodes::Error code, StringData reason) {
        Status result(code, reason);
        if (context.data())
            result.addContext(context);
        return result;
    };

    if (ec == asio::error::operation_aborted) {
        return makeStatus(ErrorCodes::CallbackCanceled, "Callback was canceled");
    }

#ifdef _WIN32
    if (ec == asio::error::timed_out) {
#else
    if (ec == asio::error::try_again || ec == asio::error::would_block) {
#endif
        return makeStatus(ErrorCodes::NetworkTimeout, "Socket operation timed out");
    } else if (ec == asio::error::eof) {
        return makeStatus(ErrorCodes::HostUnreachable, "Connection closed by peer");
    } else if (ec == asio::error::connection_reset) {
        return makeStatus(ErrorCodes::HostUnreachable, "Connection reset by peer");
    } else if (ec == asio::error::network_reset) {
        return makeStatus(ErrorCodes::HostUnreachable, "Connection reset by network");
    } else if (ec == asio::error::in_progress) {
        return makeStatus(ErrorCodes::ConnectionError, "Socket operation in progress");
    }

    // If the ec.category() is a mongoErrorCategory() then this error was propogated from
    // mongodb code and we should just pass the error cdoe along as-is.
    ErrorCodes::Error errorCode = (ec.category() == mongoErrorCategory())
        ? ErrorCodes::Error(ec.value())
        // Otherwise it's an error code from the network and we should pass it along as a
        // SocketException
        : ErrorCodes::SocketException;
    // Either way, include the error message.
    return makeStatus(errorCode, ec.message());
}

template <typename T>
auto toUnsignedEquivalent(T x) {
    return static_cast<std::make_unsigned_t<T>>(x);
}

template <typename Dur>
timeval toTimeval(Dur dur) {
    auto sec = duration_cast<Seconds>(dur);
    timeval tv{};
    tv.tv_sec = sec.count();
    tv.tv_usec = duration_cast<Microseconds>(dur - sec).count();
    return tv;
}

BSONObj errorDescription(const std::system_error& ex) {
    return BSONObjBuilder{}
        .append("what", ex.what())
        .append("message", ex.code().message())
        .append("category", ex.code().category().name())
        .append("value", ex.code().value())
        .obj();
}

asio::generic::stream_protocol::endpoint getLocalEndpoint(
    asio::generic::stream_protocol::socket& sock, StringData errorLogNote, logv2::LogSeverity sev) {
    try {
        return sock.local_endpoint();
    } catch (const std::system_error& ex) {
        LOGV2_DEBUG(6819700,
                    sev.toInt(),
                    "Asio socket.local_endpoint failed with std::system_error",
                    "note"_attr = errorLogNote,
                    "error"_attr = errorDescription(ex));
        throw;
    }
}

asio::generic::stream_protocol::endpoint getRemoteEndpoint(
    asio::generic::stream_protocol::socket& sock, StringData errorLogNote, logv2::LogSeverity sev) {
    try {
        return sock.remote_endpoint();
    } catch (const std::system_error& ex) {
        LOGV2_DEBUG(6819701,
                    sev.toInt(),
                    "Asio socket.remote_endpoint failed with std::system_error",
                    "note"_attr = errorLogNote,
                    "error"_attr = errorDescription(ex));
        throw;
    }
}

StatusWith<unsigned> pollASIOSocket(asio::generic::stream_protocol::socket& socket,
                                    unsigned mask,
                                    Milliseconds timeout) {
#ifdef _WIN32
    // On Windows, use `select` to approximate `poll`.
    // Windows `select` has a couple special rules:
    //   - any empty fd_set args *must* be passed as nullptr.
    //   - the fd_set args can't *all* be nullptr.
    struct FlagFdSet {
        unsigned pollFlag;
        fd_set fds;
    };
    std::array sets{
        FlagFdSet{toUnsignedEquivalent(POLLIN)},
        FlagFdSet{toUnsignedEquivalent(POLLOUT)},
        FlagFdSet{toUnsignedEquivalent(POLLERR)},
    };
    auto fd = socket.native_handle();
    mask |= POLLERR;  // Always interested in errors.
    for (auto& [pollFlag, fds] : sets) {
        FD_ZERO(&fds);
        if (mask & pollFlag)
            FD_SET(fd, &fds);
    }

    auto timeoutTv = toTimeval(timeout);
    auto fdsPtr = [&](size_t i) {
        fd_set* ptr = &sets[i].fds;
        return FD_ISSET(fd, ptr) ? ptr : nullptr;
    };
    int result = ::select(fd + 1, fdsPtr(0), fdsPtr(1), fdsPtr(2), &timeoutTv);
    if (result == SOCKET_ERROR) {
        auto ec = lastSocketError();
        return {ErrorCodes::InternalError, errorMessage(ec)};
    } else if (result == 0) {
        return {ErrorCodes::NetworkTimeout, "Timed out waiting for poll"};
    }

    unsigned revents = 0;
    for (auto& [pollFlag, fds] : sets)
        if (FD_ISSET(fd, &fds))
            revents |= pollFlag;
    return revents;
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
        auto ec = lastPosixError();
        return {ErrorCodes::InternalError, errorMessage(ec)};
    } else if (result == 0) {
        return {ErrorCodes::NetworkTimeout, "Timed out waiting for poll"};
    }
    return toUnsignedEquivalent(pollItem.revents);
#endif
}

void failedSetSocketOption(const std::system_error& ex,
                           StringData errorLogNote,
                           BSONObj optionDescription,
                           logv2::LogSeverity errorLogSeverity) {
    LOGV2_DEBUG(5693100,
                errorLogSeverity.toInt(),
                "Asio socket.set_option failed with std::system_error",
                "note"_attr = errorLogNote,
                "option"_attr = optionDescription,
                "error"_attr = errorDescription(ex));
}

}  // namespace mongo::transport
