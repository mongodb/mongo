/**
 *    Copyright (C) 2017 MongoDB Inc.
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

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/stdx/memory.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/mock_ticket.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_state_machine.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/log.h"
#include "mongo/util/net/op_msg.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace {
class MockSEP : public ServiceEntryPoint {
public:
    virtual ~MockSEP() = default;

    void startSession(transport::SessionHandle session) override {}

    DbResponse handleRequest(OperationContext* opCtx, const Message& request) override {
        log() << "In handleRequest";
        _ranHandler = true;
        ASSERT_TRUE(haveClient());

        auto req = OpMsgRequest::parse(request);
        ASSERT_BSONOBJ_EQ(BSON("ping" << 1), req.body);

        // Build out a dummy reply
        OpMsgBuilder builder;
        builder.setBody(BSON("ok" << 1));

        if (_uassertInHandler)
            uassert(40469, "Synthetic uassert failure", false);

        return DbResponse{builder.finish()};
    }

    void setUassertInHandler() {
        _uassertInHandler = true;
    }

    bool ranHandler() {
        bool ret = _ranHandler;
        _ranHandler = false;
        return ret;
    }

private:
    bool _uassertInHandler = false;
    bool _ranHandler = false;
};

using namespace transport;
class MockTL : public TransportLayerMock {
public:
    ~MockTL() = default;

    Ticket sourceMessage(const SessionHandle& session,
                         Message* message,
                         Date_t expiration = Ticket::kNoExpirationDate) override {
        ASSERT_EQ(_ssm->state(), ServiceStateMachine::State::Source);
        _lastTicketSource = true;

        _ranSource = true;
        log() << "In sourceMessage";

        if (_nextShouldFail & Source) {
            return TransportLayer::TicketSessionClosedStatus;
        }

        if (_nextMessage) {
            *message = *_nextMessage;
        }

        return TransportLayerMock::sourceMessage(session, message, expiration);
    }

    Ticket sinkMessage(const SessionHandle& session,
                       const Message& message,
                       Date_t expiration = Ticket::kNoExpirationDate) override {
        ASSERT_EQ(_ssm->state(), ServiceStateMachine::State::Process);
        _lastTicketSource = false;

        log() << "In sinkMessage";
        _ranSink = true;

        if (_nextShouldFail & Sink) {
            return TransportLayer::TicketSessionClosedStatus;
        }

        _lastSunk = message;

        return TransportLayerMock::sinkMessage(session, message, expiration);
    }

    Status wait(Ticket&& ticket) override {
        if (!ticket.valid()) {
            return ticket.status();
        }
        ASSERT_EQ(_ssm->state(),
                  _lastTicketSource ? ServiceStateMachine::State::SourceWait
                                    : ServiceStateMachine::State::SinkWait);
        std::stringstream ss;
        ss << _ssm->state();
        log() << "In wait. ssm state: " << ss.str();
        return TransportLayerMock::wait(std::move(ticket));
    }

    void asyncWait(Ticket&& ticket, TicketCallback callback) override {
        MONGO_UNREACHABLE;
    }

    void setNextMessage(Message&& message) {
        _nextMessage = std::move(message);
    }

    void setSSM(ServiceStateMachine* ssm) {
        _ssm = ssm;
    }

    enum FailureMode { Nothing = 0, Source = 0x1, Sink = 0x10 };

    void setNextFailure(FailureMode mode = Source) {
        _nextShouldFail = mode;
    }

    Message&& getLastSunk() {
        return std::move(_lastSunk);
    }

    bool ranSink() const {
        return _ranSink;
    }

    bool ranSource() const {
        return _ranSource;
    }

private:
    bool _lastTicketSource = true;
    bool _ranSink = false;
    bool _ranSource = false;
    boost::optional<Message> _nextMessage;
    FailureMode _nextShouldFail = Nothing;
    Message _lastSunk;
    ServiceStateMachine* _ssm;
};

Message buildRequest(BSONObj input) {
    OpMsgBuilder builder;
    builder.setBody(input);
    return builder.finish();
}

}  // namespace

class ServiceStateMachineFixture : public unittest::Test {
protected:
    void setUp() override {

        auto scOwned = stdx::make_unique<ServiceContextNoop>();
        auto sc = scOwned.get();
        setGlobalServiceContext(std::move(scOwned));

        sc->setTickSource(stdx::make_unique<TickSourceMock>());
        sc->setFastClockSource(stdx::make_unique<ClockSourceMock>());

        auto sep = stdx::make_unique<MockSEP>();
        _sep = sep.get();
        sc->setServiceEntryPoint(std::move(sep));

        auto tl = stdx::make_unique<MockTL>();
        _tl = tl.get();
        sc->setTransportLayer(std::move(tl));
        _tl->start().transitional_ignore();

        _ssm = stdx::make_unique<ServiceStateMachine>(
            getGlobalServiceContext(), _tl->createSession(), true);
        _tl->setSSM(_ssm.get());
    }

    void tearDown() override {
        getGlobalServiceContext()->getTransportLayer()->shutdown();
    }

    ServiceStateMachine::State runPingTest();
    void checkPingOk();

    MockTL* _tl;
    MockSEP* _sep;
    SessionHandle _session;
    std::unique_ptr<ServiceStateMachine> _ssm;
    bool _ranHandler;
};

ServiceStateMachine::State ServiceStateMachineFixture::runPingTest() {
    _tl->setNextMessage(buildRequest(BSON("ping" << 1)));

    ASSERT_FALSE(haveClient());
    ASSERT_EQ(_ssm->state(), ServiceStateMachine::State::Source);
    log() << "run next";
    _ssm->runNext();
    auto ret = _ssm->state();
    ASSERT_FALSE(haveClient());

    return ret;
}

void ServiceStateMachineFixture::checkPingOk() {
    auto msg = _tl->getLastSunk();
    auto reply = OpMsg::parse(msg);
    ASSERT_BSONOBJ_EQ(reply.body, BSON("ok" << 1));
}

TEST_F(ServiceStateMachineFixture, TestOkaySimpleCommand) {
    ASSERT_EQ(ServiceStateMachine::State::Source, runPingTest());
    checkPingOk();
}

TEST_F(ServiceStateMachineFixture, TestThrowHandling) {
    _sep->setUassertInHandler();

    ASSERT_EQ(ServiceStateMachine::State::Ended, runPingTest());
    ASSERT_THROWS(checkPingOk(), MsgAssertionException);
    ASSERT_TRUE(_tl->ranSource());
    ASSERT_FALSE(_tl->ranSink());
}

TEST_F(ServiceStateMachineFixture, TestSourceError) {
    _tl->setNextFailure(MockTL::Source);

    ASSERT_EQ(ServiceStateMachine::State::Ended, runPingTest());
    ASSERT_THROWS(checkPingOk(), MsgAssertionException);
    ASSERT_TRUE(_tl->ranSource());
    ASSERT_FALSE(_tl->ranSink());
}

TEST_F(ServiceStateMachineFixture, TestSinkError) {
    _tl->setNextFailure(MockTL::Sink);

    ASSERT_EQ(ServiceStateMachine::State::Ended, runPingTest());
    ASSERT_TRUE(_tl->ranSource());
    ASSERT_TRUE(_tl->ranSink());
}

}  // namespace mongo
