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
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_task_names.h"
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
inline std::string stateToString(ServiceStateMachine::State state) {
    std::string ret = str::stream() << state;
    return ret;
}

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

    void endAllSessions(transport::Session::TagMask tags) override {}

    Status start() override {
        return Status::OK();
    }

    bool shutdown(Milliseconds timeout) override {
        return true;
    }

    void appendStats(BSONObjBuilder*) const override {}

    size_t numOpenSessions() const override {
        return 0ULL;
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

        OpMsgBuilder builder;
        builder.setBody(BSON("ping" << 1));
        *message = builder.finish();

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

        log() << "In wait. ssm state: " << stateToString(_ssm->state());
        if (_waitHook)
            _waitHook();
        return TransportLayerMock::wait(std::move(ticket));
    }

    void asyncWait(Ticket&& ticket, TicketCallback callback) override {
        MONGO_UNREACHABLE;
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

    void setWaitHook(stdx::function<void()> hook) {
        _waitHook = std::move(hook);
    }

private:
    bool _lastTicketSource = true;
    bool _ranSink = false;
    bool _ranSource = false;
    FailureMode _nextShouldFail = Nothing;
    Message _lastSunk;
    ServiceStateMachine* _ssm;
    stdx::function<void()> _waitHook;
};

Message buildRequest(BSONObj input) {
    OpMsgBuilder builder;
    builder.setBody(input);
    return builder.finish();
}

class MockServiceExecutor : public ServiceExecutor {
public:
    explicit MockServiceExecutor(ServiceContext* ctx) {}

    using ScheduleHook = stdx::function<bool(Task)>;

    Status start() override {
        return Status::OK();
    }
    Status shutdown(Milliseconds timeout) override {
        return Status::OK();
    }
    Status schedule(Task task, ScheduleFlags flags, ServiceExecutorTaskName taskName) override {
        if (!_scheduleHook) {
            return Status::OK();
        } else {
            return _scheduleHook(std::move(task)) ? Status::OK() : Status{ErrorCodes::InternalError,
                                                                          "Hook returned error!"};
        }
    }

    Mode transportMode() const override {
        return Mode::kSynchronous;
    }

    void appendStats(BSONObjBuilder* bob) const override {}

    void setScheduleHook(ScheduleHook hook) {
        _scheduleHook = std::move(hook);
    }

private:
    ScheduleHook _scheduleHook;
};

class SimpleEvent {
public:
    void signal() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _signaled = true;
        _cond.notify_one();
    }

    void wait() {
        stdx::unique_lock<stdx::mutex> lk(_mutex);
        _cond.wait(lk, [this] { return _signaled; });
        _signaled = false;
    }

private:
    stdx::mutex _mutex;
    stdx::condition_variable _cond;
    bool _signaled = false;
};

using State = ServiceStateMachine::State;

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

        auto se = stdx::make_unique<MockServiceExecutor>(sc);
        _sexec = se.get();
        sc->setServiceExecutor(std::move(se));

        auto tl = stdx::make_unique<MockTL>();
        _tl = tl.get();
        sc->setTransportLayer(std::move(tl));
        _tl->start().transitional_ignore();

        _ssm = ServiceStateMachine::create(
            getGlobalServiceContext(), _tl->createSession(), transport::Mode::kSynchronous);
        _tl->setSSM(_ssm.get());
    }

    void tearDown() override {
        _tl->shutdown();
    }

    void runPingTest(State first, State second);
    void checkPingOk();

    MockTL* _tl;
    MockSEP* _sep;
    MockServiceExecutor* _sexec;
    SessionHandle _session;
    std::shared_ptr<ServiceStateMachine> _ssm;
    bool _ranHandler;
};

void ServiceStateMachineFixture::runPingTest(State first, State second) {
    ASSERT_FALSE(haveClient());
    ASSERT_EQ(_ssm->state(), State::Created);
    log() << "run next";
    _ssm->runNext();

    ASSERT_EQ(_ssm->state(), first);
    if (first == State::Ended)
        return;

    _ssm->runNext();
    ASSERT_FALSE(haveClient());

    ASSERT_EQ(_ssm->state(), second);
}

void ServiceStateMachineFixture::checkPingOk() {
    auto msg = _tl->getLastSunk();
    auto reply = OpMsg::parse(msg);
    ASSERT_BSONOBJ_EQ(reply.body, BSON("ok" << 1));
}

TEST_F(ServiceStateMachineFixture, TestOkaySimpleCommand) {
    runPingTest(State::Process, State::Source);
    checkPingOk();
}

