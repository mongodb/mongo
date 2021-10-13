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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include <memory>
#include <vector>

#include "mongo/base/checked_cast.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/client_strand.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/mutex.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/mock_session.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_entry_point_impl.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_utils.h"
#include "mongo/transport/service_state_machine.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/producer_consumer_queue.h"
#include "mongo/util/tick_source_mock.h"

namespace mongo {
namespace transport {
namespace {

// TODO(cr) Does this go into its own header?
/**
 * The ServiceStateMachineTest is a fixture that mocks the external inputs into the
 * ServiceStateMachine so as to provide a deterministic way to evaluate the state machine
 * implemenation.
 */
class ServiceStateMachineTest : public ServiceContextTest {
    /**
     * This class stores and synchronizes the shared data between the test fixture and its various
     * wrappers.
     */
    struct Data {
        Mutex mutex = MONGO_MAKE_LATCH("ServiceStateMachineTest::Data::_mutex");
        stdx::condition_variable cv;

        AtomicWord<bool> isConnected{true};

        boost::optional<Status> pollResult;
        boost::optional<StatusWith<Message>> sourceResult;
        boost::optional<StatusWith<DbResponse>> processResult;
        boost::optional<Status> sinkResult;
    };

public:
    class ServiceEntryPoint;
    class Session;
    class StepRunner;

    static inline const auto kClosedSessionError =
        Status{ErrorCodes::SocketException, "Session is closed"};
    static inline const auto kNetworkError =
        Status{ErrorCodes::HostUnreachable, "Someone is unreachable"};
    static inline const auto kShutdownError =
        Status{ErrorCodes::ShutdownInProgress, "Something is shutting down"};
    static inline const auto kArbitraryError =
        Status{ErrorCodes::InternalError, "Something happened"};

    /**
     * FailureCondition represents a set of the ways any state in the ServiceStateMachine can fail.
     */
    enum class FailureCondition {
        kNone,
        kTerminate,   // External termination via the ServiceEntryPoint.
        kDisconnect,  // Socket disconnection by peer.
        kNetwork,     // Unspecified network failure (ala host unreachable).
        kShutdown,    // System shutdown.
        kArbitrary,   // An arbitrary error that does not fall under the other conditions.
    };

    /**
     * IngressState represents the externally observable state of the ServiceStateMachine. These
     * states map relatively closely to the internals of the ServiceStateMachine::Impl. That said,
     * this enum represents the ServiceStateMachineTest's external understanding of the internal
     * state.
     */
    enum class IngressState {
        kStart,
        kPoll,
        kSource,
        kProcess,
        kSink,
        kEnd,
    };

    /**
     * IngressMode represents the mode of operation of the ServiceStateMachine. Depending on various
     * message flags and conditions, the ServiceStateMachine will transition between states
     * differently.
     */
    enum class IngressMode {
        kDefault,
        kExhaust,
        kMoreToCome,
    };

    /**
     * Make a generic thread pool to deliver external inputs out of line (mocking the network or
     * database workers).
     */
    static std::shared_ptr<ThreadPool> makeThreadPool() {
        auto options = ThreadPool::Options{};
        options.poolName = "ServiceStateMachineTest";

        return std::make_shared<ThreadPool>(std::move(options));
    }

    ServiceStateMachineTest(boost::optional<ServiceExecutor::ThreadingModel> threadingModel = {})
        : _threadingModel(std::move(threadingModel)) {}

    void setUp() override;
    void tearDown() override;

    /**
     * This function blocks until the ServiceStateMachineTest observes a state change.
     */
    IngressState popIngressState() {
        return _stateQueue.pop();
    }

    /**
     * This function asserts that the ServiceStateMachineTest has not yet observed a state change.
     *
     * Note that this function does not guarantee that it will not observe a state change in the
     * future.
     */
    void assertNoIngressState() {
        if (auto maybeState = _stateQueue.tryPop()) {
            FAIL("The queue is not empty, state: ") << *maybeState;
        }
    }

