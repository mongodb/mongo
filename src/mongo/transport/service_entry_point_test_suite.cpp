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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/transport/service_entry_point_test_suite.h"

#include <boost/optional.hpp>
#include <unordered_map>
#include <unordered_set>

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/functional.h"
#include "mongo/stdx/future.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/session.h"
#include "mongo/transport/ticket.h"
#include "mongo/transport/ticket_impl.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message.h"

namespace mongo {

using namespace transport;
using namespace stdx::placeholders;

using TicketCallback = TransportLayer::TicketCallback;

namespace {

// Helper function to populate a message with { ping : 1 } command
void setPingCommand(Message* m) {
    BufBuilder b{};

    // Leave room for the message header.
    b.skip(mongo::MsgData::MsgDataHeaderSize);

    b.appendStr("admin");
    b.appendStr("ping");

    auto commandObj = BSON("ping" << 1);
    commandObj.appendSelfToBufBuilder(b);

    auto metadata = BSONObj();
    metadata.appendSelfToBufBuilder(b);

    // Set Message header fields.
    MsgData::View msg = b.buf();
    msg.setLen(b.len());
    msg.setOperation(dbCommand);

    m->reset();

    // Transfer buffer ownership to the Message.
    m->setData(b.release());
}

// Some default method implementations
const auto kDefaultEnd = [](const Session& session) { return; };
const auto kDefaultAsyncWait = [](Ticket, TicketCallback cb) { cb(Status::OK()); };
const auto kNoopFunction = [] { return; };

// "End connection" error status
const auto kEndConnectionStatus = Status(ErrorCodes::HostUnreachable, "connection closed");

}  // namespace

ServiceEntryPointTestSuite::MockTicket::MockTicket(const Session& session,
                                                   Message* message,
                                                   Date_t expiration)
    : _message(message), _sessionId(session.id()), _expiration(expiration) {}

ServiceEntryPointTestSuite::MockTicket::MockTicket(const Session& session, Date_t expiration)
    : _sessionId(session.id()), _expiration(expiration) {}

Session::Id ServiceEntryPointTestSuite::MockTicket::sessionId() const {
    return _sessionId;
}

Date_t ServiceEntryPointTestSuite::MockTicket::expiration() const {
    return _expiration;
}

boost::optional<Message*> ServiceEntryPointTestSuite::MockTicket::message() {
    return _message;
}

ServiceEntryPointTestSuite::MockTLHarness::MockTLHarness()
    : _sourceMessage(
          stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_defaultSource, this, _1, _2, _3)),
      _sinkMessage(
          stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_defaultSink, this, _1, _2, _3)),
      _wait(stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_defaultWait, this, _1)),
      _asyncWait(kDefaultAsyncWait),
      _end(kDefaultEnd) {}

Ticket ServiceEntryPointTestSuite::MockTLHarness::sourceMessage(const Session& session,
                                                                Message* message,
                                                                Date_t expiration) {
    return _sourceMessage(session, message, expiration);
}

Ticket ServiceEntryPointTestSuite::MockTLHarness::sinkMessage(const Session& session,
                                                              const Message& message,
                                                              Date_t expiration) {
    return _sinkMessage(session, message, expiration);
}

Status ServiceEntryPointTestSuite::MockTLHarness::wait(Ticket&& ticket) {
    return _wait(std::move(ticket));
}

void ServiceEntryPointTestSuite::MockTLHarness::asyncWait(Ticket&& ticket,
                                                          TicketCallback callback) {
    return _asyncWait(std::move(ticket), std::move(callback));
}

std::string ServiceEntryPointTestSuite::MockTLHarness::getX509SubjectName(const Session& session) {
    return "mock";
}

void ServiceEntryPointTestSuite::MockTLHarness::registerTags(const Session& session) {}

TransportLayer::Stats ServiceEntryPointTestSuite::MockTLHarness::sessionStats() {
    return Stats();
}

void ServiceEntryPointTestSuite::MockTLHarness::end(const Session& session) {
    return _end(session);
}

void ServiceEntryPointTestSuite::MockTLHarness::endAllSessions(Session::TagMask tags) {
    return _endAllSessions(tags);
}

Status ServiceEntryPointTestSuite::MockTLHarness::start() {
    return _start();
}

void ServiceEntryPointTestSuite::MockTLHarness::shutdown() {
    return _shutdown();
}

Status ServiceEntryPointTestSuite::MockTLHarness::_defaultWait(transport::Ticket ticket) {
    auto mockTicket = getMockTicket(ticket);
    if (mockTicket->message()) {
        setPingCommand(*(mockTicket->message()));
    }
    return Status::OK();
}

Status ServiceEntryPointTestSuite::MockTLHarness::_waitError(transport::Ticket ticket) {
    return kEndConnectionStatus;
}

Status ServiceEntryPointTestSuite::MockTLHarness::_waitOnceThenError(transport::Ticket ticket) {
    _wait = stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_waitError, this, _1);
    return _defaultWait(std::move(ticket));
}

