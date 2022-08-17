/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */


#include "mongo/platform/basic.h"

#include <array>
#include <deque>
#include <initializer_list>
#include <memory>
#include <type_traits>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/locker_noop_service_context_test_fixture.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/variant.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_entry_point_impl.h"
#include "mongo/transport/session_workflow.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo::transport {
namespace {

/** Scope guard to set and restore an object value. */
template <typename T>
class ScopedValueOverride {
public:
    ScopedValueOverride(T& target, T v)
        : _target{target}, _saved{std::exchange(_target, std::move(v))} {}
    ~ScopedValueOverride() {
        _target = std::move(_saved);
    }

private:
    T& _target;
    T _saved;
};

template <typename T>
StringData findEnumName(const std::vector<std::pair<T, StringData>>& arr, T k) {
    return std::find_if(arr.begin(), arr.end(), [&](auto&& e) { return e.first == k; })->second;
}

template <typename T>
static std::string typeName() {
    return demangleName(typeid(T));
}
template <typename T>
static std::ostream& stream(std::ostream& os, const T&) {
    return os << "[{}]"_format(typeName<T>());
}
static std::ostream& stream(std::ostream& os, const Status& v) {
    return os << v;
}
template <typename T>
static std::ostream& stream(std::ostream& os, const StatusWith<T>& v) {
    if (!v.isOK())
        return stream(os, v.getStatus());
    return stream(os, v.getValue());
}

class CallbackMockSession : public MockSessionBase {
public:
    TransportLayer* getTransportLayer() const override {
        return getTransportLayerCb();
    }

    void end() override {
        endCb();
    }

    bool isConnected() override {
        return isConnectedCb();
    }

    Status waitForData() noexcept override {
        return waitForDataCb();
    }

    StatusWith<Message> sourceMessage() noexcept override {
        return sourceMessageCb();
    }

    Status sinkMessage(Message message) noexcept override {
        return sinkMessageCb(message);
    }

    Future<void> asyncWaitForData() noexcept override {
        return asyncWaitForDataCb();
    }

    Future<Message> asyncSourceMessage(const BatonHandle& handle) noexcept override {
        return asyncSourceMessageCb(handle);
    }

    Future<void> asyncSinkMessage(Message message, const BatonHandle& handle) noexcept override {
        return asyncSinkMessageCb(message, handle);
    }

    std::function<TransportLayer*()> getTransportLayerCb;
    std::function<void()> endCb;
    std::function<bool()> isConnectedCb;
    std::function<Status()> waitForDataCb;
    std::function<StatusWith<Message>()> sourceMessageCb;
    std::function<Status(Message)> sinkMessageCb;
    std::function<Future<void>()> asyncWaitForDataCb;
    std::function<Future<Message>(const BatonHandle&)> asyncSourceMessageCb;
    std::function<Future<void>(Message, const BatonHandle&)> asyncSinkMessageCb;
};

class MockServiceEntryPoint : public ServiceEntryPointImpl {
public:
    explicit MockServiceEntryPoint(ServiceContext* svcCtx) : ServiceEntryPointImpl(svcCtx) {}

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request) noexcept override {
        return handleRequestCb(opCtx, request);
    }

    void onEndSession(const SessionHandle& handle) override {
        onEndSessionCb(handle);
    }

    void derivedOnClientDisconnect(Client* client) override {
        derivedOnClientDisconnectCb(client);
    }

    std::function<Future<DbResponse>(OperationContext*, const Message&)> handleRequestCb;
    std::function<void(const SessionHandle)> onEndSessionCb;
    std::function<void(Client*)> derivedOnClientDisconnectCb;
};

/**
 * Events generated by SessionWorkflow, mostly by virtual function calls to mock
 * objects connected to SessionWorkflow.
 */
enum class Event {
    kStart,
    kWaitForData,
    kSource,
    kProcess,
    kSink,
    kEnd,
};

