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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/transport_layer_asio.h"

#include <asio.hpp>
#include <boost/algorithm/string.hpp>

#include "mongo/config.h"

#include "mongo/base/system_error.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl.hpp"
#endif

// session_asio.h has some header dependencies that require it to be the last header.
#include "mongo/transport/session_asio.h"

namespace mongo {
namespace transport {

TransportLayerASIO::Options::Options(const ServerGlobalParams* params)
    : port(params->port),
      ipList(params->bind_ip),
#ifndef _WIN32
      useUnixSockets(!params->noUnixSocket),
#endif
      enableIPv6(params->enableIPv6),
      maxConns(params->maxConns) {
}

TransportLayerASIO::TransportLayerASIO(const TransportLayerASIO::Options& opts,
                                       ServiceEntryPoint* sep)
    : _workerIOContext(std::make_shared<asio::io_context>()),
      _acceptorIOContext(stdx::make_unique<asio::io_context>()),
#ifdef MONGO_CONFIG_SSL
      _ingressSSLContext(nullptr),
      _egressSSLContext(nullptr),
#endif
      _sep(sep),
      _listenerOptions(opts) {
}

TransportLayerASIO::~TransportLayerASIO() = default;

StatusWith<SessionHandle> TransportLayerASIO::connect(HostAndPort peer,
                                                      ConnectSSLMode sslMode,
                                                      Milliseconds timeout) {
    std::error_code ec;
    GenericSocket sock(*_workerIOContext);
#ifndef _WIN32
    if (mongoutils::str::contains(peer.host(), '/')) {
        invariant(!peer.hasPort());
        auto res =
            _doSyncConnect(asio::local::stream_protocol::endpoint(peer.host()), peer, timeout);
        if (!res.isOK()) {
            return res.getStatus();
        } else {
            return static_cast<SessionHandle>(std::move(res.getValue()));
        }
    }
#endif

    using Resolver = asio::ip::tcp::resolver;
    Resolver resolver(*_workerIOContext);
    std::string portNumberStr = std::to_string(peer.port());
    auto doResolve = [&](auto resolverFlags) -> StatusWith<Resolver::iterator> {
        // If IPv6 is disabled, then we should specify that we only want IPv4 addresses, otherwise
        // we should do a normal AF_UNSPEC resolution to get both IPv4/IPv6
        Resolver::iterator resolverIt;
        if (_listenerOptions.enableIPv6) {
            resolverIt = resolver.resolve(peer.host(), portNumberStr, resolverFlags, ec);
        } else {
            resolverIt = resolver.resolve(
                asio::ip::tcp::v4(), peer.host(), portNumberStr, resolverFlags, ec);
        }

        if (ec) {
            return {ErrorCodes::HostNotFound,
                    str::stream() << "Could not find address for " << peer.host() << ": "
                                  << ec.message()};
        } else if (resolverIt == Resolver::iterator()) {
            return {ErrorCodes::HostNotFound,
                    str::stream() << "Could not find address for " << peer.host()};
        }

        return resolverIt;
    };

    // We always want to resolve the "service" (port number) as a numeric.
    //
    // We intentionally don't set the Resolver::address_configured flag because it might prevent us
    // from connecting to localhost on hosts with only a loopback interface (see SERVER-1579).
    const auto resolverFlags = Resolver::numeric_service;

    // We resolve in two steps, the first step tries to resolve the hostname as an IP address -
    // that way if there's a DNS timeout, we can still connect to IP addresses quickly.
    // (See SERVER-1709)
    //
    // Then, if the numeric (IP address) lookup failed, we fall back to DNS or return the error
    // from the resolver.
    auto swResolverIt = doResolve(resolverFlags | Resolver::numeric_host);
    if (!swResolverIt.isOK()) {
        if (swResolverIt == ErrorCodes::HostNotFound) {
            swResolverIt = doResolve(resolverFlags);
            if (!swResolverIt.isOK()) {
                return swResolverIt.getStatus();
            }
        } else {
            return swResolverIt.getStatus();
        }
    }

    auto& resolverIt = swResolverIt.getValue();
    auto sws = _doSyncConnect(resolverIt->endpoint(), peer, timeout);
    if (!sws.isOK()) {
        return sws.getStatus();
    }

    auto session = std::move(sws.getValue());
    session->ensureSync();

#ifndef MONGO_CONFIG_SSL
    if (sslMode == kEnableSSL) {
        return {ErrorCodes::InvalidSSLConfiguration, "SSL requested but not supported"};
    }
#else
    auto globalSSLMode = _sslMode();
    if (sslMode == kEnableSSL ||
        (sslMode == kGlobalSSLMode && ((globalSSLMode == SSLParams::SSLMode_preferSSL) ||
                                       (globalSSLMode == SSLParams::SSLMode_requireSSL)))) {
        auto sslStatus = session->handshakeSSLForEgress(peer).getNoThrow();
        if (!sslStatus.isOK()) {
            return sslStatus;
        }
    }
#endif

    return static_cast<SessionHandle>(std::move(session));
}

template <typename Endpoint>
StatusWith<TransportLayerASIO::ASIOSessionHandle> TransportLayerASIO::_doSyncConnect(
    Endpoint endpoint, const HostAndPort& peer, const Milliseconds& timeout) {
    GenericSocket sock(*_workerIOContext);
    std::error_code ec;
    sock.open(endpoint.protocol());
    sock.non_blocking(true);

    auto now = Date_t::now();
    auto expiration = now + timeout;
    do {
        auto curTimeout = expiration - now;
        sock.connect(endpoint, curTimeout.toSystemDuration(), ec);
        if (ec) {
            now = Date_t::now();
        }
        // We loop below if ec == interrupted to deal with EINTR failures, otherwise we handle
        // the error/timeout below.
    } while (ec == asio::error::interrupted && now < expiration);

    if (ec) {
        return errorCodeToStatus(ec);
    } else if (now >= expiration) {
        return {ErrorCodes::NetworkTimeout, str::stream() << "Timed out connecting to " << peer};
    }

    sock.non_blocking(false);
    return std::make_shared<ASIOSession>(this, std::move(sock));
}

void TransportLayerASIO::asyncConnect(HostAndPort peer,
                                      ConnectSSLMode sslMode,
                                      Milliseconds timeout,
                                      std::function<void(StatusWith<SessionHandle>)> callback) {
    MONGO_UNREACHABLE;
}

Status TransportLayerASIO::setup() {
    std::vector<std::string> listenAddrs;
    if (_listenerOptions.ipList.empty() && _listenerOptions.isIngress()) {
        listenAddrs = {"127.0.0.1"};
        if (_listenerOptions.enableIPv6) {
            listenAddrs.emplace_back("::1");
        }
    } else if (!_listenerOptions.ipList.empty()) {
        boost::split(
            listenAddrs, _listenerOptions.ipList, boost::is_any_of(","), boost::token_compress_on);
    }

#ifndef _WIN32
    if (_listenerOptions.useUnixSockets && _listenerOptions.isIngress()) {
        listenAddrs.emplace_back(makeUnixSockPath(_listenerOptions.port));
    }
#endif

    if (!(_listenerOptions.isIngress()) && !listenAddrs.empty()) {
        return {ErrorCodes::BadValue,
                "Cannot bind to listening sockets with ingress networking is disabled"};
    }

    _listenerPort = _listenerOptions.port;

    for (auto& ip : listenAddrs) {
        std::error_code ec;
        if (ip.empty()) {
            warning() << "Skipping empty bind address";
            continue;
        }

        const auto addrs = SockAddr::createAll(
            ip, _listenerOptions.port, _listenerOptions.enableIPv6 ? AF_UNSPEC : AF_INET);
        if (addrs.empty()) {
            warning() << "Found no addresses for " << ip;
            continue;
        }

        for (const auto& addr : addrs) {
            asio::generic::stream_protocol::endpoint endpoint(addr.raw(), addr.addressSize);

#ifndef _WIN32
            if (addr.getType() == AF_UNIX) {
                if (::unlink(ip.c_str()) == -1 && errno != ENOENT) {
                    error() << "Failed to unlink socket file " << ip << " "
                            << errnoWithDescription(errno);
                    fassertFailedNoTrace(40486);
                }
            }
#endif
            if (addr.getType() == AF_INET6 && !_listenerOptions.enableIPv6) {
                error() << "Specified ipv6 bind address, but ipv6 is disabled";
                fassertFailedNoTrace(40488);
            }

            GenericAcceptor acceptor(*_acceptorIOContext);
            acceptor.open(endpoint.protocol());
            acceptor.set_option(GenericAcceptor::reuse_address(true));
            if (addr.getType() == AF_INET6) {
                acceptor.set_option(asio::ip::v6_only(true));
            }

            acceptor.non_blocking(true, ec);
            if (ec) {
                return errorCodeToStatus(ec);
            }

            acceptor.bind(endpoint, ec);
            if (ec) {
                return errorCodeToStatus(ec);
            }

#ifndef _WIN32
            if (addr.getType() == AF_UNIX) {
                if (::chmod(ip.c_str(), serverGlobalParams.unixSocketPermissions) == -1) {
                    error() << "Failed to chmod socket file " << ip << " "
                            << errnoWithDescription(errno);
                    fassertFailedNoTrace(40487);
                }
            }
#endif
            if (_listenerOptions.port == 0 &&
                (addr.getType() == AF_INET || addr.getType() == AF_INET6)) {
                if (_listenerPort != _listenerOptions.port) {
                    return Status(ErrorCodes::BadValue,
                                  "Port 0 (ephemeral port) is not allowed when"
                                  " listening on multiple IP interfaces");
                }
                std::error_code ec;
                auto endpoint = acceptor.local_endpoint(ec);
                if (ec) {
                    return errorCodeToStatus(ec);
                }
                _listenerPort = endpointToHostAndPort(endpoint).port();
            }
            _acceptors.emplace_back(std::move(addr), std::move(acceptor));
        }
    }

    if (_acceptors.empty() && _listenerOptions.isIngress()) {
        return Status(ErrorCodes::SocketException, "No available addresses/ports to bind to");
    }

#ifdef MONGO_CONFIG_SSL
    const auto& sslParams = getSSLGlobalParams();
    auto sslManager = getSSLManager();

    if (_sslMode() != SSLParams::SSLMode_disabled && _listenerOptions.isIngress()) {
        _ingressSSLContext = stdx::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);

        Status status =
            sslManager->initSSLContext(_ingressSSLContext->native_handle(),
                                       sslParams,
                                       SSLManagerInterface::ConnectionDirection::kIncoming);
        if (!status.isOK()) {
            return status;
        }
    }

