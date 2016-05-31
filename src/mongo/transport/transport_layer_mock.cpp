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

#include <memory>

#include "mongo/base/status.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ticket.h"
#include "mongo/transport/ticket_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/time_support.h"

#include "mongo/stdx/memory.h"

namespace mongo {
namespace transport {

Session::Id TransportLayerMock::MockTicket::sessionId() const {
    return Session::Id{};
}

Date_t TransportLayerMock::MockTicket::expiration() const {
    return Date_t::now();
}

Ticket TransportLayerMock::sourceMessage(const Session& session,
                                         Message* message,
                                         Date_t expiration) {
    return Ticket(this, stdx::make_unique<MockTicket>());
}

Ticket TransportLayerMock::sinkMessage(const Session& session,
                                       const Message& message,
                                       Date_t expiration) {
    return Ticket(this, stdx::make_unique<MockTicket>());
}

Status TransportLayerMock::wait(Ticket&& ticket) {
    return Status::OK();
}

void TransportLayerMock::asyncWait(Ticket&& ticket, TicketCallback callback) {
    callback(Status::OK());
}

std::string TransportLayerMock::getX509SubjectName(const Session& session) {
    return session.getX509SubjectName();
}

TransportLayer::Stats TransportLayerMock::sessionStats() {
    return Stats();
}

void TransportLayerMock::registerTags(const Session& session) {}

void TransportLayerMock::end(const Session& session) {}

void TransportLayerMock::endAllSessions(Session::TagMask tags) {}

Status TransportLayerMock::start() {
    return Status::OK();
}

void TransportLayerMock::shutdown() {}

}  // namespace transport
}  // namespace mongo