StringData toString(Event e) {
    return findEnumName(
        {
            {Event::kStart, "Start"_sd},
            {Event::kWaitForData, "WaitForData"_sd},
            {Event::kSource, "Source"_sd},
            {Event::kProcess, "Process"_sd},
            {Event::kSink, "Sink"_sd},
            {Event::kEnd, "End"_sd},
        },
        e);
}

std::ostream& operator<<(std::ostream& os, Event e) {
    return os << toString(e);
}

class Result {
    using Variant =
        stdx::variant<stdx::monostate, Status, StatusWith<Message>, StatusWith<DbResponse>>;

public:
    Result() = default;

    template <typename T, std::enable_if_t<std::is_constructible_v<Variant, T&&>, int> = 0>
    explicit Result(T&& v) : _value{std::forward<T>(v)} {}

    template <typename T>
    T consumeAs() && {
        return stdx::visit(
            [](auto&& alt) -> T {
                using A = decltype(alt);
                if constexpr (std::is_convertible<A, T>())
                    return std::forward<A>(alt);
                invariant(0, "{} => {}"_format(typeName<A>(), typeName<T>()));
                MONGO_UNREACHABLE;
            },
            std::exchange(_value, {}));
    }

    explicit operator bool() const {
        return _value.index() != 0;
    }

private:
    friend std::string toString(const Result& r) {
        return stdx::visit(
            [](auto&& alt) -> std::string {
                using A = std::decay_t<decltype(alt)>;
                std::ostringstream os;
                stream(os << "[{}]"_format(typeName<A>()), alt);
                return os.str();
            },
            r._value);
    }

    Variant _value;
};

class SessionEventTracker {
public:
    /**
     * Prepare a response for the `event`. Called by the test to inject
     * a behavior for the next mock object that calls `consumeExpectation` with
     * the same `event`. Only one response can be prepared at a time.
     */
    void prepareResponse(Event event, Result v) {
        switch (event) {
            case Event::kWaitForData:
            case Event::kSource:
            case Event::kProcess:
            case Event::kSink: {
                stdx::lock_guard lk{_mutex};
                invariant(!_expect);
                _expect = std::unique_ptr<EventAndResult>{new EventAndResult{event, std::move(v)}};
                LOGV2(6742612,
                      "SessionEventTracker::set",
                      "event"_attr = toString(_expect->event),
                      "value"_attr = toString(_expect->result));
                _cv.notify_one();
                return;
            }
            default:
                invariant(0, "SessionEventTracker::set for bad event={}"_format(toString(event)));
                MONGO_UNREACHABLE;
        }
    }

    void endSession() {
        if (setConnected(false))
            LOGV2(5014101, "Ending session");
    }

    bool isConnected() const {
        stdx::unique_lock lk{_mutex};
        return _isConnected;
    }

    bool setConnected(bool b) {
        stdx::unique_lock lk{_mutex};
        bool old = _isConnected;
        if (old != b) {
            _isConnected = b;
            _cv.notify_all();
        }
        return old;
    }

    /**
     * Called by mock objects to inject behavior into them. The mock function
     * call is identified by an `event`. Waits if necessary until a response
     * has been prepared for that event.
     */
    Result consumeExpectation(Event event) {
        stdx::unique_lock lk{_mutex};
        _cv.wait(lk, [&] { return (_expect && _expect->event == event) || !_isConnected; });
        if (!(_expect && _expect->event == event))
            return Result(Status{ErrorCodes::SocketException, "Session is closed"});
        invariant(_expect);
        invariant(_expect->event == event);
        invariant(_expect->result);
        return std::exchange(_expect, {})->result;
    }

private:
    struct EventAndResult {
        Event event;
        Result result;
    };

    mutable Mutex _mutex;
    stdx::condition_variable _cv;
    bool _isConnected = true;
    std::unique_ptr<EventAndResult> _expect;
};

