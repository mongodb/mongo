/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/util/net/asio_message_port.h"

#include <set>

#include "mongo/base/init.h"
#include "mongo/config.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/invariant.h"
#include "mongo/util/log.h"
#include "mongo/util/net/asio_ssl_context.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

namespace {
const char kGET[] = "GET";
const int kHeaderLen = sizeof(MSGHEADER::Value);
const int kInitialMessageSize = 1024;

#ifdef MONGO_CONFIG_SSL
struct ASIOSSLContextPair {
    ASIOSSLContext server;
    ASIOSSLContext client;
};

const auto sslDecoration = SSLManagerInterface::declareDecoration<ASIOSSLContextPair>();

MONGO_INITIALIZER_WITH_PREREQUISITES(ASIOSSLContextSetup, ("SSLManager"))(InitializerContext*) {
    if (getSSLManager()) {
        sslDecoration(getSSLManager())
            .server.init(SSLManagerInterface::ConnectionDirection::kIncoming);
        sslDecoration(getSSLManager())
            .client.init(SSLManagerInterface::ConnectionDirection::kOutgoing);
    }
    return Status::OK();
}
#endif

}  // namespace

ASIOMessagingPort::ASIOMessagingPort(int fd, SockAddr farEnd)
    : _service(1),
      _timer(_service),
      _creationTime(curTimeMicros64()),
      _timeout(),
      _remote(),
      _isEncrypted(false),
      _awaitingHandshake(true),
      _x509SubjectName(),
      _bytesIn(0),
      _bytesOut(0),
      _logLevel(logger::LogSeverity::Log()),
      _connectionId(),
      _tag(),
