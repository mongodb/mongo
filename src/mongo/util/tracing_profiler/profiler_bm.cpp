// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/tracing_profiler/profiler.h"

#include <benchmark/benchmark.h>

namespace mongo::tracing_profiler {
using namespace mongo::tracing_profiler::internal;

class ProfilerServiceFixture : public benchmark::Fixture {
public:
    ProfilerServiceFixture() : _profiler(nullptr, {}) {}

    class ThreadLocalContext {
    public:
        ThreadLocalContext(ProfilerServiceFixture* fixture)
            : _fixture(fixture), _shard(_fixture->_profiler.createShard()) {}

        ProfilerServiceFixture* _fixture;
        Profiler::ShardUniquePtr _shard;
    };

private:
    Profiler _profiler;
};
BENCHMARK_DEFINE_F(ProfilerServiceFixture, BM_enterLeaveSpanX5Narrow)
(benchmark::State& state) {
    ThreadLocalContext ctx(this);
    ProfilerBenchmark b(ctx._shard.get());
    for (auto _ : state) {
        benchmark::DoNotOptimize(b.doX5Narrow());
    }
}
BENCHMARK_REGISTER_F(ProfilerServiceFixture, BM_enterLeaveSpanX5Narrow)->ThreadRange(1, 16);

BENCHMARK_DEFINE_F(ProfilerServiceFixture, BM_enterLeaveSpanX25Wide)
(benchmark::State& state) {
    ThreadLocalContext ctx(this);
    ProfilerBenchmark b(ctx._shard.get());
    for (auto _ : state) {
        benchmark::DoNotOptimize(b.doX25Wide());
    }
}
BENCHMARK_REGISTER_F(ProfilerServiceFixture, BM_enterLeaveSpanX25Wide)->ThreadRange(1, 16);


#if MONGO_CONFIG_USE_TRACING_PROFILER
BENCHMARK_DEFINE_F(ProfilerServiceFixture, BM_end2endX5Narrow)
(benchmark::State& state) {
    for (auto _ : state) {
        auto s1 = MONGO_PROFILER_SPAN_ENTER("span1");

        auto s2 = MONGO_PROFILER_SPAN_ENTER("span2");
        auto s3 = MONGO_PROFILER_SPAN_ENTER("span3");
        MONGO_PROFILER_SPAN_LEAVE(s3);
        MONGO_PROFILER_SPAN_LEAVE(s2);

        auto s4 = MONGO_PROFILER_SPAN_ENTER("span4");
        auto s5 = MONGO_PROFILER_SPAN_ENTER("span5");
        MONGO_PROFILER_SPAN_LEAVE(s5);
        MONGO_PROFILER_SPAN_LEAVE(s4);

        MONGO_PROFILER_SPAN_LEAVE(s1);
    }
}
BENCHMARK_REGISTER_F(ProfilerServiceFixture, BM_end2endX5Narrow)->ThreadRange(1, 16);
#endif

}  // namespace mongo::tracing_profiler
