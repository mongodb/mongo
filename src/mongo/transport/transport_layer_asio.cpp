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

#include "boost/algorithm/string.hpp"

#include "asio.hpp"

#include "mongo/config.h"
#ifdef MONGO_CONFIG_SSL
#include "asio/ssl.hpp"
#endif

#include "mongo/base/checked_cast.h"
#include "mongo/base/system_error.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/ticket.h"
#include "mongo/transport/ticket_asio.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/message.h"
#include "mongo/util/net/sock.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

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

std::shared_ptr<TransportLayerASIO::ASIOSession> TransportLayerASIO::createSession() {
    GenericSocket socket(*_ioContext);
    std::shared_ptr<ASIOSession> ret(new ASIOSession(this, std::move(socket)));
    return ret;
}

TransportLayerASIO::TransportLayerASIO(const TransportLayerASIO::Options& opts,
                                       ServiceEntryPoint* sep)
    : _ioContext(stdx::make_unique<asio::io_context>()),
#ifdef MONGO_CONFIG_SSL
      _sslContext(nullptr),
#endif
      _sep(sep),
      _listenerOptions(opts) {
}

TransportLayerASIO::~TransportLayerASIO() = default;

Ticket TransportLayerASIO::sourceMessage(const SessionHandle& session,
                                         Message* message,
                                         Date_t expiration) {
    auto asioSession = checked_pointer_cast<ASIOSession>(session);
    auto ticket = stdx::make_unique<ASIOSourceTicket>(asioSession, expiration, message);
    return {this, std::move(ticket)};
}

Ticket TransportLayerASIO::sinkMessage(const SessionHandle& session,
                                       const Message& message,
                                       Date_t expiration) {
    auto asioSession = checked_pointer_cast<ASIOSession>(session);
    auto ticket = stdx::make_unique<ASIOSinkTicket>(asioSession, expiration, message);
    return {this, std::move(ticket)};
}

Status TransportLayerASIO::wait(Ticket&& ticket) {
    auto ownedASIOTicket = getOwnedTicketImpl(std::move(ticket));
    auto asioTicket = checked_cast<ASIOTicket*>(ownedASIOTicket.get());

    Status waitStatus = Status::OK();
    asioTicket->fill(true, [&waitStatus](Status result) { waitStatus = result; });

    return waitStatus;
}

void TransportLayerASIO::asyncWait(Ticket&& ticket, TicketCallback callback) {
    auto ownedASIOTicket = std::shared_ptr<TicketImpl>(getOwnedTicketImpl(std::move(ticket)));
    auto asioTicket = checked_cast<ASIOTicket*>(ownedASIOTicket.get());

    asioTicket->fill(
        false,
        [ callback = std::move(callback),
          ownedASIOTicket = std::move(ownedASIOTicket) ](Status status) { callback(status); });
}

TransportLayer::Stats TransportLayerASIO::sessionStats() {
    TransportLayer::Stats ret;
    auto sessionCount = [this] {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        return _sessions.size();
    }();
    ret.numOpenSessions = sessionCount;
    ret.numCreatedSessions = _createdConnections.load();
    ret.numAvailableSessions = static_cast<size_t>(_listenerOptions.maxConns) - sessionCount;
    return ret;
}

// Must not be called while holding the TransportLayerASIO mutex.
void TransportLayerASIO::end(const SessionHandle& session) {
    auto asioSession = checked_pointer_cast<ASIOSession>(session);
    asioSession->shutdown();
}

void TransportLayerASIO::endAllSessions(Session::TagMask tags) {
    log() << "ASIO transport layer closing all connections";
    std::vector<ASIOSessionHandle> sessions;
    // This is more complicated than it seems. We need to lock() all the weak_ptrs in _sessions
    // and then end them if their tags don't match the tags passed into the function.
    //
    // When you lock the session, the lifetime of the session is extended by creating a new
    // shared_ptr, but the session could end before this lock is released, which means that we
    // must extend the lifetime of the session past the scope of the lock_guard, or else the
    // session's destructor will acquire a lock already held here and deadlock/crash.
    {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        sessions.reserve(_sessions.size());
        for (auto&& weakSession : _sessions) {
            if (auto session = weakSession.lock()) {
                sessions.emplace_back(std::move(session));
            }
        }
    }

    // Outside the lock we kill any sessions that don't match our tags.
    for (auto&& session : sessions) {
        if (session->getTags() & tags) {
            log() << "Skip closing connection for connection # " << session->id();
        } else {
            end(session);
        }
    }

    // Any other sessions that may have ended while this was running will get cleaned up by
    // sessions being destructed at the end of this function.
}