/** Fixture to mock interactions with the SessionWorkflow. */
class SessionWorkflowTest : public LockerNoopServiceContextTest {
public:
    void setUp() override {
        ServiceContextTest::setUp();
        auto sc = getServiceContext();
        sc->setServiceEntryPoint(_makeServiceEntryPoint(sc));
        _makeSession();
        invariant(sep()->start());
        _threadPool->startup();
    }

    void tearDown() override {
        ScopeGuard guard = [&] { ServiceContextTest::tearDown(); };
        endSession();
        // Normal shutdown is a noop outside of ASAN.
        invariant(sep()->shutdownAndWait(Seconds{10}));
        _threadPool->shutdown();
        _threadPool->join();
    }

    /** Blocks until the SessionWorkflowTest observes an event. */
    Event awaitEvent() {
        return _eventSlot.wait();
    }

    /** Stores an event response to be consumed by a mock. */
    void prepareResponse(Event event, Result result) {
        _sessionEventTracker.prepareResponse(event, std::move(result));
    }

    /** Waits for the current Session and SessionWorkflow to end. */
    void joinSession() {
        ASSERT(sep()->waitForNoSessions(Seconds{1}));
        ASSERT_FALSE(_eventSlot) << "An unconsumed expectation is an error in the test";
    }

    /** Launches a SessionWorkflow for the current session. */
    void startSession() {
        LOGV2(6742613, "Starting session");
        _sessionEventTracker.setConnected(true);
        sep()->startSession(_session);
    }

    void endSession() {
        _sessionEventTracker.endSession();
    }

    void terminateViaServiceEntryPoint() {
        sep()->endAllSessionsNoTagMask();
    }

    MockServiceEntryPoint* sep() {
        return checked_cast<MockServiceEntryPoint*>(getServiceContext()->getServiceEntryPoint());
    }

private:
    std::shared_ptr<ThreadPool> _makeThreadPool() {
        ThreadPool::Options options{};
        options.poolName = "SessionWorkflowTest";
        return std::make_shared<ThreadPool>(std::move(options));
    }

    std::unique_ptr<MockServiceEntryPoint> _makeServiceEntryPoint(ServiceContext* sc) {
        auto sep = std::make_unique<MockServiceEntryPoint>(sc);
        sep->handleRequestCb = [=](OperationContext*, const Message&) {
            return _onMockEvent<StatusWith<DbResponse>>(Event::kProcess);
        };
        sep->onEndSessionCb = [=](const SessionHandle&) { _onMockEvent<void>(Event::kEnd); };
        sep->derivedOnClientDisconnectCb = [&](Client*) {};
        return sep;
    }

    /** Create and initialize a mock Session. */
    void _makeSession() {
        auto s = std::make_shared<CallbackMockSession>();
        s->endCb = [=] { endSession(); };
        s->isConnectedCb = [=] { return _isConnected(); };
        s->waitForDataCb = [=] { return _onMockEvent<Status>(Event::kWaitForData); };
        s->sourceMessageCb = [=] { return _onMockEvent<StatusWith<Message>>(Event::kSource); };
        s->sinkMessageCb = [=](Message) { return _onMockEvent<Status>(Event::kSink); };
        // The async variants will just run the same callback on `_threadPool`.
        auto async = [this](auto cb) {
            return ExecutorFuture<void>(_threadPool).then(cb).unsafeToInlineFuture();
        };
        s->asyncWaitForDataCb = [=, cb = s->waitForDataCb] { return async([cb] { return cb(); }); };
        s->asyncSourceMessageCb = [=, cb = s->sourceMessageCb](const BatonHandle&) {
            return async([cb] { return cb(); });
        };
        s->asyncSinkMessageCb = [=, cb = s->sinkMessageCb](Message m, const BatonHandle&) {
            return async([cb, m = std::move(m)]() mutable { return cb(std::move(m)); });
        };
        _session = std::move(s);
    }

    bool _isConnected() const {
        return _sessionEventTracker.isConnected();
    }

