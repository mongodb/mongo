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

#include <memory>
#include <type_traits>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
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
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_utils.h"
#include "mongo/transport/session_workflow.h"
#include "mongo/transport/session_workflow_test_util.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/tick_source_mock.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace transport {
namespace {

const Status kClosedSessionError{ErrorCodes::SocketException, "Session is closed"};
const Status kNetworkError{ErrorCodes::HostUnreachable, "Someone is unreachable"};
const Status kShutdownError{ErrorCodes::ShutdownInProgress, "Something is shutting down"};
const Status kArbitraryError{ErrorCodes::InternalError, "Something happened"};

/**
 * FailureCondition represents a set of the ways any state in the SessionWorkflow can fail.
 */
enum class FailureCondition {
    kNone,
    kTerminate,   // External termination via the ServiceEntryPoint.
    kDisconnect,  // Socket disconnection by peer.
    kNetwork,     // Unspecified network failure (ala host unreachable).
    kShutdown,    // System shutdown.
    kArbitrary,   // An arbitrary error that does not fall under the other conditions.
};

constexpr StringData toString(FailureCondition fail) {
    switch (fail) {
        case FailureCondition::kNone:
            return "None"_sd;
        case FailureCondition::kTerminate:
            return "Terminate"_sd;
        case FailureCondition::kDisconnect:
            return "Disconnect"_sd;
        case FailureCondition::kNetwork:
            return "Network"_sd;
        case FailureCondition::kShutdown:
            return "Shutdown"_sd;
        case FailureCondition::kArbitrary:
            return "Arbitrary"_sd;
    };

    return "Unknown"_sd;
}

std::ostream& operator<<(std::ostream& os, FailureCondition fail) {
    return os << toString(fail);
}

/**
 * SessionState represents the externally observable state of the SessionWorkflow. These
 * states map relatively closely to the internals of the SessionWorkflow::Impl. That said,
 * this enum represents the SessionWorkflowTest's external understanding of the internal
 * state.
 */
enum class SessionState {
    kStart,
    kPoll,
    kSource,
    kProcess,
    kSink,
    kEnd,
};

constexpr StringData toString(SessionState state) {
    switch (state) {
        case SessionState::kStart:
            return "Start"_sd;
        case SessionState::kPoll:
            return "Poll"_sd;
        case SessionState::kSource:
            return "Source"_sd;
        case SessionState::kProcess:
            return "Process"_sd;
        case SessionState::kSink:
            return "Sink"_sd;
        case SessionState::kEnd:
            return "End"_sd;
    };

    return "Unknown"_sd;
}

std::ostream& operator<<(std::ostream& os, SessionState state) {
    return os << toString(state);
}

/**
 * RequestKind represents the type of operation of the SessionWorkflow. Depending on various
 * message flags and conditions, the SessionWorkflow will transition between states
 * differently.
 */
enum class RequestKind {
    kDefault,
    kExhaust,
    kMoreToCome,
};

constexpr StringData toString(RequestKind kind) {
    switch (kind) {
        case RequestKind::kDefault:
            return "Default"_sd;
        case RequestKind::kExhaust:
            return "Exhaust"_sd;
        case RequestKind::kMoreToCome:
            return "MoreToCome"_sd;
    };

    return "Unknown"_sd;
}

std::ostream& operator<<(std::ostream& os, RequestKind kind) {
    return os << toString(kind);
}

class ResultValue {
public:
    ResultValue() = default;
    explicit ResultValue(Status status) : _value(std::move(status)) {}
    explicit ResultValue(StatusWith<Message> message) : _value(std::move(message)) {}
    explicit ResultValue(StatusWith<DbResponse> response) : _value(std::move(response)) {}

    void setResponse(StatusWith<DbResponse> response) {
        _value = response;
    }

    StatusWith<DbResponse> getResponse() const {
        return _convertTo<StatusWith<DbResponse>>();
    }

    void setMessage(StatusWith<Message> message) {
        _value = message;
    }

    StatusWith<Message> getMessage() const {
        return _convertTo<StatusWith<Message>>();
    }

    void setStatus(Status status) {
        _value = status;
    }

    Status getStatus() const {
        return _convertTo<Status>();
    }

    bool empty() const {
        return _value.index() == 0;
    }