Status TransportLayerASIO::setup() {
    std::vector<std::string> listenAddrs;
    if (_listenerOptions.ipList.empty()) {
        listenAddrs = {"127.0.0.1"};
        if (_listenerOptions.enableIPv6) {
            listenAddrs.emplace_back("::1");
        }
    } else {
        boost::split(
            listenAddrs, _listenerOptions.ipList, boost::is_any_of(","), boost::token_compress_on);
    }

#ifndef _WIN32
    if (_listenerOptions.useUnixSockets) {
        listenAddrs.emplace_back(makeUnixSockPath(_listenerOptions.port));
    }
#endif
    for (auto& ip : listenAddrs) {
        std::error_code ec;
        auto address = asio::ip::address::from_string(ip, ec);
        if (ec) {
#ifndef _WIN32
            if (_listenerOptions.useUnixSockets) {
                asio::local::stream_protocol::endpoint endpoint(ip);
                asio::local::stream_protocol::acceptor acceptor(*_ioContext);
                acceptor.open(endpoint.protocol());
                if (::unlink(ip.c_str()) == -1 && errno != ENOENT) {
                    error() << "Failed to unlink socket file " << ip << " "
                            << errnoWithDescription(errno);
                    fassertFailedNoTrace(40486);
                }

                acceptor.bind(endpoint, ec);
                if (ec) {
                    return errorCodeToStatus(ec);
                }

                if (::chmod(ip.c_str(), serverGlobalParams.unixSocketPermissions) == -1) {
                    error() << "Failed to chmod socket file " << ip << " "
                            << errnoWithDescription(errno);
                    fassertFailedNoTrace(40487);
                }
                _acceptors.emplace_back(std::move(acceptor));
                continue;
            } else {
#endif
                std::stringstream ss;
                ss << "Bad listen address \"" << ip << ec.message();
                return Status{ErrorCodes::BadValue, ss.str()};
#ifndef _WIN32
            }
#endif
        }

        if (address.is_v6() && !_listenerOptions.enableIPv6) {
            error() << "Specified ipv6 bind address, but ipv6 is disabled";
            fassertFailedNoTrace(40488);
        }

        asio::ip::tcp::endpoint endpoint(address, _listenerOptions.port);
        asio::ip::tcp::acceptor acceptor(*_ioContext);
        acceptor.open(endpoint.protocol());
        acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor.bind(endpoint, ec);
        if (ec) {
            return errorCodeToStatus(ec);
        }
        _acceptors.emplace_back(std::move(acceptor));
    }

    invariant(!_acceptors.empty());

#ifdef MONGO_CONFIG_SSL
    const auto& sslParams = getSSLGlobalParams();
    _sslMode = static_cast<SSLParams::SSLModes>(sslParams.sslMode.load());

    if (_sslMode != SSLParams::SSLMode_disabled) {
        _sslContext = stdx::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);

        const auto sslManager = getSSLManager();
        sslManager
            ->initSSLContext(_sslContext->native_handle(),
                             sslParams,
                             SSLManagerInterface::ConnectionDirection::kOutgoing)
            .transitional_ignore();
    }
#endif

    return Status::OK();
}

Status TransportLayerASIO::start() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _running.store(true);

    // If we're in async mode then the ServiceExecutor will handle calling run_one() in a pool
    // of threads. Otherwise we need a thread to just handle the async_accept calls.
    if (!_listenerOptions.async) {
        _listenerThread = stdx::thread([this] {
            setThreadName("listener");
            while (_running.load()) {
                try {
                    _ioContext->run();
                    _ioContext->reset();
                } catch (...) {
                    severe() << "Uncaught exception in the listener: " << exceptionToStatus();
                    fassertFailed(40491);
                }
            }
        });
    }

    for (auto& acceptor : _acceptors) {
        acceptor.listen();
        _acceptConnection(acceptor);
    }

    return Status::OK();
}

void TransportLayerASIO::shutdown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _running.store(false);
    _ioContext->stop();

    if (_listenerThread.joinable()) {
        _listenerThread.join();
    }
}

void TransportLayerASIO::eraseSession(TransportLayerASIO::SessionsListIterator it) {
    if (it != _sessions.end()) {
        stdx::lock_guard<stdx::mutex> lk(_mutex);
        _sessions.erase(it);
    }
}

void TransportLayerASIO::_acceptConnection(GenericAcceptor& acceptor) {
    auto session = createSession();
    if (!session) {
        _acceptConnection(acceptor);
        return;
    }

    auto& socket = session->getSocket();
    auto acceptCb = [ this, session = std::move(session), &acceptor ](std::error_code ec) mutable {
        if (ec) {
            log() << "Error accepting new connection on "
                  << endpointToHostAndPort(acceptor.local_endpoint()) << ": " << ec.message();
            _acceptConnection(acceptor);
            return;
        }

        size_t connCount = 0;
        SessionsListIterator listIt;
        {
            stdx::lock_guard<stdx::mutex> lk(_mutex);
            if (_sessions.size() + 1 > _listenerOptions.maxConns) {
                log() << "connection refused because too many open connections: "
                      << _sessions.size();
                _acceptConnection(acceptor);
                return;
            }
            listIt = _sessions.emplace(_sessions.end(), session);
        }

        session->postAcceptSetup(_listenerOptions.async, listIt);

        _createdConnections.addAndFetch(1);
        if (!serverGlobalParams.quiet.load()) {
            const auto word = (connCount == 1 ? " connection"_sd : " connections"_sd);
            log() << "connection accepted from " << session->remote() << " #" << session->id()
                  << " (" << connCount << word << " now open)";
        }

        _sep->startSession(std::move(session));
        _acceptConnection(acceptor);
    };

    acceptor.async_accept(socket, std::move(acceptCb));
}

}  // namespace transport
}  // namespace mongo