    if (_listenerOptions.isEgress() && sslManager) {
        _egressSSLContext = stdx::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
        Status status =
            sslManager->initSSLContext(_egressSSLContext->native_handle(),
                                       sslParams,
                                       SSLManagerInterface::ConnectionDirection::kOutgoing);
        if (!status.isOK()) {
            return status;
        }
    }
#endif

    return Status::OK();
}

Status TransportLayerASIO::start() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _running.store(true);

    if (_listenerOptions.isIngress()) {
        for (auto& acceptor : _acceptors) {
            acceptor.second.listen(serverGlobalParams.listenBacklog);
            _acceptConnection(acceptor.second);
        }

        _listenerThread = stdx::thread([this] {
            setThreadName("listener");
            while (_running.load()) {
                asio::io_context::work work(*_acceptorIOContext);
                try {
                    _acceptorIOContext->run();
                } catch (...) {
                    severe() << "Uncaught exception in the listener: " << exceptionToStatus();
                    fassertFailed(40491);
                }
            }
        });

        const char* ssl = "";
#ifdef MONGO_CONFIG_SSL
        if (_sslMode() != SSLParams::SSLMode_disabled) {
            ssl = " ssl";
        }
#endif
        log() << "waiting for connections on port " << _listenerPort << ssl;
    } else {
        invariant(_acceptors.empty());
    }

