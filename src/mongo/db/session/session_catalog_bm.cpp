// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/session/session_catalog.h"

#include "mongo/db/client.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/kill_sessions.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_catalog_gen.h"
#include "mongo/db/session/session_killer.h"
#include "mongo/platform/atomic.h"
#include "mongo/unittest/benchmark_util.h"
#include "mongo/util/str.h"
#include "mongo/util/system_clock_source.h"
#include "mongo/util/tick_source_mock.h"

#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

class SessionCatalogBm : public unittest::BenchmarkWithProfiler {
public:
    // Both benchmarks take 'Sessions' as arg 0 and 'Partitions' as arg 1.
    void setUpSharedResources(benchmark::State& state) override {
        BenchmarkWithProfiler::setUpSharedResources(state);

        const auto numSessions = state.range(0);

        // The catalog reads the partition count at construction, so this must be set before the
        // ServiceContext (and its SessionCatalog decoration) is created.
        gSessionCatalogPartitions = state.range(1);

        _serviceContext = ServiceContext::make(std::make_unique<SystemClockSource>(),
                                               std::make_unique<SystemClockSource>(),
                                               std::make_unique<TickSourceMock<Microseconds>>());

        _sessionIds.clear();
        _sessionIds.reserve(numSessions);
        for (int64_t i = 0; i < numSessions; ++i) {
            _sessionIds.push_back(makeLogicalSessionIdForTest());
        }

        for (auto& lsid : _sessionIds) {
            catalog()->scanSession(
                lsid,
                [](const ObservableSession&) {},
                SessionCatalog::ScanSessionCreateSession::kYes);
        }
    }

    void tearDownSharedResources(benchmark::State& state) override {
        _sessionIds.clear();
        _serviceContext.reset();
        BenchmarkWithProfiler::tearDownSharedResources(state);
    }

protected:
    SessionCatalog* catalog() {
        return SessionCatalog::get(_serviceContext.get());
    }

    Service* service() {
        return _serviceContext->getService();
    }

    static SessionKiller::Matcher makeMatcherAllSessions() {
        KillAllSessionsByPattern pattern;
        return SessionKiller::Matcher(KillAllSessionsByPatternSet{{pattern, APIParameters()}});
    }

    ServiceContext::UniqueServiceContext _serviceContext;
    std::vector<LogicalSessionId> _sessionIds;
};

// Models the hot path: each command creates an opCtx, checks out its session, does work, checks
// back in. Each thread has its own dedicated session.
BENCHMARK_DEFINE_F(SessionCatalogBm, CheckoutCheckin)(benchmark::State& state) {
    auto client = service()->makeClient(str::stream() << "conn_" << state.thread_index);
    auto& lsid = _sessionIds[state.thread_index % _sessionIds.size()];

    int64_t ops = 0;
    runBenchmarkWithProfiler(
        [&] {
            auto opCtx = client->makeOperationContext();
            opCtx->setLogicalSessionId(lsid);
            OperationContextSession ocs(opCtx.get());
            ++ops;
        },
        state);

    state.counters["Throughput"] = benchmark::Counter(ops, benchmark::Counter::kIsRate);
    state.counters["ThroughputPerThread"] =
        benchmark::Counter(ops, benchmark::Counter::kAvgThreadsRate);

    client.reset();
}

