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
#include "mongo/transport/message_compressor_manager.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/net/abstract_message_port.h"
#include "mongo/util/net/socket_exception.h"
#include "mongo/util/net/ssl_types.h"

namespace mongo {
namespace transport {
namespace {
struct lock_weak {
    template <typename T>
    std::shared_ptr<T> operator()(const std::weak_ptr<T>& p) const {
        return p.lock();
    }
};
}  // namespace

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
    : _remote(amp->remote()),
      _local(amp->localAddr().toString(true)),
      _tl(tl),
      _tags(kEmptyTagMask),
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

    _listenerThread = stdx::thread([this]() { _listener->initAndListen(); });

    return Status::OK();
}

TransportLayerLegacy::~TransportLayerLegacy() = default;

Ticket TransportLayerLegacy::sourceMessage(const SessionHandle& session,
                                           Message* message,
                                           Date_t expiration) {
    auto& compressorMgr = MessageCompressorManager::forSession(session);
    auto sourceCb = [message, &compressorMgr](AbstractMessagingPort* amp) -> Status {
        if (!amp->recv(*message)) {
            return {ErrorCodes::HostUnreachable, "Recv failed"};
        }

        networkCounter.hitPhysical(message->size(), 0);
        if (message->operation() == dbCompressed) {
            auto swm = compressorMgr.decompressMessage(*message);
            if (!swm.isOK())
                return swm.getStatus();
            *message = swm.getValue();
        }
        networkCounter.hitLogical(message->size(), 0);
        return Status::OK();
    };

    auto legacySession = checked_pointer_cast<LegacySession>(session);
    return Ticket(
        this,
        stdx::make_unique<LegacyTicket>(std::move(legacySession), expiration, std::move(sourceCb)));
}

TransportLayer::Stats TransportLayerLegacy::sessionStats() {
    Stats stats;
    {
        stdx::lock_guard<stdx::mutex> lk(_sessionsMutex);
        stats.numOpenSessions = _sessions.size();
    }

    stats.numAvailableSessions = Listener::globalTicketHolder.available();
    stats.numCreatedSessions = Listener::globalConnectionNumber.load();

    return stats;
}

Ticket TransportLayerLegacy::sinkMessage(const SessionHandle& session,
                                         const Message& message,
                                         Date_t expiration) {
    auto& compressorMgr = MessageCompressorManager::forSession(session);
    auto sinkCb = [&message, &compressorMgr](AbstractMessagingPort* amp) -> Status {
        try {
            networkCounter.hitLogical(0, message.size());
            auto swm = compressorMgr.compressMessage(message);
            if (!swm.isOK())
                return swm.getStatus();
            const auto& compressedMessage = swm.getValue();
            amp->say(compressedMessage);
            networkCounter.hitPhysical(0, compressedMessage.size());

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
    conn->closed = true;
    conn->amp->shutdown();
    Listener::globalTicketHolder.release();
}

// Capture all of the weak pointers behind the lock, to delay their expiry until we leave the
// locking context. This function requires proof of locking, by passing the lock guard.
auto TransportLayerLegacy::lockAllSessions(const stdx::unique_lock<stdx::mutex>&) const
    -> std::vector<LegacySessionHandle> {
    using std::begin;
    using std::end;
    std::vector<std::shared_ptr<LegacySession>> result;
    std::transform(begin(_sessions), end(_sessions), std::back_inserter(result), lock_weak());
    // Skip expired weak pointers.
    result.erase(std::remove(begin(result), end(result), nullptr), end(result));
    return result;
}

void TransportLayerLegacy::endAllSessions(Session::TagMask tags) {
    log() << "legacy transport layer closing all connections";
    {
        stdx::unique_lock<stdx::mutex> lk(_sessionsMutex);
        // We want to capture the shared_ptrs to our sessions in a way which lets us destroy them
        // outside of the lock.
        const auto sessions = lockAllSessions(lk);

        for (auto&& session : sessions) {
            if (session->getTags() & tags) {
                log() << "Skip closing connection for connection # "
                      << session->conn()->connectionId;
            } else {
                _closeConnection(session->conn());
            }
        }
        // TODO(SERVER-27069): Revamp this lock to not cover the loop. This unlock was put here
        // specifically to minimize risk, just before the release of 3.4. The risk is that we would
        // be in the loop without the lock, which most of our testing didn't do. We must unlock
        // manually here, because the `sessions` vector must be destroyed *outside* of the lock.
        lk.unlock();
    }
}

void TransportLayerLegacy::shutdown() {
    _running.store(false);
    _listener->shutdown();
    _listenerThread.join();
    endAllSessions(Session::kEmptyTagMask);
}

void TransportLayerLegacy::_destroy(LegacySession& session) {
    if (!session.conn()->closed) {
        _closeConnection(session.conn());
    }

    stdx::lock_guard<stdx::mutex> lk(_sessionsMutex);
    _sessions.erase(session.getIter());
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
    if (!Listener::globalTicketHolder.tryAcquire()) {
        log() << "connection refused because too many open connections: "
              << Listener::globalTicketHolder.used();
        amp->shutdown();
        return;
    }

    amp->setLogLevel(logger::LogSeverity::Debug(1));
    auto session = LegacySession::create(std::move(amp), this);

    stdx::list<std::weak_ptr<LegacySession>> list;
    auto it = list.emplace(list.begin(), session);

    {
        // Add the new session to our list
        stdx::lock_guard<stdx::mutex> lk(_sessionsMutex);
        session->setIter(it);
        _sessions.splice(_sessions.begin(), list, it);
    }

    invariant(_sep);
    _sep->startSession(std::move(session));
}

}  // namespace transport
}  // namespace mongo