    explicit operator bool() const {
        return !empty();
    }

private:
    template <typename Target>
    Target _convertTo() const {
        return stdx::visit(
            [](auto alt) -> Target {
                if constexpr (std::is_convertible<decltype(alt), Target>())
                    return alt;
                invariant(false, "ResultValue not convertible to target type");
                MONGO_COMPILER_UNREACHABLE;
            },
            _value);
    }

    stdx::variant<stdx::monostate, Status, StatusWith<Message>, StatusWith<DbResponse>> _value;
};

/**
 * This class stores and synchronizes the shared state result between the test
 * fixture and its various wrappers.
 */
struct StateResult {
    Mutex mutex = MONGO_MAKE_LATCH("StateResult::_mutex");
    stdx::condition_variable cv;

    AtomicWord<bool> isConnected{true};

    ResultValue result;
    SessionState state;
};

/**
 * The SessionWorkflowTest is a fixture that mocks the external inputs into the
 * SessionWorkflow so as to provide a deterministic way to evaluate the session workflow
 * implemenation.
 */
class SessionWorkflowTest : public LockerNoopServiceContextTest {
public:
    /**
     * Make a generic thread pool to deliver external inputs out of line (mocking the network or
     * database workers).
     */
    static std::shared_ptr<ThreadPool> makeThreadPool() {
        auto options = ThreadPool::Options{};
        options.poolName = "SessionWorkflowTest";

        return std::make_shared<ThreadPool>(std::move(options));
    }

    void setUp() override;
    void tearDown() override;

    /**
     * This function blocks until the SessionWorkflowTest observes a state change.
     */
    SessionState popSessionState() {
        return _stateQueue.pop();
    }

    /**
     * This function asserts that the SessionWorkflowTest has not yet observed a state change.
     *
     * Note that this function does not guarantee that it will not observe a state change in the
     * future.
     */
    void assertNoSessionState() {
        if (auto maybeState = _stateQueue.tryPop()) {
            FAIL("The queue is not empty, state: ") << *maybeState;
        }
    }

    /**
     * This function stores an external response to be delivered out of line to the
     * SessionWorkflow.
     */
    void setResult(SessionState state, ResultValue result) {
        auto lk = stdx::lock_guard(_stateResult->mutex);
        invariant(state == SessionState::kPoll || state == SessionState::kSource ||
                  state == SessionState::kProcess || state == SessionState::kSink);
        _stateResult->result = std::move(result);
        _stateResult->state = state;
        _stateResult->cv.notify_one();
    }

    /**
     * This function makes a generic result appropriate for a successful state change given
     * SessionState and RequestKind.
     */
    ResultValue makeGenericResult(SessionState state, RequestKind kind) {
        ResultValue result;
        switch (state) {
            case SessionState::kPoll:
            case SessionState::kSink:
                result.setStatus(Status::OK());
                break;
            case SessionState::kSource: {
                Message message = _makeIndexedBson();
                switch (kind) {
                    case RequestKind::kExhaust:
                        OpMsg::setFlag(&message, OpMsg::kExhaustSupported);
                        break;
                    case RequestKind::kDefault:
                    case RequestKind::kMoreToCome:
                        break;
                }
                result.setMessage(StatusWith<Message>(message));
            } break;
            case SessionState::kProcess: {
                DbResponse response;
                switch (kind) {
                    case RequestKind::kDefault:
                        response.response = _makeIndexedBson();
                        break;
                    case RequestKind::kExhaust:
                        response.response = _makeIndexedBson();
                        response.shouldRunAgainForExhaust = true;
                        break;
                    case RequestKind::kMoreToCome:
                        break;
                }
                result.setResponse(response);
            } break;
            default:
                invariant(
                    false,
                    "Unable to make generic result for this state: {}"_format(toString(state)));
        }
        return result;
    }

    /**
     * Initialize a new Session.
     */
    void initNewSession();

    /**
     * Launch a SessionWorkflow for the current session.
     */
    void startSession();

    /**
     * Wait for the current Session and SessionWorkflow to end.
     */
    void joinSession();

    /**
     * Mark the session as no longer connected.
     */
    void endSession() {
        auto lk = stdx::lock_guard(_stateResult->mutex);
        if (_stateResult->isConnected.swap(false)) {
            LOGV2(5014101, "Ending session");
            _stateResult->cv.notify_one();
        }
    }

