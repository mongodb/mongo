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

#include "mongo/db/query/query_planner.h"

#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_test_service_context.h"

#include <benchmark/benchmark.h>

namespace mongo {

constexpr auto kDbName = "testdb";
constexpr auto kCollName = "coll";

struct Query {
    BSONObj filter;
    BSONObj proj;
    BSONObj sort;
    BSONObj hint;
};

std::unique_ptr<CanonicalQuery> getCanonicalQuery(OperationContext* opCtx, Query query) {
    auto findCommand = query_request_helper::makeFromFindCommandForTests(
        BSON("find" << kCollName << "$db" << kDbName << "filter" << query.filter << "projection"
                    << query.proj << "sort" << query.sort << "hint" << query.hint));
    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx, *findCommand).build();
    return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx,
        .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)},
    });
}

IndexEntry createIndexEntry(BSONObj keyPattern) {
    return IndexEntry(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                      IndexConfig::kLatestIndexVersion,
                      false /*multikey*/,
                      {} /*mutikeyPaths*/,
                      {} /*multikeyPathSet*/,
                      false /*sparse*/,
                      false /*unique*/,
                      IndexEntry::Identifier{"ident"},
                      nullptr /*filterExpr*/,
                      BSONObj() /*infoObj*/,
                      nullptr /*collatorInterface*/,
                      nullptr /*wildcardProjection*/);
}

void BM_NoIndexes(benchmark::State& state) {
    auto filter = BSON("a" << 1);
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void BM_SingleIndex(benchmark::State& state) {
    auto filter = BSON("a" << 1);
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    plannerParams.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1))};
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void BM_MultipleIndexes(benchmark::State& state) {
    auto filter = BSON("a" << 1 << "b" << 1);
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    plannerParams.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1)),
                                                createIndexEntry(BSON("a" << 1 << "b" << 1))};
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void populateIn(BSONObjBuilder& bob) {
    BSONArrayBuilder inList(bob.subarrayStart("$in"));
    for (int i = 0; i < 20000; ++i) {
        inList.append(i);
    }
    inList.done();
    bob.done();
}

void BM_LargeIn(benchmark::State& state) {
    BSONObjBuilder bob;
    BSONObjBuilder subA(bob.subobjStart("a"));
    populateIn(subA);
    BSONObjBuilder subB(bob.subobjStart("b"));
    populateIn(subB);
    auto filter = bob.obj();
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    plannerParams.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1 << "b" << 1)),
                                                createIndexEntry(BSON("a" << 1))};
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void BM_LargeEq(benchmark::State& state) {
    // Tests single-field equality to a large array.
    BSONObjBuilder bob;
    BSONObjBuilder subA(bob.subobjStart("a"));

    BSONArrayBuilder eqValue(subA.subarrayStart("$eq"));
    for (int i = 0; i < 20000; ++i) {
        eqValue.append(i);
    }
    eqValue.done();
    subA.done();

    auto filter = bob.obj();

    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    plannerParams.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1))};
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void BM_IndexedContainedOrFilter(benchmark::State& state) {
    // Contained $or query with exponential number of solutions.
    auto original = internalQueryEnumerationMaxOrSolutions.load();
    ON_BLOCK_EXIT([&] { internalQueryEnumerationMaxOrSolutions.store(original); });
    internalQueryEnumerationMaxOrSolutions.store(64);
    // {a: 1, $or: [{b: 1, c: 1}, {b: 2, c: 2}, {b: 3, c: 3}, {b: 4, c: 4}, {b: 5, c: 5}]}
    auto filter = BSON("a" << 1 << "$or"
                           << BSON_ARRAY(BSON("b" << 1 << "c" << 1)
                                         << BSON("b" << 2 << "c" << 2) << BSON("b" << 3 << "c" << 3)
                                         << BSON("b" << 4 << "c" << 4) << BSON("b" << 5 << "c" << 5)
                                         << BSON("b" << 6 << "c" << 6)));
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    plannerParams.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1 << "b" << 1)),
                                                createIndexEntry(BSON("a" << 1 << "c" << 1))};
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void BM_IndexIntersection(benchmark::State& state) {
    // Index intersection plan with quadratic number of solutions.
    auto original = internalQueryEnumerationMaxIntersectPerAnd.load();
    ON_BLOCK_EXIT([&] { internalQueryEnumerationMaxIntersectPerAnd.store(original); });
    internalQueryEnumerationMaxIntersectPerAnd.store(64);
    // {a: 1, b: 1, c: 1, d: 1, e: 1, f: 1}
    auto filter = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1 << "f" << 1);
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    plannerParams.mainCollectionInfo.options = QueryPlannerParams::Options::INDEX_INTERSECTION;
    plannerParams.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1)),
                                                createIndexEntry(BSON("b" << 1)),
                                                createIndexEntry(BSON("c" << 1)),
                                                createIndexEntry(BSON("d" << 1)),
                                                createIndexEntry(BSON("e" << 1)),
                                                createIndexEntry(BSON("f" << 1))};
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

