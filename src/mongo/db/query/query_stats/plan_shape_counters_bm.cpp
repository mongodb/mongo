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

#include "mongo/db/query/query_stats/plan_shape_counters.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution_test_util.h"

#include <memory>

#include <benchmark/benchmark.h>

namespace mongo::plan_shape_counters {
namespace {

// 'identifyPlanShapeForCounters' is called for many query executions and is a potential hot path.
// This benchmark measures the overhead in isolation so we have an idea of the cost and can easily
// measure improvements or regressions to it.

NamespaceString getNss() {
    return NamespaceString::createNamespaceString_forTest("test.coll");
}

std::unique_ptr<IndexScanNode> makeIxScan(BSONObj keyPattern = BSON("a" << 1)) {
    return std::make_unique<IndexScanNode>(getNss(), buildSimpleIndexEntry(keyPattern));
}

std::unique_ptr<FetchNode> makeFetch(std::unique_ptr<QuerySolutionNode> child) {
    return std::make_unique<FetchNode>(std::move(child), getNss());
}


std::unique_ptr<LimitNode> makeLimit(std::unique_ptr<QuerySolutionNode> child) {
    return std::make_unique<LimitNode>(
        std::move(child), 5 /* limit */, LimitSkipParameterization::Disabled);
}


std::unique_ptr<QuerySolutionNode> buildFetchOrIxscan(int numBranches) {
    auto orNode = std::make_unique<OrNode>();
    for (int64_t i = 0; i < numBranches; ++i) {
        orNode->children.push_back(makeIxScan());
    }
    return makeFetch(std::move(orNode));
}

std::unique_ptr<QuerySolution> makePlan(std::unique_ptr<QuerySolutionNode> root) {
    auto solution = std::make_unique<QuerySolution>();
    solution->setRoot(std::move(root));
    return solution;
}

void runBenchmark(benchmark::State& state, std::unique_ptr<QuerySolution> solution) {
    for (auto _ : state) {
        benchmark::DoNotOptimize(identifyPlanShapeForCounters(*solution));
    }
}

// COLLSCAN: the smallest matchable plan (a single node).
void BM_Collscan(benchmark::State& state) {
    runBenchmark(state, makePlan(std::make_unique<CollectionScanNode>(getNss())));
}

// FETCH -> IXSCAN: the most common indexed point/range plan, matching kIxscanFetch.
void BM_IxscanFetch(benchmark::State& state) {
    runBenchmark(state, makePlan(makeFetch(makeIxScan())));
}

// Measure analysis time for a small plan with three branches
// FETCH -> OR -> [IXSCAN, IXSCAN, IXSCAN]
void BM_OrFetchFewBranches(benchmark::State& state) {
    constexpr int numBranches = 3;
    runBenchmark(state, makePlan(buildFetchOrIxscan(numBranches)));
}

// Measure analysis time for a large plan with 200 branches
// FETCH -> OR -> [IXSCAN, IXSCAN, IXSCAN, ...]
void BM_OrFetchManyBranches(benchmark::State& state) {
    constexpr int numBranches = 200;
    runBenchmark(state, makePlan(buildFetchOrIxscan(numBranches)));
}

// A matchable FETCH -> IXSCAN plan buried under a chain of ignored limit stages.
// Plan counter analysis ignores limit stages. We measure the cost of these
// ignored stages here.
void BM_IgnoredNodeChain(benchmark::State& state) {
    constexpr auto depth = 10;
    std::unique_ptr<QuerySolutionNode> node = makeFetch(makeIxScan());
    for (int64_t i = 0; i < depth; ++i) {
        node = makeLimit(std::move(node));
    }
    runBenchmark(state, makePlan(std::move(node)));
}

// Measure cost for an unmatched shape (index intersection).
void BM_NoMatch(benchmark::State& state) {
    auto andSorted = std::make_unique<AndSortedNode>();
    andSorted->children.push_back(makeIxScan());
    andSorted->children.push_back(makeIxScan(BSON("b" << 1)));
    runBenchmark(state, makePlan(makeFetch(std::move(andSorted))));
}

BENCHMARK(BM_Collscan);
BENCHMARK(BM_IxscanFetch);
BENCHMARK(BM_OrFetchFewBranches);
BENCHMARK(BM_OrFetchManyBranches);
BENCHMARK(BM_IgnoredNodeChain);
BENCHMARK(BM_NoMatch);

}  // namespace
}  // namespace mongo::plan_shape_counters
