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

#include "mongo/stdx/memory.h"
#include "mongo/transport/session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace transport {

class TransportLayerMockTest : public mongo::unittest::Test {
public:
    void setUp() {
        _transportLayer = stdx::make_unique<TransportLayerMock>();
    }

    TransportLayerMock* tl() {
        return _transportLayer.get();
    }

private:
    std::unique_ptr<TransportLayerMock> _transportLayer;
};

// sinkMessage() generates a valid Ticket
TEST_F(TransportLayerMockTest, SinkMessageGeneratesTicket) {
    Message msg{};
    Session* session = tl()->createSession();

    // call sinkMessage() with no expiration
    Ticket ticket = tl()->sinkMessage(*session, msg);
    ASSERT(ticket.valid());
    ASSERT_OK(ticket.status());
    ASSERT_EQUALS(ticket.sessionId(), session->id());
    ASSERT_EQUALS(ticket.expiration(), Ticket::kNoExpirationDate);

    // call sinkMessage() with an expiration
    Date_t expiration = Date_t::now() + Hours(1);
    ticket = tl()->sinkMessage(*session, msg, expiration);
    ASSERT(ticket.valid());
    ASSERT_OK(ticket.status());
    ASSERT_EQUALS(ticket.sessionId(), session->id());
    ASSERT_EQUALS(ticket.expiration(), expiration);
}

// sinkMessage() generates an invalid Ticket if the Session is closed
TEST_F(TransportLayerMockTest, SinkMessageSessionClosed) {
    Message msg{};
    Session* session = tl()->createSession();

    tl()->end(*session);

    Ticket ticket = tl()->sinkMessage(*session, msg);
    ASSERT_FALSE(ticket.valid());
    ASSERT_EQUALS(ticket.status().code(), ErrorCodes::TransportSessionClosed);
}

// sinkMessage() generates an invalid Ticket if the TransportLayer does not own the Session
TEST_F(TransportLayerMockTest, SinkMessageSessionUnknown) {
    Message msg{};

    std::unique_ptr<TransportLayerMock> anotherTL = stdx::make_unique<TransportLayerMock>();
    Session* session = anotherTL->createSession();

    Ticket ticket = tl()->sinkMessage(*session, msg);
    ASSERT_FALSE(ticket.valid());
    ASSERT_EQUALS(ticket.status().code(), ErrorCodes::TransportSessionUnknown);
}

// sinkMessage() generates an invalid Ticket if the TransportLayer is in shutdown
TEST_F(TransportLayerMockTest, SinkMessageTLShutdown) {
    Message msg{};
    Session* session = tl()->createSession();

    tl()->shutdown();

    Ticket ticket = tl()->sinkMessage(*session, msg);
    ASSERT_FALSE(ticket.valid());
    ASSERT_EQUALS(ticket.status().code(), ErrorCodes::ShutdownInProgress);
}

// sourceMessage() generates a valid ticket
TEST_F(TransportLayerMockTest, SourceMessageGeneratesTicket) {
    Message msg{};
    Session* session = tl()->createSession();

    // call sourceMessage() with no expiration
    Ticket ticket = tl()->sourceMessage(*session, &msg);
    ASSERT(ticket.valid());
    ASSERT_OK(ticket.status());
    ASSERT_EQUALS(ticket.sessionId(), session->id());
    ASSERT(msg.empty());
    ASSERT_EQUALS(ticket.expiration(), Ticket::kNoExpirationDate);

    // call sourceMessage() with an expiration
    Date_t expiration = Date_t::now() + Hours(1);
    ticket = tl()->sourceMessage(*session, &msg, expiration);
    ASSERT(ticket.valid());
    ASSERT_OK(ticket.status());
    ASSERT_EQUALS(ticket.sessionId(), session->id());
    ASSERT(msg.empty());
    ASSERT_EQUALS(ticket.expiration(), expiration);
}

// sourceMessage() generates an invalid ticket if the Session is closed
TEST_F(TransportLayerMockTest, SourceMessageSessionClosed) {
    Message msg{};
    Session* session = tl()->createSession();

    tl()->end(*session);

    Ticket ticket = tl()->sourceMessage(*session, &msg);
    ASSERT_FALSE(ticket.valid());
    ASSERT_EQUALS(ticket.status().code(), ErrorCodes::TransportSessionClosed);
}

// sourceMessage() generates an invalid ticket if the TransportLayer does not own the Session
TEST_F(TransportLayerMockTest, SourceMessageSessionUnknown) {
    Message msg{};

    std::unique_ptr<TransportLayerMock> anotherTL = stdx::make_unique<TransportLayerMock>();
    Session* session = anotherTL->createSession();

    Ticket ticket = tl()->sourceMessage(*session, &msg);
    ASSERT_FALSE(ticket.valid());
    ASSERT_EQUALS(ticket.status().code(), ErrorCodes::TransportSessionUnknown);
}