    /**
     * Start a brand new session, run the given function, and then join the session.
     */
    template <typename F>
    void runWithNewSession(F&& func) {
        initNewSession();
        startSession();

        auto firstState = popSessionState();
        ASSERT(firstState == SessionState::kSource || firstState == SessionState::kPoll)
            << "State was instead: " << toString(firstState);

        std::forward<F>(func)();

        joinSession();
    }

    void terminateViaServiceEntryPoint();

    bool isConnected() const {
        return _stateResult->isConnected.load();
    }

    int onClientDisconnectCalledTimes() const {
        return _onClientDisconnectCalled;
    }

private:
    /**
     * Generate a resonably generic BSON with an id for use in debugging.
     */
    static Message _makeIndexedBson() {
        auto bob = BSONObjBuilder();
        static auto nextId = AtomicWord<int>{0};
        bob.append("id", nextId.fetchAndAdd(1));

        auto omb = OpMsgBuilder{};
        omb.setBody(bob.obj());
        return omb.finish();
    }

    /**
     * Use an external result to mock handling a request.
     */
    StatusWith<DbResponse> _processRequest(OperationContext* opCtx, const Message&) noexcept {
        _stateQueue.push(SessionState::kProcess);

        auto result = [&]() -> StatusWith<DbResponse> {
            auto lk = stdx::unique_lock(_stateResult->mutex);
            _stateResult->cv.wait(lk, [this] {
                return (_stateResult->result && _stateResult->state == SessionState::kProcess) ||
                    !isConnected();
            });

            if (!isConnected()) {
                return kClosedSessionError;
            }

            invariant(_stateResult->result);
            return std::exchange(_stateResult->result, {}).getResponse();
        }();

        LOGV2(5014100, "Handled request", "error"_attr = result.getStatus());

        return result;
    }

    Future<DbResponse> _handleRequest(OperationContext* opCtx, const Message& request) noexcept {
        auto [p, f] = makePromiseFuture<DbResponse>();
        ExecutorFuture<void>(_threadPool)
            .then([this, opCtx, &request, p = std::move(p)]() mutable {
                auto strand = ClientStrand::get(opCtx->getClient());
                strand->run([&] { p.setWith([&] { return _processRequest(opCtx, request); }); });
            })
            .getAsync([](auto&&) {});
        return std::move(f);
    }

    /**
     * Use an external result to mock polling for data and observe the state.
     */
    Status _waitForData() {
        _stateQueue.push(SessionState::kPoll);

        auto result = [&]() -> Status {
            auto lk = stdx::unique_lock(_stateResult->mutex);
            _stateResult->cv.wait(lk, [this] {
                return (_stateResult->result && _stateResult->state == SessionState::kPoll) ||
                    !isConnected();
            });

            if (!isConnected()) {
                return kClosedSessionError;
            }

            invariant(_stateResult->result);
            return std::exchange(_stateResult->result, {}).getStatus();
        }();

        LOGV2(5014102, "Finished waiting for data", "error"_attr = result);
        return result;
    }

    /**
     * Use an external result to mock reading data and observe the state.
     */
    StatusWith<Message> _sourceMessage() {
        _stateQueue.push(SessionState::kSource);

        auto result = [&]() -> StatusWith<Message> {
            auto lk = stdx::unique_lock(_stateResult->mutex);
            _stateResult->cv.wait(lk, [this] {
                return (_stateResult->result && _stateResult->state == SessionState::kSource) ||
                    !isConnected();
            });

            if (!isConnected()) {
                return kClosedSessionError;
            }

            invariant(_stateResult->result);
            return std::exchange(_stateResult->result, {}).getMessage();
        }();

        LOGV2(5014103, "Sourced message", "error"_attr = result.getStatus());

        return result;
    }

