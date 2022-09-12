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

#include <chrono>
#include <memory>

#include <benchmark/benchmark.h>

#include "mongo/bson/bsonelement.h"
#include "mongo/db/concurrency/locker_noop_client_observer.h"
#include "mongo/db/dbmessage.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/transport/mock_service_executor.h"
#include "mongo/transport/service_entry_point_impl.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_workflow_test_util.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/assert_util_core.h"
#include "mongo/util/out_of_line_executor.h"
#include "mongo/util/processinfo.h"

namespace mongo::transport {
namespace {

Status makeClosedSessionError() {
    return Status{ErrorCodes::SocketException, "Session is closed"};
}

class NoopReactor : public Reactor {
public:
    void run() noexcept override {}
    void stop() override {}

    void runFor(Milliseconds time) noexcept override {
        MONGO_UNREACHABLE;
    }

    void drain() override {
        MONGO_UNREACHABLE;
    }

    void schedule(Task) override {
        MONGO_UNREACHABLE;
    }

    void dispatch(Task) override {
        MONGO_UNREACHABLE;
    }

    bool onReactorThread() const override {
        MONGO_UNREACHABLE;
    }

    std::unique_ptr<ReactorTimer> makeTimer() override {
        MONGO_UNREACHABLE;
    }

    Date_t now() override {
        MONGO_UNREACHABLE;
    }

    void appendStats(BSONObjBuilder&) const {
        MONGO_UNREACHABLE;
    }
};

class TransportLayerMockWithReactor : public TransportLayerMock {
public:
    ReactorHandle getReactor(WhichReactor) override {
        return _mockReactor;
    }

private:
    ReactorHandle _mockReactor = std::make_unique<NoopReactor>();
};

Message makeMessageWithBenchmarkRunNumber(int runNumber) {
    OpMsgBuilder builder;
    builder.setBody(BSON("ping" << 1 << "benchmarkRunNumber" << runNumber));
    Message request = builder.finish();
    OpMsg::setFlag(&request, OpMsg::kExhaustSupported);
    return request;
}

std::shared_ptr<CallbackMockSession> makeSession(Message message) {
    auto session = std::make_shared<CallbackMockSession>();
    session = std::make_shared<CallbackMockSession>();
    session->endCb = [] {};
    session->waitForDataCb = [&] { return Status::OK(); };
    session->sourceMessageCb = [&, message] { return StatusWith<Message>(message); };
    session->sinkMessageCb = [&](Message message) {
        if (OpMsg::parse(message).body["benchmarkRunNumber"].numberInt() > 0)
            return Status::OK();
        return makeClosedSessionError();
    };
    session->asyncWaitForDataCb = [&] { return Future<void>::makeReady(); };
    return session;
}

class SessionWorkflowFixture : public benchmark::Fixture {
public:
    Future<DbResponse> handleRequest(const Message& request) {
        DbResponse response;
        response.response = request;

        BSONObj obj = OpMsg::parse(request).body;

        // Check "benchmarkRunNumber" field for how many times to run in exhaust
        if (obj["benchmarkRunNumber"].numberInt() > 0) {
            BSONObjBuilder bsonBuilder;
            bsonBuilder.append(obj.firstElement());
            bsonBuilder.append("benchmarkRunNumber", obj["benchmarkRunNumber"].numberInt() - 1);
            BSONObj newObj = bsonBuilder.obj();

            response.response = request;
            response.nextInvocation = newObj;
            response.shouldRunAgainForExhaust = true;
        }

        return Future<DbResponse>::makeReady(StatusWith<DbResponse>(response));
    }

    void commonSetUp() {
        serviceCtx = [] {
            auto serviceContext = ServiceContext::make();
            auto serviceContextPtr = serviceContext.get();
            setGlobalServiceContext(std::move(serviceContext));
            return serviceContextPtr;
        }();
        invariant(serviceCtx);
        serviceCtx->registerClientObserver(std::make_unique<LockerNoopClientObserver>());

        auto uniqueSep = std::make_unique<MockServiceEntryPoint>(serviceCtx);
        uniqueSep->handleRequestCb = [&](OperationContext*, const Message& request) {
            return handleRequest(request);
        };
        uniqueSep->onEndSessionCb = [&](const SessionHandle&) {};
        uniqueSep->derivedOnClientDisconnectCb = [&](Client*) {};
        sep = uniqueSep.get();

        serviceCtx->setServiceEntryPoint(std::move(uniqueSep));
        serviceCtx->setTransportLayer(std::make_unique<TransportLayerMockWithReactor>());
    }

    void SetUp(benchmark::State& state) override {
        // Call SetUp on only one thread
        if (state.thread_index != 0)
            return;
        commonSetUp();
        makeSessionCb = makeSession;
        invariant(sep->start());
    }

    void TearDown(benchmark::State& state) override {
        if (state.thread_index != 0)
            return;
        invariant(sep->shutdownAndWait(Seconds{10}));
        setGlobalServiceContext({});
    }

