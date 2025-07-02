/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/admission/ingress_request_rate_limiter.h"
#include "mongo/db/admission/ingress_request_rate_limiter_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/message.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/asio/asio_session_manager.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/service_executor_synchronous.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_workflow_test_util.h"
#include "mongo/transport/test_fixtures.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager_impl.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/time_support.h"

#include <array>
#include <cstddef>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

#include <benchmark/benchmark.h>
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kExecutor

namespace mongo::transport {
namespace {

/** For troubleshooting the benchmark. */
constexpr bool enableInstrumentation = false;

/** Benchmarks can't do this with command line flags like unit tests can. */
void initializeInstrumentation() {
    constexpr auto kLogLevel =
        enableInstrumentation ? logv2::LogSeverity::Debug(4) : logv2::LogSeverity::Warning();
    std::array components = {
        std::pair{logv2::LogComponent::kExecutor, kLogLevel},
        std::pair{logv2::LogComponent::kNetwork, kLogLevel},
    };
    for (auto&& [comp, sev] : components)
        logv2::LogManager::global().getGlobalSettings().setMinimumLoggedSeverity(comp, sev);
    for (auto&& [comp, sev] : components)
        invariant(logv2::shouldLog(comp, sev));
}

Status makeClosedSessionError() {
    return Status{ErrorCodes::SocketException, "Session is closed"};
}

/**
 * Coordinate between a mock Session and ServiceEntryPoint to implement
 * a prescribed number of exhaust rounds.
 */
class MockCoordinator {
public:
    MockCoordinator(ServiceContext* sc, int rounds) : _sc{sc}, _rounds{rounds} {}

    class Session : public CallbackMockSession {
    public:
        explicit Session(MockCoordinator* mc) : _mc{mc} {
            LOGV2_DEBUG(7015130, 3, "MockCoordinator::Session ctor");
        }
        ~Session() override {
            LOGV2_DEBUG(7015131, 3, "MockCoordinator::Session dtor");
        }

        void end() override {
            _observeEnd.promise.emplaceValue();
        }
        Status waitForData() override {
            return Status::OK();
        }
        Status sinkMessage(Message) override {
            return Status::OK();
        }
        Future<void> asyncWaitForData() override {
            return {};
        }
        StatusWith<Message> sourceMessage() override {
            LOGV2_DEBUG(7015132, 3, "sourceMessage", "rounds"_attr = _rounds);
            if (!_rounds)
                return makeClosedSessionError();

            // If only one round is left, we don't have more exhaust loops to fulfill.
            // Decrement the number of rounds to allow the session to close before reaching
            // handleRequest.
            if (_rounds == 1) {
                _rounds--;
            }

            return _request;
        }

        /** Return a future that is ready when this session is ended. */
        Future<void> observeEnd() {
            return std::move(_observeEnd.future);
        }

        int& rounds() {
            return _rounds;
        }

    private:
        static Message _makeRequest() {
            Message request = [&] {
                OpMsgBuilder builder;
                builder.beginBody().append("ping", 1);
                return builder.finish();
            }();
            OpMsg::setFlag(&request, OpMsg::kExhaustSupported);
            return request;
        }

        MockCoordinator* _mc;
        Message _request = _makeRequest();
        int _rounds = _mc->_rounds;
        PromiseAndFuture<void> _observeEnd;
    };

    class Sep : public MockServiceEntryPoint {
    public:
        explicit Sep(MockCoordinator* mc) : MockServiceEntryPoint(), _mc{mc} {}

        Future<DbResponse> handleRequest(OperationContext* opCtx, const Message& request) override {
            DbResponse response;
            response.response = request;

            auto session = _mc->opCtxToSession(opCtx);
            invariant(session);
            if (auto& rounds = session->rounds()) {
                response.nextInvocation = BSONObjBuilder{}.append("ping", 1).obj();
                response.shouldRunAgainForExhaust = true;
                rounds--;
            }
            return Future{std::move(response)};
        }

