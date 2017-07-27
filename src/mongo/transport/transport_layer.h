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

#include "mongo/base/status.h"
#include "mongo/stdx/functional.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ticket.h"
#include "mongo/util/net/message.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace transport {

class TicketImpl;

/**
 * The TransportLayer moves Messages between transport::Endpoints and the database.
 * This class owns an Acceptor that generates new endpoints from which it can
 * source Messages.
 *
 * The TransportLayer creates Session objects and maps them internally to
 * endpoints. New Sessions are passed to the database (via a ServiceEntryPoint)
 * to be run. The database must then call additional methods on the TransportLayer
 * to manage the Session in a get-Message, handle-Message, return-Message cycle.
 * It must do this on its own thread(s).
 *
 * References to the TransportLayer should be stored on service context objects.
 */
class TransportLayer {
    MONGO_DISALLOW_COPYING(TransportLayer);

public:
    static const Status SessionUnknownStatus;
    static const Status ShutdownStatus;
    static const Status TicketSessionUnknownStatus;
    static const Status TicketSessionClosedStatus;

    friend class Session;

    virtual ~TransportLayer() = default;

    /**
     * Source (receive) a new Message for this Session.
     *
     * This method returns a work Ticket. The caller must complete the Ticket by
     * passing it to either TransportLayer::wait() or TransportLayer::asyncWait().
     *
     * If an expiration date is given, the returned Ticket will expire at that time.
     *
     * When run, the returned Ticket will be exchanged for a Status. If the
     * TransportLayer is unable to source a Message, this will be a failed status,
     * and the passed-in Message buffer may be left in an invalid state.
     */
    virtual Ticket sourceMessage(const SessionHandle& session,
                                 Message* message,
                                 Date_t expiration = Ticket::kNoExpirationDate) = 0;

    /**
     * Sink (send) a new Message for this Session. This method should be used
     * to send replies to a given host.
     *
     * This method returns a work Ticket. The caller must complete the Ticket by
     * passing it to either TransportLayer::wait() or TransportLayer::asyncWait().
     *
     * If an expiration date is given, the returned Ticket will expire at that time.
     *
     * When run, the returned Ticket will be exchanged for a Status. If the
     * TransportLayer is unable to sink the Message, this will be a failed status.
     *
     * This method does NOT take ownership of the sunk Message, which must be cleaned
     * up by the caller.
     */
    virtual Ticket sinkMessage(const SessionHandle& session,
                               const Message& message,
                               Date_t expiration = Ticket::kNoExpirationDate) = 0;

    /**
     * Perform a synchronous wait on the given work Ticket. When this call returns,
     * the Ticket will have been completed. A call to wait() consumes the Ticket.
     *
     * This thread may be used by the TransportLayer to run other Tickets that were
     * enqueued prior to this call.
     */
    virtual Status wait(Ticket&& ticket) = 0;

    /**
     * Callback for Tickets that are run via asyncWait().
     */
    using TicketCallback = stdx::function<void(Status)>;

    /**
     * Perform an asynchronous wait on the given work Ticket. Once the Ticket has been
     * completed, the passed-in callback will be invoked.
     *
     * This thread will not be used by the TransportLayer to perform work. The callback
     * passed to asyncWait() may be run on any thread.
     */
    virtual void asyncWait(Ticket&& ticket, TicketCallback callback) = 0;

    /**
     * End the given Session. Tickets for this Session that have already been
     * started via wait() or asyncWait() will complete, but may return a failed Status.
     * Future calls to wait() or asyncWait() for this Session will fail. If this
     * TransportLayer implementation is networked, any connections for this Session will
     * be closed.
     *
     * ~Session() will automatically call end() with itself.
     *
     * This method is idempotent and synchronous.
     */
    virtual void end(const SessionHandle& session) = 0;

    /**
     * Start the TransportLayer. After this point, the TransportLayer will begin accepting active
     * sessions from new transport::Endpoints.
     */
    virtual Status start() = 0;

    /**
     * Shut the TransportLayer down. After this point, the TransportLayer will
     * end all active sessions and won't accept new transport::Endpoints. Any
     * future calls to wait() or asyncWait() will fail. This method is synchronous and
     * will not return until all sessions have ended and any network connections have been
     * closed.
     */
    virtual void shutdown() = 0;

    /**
     * Optional method for subclasses to setup their state before being ready to accept
     * connections.
     */
    virtual Status setup() = 0;

protected:
    TransportLayer() = default;

    /**
     * Return the implementation of this Ticket.
     */
    TicketImpl* getTicketImpl(const Ticket& ticket) {
        return ticket.impl();
    }

    std::unique_ptr<TicketImpl> getOwnedTicketImpl(Ticket&& ticket) {
        return std::move(ticket).releaseImpl();
    }

    /**
     * Return the transport layer of this Ticket.
     */
    TransportLayer* getTicketTransportLayer(const Ticket& ticket) {
        return ticket._tl;
    }
};

}  // namespace transport
}  // namespace mongo