    /** Called by all mock functions to notify test thread and get a value with which to respond. */
    template <typename Target>
    Target _onMockEvent(Event event) {
        LOGV2(6742616, "Arrived", "event"_attr = toString(event));
        _eventSlot.signal(std::move(event));
        if constexpr (std::is_same_v<Target, void>) {
            return;
        } else {
            auto r = _sessionEventTracker.consumeExpectation(event);
            LOGV2(6742618, "Waited for Event", "event"_attr = event, "result"_attr = toString(r));
            return std::move(r).consumeAs<Target>();
        }
    }

    /** An awaitable event slot. */
    class SyncMockEventSlot {
    public:
        void signal(Event e) {
            stdx::unique_lock lk(_mu);
            invariant(!_event, "All events must be consumed in order");
            _event = e;
            _arrival.notify_one();
        }

        Event wait() {
            stdx::unique_lock lk(_mu);
            _arrival.wait(lk, [&] { return _event; });
            return *std::exchange(_event, {});
        }

        explicit operator bool() const {
            stdx::unique_lock lk(_mu);
            return !!_event;
        }

    private:
        mutable Mutex _mu;
        stdx::condition_variable _arrival;
        boost::optional<Event> _event;
    };

    std::shared_ptr<Session> _session;
    std::shared_ptr<ThreadPool> _threadPool = _makeThreadPool();
    SessionEventTracker _sessionEventTracker;
    SyncMockEventSlot _eventSlot;
};

TEST_F(SessionWorkflowTest, StartThenEndSession) {
    startSession();
    ASSERT_EQ(awaitEvent(), Event::kSource);
    endSession();
    joinSession();
}

TEST_F(SessionWorkflowTest, EndBeforeStartSession) {
    endSession();
    startSession();
    ASSERT_EQ(awaitEvent(), Event::kSource);
    endSession();
    ASSERT_EQ(awaitEvent(), Event::kEnd);
    joinSession();
}

TEST_F(SessionWorkflowTest, OnClientDisconnectCalledOnCleanup) {
    int disconnects = 0;
    sep()->derivedOnClientDisconnectCb = [&](Client*) { ++disconnects; };
    startSession();
    ASSERT_EQ(awaitEvent(), Event::kSource);
    ASSERT_EQ(disconnects, 0);
    endSession();
    ASSERT_EQ(awaitEvent(), Event::kEnd);
    joinSession();
    ASSERT_EQ(disconnects, 1);
}

class StepRunner {
public:
    enum class Action {
        kDefault,
        kExhaust,
        kMoreToCome,

        kErrTerminate,   // External termination via the ServiceEntryPoint.
        kErrDisconnect,  // Socket disconnection by peer.
        kErrNetwork,     // Unspecified network failure (ala host unreachable).
        kErrShutdown,    // System shutdown.
        kErrArbitrary,   // An arbitrary error that does not fall under the other conditions.
    };
    friend StringData toString(Action k) {
        return findEnumName({{Action::kDefault, "Default"_sd},
                             {Action::kExhaust, "Exhaust"_sd},
                             {Action::kMoreToCome, "MoreToCome"_sd},
                             {Action::kErrTerminate, "ErrTerminate"_sd},
                             {Action::kErrDisconnect, "ErrDisconnect"_sd},
                             {Action::kErrNetwork, "ErrNetwork"_sd},
                             {Action::kErrShutdown, "ErrShutdown"_sd},
                             {Action::kErrArbitrary, "ErrArbitrary"_sd}},
                            k);
    }

    /** Encodes a response to `event` by taking `action`. */
    struct Step {
        Event event;
        Action action = Action::kDefault;
    };

    // The final step is assumed to have `kErrDisconnect` as an action,
    // yielding an implied `kEnd` step.
    StepRunner(SessionWorkflowTest* fixture, std::deque<Step> steps)
        : _fixture{fixture}, _steps{[&, at = steps.size() - 1] {
              return _appendTermination(std::move(steps), at, Action::kErrDisconnect);
          }()} {}