TEST_F(ServiceStateMachineFixture, TestThrowHandling) {
    _sep->setUassertInHandler();

    runPingTest(State::Process, State::Ended);
    ASSERT(_tl->getLastSunk().empty());
    ASSERT_TRUE(_tl->ranSource());
    ASSERT_FALSE(_tl->ranSink());
}

TEST_F(ServiceStateMachineFixture, TestSourceError) {
    _tl->setNextFailure(MockTL::Source);


    runPingTest(State::Ended, State::Ended);
    ASSERT(_tl->getLastSunk().empty());
    ASSERT_TRUE(_tl->ranSource());
    ASSERT_FALSE(_tl->ranSink());
}

TEST_F(ServiceStateMachineFixture, TestSinkError) {
    _tl->setNextFailure(MockTL::Sink);

    runPingTest(State::Process, State::Ended);
    ASSERT_TRUE(_tl->ranSource());
    ASSERT_TRUE(_tl->ranSink());
}

// This test checks that after the SSM has been cleaned up, the SessionHandle that it passed
// into the Client doesn't have any dangling shared_ptr copies.
TEST_F(ServiceStateMachineFixture, TestSessionCleanupOnDestroy) {
    // Set a cleanup hook so we know that the cleanup hook actually gets run when the session
    // is destroyed
    bool hookRan = false;
    _ssm->setCleanupHook([&hookRan] { hookRan = true; });

    // Do a regular ping test so that all the processMessage/sinkMessage code gets exercised
    runPingTest(State::Process, State::Source);

    // Set the next run up to fail on source (like a disconnected client) and run it
    _tl->setNextFailure(MockTL::Source);
    _ssm->runNext();
    ASSERT_EQ(State::Ended, _ssm->state());

    // Check that after the failure and the session getting cleaned up that the SessionHandle
    // only has one use (our copy in _sessionHandle)
    ASSERT_EQ(_ssm.use_count(), 1);

    // Make sure the cleanup hook actually ran.
    ASSERT_TRUE(hookRan);
}

// This tests that SSMs that fail to schedule their first task get cleaned up correctly.
// (i.e. we couldn't create a worker thread after accept()).
TEST_F(ServiceStateMachineFixture, ScheduleFailureDuringCreateCleanup) {
    _sexec->setScheduleHook([](auto) { return false; });
    // Set a cleanup hook so we know that the cleanup hook actually gets run when the session
    // is destroyed
    bool hookRan = false;
    _ssm->setCleanupHook([&hookRan] { hookRan = true; });

    _ssm->start(ServiceStateMachine::Ownership::kOwned);
    ASSERT_EQ(State::Ended, _ssm->state());
    ASSERT_EQ(_ssm.use_count(), 1);
    ASSERT_TRUE(hookRan);
}

// This tests that calling terminate() actually ends and cleans up the SSM during all the
// states.
TEST_F(ServiceStateMachineFixture, TerminateWorksForAllStates) {
    SimpleEvent hookRan, okayToContinue;

    auto cleanupHook = [&hookRan] {
        log() << "Cleaning up session";
        hookRan.signal();
    };

    // This is a shared hook between the executor/TL that lets us notify the test that the SSM
    // has reached a certain state and then gets terminated during that state.
    State waitFor = State::Created;
    SimpleEvent atDesiredState;
    auto waitForHook = [this, &waitFor, &atDesiredState, &okayToContinue]() {
        log() << "Checking for wakeup at " << stateToString(_ssm->state()) << ". Expecting "
              << stateToString(waitFor);
        if (_ssm->state() == waitFor) {
            atDesiredState.signal();
            okayToContinue.wait();
        }
    };

    // This wraps the waitForHook so that schedules always succeed.
    _sexec->setScheduleHook([waitForHook](auto) {
        waitForHook();
        return true;
    });

    // This just lets us intercept calls to _tl->wait() and terminate during them.
    _tl->setWaitHook(waitForHook);

    // Run this same test for each state.
    auto states = {State::Source, State::SourceWait, State::Process, State::SinkWait};
    for (const auto testState : states) {
        log() << "Testing termination during " << stateToString(testState);

        // Reset the _ssm to a fresh SSM and reset our tracking variables.
        _ssm = ServiceStateMachine::create(
            getGlobalServiceContext(), _tl->createSession(), transport::Mode::kSynchronous);
        _tl->setSSM(_ssm.get());
        _ssm->setCleanupHook(cleanupHook);

        waitFor = testState;
        // This is a dummy thread that just advances the SSM while we track its state/kill it
        stdx::thread runner([ssm = _ssm] {
            while (ssm->state() != State::Ended) {
                ssm->runNext();
            }
        });

        // Wait for the SSM to advance to the expected state
        atDesiredState.wait();
        log() << "Terminating session at " << stateToString(_ssm->state());

        // Terminate the SSM
        _ssm->terminate();

        // Notify the waitForHook to continue and end the session
        okayToContinue.signal();

        // Wait for the SSM to terminate and the thread to end.
        hookRan.wait();
        runner.join();

        // Verify that the SSM terminated and is in the correct state
        ASSERT_EQ(State::Ended, _ssm->state());
        ASSERT_EQ(_ssm.use_count(), 1);
    }
}