    /**
     * Use an external result to mock writing data and observe the state.
     */
    Status _sinkMessage(Message message) {
        _stateQueue.push(SessionState::kSink);

        auto result = [&]() -> Status {
            auto lk = stdx::unique_lock(_stateResult->mutex);
            _stateResult->cv.wait(lk, [this] {
                return (_stateResult->result && _stateResult->state == SessionState::kSink) ||
                    !isConnected();
            });

            if (!isConnected()) {
                return kClosedSessionError;
            }

            invariant(_stateResult->result);
            return std::exchange(_stateResult->result, {}).getStatus();
        }();

        LOGV2(5014104, "Sunk message", "error"_attr = result);
        return result;
    }

    Future<void> _asyncWaitForData() noexcept {
        return ExecutorFuture<void>(_threadPool)
            .then([this] { return _waitForData(); })
            .unsafeToInlineFuture();
    }

    Future<Message> _asyncSourceMessage() noexcept {
        return ExecutorFuture<void>(_threadPool)
            .then([this] { return _sourceMessage(); })
            .unsafeToInlineFuture();
    }

    Future<void> _asyncSinkMessage(Message message) noexcept {
        return ExecutorFuture<void>(_threadPool)
            .then([this, message = std::move(message)]() mutable {
                return _sinkMessage(std::move(message));
            })
            .unsafeToInlineFuture();
    }

    MockServiceEntryPoint* _sep;

    const std::shared_ptr<ThreadPool> _threadPool = makeThreadPool();

    std::unique_ptr<StateResult> _stateResult;

    std::shared_ptr<CallbackMockSession> _session;
    SingleProducerSingleConsumerQueue<SessionState> _stateQueue;

    int _onClientDisconnectCalled{0};
};

/**
 * This class iterates over the potential methods of failure for a set of steps.
 */
class StepRunner {
    /**
     * This is a simple data structure describing the external response for one state in the
     * session workflow.
     */
    struct Step {
        SessionState state;
        RequestKind kind;
    };
    using StepList = std::vector<Step>;

public:
    StepRunner(SessionWorkflowTest* fixture) : _fixture{fixture} {}
    ~StepRunner() {
        invariant(_runCount > 0, "StepRunner expects to be run at least once");
    }

    /**
     * Given a FailureCondition, cause an external result to be delivered that is appropriate for
     * the given state and request kind.
     */
    SessionState doGenericStep(const Step& step, FailureCondition fail) {
        switch (fail) {
            case FailureCondition::kNone: {
                _fixture->setResult(step.state, _fixture->makeGenericResult(step.state, step.kind));
            } break;
            case FailureCondition::kTerminate: {
                _fixture->terminateViaServiceEntryPoint();
                // We expect that the session will be disconnected via the SEP, no need to set any
                // result.
            } break;
            case FailureCondition::kDisconnect: {
                _fixture->endSession();
                // We expect that the session will be disconnected directly, no need to set any
                // result.
            } break;
            case FailureCondition::kNetwork: {
                _fixture->setResult(step.state, ResultValue(kNetworkError));
            } break;
            case FailureCondition::kShutdown: {
                _fixture->setResult(step.state, ResultValue(kShutdownError));
            } break;
            case FailureCondition::kArbitrary: {
                _fixture->setResult(step.state, ResultValue(kArbitraryError));
            } break;
        };

        return _fixture->popSessionState();
    }

    /**
     * Mark an additional expected state in the session workflow.
     */
    void expectNextState(SessionState state, RequestKind kind) {
        auto step = Step{};
        step.state = state;
        step.kind = kind;
        _steps.emplace_back(std::move(step));
    }

    /**
     * Mark the final expected state in the session workflow.
     */
    void expectFinalState(SessionState state) {
        _finalState = state;
    }

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
        invariant(_finalState);

        auto getExpectedPostState = [&](auto iter) {
            auto nextIter = ++iter;
            if (nextIter == _steps.end()) {
                return *_finalState;
            }
            return nextIter->state;
        };

        // Do one entirely clean run.
        LOGV2(5014106, "Running success case");
        _fixture->runWithNewSession([&] {
            for (auto iter = _steps.begin(); iter != _steps.end(); ++iter) {
                ASSERT_EQ(doGenericStep(*iter, FailureCondition::kNone),
                          getExpectedPostState(iter));
            }

            _fixture->endSession();
            ASSERT_EQ(_fixture->popSessionState(), SessionState::kEnd);
        });

