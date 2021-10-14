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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork
#include "mongo/transport/asio_utils.h"

#include "mongo/config.h"
#include "mongo/logv2/log.h"

namespace mongo::transport {

Status errorCodeToStatus(const std::error_code& ec) {
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
    } else if (ec == asio::error::eof) {
        return {ErrorCodes::HostUnreachable, "Connection closed by peer"};
    } else if (ec == asio::error::connection_reset) {
        return {ErrorCodes::HostUnreachable, "Connection reset by peer"};
    } else if (ec == asio::error::network_reset) {
        return {ErrorCodes::HostUnreachable, "Connection reset by network"};
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
        auto errDesc = errnoWithDescription(WSAGetLastError());
        return {ErrorCodes::InternalError, errDesc};
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
        int errCode = errno;
        return {ErrorCodes::InternalError, errnoWithDescription(errCode)};
    } else if (result == 0) {
        return {ErrorCodes::NetworkTimeout, "Timed out waiting for poll"};
    }
    return toUnsignedEquivalent(pollItem.revents);
#endif
}

#ifdef MONGO_CONFIG_SSL
boost::optional<std::array<std::uint8_t, 7>> checkTLSRequest(const asio::const_buffer& buffer) {
    // This method's caller should have read in at least one MSGHEADER::Value's worth of data.
    // The fragment we are about to examine must be strictly smaller.
    static const size_t sizeOfTLSFragmentToRead = 11;
    invariant(buffer.size() >= sizeOfTLSFragmentToRead);

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

    auto request = reinterpret_cast<const char*>(buffer.data());
    auto cdr = ConstDataRangeCursor(request, request + buffer.size());

    // Parse the record header.
    // Extract the ContentType from the header, and ensure it is a handshake.
    StatusWith<std::uint8_t> record_ContentType = cdr.readAndAdvanceNoThrow<std::uint8_t>();
    if (!record_ContentType.isOK() || record_ContentType.getValue() != ContentType_handshake) {
        return boost::none;
    }
    // Skip the record's ProtocolVersion. Clients tend to send TLS 1.0 in
    // the record, but then their real protocol version in the enclosed ClientHello.
    StatusWith<ProtocolVersion> record_protocol_version =
        cdr.readAndAdvanceNoThrow<ProtocolVersion>();
    if (!record_protocol_version.isOK()) {
        return boost::none;
    }
    // Parse the record length. It should be be larger than the remaining expected payload.
    auto record_length = cdr.readAndAdvanceNoThrow<BigEndian<std::uint16_t>>();
    if (!record_length.isOK() || record_length.getValue() < cdr.length()) {
        return boost::none;
    }

    // Parse the handshake header.
    // Extract the HandshakeType, and ensure it is a ClientHello.
    StatusWith<std::uint8_t> handshake_type = cdr.readAndAdvanceNoThrow<std::uint8_t>();
    if (!handshake_type.isOK() || handshake_type.getValue() != HandshakeType_client_hello) {
        return boost::none;
    }
    // Extract the handshake length, and ensure it is larger than the remaining expected
    // payload. This requires a little work because the packet represents it with a uint24_t.
    StatusWith<std::array<std::uint8_t, 3>> handshake_length_bytes =
        cdr.readAndAdvanceNoThrow<std::array<std::uint8_t, 3>>();
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
    StatusWith<ProtocolVersion> client_version = cdr.readAndAdvanceNoThrow<ProtocolVersion>();
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

void failedSetSocketOption(const std::system_error& ex,
                           StringData note,
                           BSONObj optionDescription,
                           logv2::LogSeverity errorLogSeverity) {
    LOGV2_DEBUG(5693100,
                errorLogSeverity.toInt(),
                "Asio socket.set_option failed with std::system_error",
                "note"_attr = note,
                "option"_attr = optionDescription,
                "error"_attr = BSONObjBuilder{}
                                   .append("what", ex.what())
                                   .append("message", ex.code().message())
                                   .append("category", ex.code().category().name())
                                   .append("value", ex.code().value())
                                   .obj());
}

}  // namespace mongo::transport
