/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/query/query_knob_configuration.h"

#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/processinfo.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
const int kMaxThreads = 1;
#else
const int kMaxThreads = 2 * ProcessInfo::getNumLogicalCores();
#endif

// ---------------------------------------------------------------------------
// Settings policies — control how QuerySettings are configured.
// ---------------------------------------------------------------------------

struct NoPQS {
    static constexpr const char* kName = "NoPQS";
    static query_settings::QuerySettings make() {
        return {};
    }
};

struct OneOverride {
    static constexpr const char* kName = "OneOverride";
    static query_settings::QuerySettings make() {
        query_settings::QuerySettings qs;
        qs.setQueryFramework(QueryFrameworkControlEnum::kTrySbeEngine);
        return qs;
    }
};

// TODO(SERVER-123182): Add MaxOverride once QuerySettingsKnobOverlay lands.

// ---------------------------------------------------------------------------
// Workload policies — control what runs in the hot loop.
// ---------------------------------------------------------------------------

struct GetBool {
    static constexpr const char* kName = "Bool";
    static void run(const QueryKnobConfiguration& c) {
        benchmark::DoNotOptimize(c.getSbeDisableGroupPushdownForOp());
    }
};

struct GetInt64 {
    static constexpr const char* kName = "Int64";
    static void run(const QueryKnobConfiguration& c) {
        benchmark::DoNotOptimize(c.getMaxGroupAccumulatorsInSbe());
    }
};

struct GetSizeT {
    static constexpr const char* kName = "SizeT";
    static void run(const QueryKnobConfiguration& c) {
        benchmark::DoNotOptimize(c.getPlanEvaluationMaxResultsForOp());
    }
};

struct GetDouble {
    static constexpr const char* kName = "Double";
    static void run(const QueryKnobConfiguration& c) {
        benchmark::DoNotOptimize(c.getPlanEvaluationCollFraction());
    }
};

struct GetEnum {
    static constexpr const char* kName = "Enum";
    static void run(const QueryKnobConfiguration& c) {
        benchmark::DoNotOptimize(c.getInternalQueryFrameworkControlForOp());
    }
};

struct AllGetters {
    static constexpr const char* kName = "AllGetters";
    static void run(const QueryKnobConfiguration& c) {
        GetBool::run(c);
        GetInt64::run(c);
        GetSizeT::run(c);
        GetDouble::run(c);
        GetEnum::run(c);
    }
};

// ---------------------------------------------------------------------------
// Fixture — parameterized by SettingsPolicy and WorkloadPolicy.
// ---------------------------------------------------------------------------

template <typename SettingsPolicy, typename WorkloadPolicy>
class QKCBenchmark : public benchmark::Fixture {
public:
    void SetUp(benchmark::State&) override {
        stdx::lock_guard lk(_mutex);
        if (!_threads++) {
            QueryFCVEnvironmentForTest::setUp();
            _serviceContext = std::make_unique<QueryTestServiceContext>();
        }
    }

    void TearDown(benchmark::State&) override {
        stdx::lock_guard lk(_mutex);
        if (!--_threads) {
            _serviceContext.reset();
        }
    }

    void run(benchmark::State& state) {
        auto qs = SettingsPolicy::make();
        for (auto _ : state) {
            QueryKnobConfiguration config(qs);
            WorkloadPolicy::run(config);
            benchmark::DoNotOptimize(config);
        }
    }

private:
    inline static stdx::mutex _mutex{};
    inline static size_t _threads = 0;
    inline static std::unique_ptr<QueryTestServiceContext> _serviceContext{};
};

// ---------------------------------------------------------------------------
// Registration macros
// ---------------------------------------------------------------------------

// Registers a single-threaded getter benchmark (one settings × one workload).
#define GETTER_BM(Settings, Workload)                                                             \
    BENCHMARK_TEMPLATE_DEFINE_F(QKCBenchmark, Getter_##Settings##_##Workload, Settings, Workload) \
    (benchmark::State & state) {                                                                  \
        run(state);                                                                               \
    }                                                                                             \
    BENCHMARK_REGISTER_F(QKCBenchmark, Getter_##Settings##_##Workload);

// Registers a threaded E2E benchmark (one settings × one workload).
#define E2E_BM(Settings, Workload)                                                             \
    BENCHMARK_TEMPLATE_DEFINE_F(QKCBenchmark, E2E_##Settings##_##Workload, Settings, Workload) \
    (benchmark::State & state) {                                                               \
        run(state);                                                                            \
    }                                                                                          \
    BENCHMARK_REGISTER_F(QKCBenchmark, E2E_##Settings##_##Workload)->ThreadRange(1, kMaxThreads);

// ---------------------------------------------------------------------------
// Getter benchmarks: one per (NoPQS x getter type). Isolate per getter type,
// so a regression points to a specific type.
// ---------------------------------------------------------------------------

GETTER_BM(NoPQS, GetBool)
GETTER_BM(NoPQS, GetInt64)
GETTER_BM(NoPQS, GetSizeT)
GETTER_BM(NoPQS, GetDouble)
GETTER_BM(NoPQS, GetEnum)

// ---------------------------------------------------------------------------
// E2E benchmarks: threaded, construct + read all getters, with and without a
// QuerySettings override. Measure the realistic hot-path cost and how it
// scales under contention.
// ---------------------------------------------------------------------------

E2E_BM(NoPQS, AllGetters)
E2E_BM(OneOverride, AllGetters)

#undef GETTER_BM
#undef E2E_BM

}  // namespace
}  // namespace mongo
