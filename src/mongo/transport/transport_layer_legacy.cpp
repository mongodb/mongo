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

#include <algorithm>
#include <iterator>
#include <memory>

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
#include "mongo/util/net/ssl_types.h"

namespace mongo {
namespace transport {

TransportLayerLegacy::Options::Options(const ServerGlobalParams* params)
    : port(params->port), ipList(params->bind_ip) {}

TransportLayerLegacy::ListenerLegacy::ListenerLegacy(const TransportLayerLegacy::Options& opts,
                                                     NewConnectionCb callback)
    : Listener("", opts.ipList, opts.port, getGlobalServiceContext(), true),
      _accepted(std::move(callback)) {}

void TransportLayerLegacy::ListenerLegacy::accepted(std::unique_ptr<AbstractMessagingPort> mp) {
    _accepted(std::move(mp));
}

TransportLayerLegacy::TransportLayerLegacy(const TransportLayerLegacy::Options& opts,
                                           ServiceEntryPoint* sep)
    : _sep(sep),
      _listener(stdx::make_unique<ListenerLegacy>(
          opts,
          stdx::bind(&TransportLayerLegacy::_handleNewConnection, this, stdx::placeholders::_1))),
      _running(false),
      _options(opts) {}

std::shared_ptr<TransportLayerLegacy::LegacySession> TransportLayerLegacy::LegacySession::create(
    std::unique_ptr<AbstractMessagingPort> amp, TransportLayerLegacy* tl) {
    std::shared_ptr<LegacySession> handle(new LegacySession(std::move(amp), tl));
    return handle;
}

TransportLayerLegacy::LegacySession::LegacySession(std::unique_ptr<AbstractMessagingPort> amp,
                                                   TransportLayerLegacy* tl)
    : _remote(amp->remoteAddr()),
      _local(amp->localAddr()),
      _tl(tl),
      _connection(stdx::make_unique<Connection>(std::move(amp))) {}

TransportLayerLegacy::LegacySession::~LegacySession() {
    _tl->_destroy(*this);
}

TransportLayerLegacy::LegacyTicket::LegacyTicket(const LegacySessionHandle& session,
                                                 Date_t expiration,
                                                 WorkHandle work)
    : _session(session),
      _sessionId(session->id()),
      _expiration(expiration),
      _fill(std::move(work)) {}

TransportLayerLegacy::LegacySessionHandle TransportLayerLegacy::LegacyTicket::getSession() {
    return _session.lock();
}

Session::Id TransportLayerLegacy::LegacyTicket::sessionId() const {
    return _sessionId;
}

Date_t TransportLayerLegacy::LegacyTicket::expiration() const {
    return _expiration;
}

Status TransportLayerLegacy::LegacyTicket::fill(AbstractMessagingPort* amp) {
    return _fill(amp);
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

    try {
        _listenerThread = stdx::thread([this]() { _listener->initAndListen(); });
        _listener->waitUntilListening();
        return Status::OK();
    } catch (...) {
        return {ErrorCodes::InternalError, "Failed to start listener thread."};
    }
}

TransportLayerLegacy::~TransportLayerLegacy() = default;

Ticket TransportLayerLegacy::sourceMessage(const SessionHandle& session,
                                           Message* message,
                                           Date_t expiration) {
    auto sourceCb = [message](AbstractMessagingPort* amp) -> Status {
        if (!amp->recv(*message)) {
            return {ErrorCodes::HostUnreachable, "Recv failed"};
        }

        networkCounter.hitPhysicalIn(message->size());
        return Status::OK();
    };

    auto legacySession = checked_pointer_cast<LegacySession>(session);
    return Ticket(
        this,
        stdx::make_unique<LegacyTicket>(std::move(legacySession), expiration, std::move(sourceCb)));
}

Ticket TransportLayerLegacy::sinkMessage(const SessionHandle& session,
                                         const Message& message,
                                         Date_t expiration) {
    auto sinkCb = [&message](AbstractMessagingPort* amp) -> Status {
        try {
            amp->say(message);
            networkCounter.hitPhysicalOut(message.size());

            return Status::OK();
        } catch (const SocketException& e) {
            return {ErrorCodes::HostUnreachable, e.what()};
        }
    };

    auto legacySession = checked_pointer_cast<LegacySession>(session);
    return Ticket(
        this,
        stdx::make_unique<LegacyTicket>(std::move(legacySession), expiration, std::move(sinkCb)));
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

void TransportLayerLegacy::end(const SessionHandle& session) {
    auto legacySession = checked_pointer_cast<const LegacySession>(session);
    _closeConnection(legacySession->conn());
}

void TransportLayerLegacy::_closeConnection(Connection* conn) {
    stdx::lock_guard<stdx::mutex> lk(conn->closeMutex);

    if (conn->closed) {
        return;
    }

    conn->closed = true;
    conn->amp->shutdown();
}

void TransportLayerLegacy::shutdown() {
    _running.store(false);
    ListeningSockets::get()->closeAll();
    _listener->shutdown();
    if (_listenerThread.joinable()) {
        _listenerThread.join();
    }
}

void TransportLayerLegacy::_destroy(LegacySession& session) {
    if (!session.conn()->closed) {
        _closeConnection(session.conn());
    }
}

Status TransportLayerLegacy::_runTicket(Ticket ticket) {
    if (!_running.load()) {
        return TransportLayer::ShutdownStatus;
    }

    if (ticket.expiration() < Date_t::now()) {
        return Ticket::ExpiredStatus;
    }

    // get the weak_ptr out of the ticket
    // attempt to make it into a shared_ptr
    auto legacyTicket = checked_cast<LegacyTicket*>(getTicketImpl(ticket));
    auto session = legacyTicket->getSession();
    if (!session) {
        return TransportLayer::TicketSessionClosedStatus;
    }

    auto conn = session->conn();
    if (conn->closed) {
        return TransportLayer::TicketSessionClosedStatus;
    }

    Status res = Status::OK();
    try {
        res = legacyTicket->fill(conn->amp.get());
    } catch (...) {
        res = exceptionToStatus();
    }

#ifdef MONGO_CONFIG_SSL
    // If we didn't have an X509 subject name, see if we have one now
    auto& sslPeerInfo = SSLPeerInfo::forSession(legacyTicket->getSession());
    if (sslPeerInfo.subjectName.empty()) {
        auto info = conn->amp->getX509PeerInfo();
        if (!info.subjectName.empty()) {
            sslPeerInfo = info;
        }
    }
#endif

    return res;
}

void TransportLayerLegacy::_handleNewConnection(std::unique_ptr<AbstractMessagingPort> amp) {
    amp->setLogLevel(logger::LogSeverity::Debug(1));

    auto session = LegacySession::create(std::move(amp), this);
    invariant(_sep);
    _sep->startSession(std::move(session));
}

}  // namespace transport
}  // namespace mongo
