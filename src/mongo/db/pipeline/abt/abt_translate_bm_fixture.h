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

#pragma once

#include "mongo/platform/basic.h"

#include <benchmark/benchmark.h>

#include "mongo/bson/bsonobj.h"

namespace mongo {

class ABTTranslateBenchmarkFixture : public benchmark::Fixture {
public:
    virtual void benchmarkABTTranslate(benchmark::State& state,
                                       BSONObj matchSpec,
                                       BSONObj projectSpec) = 0;
    virtual void benchmarkABTTranslate(benchmark::State& state,
                                       const std::vector<BSONObj>& pipeline) = 0;

    void benchmarkMatch(benchmark::State& state);
    void benchmarkMatchTwoFields(benchmark::State& state);
    void benchmarkMatchTwentyFields(benchmark::State& state);

    void benchmarkMatchDepthTwo(benchmark::State& state);
    void benchmarkMatchDepthTwenty(benchmark::State& state);

    void benchmarkMatchGtLt(benchmark::State& state);
    void benchmarkMatchIn(benchmark::State& state);
    void benchmarkMatchInLarge(benchmark::State& state);
    void benchmarkMatchElemMatch(benchmark::State& state);
    void benchmarkMatchComplex(benchmark::State& state);

    void benchmarkProjectExclude(benchmark::State& state);
    void benchmarkProjectInclude(benchmark::State& state);
    void benchmarkProjectIncludeTwoFields(benchmark::State& state);
    void benchmarkProjectIncludeTwentyFields(benchmark::State& state);

    void benchmarkProjectIncludeDepthTwo(benchmark::State& state);
    void benchmarkProjectIncludeDepthTwenty(benchmark::State& state);

    void benchmarkTwoStages(benchmark::State& state);
    void benchmarkTwentyStages(benchmark::State& state);

    void benchmarkMatchBitsAllClear(benchmark::State& state);
    void benchmarkMatchManyEqualitiesThenBitsAllClear(benchmark::State& state);
    void benchmarkProjectRound(benchmark::State& state);
    void benchmarkProjectManyInclusionsThenRound(benchmark::State& state);

    void benchmarkSampleStage(benchmark::State& state);
    void benchmarkManyStagesThenSample(benchmark::State& state);
    void benchmarkSampleThenManyStages(benchmark::State& state);
};

// These benchmarks cover some simple queries which are currently CQF-eligible. As more support
// is added to CQF, more benchmarks may be added here as needed.
#define BENCHMARK_MQL_TRANSLATION(Fixture)                                                \
                                                                                          \
    BENCHMARK_F(Fixture, Match)(benchmark::State & state) {                               \
        benchmarkMatch(state);                                                            \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchTwoFields)(benchmark::State & state) {                      \
        benchmarkMatchTwoFields(state);                                                   \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchTwentyFields)(benchmark::State & state) {                   \
        benchmarkMatchTwentyFields(state);                                                \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchDepthTwo)(benchmark::State & state) {                       \
        benchmarkMatchDepthTwo(state);                                                    \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchDepthTwenty)(benchmark::State & state) {                    \
        benchmarkMatchDepthTwenty(state);                                                 \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchGtLt)(benchmark::State & state) {                           \
        benchmarkMatchGtLt(state);                                                        \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchIn)(benchmark::State & state) {                             \
        benchmarkMatchIn(state);                                                          \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchInLarge)(benchmark::State & state) {                        \
        benchmarkMatchInLarge(state);                                                     \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchElemMatch)(benchmark::State & state) {                      \
        benchmarkMatchElemMatch(state);                                                   \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchComplex)(benchmark::State & state) {                        \
        benchmarkMatchComplex(state);                                                     \
    }                                                                                     \
    BENCHMARK_F(Fixture, ProjectExclude)(benchmark::State & state) {                      \
        benchmarkProjectExclude(state);                                                   \
    }                                                                                     \
    BENCHMARK_F(Fixture, ProjectInclude)(benchmark::State & state) {                      \
        benchmarkProjectInclude(state);                                                   \
    }                                                                                     \
    BENCHMARK_F(Fixture, ProjectIncludeTwoFields)(benchmark::State & state) {             \
        benchmarkProjectIncludeTwoFields(state);                                          \
    }                                                                                     \
    BENCHMARK_F(Fixture, ProjectIncludeTwentyFields)(benchmark::State & state) {          \
        benchmarkProjectIncludeTwentyFields(state);                                       \
    }                                                                                     \
    BENCHMARK_F(Fixture, ProjectIncludeDepthTwo)(benchmark::State & state) {              \
        benchmarkProjectIncludeDepthTwo(state);                                           \
    }                                                                                     \
    BENCHMARK_F(Fixture, ProjectIncludeDepthTwenty)(benchmark::State & state) {           \
        benchmarkProjectIncludeDepthTwenty(state);                                        \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchBitsAllClear)(benchmark::State & state) {                   \
        benchmarkMatchBitsAllClear(state);                                                \
    }                                                                                     \
    BENCHMARK_F(Fixture, MatchManyEqualitiesThenBitsAllClear)(benchmark::State & state) { \
        benchmarkMatchManyEqualitiesThenBitsAllClear(state);                              \
    }                                                                                     \
    BENCHMARK_F(Fixture, ProjectRound)(benchmark::State & state) {                        \
        benchmarkProjectRound(state);                                                     \
    }                                                                                     \
    BENCHMARK_F(Fixture, ProjectManyInclusionsThenRound)(benchmark::State & state) {      \
        benchmarkProjectManyInclusionsThenRound(state);                                   \
    }

// Queries which are expressed as pipelines should be added here because they cannot go through
// find translation.
#define BENCHMARK_MQL_PIPELINE_TRANSLATION(Fixture)                        \
                                                                           \
    BENCHMARK_F(Fixture, TwoStages)(benchmark::State & state) {            \
        benchmarkTwoStages(state);                                         \
    }                                                                      \
    BENCHMARK_F(Fixture, TwentyStages)(benchmark::State & state) {         \
        benchmarkTwentyStages(state);                                      \
    }                                                                      \
    BENCHMARK_F(Fixture, SampleStage)(benchmark::State & state) {          \
        benchmarkSampleStage(state);                                       \
    }                                                                      \
    BENCHMARK_F(Fixture, ManyStagesThenSample)(benchmark::State & state) { \
        benchmarkManyStagesThenSample(state);                              \
    }                                                                      \
    BENCHMARK_F(Fixture, SampleThenManyStages)(benchmark::State & state) { \
        benchmarkSampleThenManyStages(state);                              \
    }
}  // namespace mongo
