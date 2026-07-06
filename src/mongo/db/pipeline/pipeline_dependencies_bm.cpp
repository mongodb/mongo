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

#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_optimization_bm_fixture.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

class PipelineDependenciesBMFixture : public PipelineOptimizationBMFixture {};

// Report cost of Pipeline::getDependencies().
BENCHMARK_DEFINE_F(PipelineDependenciesBMFixture, BM_PipelineGetDependencies)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(pipeline->getDependencies({}));
    }
}
BENCHMARK_REGISTER_F(PipelineDependenciesBMFixture, BM_PipelineGetDependencies)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// Report the cost to of calling getModifiedPaths() for each stage.
BENCHMARK_DEFINE_F(PipelineDependenciesBMFixture, BM_DocumentSourceGetModifiedPaths)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        for (auto&& stage : pipeline->getSources()) {
            benchmark::DoNotOptimize(stage->getModifiedPaths());
        }
    }
}
BENCHMARK_REGISTER_F(PipelineDependenciesBMFixture, BM_DocumentSourceGetModifiedPaths)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

// Report the cost to of calling getDependencies() for each stage.
BENCHMARK_DEFINE_F(PipelineDependenciesBMFixture, BM_DocumentSourceGetDependencies)
(benchmark::State& state) {
    auto pipeline = makePipeline(state);
    for (auto keepRunning : state) {
        for (auto&& stage : pipeline->getSources()) {
            DepsTracker deps;
            benchmark::DoNotOptimize(stage->getDependencies(&deps));
        }
    }
}
BENCHMARK_REGISTER_F(PipelineDependenciesBMFixture, BM_DocumentSourceGetDependencies)
    ->Arg(10)
    ->Arg(50)
    ->Arg(100)
    ->Arg(1000)
    ->Unit(benchmark::kMicrosecond);

}  // namespace
}  // namespace mongo