    /**
     * This function stores an external response to be delivered out of line to the
     * ServiceStateMachine.
     */
    template <IngressState kMode, typename ResultT>
    void setResult(ResultT result) {
        auto lk = stdx::lock_guard(_data->mutex);
        if constexpr (kMode == IngressState::kPoll) {
            _data->pollResult = std::move(result);
        } else if constexpr (kMode == IngressState::kSource) {
            _data->sourceResult = std::move(result);
        } else if constexpr (kMode == IngressState::kProcess) {
            _data->processResult = std::move(result);
        } else if constexpr (kMode == IngressState::kSink) {
            _data->sinkResult = std::move(result);
        } else {
            FAIL("Cannot set a result for this state");
        }
        _data->cv.notify_one();
    }

    /**
     * This function makes a generic result appropriate for a successful state change given
     * IngressState and IngressMode.
     */
    template <IngressState kState, IngressMode kMode>
    auto makeGenericResult() {
        if constexpr (kState == IngressState::kPoll || kState == IngressState::kSink) {
            return Status::OK();
        } else if constexpr (kState == IngressState::kSource) {
            Message result = _makeIndexedBson();
            if constexpr (kMode == IngressMode::kExhaust) {
                OpMsg::setFlag(&result, OpMsg::kExhaustSupported);
            } else {
                static_assert(kMode == IngressMode::kDefault || kMode == IngressMode::kMoreToCome);
            }
            return result;
        } else if constexpr (kState == IngressState::kProcess) {
            DbResponse response;
            if constexpr (kMode == IngressMode::kDefault) {
                response.response = _makeIndexedBson();
            } else if constexpr (kMode == IngressMode::kExhaust) {
                response.response = _makeIndexedBson();
                response.shouldRunAgainForExhaust = true;
            } else {
                static_assert(kMode == IngressMode::kMoreToCome);
            }
            return response;
        } else {
            FAIL("Unable to make generic result for this state");
        }
    };

    /**
     * Initialize a new Session.
     */
    void initNewSession();

    /**
     * Launch a ServiceStateMachine for the current session.
     */
    void startSession();

    /**
     * Wait for the current Session and ServiceStateMachine to end.
     */
    void joinSession();

    /**
     * Mark the session as no longer connected.
     */
    void endSession() {
        auto lk = stdx::lock_guard(_data->mutex);
        if (_data->isConnected.swap(false)) {
            LOGV2(5014101, "Ending session");
            _data->cv.notify_one();
        }
    }

    void terminateViaServiceEntryPoint();

    bool isConnected() const {
        return _data->isConnected.load();
    }

    int onClientDisconnectCalledTimes() const {
        return _onClientDisconnectCalled;
    }