        const auto failList = std::vector<FailureCondition>{FailureCondition::kTerminate,
                                                            FailureCondition::kDisconnect,
                                                            FailureCondition::kNetwork,
                                                            FailureCondition::kShutdown,
                                                            FailureCondition::kArbitrary};

        for (auto failIter = _steps.begin(); failIter != _steps.end(); ++failIter) {
            // Incrementally push forward the step where we fail.
            for (auto fail : failList) {
                LOGV2(5014105,
                      "Running failure case",
                      "failureCase"_attr = fail,
                      "sessionState"_attr = failIter->state,
                      "requestKind"_attr = failIter->kind);

                _fixture->runWithNewSession([&] {
                    auto iter = _steps.begin();
                    for (; iter != failIter; ++iter) {
                        // Run through each step until our point of failure with
                        // FailureCondition::kNone.
                        ASSERT_EQ(doGenericStep(*iter, FailureCondition::kNone),
                                  getExpectedPostState(iter))
                            << "Current state: (" << iter->state << ", " << iter->kind << ")";
                    }

                    // Finally fail on a given step.
                    ASSERT_EQ(doGenericStep(*iter, fail), SessionState::kEnd);
                });
            }
        }

        _runCount += 1;
    }

private:
    SessionWorkflowTest* const _fixture;

    boost::optional<SessionState> _finalState;
    StepList _steps;

    // This variable is currently used as a post-condition to make sure that the StepRunner has been
    // run. In the current form, it could be a boolean. That said, if you need to stress test the
    // SessionWorkflow, you will want to check this variable to make sure you have run as many
    // times as you expect.
    size_t _runCount = 0;
};

void SessionWorkflowTest::initNewSession() {
    assertNoSessionState();

    _session = std::make_shared<CallbackMockSession>();
    _session->endCb = [&] { endSession(); };
    _session->isConnectedCb = [&] { return isConnected(); };
    _session->waitForDataCb = [&] { return _waitForData(); };
    _session->sourceMessageCb = [&] { return _sourceMessage(); };
    _session->sinkMessageCb = [&](Message message) { return _sinkMessage(std::move(message)); };
    _session->asyncWaitForDataCb = [&] { return _asyncWaitForData(); };
    _session->asyncSourceMessageCb = [&](const BatonHandle&) { return _asyncSourceMessage(); };
    _session->asyncSinkMessageCb = [&](Message message, const BatonHandle&) {
        return _asyncSinkMessage(std::move(message));
    };
    _stateResult->isConnected.store(true);
}

void SessionWorkflowTest::joinSession() {
    ASSERT(_sep->waitForNoSessions(Seconds{1}));

    assertNoSessionState();
}

void SessionWorkflowTest::startSession() {
    _sep->startSession(_session);
}

void SessionWorkflowTest::terminateViaServiceEntryPoint() {
    _sep->endAllSessionsNoTagMask();
}

void SessionWorkflowTest::setUp() {
    ServiceContextTest::setUp();

    auto sep = std::make_unique<MockServiceEntryPoint>(getServiceContext());
    sep->handleRequestCb = [&](OperationContext* opCtx, const Message& request) {
        return _handleRequest(opCtx, request);
    };
    sep->onEndSessionCb = [&](const transport::SessionHandle& session) {
        invariant(session == _session);
        _stateQueue.push(SessionState::kEnd);
    };
    sep->derivedOnClientDisconnectCb = [&](Client*) { ++_onClientDisconnectCalled; };
    _sep = sep.get();
    getServiceContext()->setServiceEntryPoint(std::move(sep));
    invariant(_sep->start());

    _threadPool->startup();

    _stateResult = std::make_unique<StateResult>();
}

void SessionWorkflowTest::tearDown() {
    ON_BLOCK_EXIT([&] { ServiceContextTest::tearDown(); });

    endSession();

    // Normal shutdown is a noop outside of ASAN.
    invariant(_sep->shutdownAndWait(Seconds{10}));

    _threadPool->shutdown();
    _threadPool->join();
}

template <bool useDedicatedThread>
class DedicatedThreadOverrideTest : public SessionWorkflowTest {
    ScopedValueOverride<bool> _svo{gInitialUseDedicatedThread, useDedicatedThread};
};

using SessionWorkflowWithBorrowedThreadsTest = DedicatedThreadOverrideTest<false>;
using SessionWorkflowWithDedicatedThreadsTest = DedicatedThreadOverrideTest<true>;

