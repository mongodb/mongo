// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/asio/asio_utils.h"

#include "mongo/config.h"
#include "mongo/logv2/log.h"

#include <string_view>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::transport {

Status errorCodeToStatus(const std::error_code& ec) {
    return errorCodeToStatus(ec, {});
}

Status errorCodeToStatus(const std::error_code& ec, std::string_view context) {
    if (!ec)
        return Status::OK();

    // Add additional context string to Status reason if included.
    auto makeStatus = [&](ErrorCodes::Error code, std::string_view reason) {
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
    asio::generic::stream_protocol::socket& sock,
    std::string_view errorLogNote,
    logv2::LogSeverity sev) {
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
    asio::generic::stream_protocol::socket& sock,
    std::string_view errorLogNote,
    logv2::LogSeverity sev) {
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
                           std::string_view errorLogNote,
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
