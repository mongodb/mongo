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

#include "mongo/base/disallow_copying.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ticket_impl.h"
#include "mongo/util/time_support.h"

namespace mongo {

class Message;

namespace transport {

/**
 * A mock ticket class for our test suite.
 */
class MockTicket : public TicketImpl {
    MONGO_DISALLOW_COPYING(MockTicket);

public:
    // Source constructor
    MockTicket(const SessionHandle& session,
               Message* message,
               Date_t expiration = Ticket::kNoExpirationDate)
        : _id(session->id()), _message(message), _expiration(expiration) {}

    // Sink constructor
    MockTicket(const SessionHandle& session, Date_t expiration = Ticket::kNoExpirationDate)
        : _id(session->id()), _expiration(expiration) {}

    SessionId sessionId() const override {
        return _id;
    }

    Date_t expiration() const override {
        return _expiration;
    }

    boost::optional<Message*> message() const {
        return _message;
    }

private:
    Session::Id _id;
    boost::optional<Message*> _message;
    Date_t _expiration;
};

}  // namespace transport
}  // namespace mongo