// sourceMessage() generates an invalid ticket if the TransportLayer is in shutdown
TEST_F(TransportLayerMockTest, SourceMessageTLShutdown) {
    Message msg{};
    Session* session = tl()->createSession();

    tl()->shutdown();

    Ticket ticket = tl()->sourceMessage(*session, &msg);
    ASSERT_FALSE(ticket.valid());
    ASSERT_EQUALS(ticket.status().code(), ErrorCodes::ShutdownInProgress);
}

// wait() returns an OK status
TEST_F(TransportLayerMockTest, Wait) {
    Session* session = tl()->createSession();
    Ticket ticket = Ticket(tl(), stdx::make_unique<TransportLayerMock::TicketMock>(session));

    Status status = tl()->wait(std::move(ticket));
    ASSERT_OK(status);
}

// wait() returns an TicketExpired error status if the Ticket expired
TEST_F(TransportLayerMockTest, WaitExpiredTicket) {
    Session* session = tl()->createSession();
    Ticket expiredTicket =
        Ticket(tl(), stdx::make_unique<TransportLayerMock::TicketMock>(session, Date_t::now()));

    Status status = tl()->wait(std::move(expiredTicket));
    ASSERT_EQUALS(status.code(), ErrorCodes::ExceededTimeLimit);
}

// wait() returns the invalid Ticket's Status
TEST_F(TransportLayerMockTest, WaitInvalidTicket) {
    Ticket invalidTicket = Ticket(Status(ErrorCodes::UnknownError, ""));
    ASSERT_FALSE(invalidTicket.valid());

    Status status = tl()->wait(std::move(invalidTicket));
    ASSERT_EQUALS(status.code(), ErrorCodes::UnknownError);
}

// wait() returns a SessionClosed error status if the Ticket's Session is closed
TEST_F(TransportLayerMockTest, WaitSessionClosed) {
    Session* session = tl()->createSession();
    Ticket ticket = Ticket(tl(), stdx::make_unique<TransportLayerMock::TicketMock>(session));

    tl()->end(*session);

    Status status = tl()->wait(std::move(ticket));
    ASSERT_EQUALS(status.code(), ErrorCodes::TransportSessionClosed);
}

// wait() returns a SessionUnknown error status if the TransportLayer does not own the Ticket's
// Session
TEST_F(TransportLayerMockTest, WaitSessionUnknown) {
    std::unique_ptr<TransportLayerMock> anotherTL = stdx::make_unique<TransportLayerMock>();
    Session* session = anotherTL->createSession();
    Ticket ticket = Ticket(tl(), stdx::make_unique<TransportLayerMock::TicketMock>(session));

    Status status = tl()->wait(std::move(ticket));
    ASSERT_EQUALS(status.code(), ErrorCodes::TransportSessionUnknown);
}

// wait() returns a ShutdownInProgress status if the TransportLayer is in shutdown
TEST_F(TransportLayerMockTest, WaitTLShutdown) {
    Session* session = tl()->createSession();
    Ticket ticket = Ticket(tl(), stdx::make_unique<TransportLayerMock::TicketMock>(session));

    tl()->shutdown();

    Status status = tl()->wait(std::move(ticket));
    ASSERT_EQUALS(status.code(), ErrorCodes::ShutdownInProgress);
}

// end() closes the session
TEST_F(TransportLayerMockTest, EndSession) {
    Session* session = tl()->createSession();
    tl()->end(*session);
    ASSERT(session->ended());
}

std::vector<Session*> createSessions(TransportLayerMock* tl) {
    int numSessions = 10;
    std::vector<Session*> sessions;
    for (int i = 0; i < numSessions; i++) {
        Session* session = tl->createSession();
        sessions.push_back(session);
    }
    return sessions;
}

void assertEnded(std::vector<Session*> sessions) {
    for (auto session : sessions) {
        ASSERT(session->ended());
    }
}

// endAllSessions() ends all sessions
TEST_F(TransportLayerMockTest, EndAllSessions) {
    std::vector<Session*> sessions = createSessions(tl());
    tl()->endAllSessions();
    assertEnded(sessions);
}

// shutdown() ends all sessions and shuts down
TEST_F(TransportLayerMockTest, Shutdown) {
    std::vector<Session*> sessions = createSessions(tl());
    tl()->shutdown();
    assertEnded(sessions);
    ASSERT(tl()->inShutdown());
}

}  // namespace transport
}  // namespace mongo