#ifdef MONGO_CONFIG_SSL
      _context(ASIOSSLContext()),
      _sslSock(_service,
               getSSLManager() ? sslDecoration(getSSLManager()).server.getContext()
                               : _context->getContext()) {
    if (getSSLManager()) {
        _context = boost::none;
    }
    _sslSock.lowest_layer().assign(
        asio::generic::stream_protocol(farEnd.getType(),
                                       farEnd.getType() == AF_UNIX ? 0 : IPPROTO_TCP),
        fd);
#else
      _sock(_service,
            asio::generic::stream_protocol(farEnd.getType(),
                                           farEnd.getType() == AF_UNIX ? 0 : IPPROTO_TCP),
            fd) {
#endif  // MONGO_CONFIG_SSL
    _getSocket().non_blocking(true);
    _remote = HostAndPort(farEnd.getAddr(), farEnd.getPort());
    _timer.expires_at(decltype(_timer)::time_point::max());
    _setTimerCallback();
}

ASIOMessagingPort::ASIOMessagingPort(Milliseconds timeout, logger::LogSeverity logLevel)
    : _service(1),
      _timer(_service),
      _creationTime(curTimeMicros64()),
      _timeout(timeout),
      _remote(),
      _isEncrypted(false),
      _awaitingHandshake(true),
      _x509SubjectName(),
      _bytesIn(0),
      _bytesOut(0),
      _logLevel(logLevel),
      _connectionId(),
      _tag(),
#ifdef MONGO_CONFIG_SSL
      _context(ASIOSSLContext()),
      _sslSock(_service,
               getSSLManager() ? sslDecoration(getSSLManager()).client.getContext()
                               : _context->getContext()) {
    if (getSSLManager()) {
        _context = boost::none;
    }
#else
      _sock(_service) {
#endif  // MONGO_CONFIG_SSL
    if (*_timeout == Milliseconds(0)) {
        _timeout = boost::none;
    }
    _timer.expires_at(decltype(_timer)::time_point::max());
    _setTimerCallback();
}

ASIOMessagingPort::~ASIOMessagingPort() {
    shutdown();
}

void ASIOMessagingPort::setTimeout(Milliseconds millis) {
    if (millis == Milliseconds(0)) {
        _timeout = boost::none;
        _timer.expires_at(decltype(_timer)::time_point::max());
    } else {
        _timeout = millis;
    }
}

void ASIOMessagingPort::shutdown() {
    if (!_inShutdown.swap(true)) {
        if (_getSocket().native_handle() >= 0) {
            _getSocket().cancel();

            stdx::lock_guard<stdx::mutex> opInProgressGuard(_opInProgress);

            _getSocket().close();
            _awaitingHandshake = true;
            _isEncrypted = false;
        }
    }
}

asio::error_code ASIOMessagingPort::_read(char* buf, std::size_t size) {
    invariant(buf);

    stdx::lock_guard<stdx::mutex> opInProgressGuard(_opInProgress);

    // Try to do optimistic reads.
    asio::error_code ec;
    std::size_t bytesRead;
    if (!_isEncrypted) {
        bytesRead = asio::read(_getSocket(), asio::buffer(buf, size), ec);
    }
#ifdef MONGO_CONFIG_SSL
    else {
        bytesRead = asio::read(_sslSock, asio::buffer(buf, size), ec);
    }
#endif
    if (!ec && bytesRead == size) {
        _bytesIn += size;
    }
    if (ec != asio::error::would_block) {
        return ec;
    }

    // Fall back to async with timer if the operation would block.
    if (_timeout) {
        _timer.expires_from_now(decltype(_timer)::duration(
            durationCount<Duration<decltype(_timer)::duration::period>>(*_timeout)));
    }
    if (!_isEncrypted) {
        asio::async_read(
            _getSocket(),
            asio::buffer(buf + bytesRead, size - bytesRead),
            [&ec, size, bytesRead](const asio::error_code& err, std::size_t size_read) {
                invariant(err || (size - bytesRead) == size_read);
                ec = err;
            });
    }
#ifdef MONGO_CONFIG_SSL
    else {
        asio::async_read(
            _sslSock,
            asio::buffer(buf + bytesRead, size - bytesRead),
            [&ec, size, bytesRead](const asio::error_code& err, std::size_t size_read) {
                invariant(err || (size - bytesRead) == size_read);
                ec = err;
            });
    }
#endif  // MONGO_CONFIG_SSL
    do {
        _service.run_one();
    } while (ec == asio::error::would_block);

    if (!ec) {
        _bytesIn += size;
    }
    return ec;
}

asio::error_code ASIOMessagingPort::_write(const char* buf, std::size_t size) {
    invariant(buf);

    stdx::lock_guard<stdx::mutex> opInProgressGuard(_opInProgress);

    // Try to do optimistic writes.
    asio::error_code ec;
    std::size_t bytesWritten;
    if (!_isEncrypted) {
        bytesWritten = asio::write(_getSocket(), asio::buffer(buf, size), ec);
    }
#ifdef MONGO_CONFIG_SSL
    else {
        bytesWritten = asio::write(_sslSock, asio::buffer(buf, size), ec);
    }
#endif
    if (!ec && bytesWritten == size) {
        _bytesOut += size;
    }
    if (ec != asio::error::would_block) {
        return ec;
    }

    // Fall back to async with timer if the operation would block.
    if (_timeout) {
        _timer.expires_from_now(decltype(_timer)::duration(
            durationCount<Duration<decltype(_timer)::duration::period>>(*_timeout)));
    }

    if (!_isEncrypted) {
        asio::async_write(
            _getSocket(),
            asio::buffer(buf + bytesWritten, size - bytesWritten),
            [&ec, size, bytesWritten](const asio::error_code& err, std::size_t size_written) {
                invariant(err || (size - bytesWritten) == size_written);
                ec = err;
            });
    }
#ifdef MONGO_CONFIG_SSL
    else {
        asio::async_write(
            _sslSock,
            asio::buffer(buf + bytesWritten, size - bytesWritten),
            [&ec, size, bytesWritten](const asio::error_code& err, std::size_t size_written) {
                invariant(err || (size - bytesWritten) == size_written);
                ec = err;
            });
    }
#endif  // MONGO_CONFIG_SSL
    do {
        _service.run_one();
    } while (ec == asio::error::would_block);

    if (!ec) {
        _bytesOut += size;
    }
    return ec;
}

asio::error_code ASIOMessagingPort::_handshake(bool isServer, const char* buf, std::size_t size) {
#ifdef MONGO_CONFIG_SSL
    auto handshakeType = isServer ? decltype(_sslSock)::server : decltype(_sslSock)::client;

    stdx::lock_guard<stdx::mutex> opInProgressGuard(_opInProgress);

    if (_timeout) {
        _timer.expires_from_now(decltype(_timer)::duration(
            durationCount<Duration<decltype(_timer)::duration::period>>(*_timeout)));
    }

    asio::error_code ec = asio::error::would_block;
    if (buf) {
        _sslSock.async_handshake(handshakeType,
                                 asio::buffer(buf, size),
                                 [&ec](const asio::error_code& err, std::size_t) { ec = err; });
    } else {
        _sslSock.async_handshake(handshakeType, [&ec](const asio::error_code& err) { ec = err; });
    }

    do {
        _service.run_one();
    } while (ec == asio::error::would_block);

    return ec;
#else
    return asio::error::operation_not_supported;
#endif
}

const asio::generic::stream_protocol::socket& ASIOMessagingPort::_getSocket() const {
#ifdef MONGO_CONFIG_SSL
    return _sslSock.next_layer();
#else
    return _sock;
#endif
}

asio::generic::stream_protocol::socket& ASIOMessagingPort::_getSocket() {
    return const_cast<asio::generic::stream_protocol::socket&>(
        const_cast<const ASIOMessagingPort*>(this)->_getSocket());
}

bool ASIOMessagingPort::recv(Message& m) {
    try {
        if (getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->shouldFail()) {
            throw SocketException(SocketException::RECV_ERROR, "fail point set");
        }
        SharedBuffer buf = SharedBuffer::allocate(kInitialMessageSize);
        MsgData::View md = buf.get();

        asio::error_code ec = _read(md.view2ptr(), kHeaderLen);
        if (ec) {
            if (ec == asio::error::misc_errors::eof) {
                // When the socket is closed, no need to log an error.
                return false;
            }
            throw asio::system_error(ec);
        }

        if (_awaitingHandshake) {
            static_assert(sizeof(kGET) - 1 <= kHeaderLen,
                          "HTTP GET string must be smaller than the message header.");
            if (memcmp(md.view2ptr(), kGET, strlen(kGET)) == 0) {
                std::string httpMsg =
                    "It looks like you are trying to access MongoDB over HTTP on the native driver "
                    "port.\n";
                LOG(_logLevel) << httpMsg;
                std::stringstream ss;
                ss << "HTTP/1.0 200 OK\r\nConnection: close\r\nContent-Type: "
                      "text/plain\r\nContent-Length: "
                   << httpMsg.size() << "\r\n\r\n"
                   << httpMsg;
                auto s = ss.str();
                send(s.c_str(), s.size(), nullptr);
                return false;
            }
#ifndef MONGO_CONFIG_SSL
            // If responseTo is not 0 or -1 for first packet assume SSL
            if (md.getResponseToMsgId() != 0 && md.getResponseToMsgId() != -1) {
                uasserted(40130,
                          "SSL handshake requested, SSL feature not available in this build");
            }
#else
            if (md.getResponseToMsgId() != 0 && md.getResponseToMsgId() != -1) {
                uassert(40131,
                        "SSL handshake received but server is started without SSL support",
                        sslGlobalParams.sslMode.load() != SSLParams::SSLMode_disabled);

                auto ec = _handshake(true, md.view2ptr(), kHeaderLen);
                if (ec) {
                    throw asio::system_error(ec);
                }

                auto swPeerSubjectName =
                    getSSLManager()->parseAndValidatePeerCertificate(_sslSock.native_handle(), "");
                if (!swPeerSubjectName.isOK()) {
                    throw SocketException(SocketException::CONNECT_ERROR,
                                          swPeerSubjectName.getStatus().reason());
                }
                setX509SubjectName(swPeerSubjectName.getValue().get_value_or(""));

                _isEncrypted = true;
                _awaitingHandshake = false;
                return recv(m);
            }
            uassert(40132,
                    "The server is configured to only allow SSL connections",
                    sslGlobalParams.sslMode.load() != SSLParams::SSLMode_requireSSL);

#endif  // MONGO_CONFIG_SSL
            _awaitingHandshake = false;
        }

        int msgLen = md.getLen();

        if (static_cast<size_t>(msgLen) < sizeof(MSGHEADER::Value) ||
            static_cast<size_t>(msgLen) > MaxMessageSizeBytes) {
            LOG(_logLevel) << "recv(): message len " << msgLen << " is invalid. "
                           << "Min: " << sizeof(MSGHEADER::Value)
                           << ", Max: " << MaxMessageSizeBytes;
            return false;
        }

        if (msgLen > kInitialMessageSize) {
            buf.realloc(msgLen);
            md = buf.get();
        }

        ec = _read(md.data(), msgLen - kHeaderLen);
        if (ec) {
            throw asio::system_error(ec);
        }

        m.setData(std::move(buf));
        return true;

    } catch (const asio::system_error& e) {
        LOG(_logLevel) << "SocketException: remote: " << remote() << " error: " << e.what();
        m.reset();
        return false;
    }
}

void ASIOMessagingPort::reply(Message& received, Message& response) {
    say(response, received.header().getId());
}

void ASIOMessagingPort::reply(Message& received, Message& response, int32_t responseToMsgId) {
    say(response, responseToMsgId);
}

bool ASIOMessagingPort::call(Message& toSend, Message& response) {
    try {
        say(toSend);
    } catch (const asio::system_error&) {
        return false;
    }
    bool success = recv(response);
    if (success) {
        if (response.header().getResponseToMsgId() != toSend.header().getId()) {
            response.reset();
            uasserted(40133, "Response ID did not match the sent message ID.");
        }
    }
    return success;
}

void ASIOMessagingPort::say(Message& toSend, int responseTo) {
    invariant(!toSend.empty());
    toSend.header().setId(nextMessageId());
    toSend.header().setResponseToMsgId(responseTo);
    return say(const_cast<const Message&>(toSend));
}

void ASIOMessagingPort::say(const Message& toSend) {
    invariant(!toSend.empty());
    auto buf = toSend.buf();
    if (buf) {
        send(buf, MsgData::ConstView(buf).getLen(), nullptr);
    }
}

unsigned ASIOMessagingPort::remotePort() const {
    return _remote.port();
}

HostAndPort ASIOMessagingPort::remote() const {
    return _remote;
}

SockAddr ASIOMessagingPort::remoteAddr() const {
    return SockAddr(_remote.host(), _remote.port());
}

SockAddr ASIOMessagingPort::localAddr() const {
    auto ep = _getSocket().local_endpoint();
    switch (ep.protocol().family()) {
        case AF_INET:
        case AF_INET6: {
            asio::ip::tcp::endpoint tcpEP;
            tcpEP.resize(ep.size());
            memcpy(tcpEP.data(), ep.data(), ep.size());
            return SockAddr(tcpEP.address().to_string(), tcpEP.port());
        }
#ifndef _WIN32
        case AF_UNIX: {
            asio::local::stream_protocol::endpoint localEP;
            localEP.resize(ep.size());
            memcpy(localEP.data(), ep.data(), ep.size());
            return SockAddr(localEP.path(), 0);
        }
#endif  // _WIN32
        default: { MONGO_UNREACHABLE; }
    }
}

void ASIOMessagingPort::send(const char* data, int len, const char*) {
    if (getGlobalFailPointRegistry()->getFailPoint("throwSockExcep")->shouldFail()) {
        throw SocketException(SocketException::SEND_ERROR, "fail point set");
    }
    asio::error_code ec = _write(data, len);
    if (ec) {
        throw SocketException(SocketException::SEND_ERROR, asio::system_error(ec).what());
    }
}

void ASIOMessagingPort::send(const std::vector<std::pair<char*, int>>& data, const char*) {
    for (auto&& pair : data) {
        send(pair.first, pair.second, nullptr);
    }
}

bool ASIOMessagingPort::connect(SockAddr& farEnd) {
    if (_timeout) {
        _timer.expires_from_now(decltype(_timer)::duration(
            durationCount<Duration<decltype(_timer)::duration::period>>(*_timeout)));
    }
    _remote = HostAndPort(farEnd.getAddr(), farEnd.getPort());

    asio::ip::tcp::resolver resolver(_service);
    asio::error_code ec = asio::error::would_block;

    stdx::lock_guard<stdx::mutex> opInProgressGuard(_opInProgress);
    if (farEnd.getType() == AF_UNIX) {
#ifndef _WIN32
        asio::local::stream_protocol::endpoint endPoint(farEnd.getAddr());
        _getSocket().async_connect(endPoint, [&ec](const asio::error_code& err) { ec = err; });
#else
        uasserted(40135, "Connect called on a Unix socket under windows build.");
#endif  // _WIN32
    } else {
        asio::ip::tcp::resolver::query query(_remote.host(), std::to_string(_remote.port()));

        resolver.async_resolve(
            query,
            [&ec, this](const asio::error_code& resolveErr, asio::ip::tcp::resolver::iterator i) {
                if (!resolveErr) {
                    asio::ip::tcp::endpoint tcpEndpoint(*i);
                    _getSocket().async_connect(tcpEndpoint,
                                               [&ec](const asio::error_code& err) { ec = err; });
                } else if (i == asio::ip::tcp::resolver::iterator()) {
                    ec = asio::error::host_unreachable;
                }
            });
    }

    do {
        _service.run_one();
    } while (ec == asio::error::would_block);

    if (ec) {
        if (ec == asio::error::connection_refused) {
            LOG(_logLevel) << "Failed to connect to " << _remote << ", reason: Connection refused";
        } else if (ec == asio::error::connection_aborted && _timeout) {
            LOG(_logLevel) << "Failed to connect to " << _remote << " after " << _timeout->count()
                           << " milliseconds, giving up.";
        } else {
            LOG(_logLevel) << "Failed to connect to " << _remote
                           << ", reason: " << asio::system_error(ec).what();
        }
        return false;
    }

    _creationTime = curTimeMicros64();
    _awaitingHandshake = false;
    _getSocket().non_blocking(true);
    if (farEnd.getType() != AF_UNIX) {
        _getSocket().set_option(asio::ip::tcp::no_delay(true));
    }

    return true;
}

bool ASIOMessagingPort::secure(SSLManagerInterface* ssl, const std::string& remoteHost) {
#ifdef MONGO_CONFIG_SSL
    auto ec = _handshake(false);
    if (ec) {
        return false;
    }

    auto swPeerSubjectName =
        getSSLManager()->parseAndValidatePeerCertificate(_sslSock.native_handle(), remoteHost);
    if (!swPeerSubjectName.isOK()) {
        throw SocketException(SocketException::CONNECT_ERROR,
                              swPeerSubjectName.getStatus().reason());
    }
    setX509SubjectName(swPeerSubjectName.getValue().get_value_or(""));

    _isEncrypted = true;
    return true;
#else
    return false;
#endif
}

bool ASIOMessagingPort::isStillConnected() const {
    return _getSocket().is_open();
}

uint64_t ASIOMessagingPort::getSockCreationMicroSec() const {
    return _creationTime;
}

void ASIOMessagingPort::setLogLevel(logger::LogSeverity logLevel) {
    _logLevel = logLevel;
}

void ASIOMessagingPort::clearCounters() {
    _bytesIn = 0;
    _bytesOut = 0;
}

long long ASIOMessagingPort::getBytesIn() const {
    return _bytesIn;
}

long long ASIOMessagingPort::getBytesOut() const {
    return _bytesOut;
}

void ASIOMessagingPort::setX509SubjectName(const std::string& x509SubjectName) {
    _x509SubjectName = x509SubjectName;
}

std::string ASIOMessagingPort::getX509SubjectName() const {
    return _x509SubjectName;
}

void ASIOMessagingPort::setConnectionId(const long long connectionId) {
    _connectionId = connectionId;
}

long long ASIOMessagingPort::connectionId() const {
    return _connectionId;
}

void ASIOMessagingPort::setTag(const AbstractMessagingPort::Tag tag) {
    _tag = tag;
}

AbstractMessagingPort::Tag ASIOMessagingPort::getTag() const {
    return _tag;
}

void ASIOMessagingPort::_setTimerCallback() {
    _timer.async_wait([this](const asio::error_code& ec) {
        if (ec != asio::error::operation_aborted &&
            (_timer.expires_at() <= decltype(_timer)::clock_type::now())) {
            _getSocket().cancel();
            _timer.expires_at(decltype(_timer)::time_point::max());
        }
        _setTimerCallback();
    });
}

}  // namespace mongo