Ticket ServiceEntryPointTestSuite::MockTLHarness::_defaultSource(const Session& s,
                                                                 Message* m,
                                                                 Date_t d) {
    return Ticket(this, stdx::make_unique<ServiceEntryPointTestSuite::MockTicket>(s, m, d));
}

Ticket ServiceEntryPointTestSuite::MockTLHarness::_defaultSink(const Session& s,
                                                               const Message&,
                                                               Date_t d) {
    return Ticket(this, stdx::make_unique<ServiceEntryPointTestSuite::MockTicket>(s, d));
}

Ticket ServiceEntryPointTestSuite::MockTLHarness::_sinkThenErrorOnWait(const Session& s,
                                                                       const Message& m,
                                                                       Date_t d) {
    _wait = stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_waitOnceThenError, this, _1);
    return _defaultSink(s, m, d);
}

void ServiceEntryPointTestSuite::MockTLHarness::_resetHooks() {
    _sourceMessage =
        stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_defaultSource, this, _1, _2, _3);
    _sinkMessage =
        stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_defaultSink, this, _1, _2, _3);
    _wait = stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_defaultWait, this, _1);
    _asyncWait = kDefaultAsyncWait;
    _end = kDefaultEnd;
}

ServiceEntryPointTestSuite::MockTicket* ServiceEntryPointTestSuite::MockTLHarness::getMockTicket(
    const transport::Ticket& ticket) {
    return dynamic_cast<ServiceEntryPointTestSuite::MockTicket*>(getTicketImpl(ticket));
}

void ServiceEntryPointTestSuite::setUp() {
    _tl = stdx::make_unique<MockTLHarness>();
}

void ServiceEntryPointTestSuite::setServiceEntryPoint(ServiceEntryPointFactory factory) {
    _sep = factory(_tl.get());
}

// Start a Session and error on get-Message
void ServiceEntryPointTestSuite::noLifeCycleTest() {
    stdx::promise<void> testComplete;
    auto testFuture = testComplete.get_future();

    _tl->_resetHooks();

    // Step 1: SEP gets a ticket to source a Message
    // Step 2: SEP calls wait() on the ticket and receives an error
    _tl->_wait = stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_waitError, _tl.get(), _1);

    // Step 3: SEP destroys the session, which calls end()
    _tl->_end = [&testComplete](const Session&) { testComplete.set_value(); };

    // Kick off the SEP
    Session s(HostAndPort(), HostAndPort(), _tl.get());
    _sep->startSession(std::move(s));

    testFuture.wait();
}

// Partial cycle: get-Message, handle-Message, error on send-Message
void ServiceEntryPointTestSuite::halfLifeCycleTest() {
    stdx::promise<void> testComplete;
    auto testFuture = testComplete.get_future();

    _tl->_resetHooks();

    // Step 1: SEP gets a ticket to source a Message
    // Step 2: SEP calls wait() on the ticket and receives a Message
    // Step 3: SEP gets a ticket to sink a Message
    _tl->_sinkMessage = [this](const Session& session, const Message& m, Date_t expiration) {

        // Step 4: SEP calls wait() on the ticket and receives an error
        _tl->_wait =
            stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_waitError, _tl.get(), _1);

        return _tl->_defaultSink(session, m, expiration);
    };

    // Step 5: SEP destroys the session, which calls end()
    _tl->_end = [&testComplete](const Session&) { testComplete.set_value(); };

    // Kick off the SEP
    Session s(HostAndPort(), HostAndPort(), _tl.get());
    _sep->startSession(std::move(s));

    testFuture.wait();
}

// Perform a full get-Message, handle-Message, send-Message cycle
void ServiceEntryPointTestSuite::fullLifeCycleTest() {
    stdx::promise<void> testComplete;
    auto testFuture = testComplete.get_future();

    _tl->_resetHooks();

    // Step 1: SEP gets a ticket to source a Message
    // Step 2: SEP calls wait() on the ticket and receives a Message
    _tl->_sinkMessage = stdx::bind(
        &ServiceEntryPointTestSuite::MockTLHarness::_sinkThenErrorOnWait, _tl.get(), _1, _2, _3);

    // Step 3: SEP gets a ticket to sink a Message
    // Step 4: SEP calls wait() on the ticket and receives Status::OK()
    // Step 5: SEP gets a ticket to source a Message
    // Step 6: SEP calls wait() on the ticket and receives and error
    // Step 7: SEP destroys the session, which calls end()
    _tl->_end = [&testComplete](const Session& session) { testComplete.set_value(); };

    // Kick off the SEP
    Session s(HostAndPort(), HostAndPort(), _tl.get());
    _sep->startSession(std::move(s));

    testFuture.wait();
}