    private:
        MockCoordinator* _mc;
    };

    Session* opCtxToSession(OperationContext* opCtx) const {
        return dynamic_cast<Session*>(opCtx->getClient()->session().get());
    }


    std::shared_ptr<Session> makeSession() {
        return std::make_shared<Session>(this);
    }

private:
    ServiceContext* _sc;
    int _rounds = 0;
};

class SessionWorkflowBm : public benchmark::Fixture {
public:
    SessionWorkflowBm() {
        initializeInstrumentation();
        LOGV2_DEBUG(7015133, 3, "SessionWorkflowBm ctor");
    }

    void SetUp(benchmark::State& state) override {
        stdx::lock_guard lk{_setupMutex};
        LOGV2_DEBUG(7015134, 3, "SetUp", "configuredThreads"_attr = _configuredThreads);
        if (_configuredThreads++)
            return;

        size_t argIndex = 0;
        int exhaustRounds = state.range(argIndex++);
        int reserved = state.range(argIndex++);

        LOGV2_DEBUG(7015135,
                    3,
                    "SetUp (first)",
                    "exhaustRounds"_attr = exhaustRounds,
                    "reserved"_attr = reserved);

#if TRANSITIONAL_SERVICE_EXECUTOR_SYNCHRONOUS_HAS_RESERVE
        _savedDefaultReserved.emplace(ServiceExecutorSynchronous::defaultReserved, reserved);
#endif
        setGlobalServiceContext(ServiceContext::make());
        auto sc = getGlobalServiceContext();
        _coordinator = std::make_unique<MockCoordinator>(sc, exhaustRounds + 1);
        sc->getService()->setServiceEntryPoint(
            std::make_unique<MockCoordinator::Sep>(_coordinator.get()));
        _initTransportLayerManager(sc);

        doConfigureServerParameters(sc);
    }

    virtual void doConfigureServerParameters(ServiceContext*) {}

    void _initTransportLayerManager(ServiceContext* svcCtx) {
        auto sm = std::make_unique<AsioSessionManager>(svcCtx);
        _sessionManager = sm.get();
        auto tl = std::make_unique<test::TransportLayerMockWithReactor>(std::move(sm));
        _transportLayer = tl.get();
        svcCtx->setTransportLayerManager(
            std::make_unique<transport::TransportLayerManagerImpl>(std::move(tl)));
        LOGV2_DEBUG(7015136, 3, "About to start sep");
        invariant(svcCtx->getTransportLayerManager()->setup());
        invariant(svcCtx->getTransportLayerManager()->start());
        ServiceExecutor::startupAll(svcCtx);
    }

    AsioSessionManager* sessionManager() const {
        return _sessionManager;
    }

    void TearDown(benchmark::State& state) override {
        stdx::lock_guard lk{_setupMutex};
        LOGV2_DEBUG(7015137, 3, "TearDown", "configuredThreads"_attr = _configuredThreads);
        if (--_configuredThreads)
            return;
        LOGV2_DEBUG(7015138, 3, "TearDown (last)");
        getGlobalServiceContext()->getTransportLayerManager()->shutdown();
        ServiceExecutor::shutdownAll(getGlobalServiceContext(), Seconds(1));
        setGlobalServiceContext({});
        _savedDefaultReserved.reset();
    }

