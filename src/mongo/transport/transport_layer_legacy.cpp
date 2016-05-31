/**
 *    Copyright (C) 2016 MongoDB Inc.
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

#include "mongo/transport/transport_layer_legacy.h"

#include "mongo/base/checked_cast.h"
#include "mongo/config.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/stdx/functional.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/abstract_message_port.h"
#include "mongo/util/net/socket_exception.h"

namespace mongo {
namespace transport {

TransportLayerLegacy::ListenerLegacy::ListenerLegacy(const TransportLayerLegacy::Options& opts,
                                                     NewConnectionCb callback)
    : Listener("", opts.ipList, opts.port, getGlobalServiceContext(), true),
      _accepted(std::move(callback)) {}

void TransportLayerLegacy::ListenerLegacy::accepted(std::unique_ptr<AbstractMessagingPort> mp) {
    _accepted(std::move(mp));
}

TransportLayerLegacy::TransportLayerLegacy(const TransportLayerLegacy::Options& opts,
                                           std::shared_ptr<ServiceEntryPoint> sep)
    : _sep(sep),
      _listener(stdx::make_unique<ListenerLegacy>(
          opts,
          stdx::bind(&TransportLayerLegacy::_handleNewConnection, this, stdx::placeholders::_1))),
      _running(false),
      _options(opts) {}

TransportLayerLegacy::LegacyTicket::LegacyTicket(const Session& session,
                                                 Date_t expiration,
                                                 WorkHandle work)
    : _sessionId(session.id()), _expiration(expiration), _fill(std::move(work)) {}

Session::Id TransportLayerLegacy::LegacyTicket::sessionId() const {
    return _sessionId;
}

Date_t TransportLayerLegacy::LegacyTicket::expiration() const {
    return _expiration;
}

Status TransportLayerLegacy::setup() {
    if (!_listener->setupSockets()) {
        error() << "Failed to set up sockets during startup.";
        return {ErrorCodes::InternalError, "Failed to set up sockets"};
    }

    return Status::OK();
}

Status TransportLayerLegacy::start() {
    if (_running.swap(true)) {
        return {ErrorCodes::InternalError, "TransportLayer is already running"};
    }

    _listenerThread = stdx::thread([this]() { _listener->initAndListen(); });

    return Status::OK();
}

TransportLayerLegacy::~TransportLayerLegacy() = default;

Ticket TransportLayerLegacy::sourceMessage(const Session& session,
                                           Message* message,
                                           Date_t expiration) {
    auto sourceCb = [message](AbstractMessagingPort* amp) -> Status {
        if (!amp->recv(*message)) {
            return {ErrorCodes::HostUnreachable, "Recv failed"};
        }
        return Status::OK();
    };

    return Ticket(this, stdx::make_unique<LegacyTicket>(session, expiration, std::move(sourceCb)));
}

std::string TransportLayerLegacy::getX509SubjectName(const Session& session) {
    {
        stdx::lock_guard<stdx::mutex> lk(_connectionsMutex);
        auto conn = _connections.find(session.id());
        if (conn == _connections.end()) {
            // Return empty string if the session is not found
            return "";
        }

        return conn->second.x509SubjectName.value_or("");
    }
}

TransportLayer::Stats TransportLayerLegacy::sessionStats() {
    Stats stats;
    {
        stdx::lock_guard<stdx::mutex> lk(_connectionsMutex);
        stats.numOpenSessions = _connections.size();
    }

    stats.numAvailableSessions = Listener::globalTicketHolder.available();
    stats.numCreatedSessions = Listener::globalConnectionNumber.load();

    return stats;
}

Ticket TransportLayerLegacy::sinkMessage(const Session& session,
                                         const Message& message,
                                         Date_t expiration) {
    auto sinkCb = [&message](AbstractMessagingPort* amp) -> Status {
        try {
            amp->say(message);
            return Status::OK();
        } catch (const SocketException& e) {
            return {ErrorCodes::HostUnreachable, e.what()};
        }
    };

    return Ticket(this, stdx::make_unique<LegacyTicket>(session, expiration, std::move(sinkCb)));
}

Status TransportLayerLegacy::wait(Ticket&& ticket) {
    return _runTicket(std::move(ticket));
}

void TransportLayerLegacy::asyncWait(Ticket&& ticket, TicketCallback callback) {
    // Left unimplemented because there is no reasonable way to offer general async waiting besides
    // offering a background thread that can handle waits for multiple tickets. We may never
    // implement this for the legacy TL.
    MONGO_UNREACHABLE;
}

void TransportLayerLegacy::end(const Session& session) {
    stdx::lock_guard<stdx::mutex> lk(_connectionsMutex);
    auto conn = _connections.find(session.id());
    if (conn != _connections.end()) {
        _endSession_inlock(conn);
    }
}

void TransportLayerLegacy::registerTags(const Session& session) {
    stdx::lock_guard<stdx::mutex> lk(_connectionsMutex);
    auto conn = _connections.find(session.id());
    if (conn != _connections.end()) {
        conn->second.tags = session.getTags();
    }
}

void TransportLayerLegacy::_endSession_inlock(
    decltype(TransportLayerLegacy::_connections.begin()) conn) {
    conn->second.amp->shutdown();

    // If the amp is in use it means that we're in the middle of fulfilling the ticket in _runTicket
    // and can't erase it immediately.
    //
    // In that case we'll rely on _runTicket to do the removal for us later
    if (conn->second.inUse) {
        conn->second.ended = true;
    } else {
        Listener::globalTicketHolder.release();
        _connections.erase(conn);
    }
}

void TransportLayerLegacy::endAllSessions(Session::TagMask tags) {
    log() << "legacy transport layer ending all sessions";
    {
        stdx::lock_guard<stdx::mutex> lk(_connectionsMutex);
        auto&& conn = _connections.begin();
        while (conn != _connections.end()) {
            // If we erase this connection below, we invalidate our iterator, use a placeholder.
            auto placeholder = conn;
            placeholder++;

            if (conn->second.tags & tags) {
                log() << "Skip closing connection for connection # " << conn->second.connectionId;
            } else {
                _endSession_inlock(conn);
            }

            conn = placeholder;
        }
    }
}

void TransportLayerLegacy::shutdown() {
    _running.store(false);
    _listener->shutdown();
    _listenerThread.join();
    endAllSessions();
}

Status TransportLayerLegacy::_runTicket(Ticket ticket) {
    if (!_running.load()) {
        return {ErrorCodes::ShutdownInProgress, "TransportLayer in shutdown"};
    }

    if (ticket.expiration() < Date_t::now()) {
        return {ErrorCodes::ExceededTimeLimit, "Ticket has expired"};
    }

    AbstractMessagingPort* amp;

    {
        stdx::lock_guard<stdx::mutex> lk(_connectionsMutex);

        auto conn = _connections.find(ticket.sessionId());
        if (conn == _connections.end()) {
            return {ErrorCodes::TransportSessionNotFound, "No such session in TransportLayer"};
        }

        // "check out" the port
        conn->second.inUse = true;
        amp = conn->second.amp.get();
    }

    amp->clearCounters();

    auto legacyTicket = checked_cast<LegacyTicket*>(getTicketImpl(ticket));
    auto res = legacyTicket->_fill(amp);

    networkCounter.hit(amp->getBytesIn(), amp->getBytesOut());

    {
        stdx::lock_guard<stdx::mutex> lk(_connectionsMutex);

        auto conn = _connections.find(ticket.sessionId());
        invariant(conn != _connections.end());

#ifdef MONGO_CONFIG_SSL
        // If we didn't have an X509 subject name, see if we have one now
        if (!conn->second.x509SubjectName) {
            auto name = amp->getX509SubjectName();
            if (name != "") {
                conn->second.x509SubjectName = name;
            }
        }
#endif
        conn->second.inUse = false;

        if (conn->second.ended) {
            Listener::globalTicketHolder.release();
            _connections.erase(conn);
        }
    }

    return res;
}

void TransportLayerLegacy::_handleNewConnection(std::unique_ptr<AbstractMessagingPort> amp) {
    if (!Listener::globalTicketHolder.tryAcquire()) {
        log() << "connection refused because too many open connections: "
              << Listener::globalTicketHolder.used();
        amp->shutdown();
        return;
    }

    Session session(amp->remote(), HostAndPort(amp->localAddr().toString(true)), this);

    amp->setLogLevel(logger::LogSeverity::Debug(1));

    {
        stdx::lock_guard<stdx::mutex> lk(_connectionsMutex);
        _connections.emplace(std::piecewise_construct,
                             std::forward_as_tuple(session.id()),
                             std::forward_as_tuple(std::move(amp), false, session.getTags()));
    }

    invariant(_sep);
    _sep->startSession(std::move(session));
}

}  // namespace transport
}  // namespace mongo