// This tests that calling terminate() actually ends and cleans up the SSM during all states, and
// with schedule() returning an error for each state.
TEST_F(ServiceStateMachineFixture, TerminateWorksForAllStatesWithScheduleFailure) {
    // Set a cleanup hook so we know that the cleanup hook actually gets run when the session
    // is destroyed
    SimpleEvent hookRan, okayToContinue;
    bool scheduleFailed = false;

    auto cleanupHook = [&hookRan] {
        log() << "Cleaning up session";
        hookRan.signal();
    };

    // This is a shared hook between the executor/TL that lets us notify the test that the SSM
    // has reached a certain state and then gets terminated during that state.
    State waitFor = State::Created;
    SimpleEvent atDesiredState;
    auto waitForHook = [this, &waitFor, &scheduleFailed, &okayToContinue, &atDesiredState]() {
        log() << "Checking for wakeup at " << stateToString(_ssm->state()) << ". Expecting "
              << stateToString(waitFor);
        if (_ssm->state() == waitFor) {
            atDesiredState.signal();
            okayToContinue.wait();
            scheduleFailed = true;
            return false;
        }
        return true;
    };

    _sexec->setScheduleHook([waitForHook](auto) { return waitForHook(); });
    // This wraps the waitForHook and discards its return status.
    _tl->setWaitHook([waitForHook] { waitForHook(); });

    auto states = {State::Source, State::SourceWait, State::Process, State::SinkWait};
    for (const auto testState : states) {
        log() << "Testing termination during " << stateToString(testState);
        _ssm = ServiceStateMachine::create(
            getGlobalServiceContext(), _tl->createSession(), transport::Mode::kSynchronous);
        _tl->setSSM(_ssm.get());
        scheduleFailed = false;
        _ssm->setCleanupHook(cleanupHook);

        waitFor = testState;
        // This is a dummy thread that just advances the SSM while we track its state/kill it
        stdx::thread runner([ ssm = _ssm, &scheduleFailed ] {
            while (ssm->state() != State::Ended && !scheduleFailed) {
                ssm->runNext();
            }
        });

        // Wait for the SSM to advance to the expected state
        atDesiredState.wait();
        ASSERT_EQ(_ssm->state(), testState);
        log() << "Terminating session at " << stateToString(_ssm->state());

        // Terminate the SSM
        _ssm->terminate();

        // Notify the waitForHook to continue and end the session
        okayToContinue.signal();
        hookRan.wait();
        runner.join();

        // Verify that the SSM terminated and is in the correct state
        ASSERT_EQ(State::Ended, _ssm->state());
        ASSERT_EQ(_ssm.use_count(), 1);
    }
}

// This makes sure that the SSM can run recursively by forcing the ServiceExecutor to run everything
// recursively
TEST_F(ServiceStateMachineFixture, SSMRunsRecursively) {
    // This lets us force the SSM to only run once. After sinking the first response, the next call
    // to sourceMessage will return with an error.
    _tl->setWaitHook([this] {
        if (_ssm->state() == State::SinkWait) {
            _tl->setNextFailure();
        }
    });

    // The scheduleHook just runs the task, effectively making this a recursive executor.
    int recursionDepth = 0;
    _sexec->setScheduleHook([&recursionDepth](auto task) {
        log() << "running task in executor. depth: " << ++recursionDepth;
        task();
        return true;
    });

    _ssm->runNext();
    // Check that the SSM actually ran, is ended, and actually ran recursively
    ASSERT_EQ(recursionDepth, 2);
    ASSERT_TRUE(_tl->ranSource());
    ASSERT_TRUE(_tl->ranSink());
    ASSERT_EQ(_ssm->state(), State::Ended);
}

}  // namespace
}  // namespace mongo