    void run(benchmark::State& state) {
        for (auto _ : state) {
            LOGV2_DEBUG(7015139, 3, "run: iteration start");
            auto session = _coordinator->makeSession();
            invariant(session);
            session->getTransportLayerCb = [this] {
                return _transportLayer;
            };
            Future<void> ended = session->observeEnd();
            sessionManager()->startSession(session);
            ended.get();
            invariant(session->rounds() == 0);
        }
        LOGV2_DEBUG(7015140, 3, "run: all iterations finished");
        invariant(sessionManager()->waitForNoSessions(Seconds{1}));
    }

private:
    stdx::mutex _setupMutex;
    int _configuredThreads = 0;
    boost::optional<ScopedValueOverride<size_t>> _savedDefaultReserved;
    std::unique_ptr<MockCoordinator> _coordinator;
    AsioSessionManager* _sessionManager;
    test::TransportLayerMockWithReactor* _transportLayer{nullptr};
};

/**
 * This benchmark enables the ingress request rate limiter, which is set by default to accept all
 * requests.
 */
class SessionWorkflowRateLimitAcceptAllBm : public SessionWorkflowBm {
    void doConfigureServerParameters(ServiceContext* sc) override {
        gFeatureFlagIngressRateLimiting.setForServerParameter(true);
        gIngressRequestRateLimiterEnabled.store(true);
    }
};

/**
 * This benchmark enables the ingress request rate limiter, and also sets parameters such that
 * every request will get rejected.
 */
class SessionWorkflowRateLimitRejectAllBm : public SessionWorkflowBm {
    void doConfigureServerParameters(ServiceContext* sc) override {
        gFeatureFlagIngressRateLimiting.setForServerParameter(true);
        gIngressRequestRateLimiterEnabled.store(true);

        auto& rateLimiter = IngressRequestRateLimiter::get(getGlobalServiceContext());

        auto const verySlowRatePerSec = 5e-6;
        auto const smallestPossibleBurstSize = 1.0;
        auto const smallestPossibleBurstCapacitySecs =
            smallestPossibleBurstSize / verySlowRatePerSec;
        rateLimiter.updateRateParameters(verySlowRatePerSec, smallestPossibleBurstCapacitySecs);
    }
};

/**
 * ASAN can't handle the # of threads the benchmark creates.
 * With sanitizers, run this in a diminished "correctness check" mode.
 */
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
const auto kMaxThreads = 1;
constexpr std::array exhaustRounds{0};
#else
/** 2x to benchmark the case of more threads than cores for curiosity's sake. */
const auto kMaxThreads = 2 * ProcessInfo::getNumLogicalCores();
constexpr std::array exhaustRounds{0, 1, 8};
#endif

static void benchmarkExhaustInnerLoop(benchmark::internal::Benchmark* b, int exhaust) {
    std::vector<int> res{0};
#if TRANSITIONAL_SERVICE_EXECUTOR_SYNCHRONOUS_HAS_RESERVE
    res = {0, 1, 4, 16};
#endif
    for (int reserved : res)
        b->Args({exhaust, reserved});
}

static void benchmarkLoop(benchmark::internal::Benchmark* b) {
    b->ArgNames({"ExhaustRounds", "ReservedThreads"});
    for (int exhaust : exhaustRounds) {
        benchmarkExhaustInnerLoop(b, exhaust);
    }
    b->ThreadRange(1, kMaxThreads);
}

static void benchmarkLoopNoExhaust(benchmark::internal::Benchmark* b) {
    b->ArgNames({"ExhaustRounds", "ReservedThreads"});
    benchmarkExhaustInnerLoop(b, 0);
    b->ThreadRange(1, kMaxThreads);
}

BENCHMARK_DEFINE_F(SessionWorkflowBm, Loop)(benchmark::State& state) {
    run(state);
}
BENCHMARK_REGISTER_F(SessionWorkflowBm, Loop)->Apply(benchmarkLoop);

BENCHMARK_DEFINE_F(SessionWorkflowRateLimitAcceptAllBm, Loop)(benchmark::State& state) {
    run(state);
}
BENCHMARK_REGISTER_F(SessionWorkflowRateLimitAcceptAllBm, Loop)->Apply(benchmarkLoop);

BENCHMARK_DEFINE_F(SessionWorkflowRateLimitRejectAllBm, Loop)(benchmark::State& state) {
    run(state);
}
BENCHMARK_REGISTER_F(SessionWorkflowRateLimitRejectAllBm, Loop)->Apply(benchmarkLoopNoExhaust);

}  // namespace
}  // namespace mongo::transport