    /**
     * Run a set of variations on the steps, failing further and further along the way.
     *
     * This function first runs all the steps without failure to set a baseline. It then checks each
     * failure condition for each step going forward. For example, if we have steps Source, Sink,
     * and End and failure conditions None, Network, and Terminate, it will run these variations in
     * this order:
     * [(Source, None),         (Sink, None),       (End)]
     * [(Source, Network),      (End)]
     * [(Source, Terminate),    (End)]
     * [(Source, None),         (Sink, Network),    (End)]
     * [(Source, None),         (Sink, Terminate),  (End)]
     */
    void run() {
        const auto baseline = std::deque<Step>(_steps.begin(), _steps.end());
        LOGV2(5014106, "Running one entirely clean run");
        _runSteps(baseline);

        static constexpr std::array fails{Action::kErrTerminate,
                                          Action::kErrDisconnect,
                                          Action::kErrNetwork,
                                          Action::kErrShutdown,
                                          Action::kErrArbitrary};

        // Incrementally push forward the step where we fail.
        for (size_t failAt = 0; failAt < baseline.size(); ++failAt) {
            LOGV2(6742614, "Injecting failures", "failAt"_attr = failAt);
            for (auto fail : fails)
                _runSteps(_appendTermination(baseline, failAt, fail));
        }
    }

private:
    /**
     * Returns a new steps sequence, formed by copying the specified `q`, and
     * modifying the copy to be terminated with a `fail` at the `failAt` index.
     */
    std::deque<Step> _appendTermination(std::deque<Step> q, size_t failAt, Action fail) const {
        LOGV2(6742617, "appendTermination", "fail"_attr = toString(fail), "failAt"_attr = failAt);
        invariant(failAt < q.size());
        q.erase(q.begin() + failAt + 1, q.end());
        q.back().action = fail;
        q.push_back({Event::kEnd});
        return q;
    }

    template <typename T>
    void _dumpTransitions(const T& q) {
        BSONArrayBuilder bab;
        for (auto&& t : q) {
            BSONObjBuilder{bab.subobjStart()}
                .append("event", toString(t.event))
                .append("action", toString(t.action));
        }
        LOGV2(6742615, "Run transitions", "transitions"_attr = bab.arr());
    }

    /** Generates an OpMsg containing a BSON with a unique 'id' field. */
    Message _makeOpMsg() {
        static auto nextId = AtomicWord<int>{0};
        auto omb = OpMsgBuilder{};
        omb.setBody(BSONObjBuilder{}.append("id", nextId.fetchAndAdd(1)).obj());
        return omb.finish();
    }

    /** Makes a result for a successful event. */
    Result _successResult(Event event, Action action) {
        switch (event) {
            case Event::kWaitForData:
            case Event::kSink:
                return Result{Status::OK()};
            case Event::kSource: {
                Message m = _makeOpMsg();
                switch (action) {
                    case Action::kExhaust:
                        OpMsg::setFlag(&m, OpMsg::kExhaustSupported);
                        break;
                    default:
                        break;
                }
                return Result{StatusWith{std::move(m)}};
            }
            case Event::kProcess: {
                DbResponse response{};
                switch (action) {
                    case Action::kDefault:
                        response.response = _makeOpMsg();
                        break;
                    case Action::kExhaust:
                        response.response = _makeOpMsg();
                        response.shouldRunAgainForExhaust = true;
                        break;
                    default:
                        break;
                }
                return Result{StatusWith{std::move(response)}};
            }
            default:
                invariant(0, "Bad event: {}"_format(toString(event)));
        }
        MONGO_UNREACHABLE;
    }