    friend constexpr StringData toString(FailureCondition fail) {
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

    friend std::ostream& operator<<(std::ostream& os, FailureCondition fail) {
        return os << toString(fail);
    }

    friend constexpr StringData toString(IngressState state) {
        switch (state) {
            case IngressState::kStart:
                return "Start"_sd;
            case IngressState::kPoll:
                return "Poll"_sd;
            case IngressState::kSource:
                return "Source"_sd;
            case IngressState::kProcess:
                return "Process"_sd;
            case IngressState::kSink:
                return "Sink"_sd;
            case IngressState::kEnd:
                return "End"_sd;
        };

        return "Unknown"_sd;
    }

    friend std::ostream& operator<<(std::ostream& os, IngressState state) {
        return os << toString(state);
    }

    friend constexpr StringData toString(IngressMode mode) {
        switch (mode) {
            case IngressMode::kDefault:
                return "Default"_sd;
            case IngressMode::kExhaust:
                return "Exhaust"_sd;
            case IngressMode::kMoreToCome:
                return "MoreToCome"_sd;
        };

        return "Unknown"_sd;
    }

    friend std::ostream& operator<<(std::ostream& os, IngressMode mode) {
        return os << toString(mode);
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
     * Start a brand new session, run the given function, and then join the session.
     */
    template <typename F>
    void _runWithNewSession(F&& func) {
        initNewSession();
        startSession();

        auto firstState = popIngressState();
        ASSERT(firstState == IngressState::kSource || firstState == IngressState::kPoll)
            << "State was instead: " << toString(firstState);

        std::forward<F>(func)();

        joinSession();
    }

    /**
     * Use an external result to mock handling a request.
     */
    StatusWith<DbResponse> _handleRequest(OperationContext* opCtx, const Message&) noexcept {
        _stateQueue.push(IngressState::kProcess);

        auto result = [&]() -> StatusWith<DbResponse> {
            auto lk = stdx::unique_lock(_data->mutex);
            _data->cv.wait(lk, [this] { return _data->processResult || !isConnected(); });

            if (!isConnected()) {
                return kClosedSessionError;
            }

            invariant(_data->processResult);
            return *std::exchange(_data->processResult, {});
        }();

        LOGV2(5014100, "Handled request", "error"_attr = result.getStatus());

        return result;
    }

    /**
     * Use an external result to mock polling for data and observe the state.
     */
    Status _waitForData() {
        _stateQueue.push(IngressState::kPoll);

        auto result = [&]() -> Status {
            auto lk = stdx::unique_lock(_data->mutex);
            _data->cv.wait(lk, [this] { return _data->pollResult || !isConnected(); });

            if (!isConnected()) {
                return kClosedSessionError;
            }

            return *std::exchange(_data->pollResult, {});
        }();

        LOGV2(5014102, "Finished waiting for data", "error"_attr = result);
        return result;
    }

    /**
     * Use an external result to mock reading data and observe the state.
     */
    StatusWith<Message> _sourceMessage() {
        _stateQueue.push(IngressState::kSource);

        auto result = [&]() -> StatusWith<Message> {
            auto lk = stdx::unique_lock(_data->mutex);
            _data->cv.wait(lk, [this] { return _data->sourceResult || !isConnected(); });

            if (!isConnected()) {
                return kClosedSessionError;
            }

            invariant(_data->sourceResult);
            return *std::exchange(_data->sourceResult, {});
        }();

        LOGV2(5014103, "Sourced message", "error"_attr = result.getStatus());

        return result;
    }

    /**
     * Use an external result to mock writing data and observe the state.
     */
    Status _sinkMessage(Message message) {
        _stateQueue.push(IngressState::kSink);

        auto result = [&]() -> Status {
            auto lk = stdx::unique_lock(_data->mutex);
            _data->cv.wait(lk, [this] { return _data->sinkResult || !isConnected(); });

            if (!isConnected()) {
                return kClosedSessionError;
            }

            invariant(_data->sinkResult);
            return *std::exchange(_data->sinkResult, {});
        }();

        LOGV2(5014104, "Sunk message", "error"_attr = result);
        return result;
    }

    /**
     * Observe the end of the session.
     */
    void _cleanup(const transport::SessionHandle& session) {
        invariant(
            session == _session,
            "This fixture and the ServiceStateMachine should have handles to the same Session");

        _stateQueue.push(IngressState::kEnd);
    }

    void _onClientDisconnect() {
        ++_onClientDisconnectCalled;
    }

    const boost::optional<ServiceExecutor::ThreadingModel> _threadingModel;
    boost::optional<ServiceExecutor::ThreadingModel> _originalThreadingModel;

    ServiceStateMachineTest::ServiceEntryPoint* _sep;

    const std::shared_ptr<ThreadPool> _threadPool = makeThreadPool();

    std::unique_ptr<Data> _data;

    std::shared_ptr<ServiceStateMachineTest::Session> _session;
    SingleProducerSingleConsumerQueue<IngressState> _stateQueue;

    int _onClientDisconnectCalled{0};
};

/**
 * This class is a simple wrapper that delegates Session behavior to the fixture.
 */
class ServiceStateMachineTest::Session final : public transport::MockSessionBase {
public:
    explicit Session(ServiceStateMachineTest* fixture)
        : transport::MockSessionBase(), _fixture{fixture} {}
    ~Session() override = default;

    TransportLayer* getTransportLayer() const override {
        MONGO_UNREACHABLE;
    }

    void end() override {
        _fixture->endSession();
    }

    bool isConnected() override {
        return _fixture->isConnected();
    }

    Status waitForData() noexcept override {
        return _fixture->_waitForData();
    }

    StatusWith<Message> sourceMessage() noexcept override {
        return _fixture->_sourceMessage();
    }

    Status sinkMessage(Message message) noexcept override {
        return _fixture->_sinkMessage(std::move(message));
    }

    Future<void> asyncWaitForData() noexcept override {
        return ExecutorFuture<void>(_fixture->_threadPool)
            .then([this] { return _fixture->_waitForData(); })
            .unsafeToInlineFuture();
    }

    Future<Message> asyncSourceMessage(const BatonHandle&) noexcept override {
        return ExecutorFuture<void>(_fixture->_threadPool)
            .then([this] { return _fixture->_sourceMessage(); })
            .unsafeToInlineFuture();
    }

    Future<void> asyncSinkMessage(Message message, const BatonHandle&) noexcept override {
        return ExecutorFuture<void>(_fixture->_threadPool)
            .then([this, message = std::move(message)]() mutable {
                return _fixture->_sinkMessage(std::move(message));
            })
            .unsafeToInlineFuture();
    }

private:
    ServiceStateMachineTest* const _fixture;
};

/**
 * This class is a simple wrapper that delegates ServiceEntryPoint behavior to the fixture.
 *
 * TODO(SERVER-54143) ServiceEntryPointImpl does a surprising amount of management for
 * ServiceStateMachines. Once we separate that concern, we should be able to define onEndSession as
 * a hook or override for that type and derive from ServiceEntryPoint instead.
 */
class ServiceStateMachineTest::ServiceEntryPoint final : public ServiceEntryPointImpl {
public:
    explicit ServiceEntryPoint(ServiceStateMachineTest* fixture)
        : ServiceEntryPointImpl(fixture->getServiceContext()), _fixture(fixture) {}
    ~ServiceEntryPoint() override = default;

    Future<DbResponse> handleRequest(OperationContext* opCtx,
                                     const Message& request) noexcept override {
        auto [p, f] = makePromiseFuture<DbResponse>();
        ExecutorFuture<void>(_fixture->_threadPool)
            .then([this, opCtx, &request, p = std::move(p)]() mutable {
                auto strand = ClientStrand::get(opCtx->getClient());
                strand->run(
                    [&] { p.setWith([&] { return _fixture->_handleRequest(opCtx, request); }); });
            })
            .getAsync([](auto) {});
        return std::move(f);
    }

    void onEndSession(const transport::SessionHandle& session) override {
        _fixture->_cleanup(session);
    }

    void onClientDisconnect(Client* client) override {
        invariant(client);
        _fixture->_onClientDisconnect();
    }

private:
    ServiceStateMachineTest* const _fixture;
};

/**
 * This class iterates over the potential methods of failure for a set of steps.
 */
class ServiceStateMachineTest::StepRunner {
    /**
     * This is a simple data structure describing the external response for one state in the service
     * state machine.
     */
    struct Step {
        ServiceStateMachineTest::IngressState state;
        ServiceStateMachineTest::IngressMode mode;
        unique_function<IngressState(FailureCondition)> func;
    };
    using StepList = std::vector<Step>;

public:
    StepRunner(ServiceStateMachineTest* fixture) : _fixture{fixture} {}
    ~StepRunner() {
        invariant(_runCount > 0, "StepRunner expects to be run at least once");
    }

    /**
     * Given a FailureCondition, cause an external result to be delivered that is appropriate for
     * the given state and mode.
     */
    template <IngressState kState, IngressMode kMode>
    IngressState doGenericStep(FailureCondition fail) {
        switch (fail) {
            case FailureCondition::kNone: {
                _fixture->setResult<kState>(_fixture->makeGenericResult<kState, kMode>());
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
                _fixture->setResult<kState>(kNetworkError);
            } break;
            case FailureCondition::kShutdown: {
                _fixture->setResult<kState>(kShutdownError);
            } break;
            case FailureCondition::kArbitrary: {
                _fixture->setResult<kState>(kArbitraryError);
            } break;
        };

        return _fixture->popIngressState();
    }

    /**
     * Mark an additional expected state in the service state machine.
     */
    template <IngressState kState, IngressMode kMode>
    void expectNextState() {
        auto step = Step{};
        step.state = kState;
        step.mode = kMode;
        step.func = [this](FailureCondition fail) { return doGenericStep<kState, kMode>(fail); };
        _steps.emplace_back(std::move(step));
    }

    /**
     * Mark the final expected state in the service state machine.
     */
    template <IngressState kState>
    void expectFinalState() {
        _finalState = kState;
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
        _fixture->_runWithNewSession([&] {
            for (auto iter = _steps.begin(); iter != _steps.end(); ++iter) {
                ASSERT_EQ(iter->func(FailureCondition::kNone), getExpectedPostState(iter));
            }

            _fixture->endSession();
            ASSERT_EQ(_fixture->popIngressState(), IngressState::kEnd);
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
                      "ingressState"_attr = failIter->state,
                      "ingressMode"_attr = failIter->mode);

                _fixture->_runWithNewSession([&] {
                    auto iter = _steps.begin();
                    for (; iter != failIter; ++iter) {
                        // Run through each step until our point of failure with
                        // FailureCondition::kNone.
                        ASSERT_EQ(iter->func(FailureCondition::kNone), getExpectedPostState(iter))
                            << "Current state: (" << iter->state << ", " << iter->mode << ")";
                    }

                    // Finally fail on a given step.
                    ASSERT_EQ(iter->func(fail), IngressState::kEnd);
                });
            }
        }

        _runCount += 1;
    }

private:
    ServiceStateMachineTest* const _fixture;

    boost::optional<ServiceStateMachineTest::IngressState> _finalState;
    StepList _steps;

    // This variable is currently used as a post-condition to make sure that the StepRunner has been
    // run. In the current form, it could be a boolean. That said, if you need to stress test the
    // ServiceStateMachine, you will want to check this variable to make sure you have run as many
    // times as you expect.
    size_t _runCount = 0;
};

void ServiceStateMachineTest::initNewSession() {
    assertNoIngressState();

    _session = std::make_shared<Session>(this);
    _data->isConnected.store(true);
}

void ServiceStateMachineTest::joinSession() {
    ASSERT(_sep->waitForNoSessions(Seconds{1}));

    assertNoIngressState();
}

void ServiceStateMachineTest::startSession() {
    _sep->startSession(_session);
}

void ServiceStateMachineTest::terminateViaServiceEntryPoint() {
    _sep->endAllSessionsNoTagMask();
}

void ServiceStateMachineTest::setUp() {
    ServiceContextTest::setUp();

    if (_threadingModel) {
        _originalThreadingModel = ServiceExecutor::getInitialThreadingModel();
        ServiceExecutor::setInitialThreadingModel(*_threadingModel);
    }

    auto sep = std::make_unique<ServiceStateMachineTest::ServiceEntryPoint>(this);
    _sep = sep.get();
    getServiceContext()->setServiceEntryPoint(std::move(sep));
    invariant(_sep->start());

    _threadPool->startup();

    _data = std::make_unique<Data>();
}

void ServiceStateMachineTest::tearDown() {
    ON_BLOCK_EXIT([&] { ServiceContextTest::tearDown(); });

    endSession();

    // Normal shutdown is a noop outside of ASAN.
    invariant(_sep->shutdownAndWait(Seconds{10}));

    _threadPool->shutdown();
    _threadPool->join();

    if (_originalThreadingModel) {
        ServiceExecutor::setInitialThreadingModel(*_originalThreadingModel);
    }
}

class ServiceStateMachineWithDedicatedThreadsTest : public ServiceStateMachineTest {
public:
    ServiceStateMachineWithDedicatedThreadsTest()
        : ServiceStateMachineTest(ServiceExecutor::ThreadingModel::kDedicated) {}
};

class ServiceStateMachineWithBorrowedThreadsTest : public ServiceStateMachineTest {
public:
    ServiceStateMachineWithBorrowedThreadsTest()
        : ServiceStateMachineTest(ServiceExecutor::ThreadingModel::kBorrowed) {}
};

TEST_F(ServiceStateMachineTest, StartThenEndSession) {
    initNewSession();
    startSession();

    ASSERT_EQ(popIngressState(), IngressState::kSource);

    endSession();
}

TEST_F(ServiceStateMachineTest, EndBeforeStartSession) {
    initNewSession();
    endSession();
    startSession();
}

TEST_F(ServiceStateMachineTest, OnClientDisconnectCalledOnCleanup) {
    initNewSession();
    startSession();
    ASSERT_EQ(popIngressState(), IngressState::kSource);
    ASSERT_EQ(onClientDisconnectCalledTimes(), 0);
    endSession();
    ASSERT_EQ(popIngressState(), IngressState::kEnd);
    joinSession();
    ASSERT_EQ(onClientDisconnectCalledTimes(), 1);
}

TEST_F(ServiceStateMachineWithDedicatedThreadsTest, DefaultLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState<IngressState::kSource, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kProcess, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kSink, IngressMode::kDefault>();
    runner.expectFinalState<IngressState::kSource>();

    runner.run();
}

TEST_F(ServiceStateMachineWithDedicatedThreadsTest, ExhaustLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState<IngressState::kSource, IngressMode::kExhaust>();
    runner.expectNextState<IngressState::kProcess, IngressMode::kExhaust>();
    runner.expectNextState<IngressState::kSink, IngressMode::kExhaust>();
    runner.expectNextState<IngressState::kProcess, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kSink, IngressMode::kDefault>();
    runner.expectFinalState<IngressState::kSource>();

    runner.run();
}

TEST_F(ServiceStateMachineWithDedicatedThreadsTest, MoreToComeLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState<IngressState::kSource, IngressMode::kMoreToCome>();
    runner.expectNextState<IngressState::kProcess, IngressMode::kMoreToCome>();
    runner.expectNextState<IngressState::kSource, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kProcess, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kSink, IngressMode::kDefault>();
    runner.expectFinalState<IngressState::kSource>();

    runner.run();
}

TEST_F(ServiceStateMachineWithBorrowedThreadsTest, DefaultLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState<IngressState::kPoll, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kSource, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kProcess, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kSink, IngressMode::kDefault>();
    runner.expectFinalState<IngressState::kPoll>();

    runner.run();
}

TEST_F(ServiceStateMachineWithBorrowedThreadsTest, ExhaustLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState<IngressState::kPoll, IngressMode::kExhaust>();
    runner.expectNextState<IngressState::kSource, IngressMode::kExhaust>();
    runner.expectNextState<IngressState::kProcess, IngressMode::kExhaust>();
    runner.expectNextState<IngressState::kSink, IngressMode::kExhaust>();
    runner.expectNextState<IngressState::kProcess, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kSink, IngressMode::kDefault>();
    runner.expectFinalState<IngressState::kPoll>();

    runner.run();
}

TEST_F(ServiceStateMachineWithBorrowedThreadsTest, MoreToComeLoop) {
    auto runner = StepRunner(this);

    runner.expectNextState<IngressState::kPoll, IngressMode::kMoreToCome>();
    runner.expectNextState<IngressState::kSource, IngressMode::kMoreToCome>();
    runner.expectNextState<IngressState::kProcess, IngressMode::kMoreToCome>();
    runner.expectNextState<IngressState::kPoll, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kSource, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kProcess, IngressMode::kDefault>();
    runner.expectNextState<IngressState::kSink, IngressMode::kDefault>();
    runner.expectFinalState<IngressState::kPoll>();

    runner.run();
}

}  // namespace
}  // namespace transport
}  // namespace mongo
