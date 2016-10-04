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

#include "mongo/base/status.h"
#include "mongo/transport/ticket.h"
#include "mongo/transport/ticket_impl.h"
#include "mongo/transport/transport_layer.h"

namespace mongo {
namespace transport {

const Date_t Ticket::kNoExpirationDate{Date_t::max()};

Status Ticket::ExpiredStatus = Status(ErrorCodes::ExceededTimeLimit, "Ticket has expired.");

Status Ticket::SessionClosedStatus =
    Status(ErrorCodes::TransportSessionClosed, "Ticket's Session is closed.");

Ticket::Ticket(TransportLayer* tl, std::unique_ptr<TicketImpl> ticket)
    : _tl(tl), _ticket(std::move(ticket)) {}

Ticket::Ticket(Status status) : _status(status) {}

Ticket::~Ticket() = default;

Ticket::Ticket(Ticket&&) = default;
Ticket& Ticket::operator=(Ticket&&) = default;

Status Ticket::wait()&& {
    return _tl->wait(std::move(*this));
}

void Ticket::asyncWait(TicketCallback cb)&& {
    return _tl->asyncWait(std::move(*this), std::move(cb));
}

bool Ticket::valid() {
    return _status == Status::OK() && !expired();
}

Status Ticket::status() const {
    return _status;
}

bool Ticket::expired() {
    bool expired = expiration() <= Date_t::now();
    if (_status == Status::OK() && expired) {
        _status = Status(ErrorCodes::ExceededTimeLimit, "Ticket has expired.");
    }
    return expired;
}

}  // namespace transport
}  // namespace mongo