    void _injectStep(const Step& t) {
        switch (t.action) {
            case Action::kErrTerminate:
                _fixture->terminateViaServiceEntryPoint();
                break;
            case Action::kErrDisconnect:
                _fixture->endSession();
                break;
            case Action::kErrNetwork:
                _fixture->prepareResponse(t.event, Result(Status{ErrorCodes::HostUnreachable, ""}));
                break;
            case Action::kErrShutdown:
                _fixture->prepareResponse(t.event,
                                          Result(Status{ErrorCodes::ShutdownInProgress, ""}));
                break;
            case Action::kErrArbitrary:
                _fixture->prepareResponse(t.event, Result(Status{ErrorCodes::InternalError, ""}));
                break;
            default:
                _fixture->prepareResponse(t.event, _successResult(t.event, t.action));
                break;
        }
    }

    /** Start a new session, run the `steps` sequence, and join the session. */
    void _runSteps(std::deque<Step> q) {
        _dumpTransitions(q);
        _fixture->startSession();
        for (; !q.empty(); q.pop_front()) {
            auto&& t = q.front();
            auto event = _fixture->awaitEvent();
            ASSERT_EQ(event, t.event);
            LOGV2(6742610,
                  "Test main thread received an event and taking action",
                  "event"_attr = toString(t.event),
                  "action"_attr = toString(t.action));
            if (t.event == Event::kEnd)
                break;
            _injectStep(t);
        }
        _fixture->joinSession();
    }

    SessionWorkflowTest* _fixture;
    std::deque<Step> _steps;
};

template <bool useDedicatedThread>
class StepRunnerSessionWorkflowTest : public SessionWorkflowTest {
public:
    using Action = StepRunner::Action;

    void runSteps(std::deque<StepRunner::Step> steps) {
        StepRunner{this, steps}.run();
    }

    std::deque<StepRunner::Step> defaultLoop() const {
        return {
            {Event::kSource},
            {Event::kProcess},
            {Event::kSink},
            {Event::kSource},
        };
    }

    std::deque<StepRunner::Step> exhaustLoop() const {
        return {
            {Event::kSource, Action::kExhaust},
            {Event::kProcess, Action::kExhaust},
            {Event::kSink},
            {Event::kProcess},
            {Event::kSink},
            {Event::kSource},
        };
    }

    std::deque<StepRunner::Step> moreToComeLoop() const {
        return {
            {Event::kSource, Action::kMoreToCome},
            {Event::kProcess, Action::kMoreToCome},
            {Event::kSource},
            {Event::kProcess},
            {Event::kSink},
            {Event::kSource},
        };
    }

private:
    ScopedValueOverride<bool> _svo{gInitialUseDedicatedThread, useDedicatedThread};
};

class DedicatedThreadSessionWorkflowTest : public StepRunnerSessionWorkflowTest<true> {};

TEST_F(DedicatedThreadSessionWorkflowTest, DefaultLoop) {
    runSteps(defaultLoop());
}

TEST_F(DedicatedThreadSessionWorkflowTest, ExhaustLoop) {
    runSteps(exhaustLoop());
}

TEST_F(DedicatedThreadSessionWorkflowTest, MoreToComeLoop) {
    runSteps(moreToComeLoop());
}

class BorrowedThreadSessionWorkflowTest : public StepRunnerSessionWorkflowTest<false> {
public:
    /**
     * Under the borrowed thread model, the steps are the same as for dedicated thread model,
     * except that kSource events are preceded by kWaitForData events.
     */
    std::deque<StepRunner::Step> borrowedSteps(std::deque<StepRunner::Step> q) {
        for (auto iter = q.begin(); iter != q.end(); ++iter) {
            if (iter->event == Event::kSource) {
                iter = q.insert(iter, {Event::kWaitForData});
                ++iter;
            }
        }
        return q;
    }
};

TEST_F(BorrowedThreadSessionWorkflowTest, DefaultLoop) {
    runSteps(borrowedSteps(defaultLoop()));
}

TEST_F(BorrowedThreadSessionWorkflowTest, ExhaustLoop) {
    runSteps(borrowedSteps(exhaustLoop()));
}

TEST_F(BorrowedThreadSessionWorkflowTest, MoreToComeLoop) {
    runSteps(borrowedSteps(moreToComeLoop()));
}

}  // namespace
}  // namespace mongo::transport
