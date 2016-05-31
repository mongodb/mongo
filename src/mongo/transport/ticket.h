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

#pragma once

#include <memory>

#include "mongo/base/disallow_copying.h"
#include "mongo/stdx/functional.h"
#include "mongo/transport/session_id.h"
#include "mongo/transport/ticket_impl.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {

class TransportLayer;

/**
 * A Ticket represents some work to be done within the TransportLayer.
 * Run Tickets by passing them in a call to either TransportLayer::wait()
 * or TransportLayer::asyncWait().
 */
class Ticket {
    MONGO_DISALLOW_COPYING(Ticket);

public:
    using TicketCallback = stdx::function<void(Status)>;

    friend class TransportLayer;

    /**
     * Indicates that there is no expiration time by when a ticket needs to complete.
     */
    static const Date_t kNoExpirationDate;

    Ticket(TransportLayer* tl, std::unique_ptr<TicketImpl> ticket);
    ~Ticket();

    /**
     * Move constructor and assignment operator.
     */
    Ticket(Ticket&&);
    Ticket& operator=(Ticket&&);

    /**
     * Return this ticket's session id.
     */
    SessionId sessionId() const {
        return _ticket->sessionId();
    }

    /**
     * Return this ticket's expiration date.
     */
    Date_t expiration() const {
        return _ticket->expiration();
    }

    /**
     * Wait for this ticket to be filled.
     *
     * This is this-rvalue qualified because it consumes the ticket
     */
    Status wait() &&;

    /**
     * Asynchronously wait for this ticket to be filled.
     *
     * This is this-rvalue qualified because it consumes the ticket
     */
    void asyncWait(TicketCallback cb) &&;

protected:
    /**
     * Return a non-owning pointer to the underlying TicketImpl type
     */
    TicketImpl* impl() const {
        return _ticket.get();
    }

private:
    TransportLayer* _tl;
    std::unique_ptr<TicketImpl> _ticket;
};

}  // namespace transport
}  // namespace mongo