    void benchmarkScheduleNewLoop(benchmark::State& state) {
        int64_t totalExhaustRounds = state.range(0);

        for (auto _ : state) {
            auto session = makeSessionCb(makeMessageWithBenchmarkRunNumber(totalExhaustRounds));
            sep->startSession(std::move(session));

            invariant(sep->waitForNoSessions(Seconds{1}));
        }
    }

    ServiceContext* serviceCtx;
    MockServiceEntryPoint* sep;
    std::function<std::shared_ptr<CallbackMockSession>(Message)> makeSessionCb;
};

template <bool useDedicatedThread>
class DedicatedThreadOverrideFixture : public SessionWorkflowFixture {
    ScopedValueOverride<bool> _svo{gInitialUseDedicatedThread, useDedicatedThread};
};

using SessionWorkflowWithBorrowedThreads = DedicatedThreadOverrideFixture<false>;
using SessionWorkflowWithDedicatedThreads = DedicatedThreadOverrideFixture<true>;

enum class StageToStop {
    kDefault,
    kSource,
    kProcess,
    kSink,
};

class SingleThreadSessionWorkflow : public SessionWorkflowFixture {
public:
    void initializeMockExecutor(benchmark::State& state) {
        serviceExecutor = std::make_unique<MockServiceExecutor>();
        serviceExecutor->runOnDataAvailableCb = [&](const SessionHandle& session,
                                                    OutOfLineExecutor::Task callback) {
            serviceExecutor->schedule(
                [callback = std::move(callback)](Status status) { callback(status); });
        };
        serviceExecutor->getRunningThreadsCb = [&] { return 0; };
        serviceExecutor->scheduleTaskCb = [&](ServiceExecutor::Task task,
                                              MockServiceExecutor::ScheduleFlags) {
            task();
            return Status::OK();
        };
    }

    void SetUp(benchmark::State& state) override {
        invariant(state.threads == 1, "Environment must be single threaded");
        auto stopAt = static_cast<StageToStop>(state.range(1));

        commonSetUp();

        // Configure SEP to use mock executor
        sep->configureServiceExecutorContextCb = [&](ServiceContext::UniqueClient& client, bool) {
            auto seCtx =
                std::make_unique<ServiceExecutorContext>([&] { return serviceExecutor.get(); });
            stdx::lock_guard lk(*client);
            ServiceExecutorContext::set(&*client, std::move(seCtx));
        };
        initializeMockExecutor(state);

        // Change callbacks so that the benchmark stops at the right stage
        if (stopAt == StageToStop::kProcess)
            sep->handleRequestCb = [&, stopAt](OperationContext* opCtx, const Message& request) {
                if (OpMsg::parse(request).body["benchmarkRunNumber"].numberInt() > 0)
                    return handleRequest(request);
                return Future<DbResponse>::makeReady(makeClosedSessionError());
            };
        makeSessionCb = [stopAt](Message message) {
            auto session = makeSession(message);
            if (stopAt == StageToStop::kSource)
                session->sourceMessageCb = [&, message] {
                    return StatusWith<Message>(makeClosedSessionError());
                };
            return session;
        };
    }

    std::unique_ptr<MockServiceExecutor> serviceExecutor;
};

const int64_t benchmarkThreadMax = ProcessInfo::getNumAvailableCores() * 2;

BENCHMARK_DEFINE_F(SessionWorkflowWithDedicatedThreads, MultiThreadScheduleNewLoop)
(benchmark::State& state) {
    benchmarkScheduleNewLoop(state);
}
BENCHMARK_REGISTER_F(SessionWorkflowWithDedicatedThreads, MultiThreadScheduleNewLoop)
    ->ArgNames({"Exhaust"})
    ->Arg(0)
    ->Arg(1)
    ->ThreadRange(1, benchmarkThreadMax);

BENCHMARK_DEFINE_F(SessionWorkflowWithBorrowedThreads, MultiThreadScheduleNewLoop)
(benchmark::State& state) {
    benchmarkScheduleNewLoop(state);
}
BENCHMARK_REGISTER_F(SessionWorkflowWithBorrowedThreads, MultiThreadScheduleNewLoop)
    ->ArgNames({"Exhaust"})
    ->Arg(0)
    ->Arg(1)
    ->ThreadRange(1, benchmarkThreadMax);

template <typename... E>
auto enumToArgs(E... e) {
    return std::vector<int64_t>{static_cast<int64_t>(e)...};
}

BENCHMARK_DEFINE_F(SingleThreadSessionWorkflow, SingleThreadScheduleNewLoop)
(benchmark::State& state) {
    benchmarkScheduleNewLoop(state);
}
BENCHMARK_REGISTER_F(SingleThreadSessionWorkflow, SingleThreadScheduleNewLoop)
    ->ArgNames({"Exhaust", "Stage to stop"})
    ->ArgsProduct({{0},
                   enumToArgs(StageToStop::kSource, StageToStop::kProcess, StageToStop::kSink)});

}  // namespace
}  // namespace mongo::transport