void ServiceEntryPointTestSuite::interruptingSessionTest() {
    Session sA(HostAndPort(), HostAndPort(), _tl.get());
    Session sB(HostAndPort(), HostAndPort(), _tl.get());
    auto idA = sA.id();
    auto idB = sB.id();

    stdx::promise<void> startB;
    auto startBFuture = startB.get_future();

    stdx::promise<void> resumeA;
    auto resumeAFuture = resumeA.get_future();

    stdx::promise<void> testComplete;

    auto testFuture = testComplete.get_future();

    _tl->_resetHooks();

    // Start Session A
    // Step 1: SEP calls sourceMessage() for A
    // Step 2: SEP calls wait() for A and we block...
    // Start Session B
    _tl->_wait = [this, idA, &startB, &resumeAFuture](Ticket t) {
        // If we're handling B, just do a default wait
        if (t.sessionId() != idA) {
            return _tl->_defaultWait(std::move(t));
        }

        // Otherwise, we need to start B and block A
        startB.set_value();
        resumeAFuture.wait();

        return Status::OK();
    };

    // Step 3: SEP calls sourceMessage() for B, gets tB
    // Step 4: SEP calls wait() for tB, gets { ping : 1 }
    // Step 5: SEP calls sinkMessage() for B, gets tB2
    _tl->_sinkMessage = stdx::bind(
        &ServiceEntryPointTestSuite::MockTLHarness::_sinkThenErrorOnWait, _tl.get(), _1, _2, _3);

    // Step 6: SEP calls wait() for tB2, gets Status::OK()
    // Step 7: SEP calls sourceMessage() for B, gets tB3
    // Step 8: SEP calls wait() for tB3, gets an error
    // Step 9: SEP calls end(B)
    _tl->_end = [this, idA, idB, &resumeA, &testComplete](const Session& session) {

        // When end(B) is called, time to resume session A
        if (session.id() == idB) {
            _tl->_wait =
                stdx::bind(&ServiceEntryPointTestSuite::MockTLHarness::_defaultWait, _tl.get(), _1);

            // Resume session A
            resumeA.set_value();
        } else {
            // Else our test is over when end(A) is called
            invariant(session.id() == idA);
            testComplete.set_value();
        }
    };

    // Resume Session A
    // Step 10: SEP calls sinkMessage() for A, gets tA
    // Step 11: SEP calls wait() for tA, gets Status::OK()
    // Step 12: SEP calls sourceMessage() for A, get tA2
    // Step 13: SEP calls wait() for tA2, receives an error
    // Step 14: SEP calls end(A)

    // Kick off the test
    _sep->startSession(std::move(sA));

    startBFuture.wait();
    _sep->startSession(std::move(sB));

    testFuture.wait();
}

void ServiceEntryPointTestSuite::burstStressTest(int numSessions,
                                                 int numCycles,
                                                 Milliseconds delay) {
    AtomicWord<int> ended{0};
    stdx::promise<void> allSessionsComplete;

    auto allCompleteFuture = allSessionsComplete.get_future();

    stdx::mutex cyclesLock;
    std::unordered_map<Session::Id, int> completedCycles;

    _tl->_resetHooks();

    // Same wait() callback for all sessions.
    _tl->_wait = [this, &completedCycles, &cyclesLock, numSessions, numCycles, &delay](
        Ticket ticket) -> Status {
        auto id = ticket.sessionId();
        int cycleCount;

        {
            stdx::lock_guard<stdx::mutex> lock(cyclesLock);
            auto item = completedCycles.find(id);
            invariant(item != completedCycles.end());
            cycleCount = item->second;
        }

        auto mockTicket = _tl->getMockTicket(ticket);
        // If we are sourcing:
        if (mockTicket->message()) {
            // If we've completed enough cycles, done.
            if (cycleCount == numCycles) {
                return kEndConnectionStatus;
            }

            // Otherwise, source another { ping : 1 }
            invariant(mockTicket->message());
            setPingCommand(*(mockTicket->message()));

            // Wait a bit before returning
            sleepmillis(delay.count());

            return Status::OK();
        }

        // We are sinking, increment numCycles and return OK.
        {
            stdx::lock_guard<stdx::mutex> lock(cyclesLock);
            auto item = completedCycles.find(id);
            invariant(item != completedCycles.end());
            ++(item->second);
        }

        return Status::OK();
    };

    // When we end the last session, end the test.
    _tl->_end = [&allSessionsComplete, numSessions, &ended](const Session& session) {
        if (ended.fetchAndAdd(1) == (numSessions - 1)) {
            allSessionsComplete.set_value();
        }
    };

    for (int i = 0; i < numSessions; i++) {
        Session s(HostAndPort(), HostAndPort(), _tl.get());
        {
            // This operation may cause a re-hash.
            stdx::lock_guard<stdx::mutex> lock(cyclesLock);
            completedCycles.emplace(s.id(), 0);
        }
        _sep->startSession(std::move(s));
    }

    // Block and wait for all sessions to finish.
    allCompleteFuture.wait();
}

void ServiceEntryPointTestSuite::longSessionStressTest() {
    return burstStressTest(1000, 100, Milliseconds(100));
}

}  // namespace mongo