// Models the scan convoy scenario. Runs as a single-threaded benchmark that internally
// spawns N scanner threads and M checkout threads, then measures aggregate checkout throughput
// over a fixed duration. This avoids Google Benchmark's calibration phase, which hangs when
// checkout threads are fully starved by the single-mutex convoy.
BENCHMARK_DEFINE_F(SessionCatalogBm, CheckoutDuringScan)(benchmark::State& state) {
    const auto numScanners = state.range(2);
    const auto numCheckoutThreads = state.range(3);
    const auto durationSec = state.range(4);

    for (auto _ : state) {
        Atomic<bool> stop{false};
        Atomic<int64_t> totalCheckoutOps{0};

        // Launch scanner threads.
        auto matcher = makeMatcherAllSessions();
        std::vector<std::thread> scanners;
        scanners.reserve(numScanners);
        for (int i = 0; i < numScanners; ++i) {
            scanners.emplace_back([this, &matcher, &stop]() {
                while (!stop.loadRelaxed()) {
                    catalog()->scanSessions(
                        matcher, [](const ObservableSession&) { benchmark::ClobberMemory(); });
                }
            });
        }

        // Launch checkout threads.
        std::vector<std::thread> checkoutThreads;
        checkoutThreads.reserve(numCheckoutThreads);
        for (int i = 0; i < numCheckoutThreads; ++i) {
            checkoutThreads.emplace_back([this, &stop, &totalCheckoutOps, i]() {
                auto client = service()->makeClient(str::stream() << "checkout_" << i);
                auto& lsid = _sessionIds[i % _sessionIds.size()];
                int64_t ops = 0;
                while (!stop.loadRelaxed()) {
                    auto opCtx = client->makeOperationContext();
                    opCtx->setLogicalSessionId(lsid);
                    OperationContextSession ocs(opCtx.get());
                    ++ops;
                }
                totalCheckoutOps.fetchAndAdd(ops);
            });
        }

        // Run for the specified duration.
        std::this_thread::sleep_for(std::chrono::seconds(durationSec));
        stop.store(true);

        for (auto& t : checkoutThreads)
            t.join();
        for (auto& t : scanners)
            t.join();

        state.counters["CheckoutOpsPerSec"] =
            benchmark::Counter(static_cast<double>(totalCheckoutOps.load()) / durationSec,
                               benchmark::Counter::kDefaults);
    }
}

// Checkout/checkin at production-scale session counts. Tests hot-path scalability in isolation
// (no concurrent scans).
BENCHMARK_REGISTER_F(SessionCatalogBm, CheckoutCheckin)
    ->ArgNames({"Sessions", "Partitions"})
    ->Args({1000000, 64})
    ->Threads(1)
    ->Threads(4)
    ->Threads(16)
    ->Threads(32);

// Partition count sweep for the uncontended hot path (16 threads), over the range
// 'sessionCatalogPartitions' accepts.
BENCHMARK_REGISTER_F(SessionCatalogBm, CheckoutCheckin)
    ->ArgNames({"Sessions", "Partitions"})
    ->Args({1000000, 1})
    ->Args({1000000, 4})
    ->Args({1000000, 16})
    ->Args({1000000, 32})
    ->Args({1000000, 64})
    ->Threads(16);

// The convoy scenario: N background scanners, M checkout threads, timed for D seconds, at the
// maximum partition count. 0 scanners is the no-interference comparison point; 1, 4, and 16
// scanners model increasingly heavy $currentOp load.
BENCHMARK_REGISTER_F(SessionCatalogBm, CheckoutDuringScan)
    ->ArgNames({"Sessions", "Partitions", "Scanners", "CheckoutThreads", "DurationSec"})
    ->Args({1000000, 64, 0, 16, 5})
    ->Args({1000000, 64, 1, 16, 5})
    ->Args({1000000, 64, 4, 16, 5})
    ->Args({1000000, 64, 16, 16, 5})
    ->Iterations(1);

// Partition count sweep under the heavy convoy (16 scanners + 16 checkout threads).
BENCHMARK_REGISTER_F(SessionCatalogBm, CheckoutDuringScan)
    ->ArgNames({"Sessions", "Partitions", "Scanners", "CheckoutThreads", "DurationSec"})
    ->Args({1000000, 1, 16, 16, 5})
    ->Args({1000000, 4, 16, 16, 5})
    ->Args({1000000, 16, 16, 16, 5})
    ->Args({1000000, 32, 16, 16, 5})
    ->Args({1000000, 64, 16, 16, 5})
    ->Iterations(1);

}  // namespace
}  // namespace mongo
