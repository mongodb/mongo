/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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
