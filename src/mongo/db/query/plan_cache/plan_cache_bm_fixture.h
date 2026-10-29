// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/util/modules.h"

#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {

class PlanCacheBenchmarkFixture : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) final {
        QueryFCVEnvironmentForTest::setUp();
    }

    virtual void benchmarkQueryMatchProject(benchmark::State& state,
                                            BSONObj matchSpec,
                                            BSONObj projectSpec) = 0;
    virtual void benchmarkPipeline(benchmark::State& state,
                                   const std::vector<BSONObj>& pipeline) = 0;

    void benchmarkMatch(benchmark::State& state);
    void benchmarkMatchTwoFields(benchmark::State& state);
    void benchmarkMatchTwentyFields(benchmark::State& state);

    void benchmarkMatchDepthTwo(benchmark::State& state);
    void benchmarkMatchDepthTwenty(benchmark::State& state);

    void benchmarkMatchGtLt(benchmark::State& state);
    void benchmarkMatchIn(benchmark::State& state);
    void benchmarkMatchInLarge(benchmark::State& state);
    void benchmarkMatchSize(benchmark::State& state);
    void benchmarkMatchElemMatch(benchmark::State& state);
    void benchmarkMatchComplex(benchmark::State& state);

    void benchmarkProjectExclude(benchmark::State& state);
    void benchmarkProjectInclude(benchmark::State& state);
    void benchmarkProjectIncludeTwoFields(benchmark::State& state);
    void benchmarkProjectIncludeTwentyFields(benchmark::State& state);

    void benchmarkProjectIncludeDepthTwo(benchmark::State& state);
    void benchmarkProjectIncludeDepthTwenty(benchmark::State& state);

    void benchmarkMatchProjectExclude(benchmark::State& state);
    void benchmarkMatchProjectInclude(benchmark::State& state);
    void benchmarkMatchProjectIncludeTwoFields(benchmark::State& state);
    void benchmarkMatchProjectIncludeTwentyFields(benchmark::State& state);

    void benchmarkMatchProjectIncludeDepthTwo(benchmark::State& state);
    void benchmarkMatchProjectIncludeDepthTwenty(benchmark::State& state);

    void benchmarkOneStage(benchmark::State& state);
    void benchmarkTwoStages(benchmark::State& state);
    void benchmarkTwentyStages(benchmark::State& state);
};

#define BENCHMARK_QUERY_ENCODING(Fixture)                                             \
                                                                                      \
    BENCHMARK_F(Fixture, Match)(benchmark::State & state) {                           \
        benchmarkMatch(state);                                                        \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchTwoFields)(benchmark::State & state) {                  \
        benchmarkMatchTwoFields(state);                                               \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchTwentyFields)(benchmark::State & state) {               \
        benchmarkMatchTwentyFields(state);                                            \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchDepthTwo)(benchmark::State & state) {                   \
        benchmarkMatchDepthTwo(state);                                                \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchDepthTwenty)(benchmark::State & state) {                \
        benchmarkMatchDepthTwenty(state);                                             \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchGtLt)(benchmark::State & state) {                       \
        benchmarkMatchGtLt(state);                                                    \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchIn)(benchmark::State & state) {                         \
        benchmarkMatchIn(state);                                                      \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchInLarge)(benchmark::State & state) {                    \
        benchmarkMatchInLarge(state);                                                 \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchSize)(benchmark::State & state) {                       \
        benchmarkMatchSize(state);                                                    \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchComplex)(benchmark::State & state) {                    \
        benchmarkMatchComplex(state);                                                 \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchProjectExclude)(benchmark::State & state) {             \
        benchmarkMatchProjectExclude(state);                                          \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchProjectInclude)(benchmark::State & state) {             \
        benchmarkMatchProjectInclude(state);                                          \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchProjectIncludeTwoFields)(benchmark::State & state) {    \
        benchmarkMatchProjectIncludeTwoFields(state);                                 \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchProjectIncludeTwentyFields)(benchmark::State & state) { \
        benchmarkMatchProjectIncludeTwentyFields(state);                              \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchProjectIncludeDepthTwo)(benchmark::State & state) {     \
        benchmarkMatchProjectIncludeDepthTwo(state);                                  \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchProjectIncludeDepthTwenty)(benchmark::State & state) {  \
        benchmarkMatchProjectIncludeDepthTwenty(state);                               \
    }                                                                                 \
    BENCHMARK_F(Fixture, MatchElemMatch)(benchmark::State & state) {                  \
        benchmarkMatchElemMatch(state);                                               \
    }                                                                                 \
    BENCHMARK_F(Fixture, ProjectExclude)(benchmark::State & state) {                  \
        benchmarkProjectExclude(state);                                               \
    }                                                                                 \
    BENCHMARK_F(Fixture, ProjectInclude)(benchmark::State & state) {                  \
        benchmarkProjectInclude(state);                                               \
    }                                                                                 \
    BENCHMARK_F(Fixture, ProjectIncludeTwoFields)(benchmark::State & state) {         \
        benchmarkProjectIncludeTwoFields(state);                                      \
    }                                                                                 \
    BENCHMARK_F(Fixture, ProjectIncludeTwentyFields)(benchmark::State & state) {      \
        benchmarkProjectIncludeTwentyFields(state);                                   \
    }                                                                                 \
    BENCHMARK_F(Fixture, ProjectIncludeDepthTwo)(benchmark::State & state) {          \
        benchmarkProjectIncludeDepthTwo(state);                                       \
    }                                                                                 \
    BENCHMARK_F(Fixture, ProjectIncludeDepthTwenty)(benchmark::State & state) {       \
        benchmarkProjectIncludeDepthTwenty(state);                                    \
    }

#define BENCHMARK_PIPELINE_QUERY_ENCODING(Fixture)                 \
                                                                   \
    BENCHMARK_F(Fixture, OneStage)(benchmark::State & state) {     \
        benchmarkOneStage(state);                                  \
    }                                                              \
    BENCHMARK_F(Fixture, TwoStages)(benchmark::State & state) {    \
        benchmarkTwoStages(state);                                 \
    }                                                              \
    BENCHMARK_F(Fixture, TwentyStages)(benchmark::State & state) { \
        benchmarkTwentyStages(state);                              \
    }
}  // namespace mongo