std::vector<IndexEntry> create64FooPrefixedIndexes() {
    std::vector<IndexEntry> idxs;
    for (int i = 0; i < 64; ++i) {
        idxs.push_back(
            createIndexEntry(BSON("foo" << 1 << std::string(1, static_cast<char>(65 + i)) << "1")));
    }
    return idxs;
}

void BM_IndexDedupping(benchmark::State& state) {
    // Stress index dedupping logic introduced in SERVER-82677
    // {foo: 1}
    auto filter = BSON("foo" << 1);
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    // 64 indexes each prefixed with 'foo' field.
    plannerParams.mainCollectionInfo.indexes = create64FooPrefixedIndexes();
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void BM_ManyIndexes(benchmark::State& state) {
    ON_BLOCK_EXIT([&] { internalQueryPlannerEnableIndexPruning.store(true); });
    internalQueryPlannerEnableIndexPruning.store(false);
    // {foo: 1}
    auto filter = BSON("foo" << 1);
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    // 64 indexes each prefixed with 'foo' field.
    plannerParams.mainCollectionInfo.indexes = create64FooPrefixedIndexes();
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void BM_IndexSatisfiesSort(benchmark::State& state) {
    // find({a: 1}).sort({b: 1})
    auto filter = BSON("a" << 1);
    auto sort = BSON("b" << 1);
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter, .sort = sort});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    // Several indexes which can satisfy the sort
    plannerParams.mainCollectionInfo.indexes = {createIndexEntry(BSON("b" << 1)),
                                                createIndexEntry(BSON("b" << 1 << "c" << 1)),
                                                createIndexEntry(BSON("b" << 1 << "d" << 1)),
                                                createIndexEntry(BSON("b" << 1 << "e" << 1)),
                                                createIndexEntry(BSON("b" << 1 << "f" << 1))};
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void BM_ShardFilter(benchmark::State& state) {
    // find({a: 1}) with shard key {b: 1}
    auto filter = BSON("a" << 1);
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    plannerParams.mainCollectionInfo.options = QueryPlannerParams::Options::INCLUDE_SHARD_FILTER;
    plannerParams.shardKey = BSON("b" << 1);
    plannerParams.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1)),
                                                createIndexEntry(BSON("b" << 1))};
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void BM_CoveredPlan(benchmark::State& state) {
    // find({a: 1}, {_id: 0, b: 1})
    auto filter = BSON("a" << 1);
    auto proj = BSON("_id" << 0 << "b" << 1);
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter, .proj = proj});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    plannerParams.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1 << "b" << 1)),
                                                createIndexEntry(BSON("a" << 1))};
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

void BM_HintedPlan(benchmark::State& state) {
    // find({a: {$gt: 5}, b: {$gt: 5}}).hint({a: 1})
    auto filter = BSON("a" << BSON("$gt" << 5) << "b" << BSON("$gt" << 5));
    auto hint = BSON("a" << 1);
    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();
    auto cq = getCanonicalQuery(opCtx.get(), {.filter = filter, .hint = hint});
    QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
    plannerParams.mainCollectionInfo.indexes = {createIndexEntry(BSON("a" << 1)),
                                                createIndexEntry(BSON("b" << 1))};
    for (auto _ : state) {
        auto solns = QueryPlanner::plan(*cq, plannerParams);
    }
}

BENCHMARK(BM_NoIndexes);
BENCHMARK(BM_SingleIndex);
BENCHMARK(BM_MultipleIndexes);
BENCHMARK(BM_LargeIn);
BENCHMARK(BM_LargeEq);
BENCHMARK(BM_IndexedContainedOrFilter);
BENCHMARK(BM_IndexIntersection);
BENCHMARK(BM_IndexDedupping);
BENCHMARK(BM_ManyIndexes);
BENCHMARK(BM_IndexSatisfiesSort);
BENCHMARK(BM_ShardFilter);
BENCHMARK(BM_CoveredPlan);
BENCHMARK(BM_HintedPlan);

}  // namespace mongo