TEST_F(SessionWorkflowTest, StartThenEndSession) {
    initNewSession();
    startSession();

    ASSERT_EQ(popSessionState(), SessionState::kSource);

    endSession();
}

TEST_F(SessionWorkflowTest, EndBeforeStartSession) {
    initNewSession();
    endSession();
    startSession();
}

TEST_F(SessionWorkflowTest, OnClientDisconnectCalledOnCleanup) {
    initNewSession();
    startSession();
    ASSERT_EQ(popSessionState(), SessionState::kSource);
    ASSERT_EQ(onClientDisconnectCalledTimes(), 0);
    endSession();
    ASSERT_EQ(popSessionState(), SessionState::kEnd);
    joinSession();
    ASSERT_EQ(onClientDisconnectCalledTimes(), 1);
}

TEST_F(SessionWorkflowWithDedicatedThreadsTest, DefaultLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState(SessionState::kSource, RequestKind::kDefault);
    runner.expectNextState(SessionState::kProcess, RequestKind::kDefault);
    runner.expectNextState(SessionState::kSink, RequestKind::kDefault);
    runner.expectFinalState(SessionState::kSource);

    runner.run();
}

TEST_F(SessionWorkflowWithDedicatedThreadsTest, ExhaustLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState(SessionState::kSource, RequestKind::kExhaust);
    runner.expectNextState(SessionState::kProcess, RequestKind::kExhaust);
    runner.expectNextState(SessionState::kSink, RequestKind::kExhaust);
    runner.expectNextState(SessionState::kProcess, RequestKind::kDefault);
    runner.expectNextState(SessionState::kSink, RequestKind::kDefault);
    runner.expectFinalState(SessionState::kSource);

    runner.run();
}

TEST_F(SessionWorkflowWithDedicatedThreadsTest, MoreToComeLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState(SessionState::kSource, RequestKind::kMoreToCome);
    runner.expectNextState(SessionState::kProcess, RequestKind::kMoreToCome);
    runner.expectNextState(SessionState::kSource, RequestKind::kDefault);
    runner.expectNextState(SessionState::kProcess, RequestKind::kDefault);
    runner.expectNextState(SessionState::kSink, RequestKind::kDefault);
    runner.expectFinalState(SessionState::kSource);

    runner.run();
}

TEST_F(SessionWorkflowWithBorrowedThreadsTest, DefaultLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState(SessionState::kPoll, RequestKind::kDefault);
    runner.expectNextState(SessionState::kSource, RequestKind::kDefault);
    runner.expectNextState(SessionState::kProcess, RequestKind::kDefault);
    runner.expectNextState(SessionState::kSink, RequestKind::kDefault);
    runner.expectFinalState(SessionState::kPoll);

    runner.run();
}

TEST_F(SessionWorkflowWithBorrowedThreadsTest, ExhaustLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState(SessionState::kPoll, RequestKind::kExhaust);
    runner.expectNextState(SessionState::kSource, RequestKind::kExhaust);
    runner.expectNextState(SessionState::kProcess, RequestKind::kExhaust);
    runner.expectNextState(SessionState::kSink, RequestKind::kExhaust);
    runner.expectNextState(SessionState::kProcess, RequestKind::kDefault);
    runner.expectNextState(SessionState::kSink, RequestKind::kDefault);
    runner.expectFinalState(SessionState::kPoll);

    runner.run();
}

TEST_F(SessionWorkflowWithBorrowedThreadsTest, MoreToComeLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState(SessionState::kPoll, RequestKind::kMoreToCome);
    runner.expectNextState(SessionState::kSource, RequestKind::kMoreToCome);
    runner.expectNextState(SessionState::kProcess, RequestKind::kMoreToCome);
    runner.expectNextState(SessionState::kPoll, RequestKind::kDefault);
    runner.expectNextState(SessionState::kSource, RequestKind::kDefault);
    runner.expectNextState(SessionState::kProcess, RequestKind::kDefault);
    runner.expectNextState(SessionState::kSink, RequestKind::kDefault);
    runner.expectFinalState(SessionState::kPoll);

    runner.run();
}

}  // namespace
}  // namespace transport
}  // namespace mongo
