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

#include "mongo/transport/session.h"
#include "mongo/transport/ticket.h"
#include "mongo/transport/ticket_impl.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/message.h"
#include "mongo/util/time_support.h"

namespace mongo {

class ServiceEntryPoint;

/**
 * Test class. Uses a mock TransportLayer to test that the ServiceEntryPoint
 * calls the expected methods on the TransportLayer in the expected order,
 * and with the expected parameters.
 *
 * Usage:
 *
 * TEST_F(ServiceEntryPointTestSuite, ServiceEntryPointImplTest) {
 *    // Set up our ServiceEntryPoint
 *    auto sepFactory = [](TransportLayer* tl){
 *       return stdx::make_unique<ServiceEntryPointImpl>(tl);
 *    };
 *
 *    setServiceEntryPoint(sepFactory);
 *
 *    // Run some tests
 *    fullLifeCycleTest();
 * }
 */
class ServiceEntryPointTestSuite : public mongo::unittest::Test {
public:
    // Need a function that takes a TransportLayer* and returns a new
    // ServiceEntryPoint
    using ServiceEntryPointFactory =
        stdx::function<std::unique_ptr<ServiceEntryPoint>(transport::TransportLayer*)>;

    void setUp() override;

    void setServiceEntryPoint(ServiceEntryPointFactory factory);

    // Lifecycle Tests
    void noLifeCycleTest();
    void halfLifeCycleTest();
    void fullLifeCycleTest();

    // Concurrent Session Tests
    void interruptingSessionTest();

    // Stress Tests
    void burstStressTest(int numSessions = 1000,
                         int numCycles = 1,
                         Milliseconds delay = Milliseconds(0));
    void longSessionStressTest();

private:
    /**
     * A mock ticket class for our test suite.
     */
    class MockTicket : public transport::TicketImpl {
    public:
        // Source constructor
        MockTicket(const transport::Session& session, Message* message, Date_t expiration);

        // Sink constructor
        MockTicket(const transport::Session& session, Date_t expiration);

        MockTicket(MockTicket&&) = default;
        MockTicket& operator=(MockTicket&&) = default;

        transport::Session::Id sessionId() const override;

        Date_t expiration() const override;

        boost::optional<Message*> message();

    private:
        boost::optional<Message*> _message;
        transport::Session::Id _sessionId;
        Date_t _expiration;
    };

    /**
     * This class mocks the TransportLayer and allows us to insert hooks beneath
     * its methods.
     */
    class MockTLHarness : public transport::TransportLayer {
    public:
        MockTLHarness();

        transport::Ticket sourceMessage(
            const transport::Session& session,
            Message* message,
            Date_t expiration = transport::Ticket::kNoExpirationDate) override;
        transport::Ticket sinkMessage(
            const transport::Session& session,
            const Message& message,
            Date_t expiration = transport::Ticket::kNoExpirationDate) override;
        Status wait(transport::Ticket&& ticket) override;
        void asyncWait(transport::Ticket&& ticket, TicketCallback callback) override;
        std::string getX509SubjectName(const transport::Session& session) override;
        void registerTags(const transport::Session& session) override;
        Stats sessionStats() override;
        void end(const transport::Session& session) override;
        void endAllSessions(
            transport::Session::TagMask tags = transport::Session::kEmptyTagMask) override;
        Status start() override;
        void shutdown() override;

        ServiceEntryPointTestSuite::MockTicket* getMockTicket(const transport::Ticket& ticket);

        // Mocked method hooks
        stdx::function<transport::Ticket(const transport::Session&, Message*, Date_t)>
            _sourceMessage;
        stdx::function<transport::Ticket(const transport::Session&, const Message&, Date_t)>
            _sinkMessage;
        stdx::function<Status(transport::Ticket)> _wait;
        stdx::function<void(transport::Ticket, TicketCallback)> _asyncWait;
        stdx::function<void(const transport::Session&)> _end;
        stdx::function<void(transport::Session::TagMask tags)> _endAllSessions =
            [](transport::Session::TagMask tags) {};
        stdx::function<Status(void)> _start = [] { return Status::OK(); };
        stdx::function<void(void)> _shutdown = [] {};

        // Pre-set hook methods
        transport::Ticket _defaultSource(const transport::Session& s, Message* m, Date_t d);
        transport::Ticket _defaultSink(const transport::Session& s, const Message&, Date_t d);
        transport::Ticket _sinkThenErrorOnWait(const transport::Session& s,
                                               const Message& m,
                                               Date_t d);

        Status _defaultWait(transport::Ticket ticket);
        Status _waitError(transport::Ticket ticket);
        Status _waitOnceThenError(transport::Ticket ticket);

        // Reset all hooks to their defaults
        void _resetHooks();
    };

    std::unique_ptr<MockTLHarness> _tl;
    std::unique_ptr<ServiceEntryPoint> _sep;
};

}  // namespace mongo
