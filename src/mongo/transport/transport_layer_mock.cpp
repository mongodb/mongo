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

#include "mongo/platform/basic.h"

#include "mongo/transport/transport_layer_mock.h"

#include "mongo/base/status.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/mock_ticket.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/net/message.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {

TransportLayerMock::TransportLayerMock() : _shutdown(false) {}

Ticket TransportLayerMock::sourceMessage(const SessionHandle& session,
                                         Message* message,
                                         Date_t expiration) {
    if (inShutdown()) {
        return Ticket(TransportLayer::ShutdownStatus);
    } else if (!owns(session->id())) {
        return Ticket(TransportLayer::SessionUnknownStatus);
    } else if (_sessions[session->id()].ended) {
        return Ticket(TransportLayer::TicketSessionClosedStatus);
    }

    return Ticket(this, stdx::make_unique<transport::MockTicket>(session, message, expiration));
}

Ticket TransportLayerMock::sinkMessage(const SessionHandle& session,
                                       const Message& message,
                                       Date_t expiration) {
    if (inShutdown()) {
        return Ticket(TransportLayer::ShutdownStatus);
    } else if (!owns(session->id())) {
        return Ticket(TransportLayer::SessionUnknownStatus);
    } else if (_sessions[session->id()].ended) {
        return Ticket(TransportLayer::TicketSessionClosedStatus);
    }

    return Ticket(this, stdx::make_unique<transport::MockTicket>(session, expiration));
}

Status TransportLayerMock::wait(Ticket&& ticket) {
    if (inShutdown()) {
        return ShutdownStatus;
    } else if (!ticket.valid()) {
        return ticket.status();
    } else if (!owns(ticket.sessionId())) {
        return TicketSessionUnknownStatus;
    } else if (_sessions[ticket.sessionId()].ended) {
        return TransportLayer::TicketSessionClosedStatus;
    }

    return Status::OK();
}

void TransportLayerMock::asyncWait(Ticket&& ticket, TicketCallback callback) {
    callback(wait(std::move(ticket)));
}

SessionHandle TransportLayerMock::createSession() {
    auto session = MockSession::create(this);
    Session::Id sessionId = session->id();

    _sessions[sessionId] = Connection{false, session, SSLPeerInfo()};

    return _sessions[sessionId].session;
}

SessionHandle TransportLayerMock::get(Session::Id id) {
    if (!owns(id))
        return nullptr;

    return _sessions[id].session;
}

bool TransportLayerMock::owns(Session::Id id) {
    return _sessions.count(id) > 0;
}

void TransportLayerMock::end(const SessionHandle& session) {
    if (!owns(session->id()))
        return;
    _sessions[session->id()].ended = true;
}

Status TransportLayerMock::setup() {
    return Status::OK();
}

Status TransportLayerMock::start() {
    return Status::OK();
}

void TransportLayerMock::shutdown() {
    if (!inShutdown()) {
        _shutdown = true;
    }
}

bool TransportLayerMock::inShutdown() const {
    return _shutdown;
}

TransportLayerMock::~TransportLayerMock() {
    shutdown();
}

}  // namespace transport
}  // namespace mongo