    return Status::OK();
}

void TransportLayerASIO::shutdown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _running.store(false);

    // Loop through the acceptors and cancel their calls to async_accept. This will prevent new
    // connections from being opened.
    for (auto& acceptor : _acceptors) {
        acceptor.second.cancel();
        auto& addr = acceptor.first;
        if (addr.getType() == AF_UNIX && !addr.isAnonymousUNIXSocket()) {
            auto path = addr.getAddr();
            log() << "removing socket file: " << path;
            if (::unlink(path.c_str()) != 0) {
                const auto ewd = errnoWithDescription();
                warning() << "Unable to remove UNIX socket " << path << ": " << ewd;
            }
        }
    }

    // If the listener thread is joinable (that is, we created/started a listener thread), then
    // the io_context is owned exclusively by the TransportLayer and we should stop it and join
    // the listener thread.
    //
    // Otherwise the ServiceExecutor may need to continue running the io_context to drain running
    // connections, so we just cancel the acceptors and return.
    if (_listenerThread.joinable()) {
        _acceptorIOContext->stop();
        _listenerThread.join();
    }
}

const std::shared_ptr<asio::io_context>& TransportLayerASIO::getIOContext() {
    return _workerIOContext;
}

void TransportLayerASIO::_acceptConnection(GenericAcceptor& acceptor) {
    auto acceptCb = [this, &acceptor](const std::error_code& ec, GenericSocket peerSocket) mutable {
        if (!_running.load())
            return;

        if (ec) {
            log() << "Error accepting new connection on "
                  << endpointToHostAndPort(acceptor.local_endpoint()) << ": " << ec.message();
            _acceptConnection(acceptor);
            return;
        }

        std::shared_ptr<ASIOSession> session(new ASIOSession(this, std::move(peerSocket)));

        _sep->startSession(std::move(session));
        _acceptConnection(acceptor);
    };

    acceptor.async_accept(*_workerIOContext, std::move(acceptCb));
}

#ifdef MONGO_CONFIG_SSL
SSLParams::SSLModes TransportLayerASIO::_sslMode() const {
    return static_cast<SSLParams::SSLModes>(getSSLGlobalParams().sslMode.load());
}
#endif

}  // namespace transport
}  // namespace mongo
