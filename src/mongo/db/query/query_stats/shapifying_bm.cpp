/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/bson/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_bm_constants.h"
#include "mongo/db/query/query_fcv_environment_for_test.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/duration.h"

#include <climits>
#include <memory>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

static const NamespaceStringOrUUID kDefaultTestNss =
    NamespaceStringOrUUID{NamespaceString::createNamespaceString_forTest("test.coll")};

static constexpr auto kCollectionType = query_shape::CollectionType::kCollection;

// This is a snapshot of the client metadata generated from our IDHACK genny workload. The
// specifics aren't so important, but it chosen in an attempt to be indicative of the size/shape
// of this kind of thing "in the wild".
const auto kMetadataWrapper = fromjson(R"({metadata: {
        "application" : {
            "name" : "Genny"
        },
        "driver" : {
            "name" : "mongoc / mongocxx",
            "version" : "1.23.2 / 3.7.0"
        },
        "os" : {
            "type" : "Linux",
            "name" : "Ubuntu",
            "version" : "22.04",
            "architecture" : "aarch64"
        },
        "platform" : "cfg=0x03215e88e9 posix=200809 stdc=201710 CC=GCC 11.3.0 CFLAGS=\"-fPIC\" LDFLAGS=\"\""
    }})");
auto kMockClientMetadataElem = kMetadataWrapper["metadata"];

/**
 * Builds a sort specification with 'count' fields, like {field_0: 1, field_1: -1, field_2: 1, ...}.
 */
BSONObj buildSortSpec(size_t count) {
    BSONObjBuilder builder;
    for (size_t i = 0; i < count; ++i) {
        builder.append((str::stream() << "field_" << i), (i % 2 == 0) ? 1 : -1);
    }
    return builder.obj();
}

auto makeFindKey(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                 const ParsedFindCommand& parsedFind) {
    return std::make_unique<const query_stats::FindKey>(
        expCtx,
        *parsedFind.findCommandRequest,
        std::make_unique<query_shape::FindCmdShape>(parsedFind, expCtx),
        kCollectionType);
}

int shapifyAndHashRequest(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          const ParsedFindCommand& parsedFind) {
    auto key = makeFindKey(expCtx, parsedFind);
    [[maybe_unused]] auto hash = absl::HashOf(key);
    return 0;
}

void runBenchmark(BSONObj predicate,
                  BSONObj projection,
                  BSONObj sort,
                  boost::optional<int64_t> limit,
                  boost::optional<int64_t> skip,
                  benchmark::State& state) {
    QueryFCVEnvironmentForTest::setUp();

    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->getService()->makeClient("query_test");

    auto opCtx = client->makeOperationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get());
    auto fcr = std::make_unique<FindCommandRequest>(expCtx->getNamespaceString());
    fcr->setFilter(predicate);
    if (!projection.isEmpty()) {
        fcr->setProjection(projection);
    }
    if (!sort.isEmpty()) {
        fcr->setSort(sort);
    }
    if (limit) {
        fcr->setLimit(limit);
    }
    if (skip) {
        fcr->setSkip(skip);
    }
    ClientMetadata::setFromMetadata(opCtx->getClient(), kMockClientMetadataElem, false);
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcr)}));

    // Run the benchmark.
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(shapifyAndHashRequest(expCtx, *parsedFind));
    }
}

void runBenchmark(BSONObj predicate, benchmark::State& state) {
    runBenchmark(predicate, BSONObj(), BSONObj(), boost::none, boost::none, state);
}

// Benchmark the performance of computing and hashing the query stats key for an IDHACK query.
void BM_ShapfiyIDHack(benchmark::State& state) {
    runBenchmark(fromjson("{_id: 4}"), state);
}

// Benchmark computing the query stats key and its hash for a mildly complex query predicate.
void BM_ShapfiyMildlyComplex(benchmark::State& state) {
    runBenchmark(fromjson(R"({
        clientId: {$nin: ["432345", "4386945", "111111"]},
        nEmployees: {$gte: 4, $lt: 20},
        deactivated: false,
        region: "US",
        yearlySpend: {$lte: 1000}
    })"),
                 state);
}

// Benchmark computing the query stats key and its hash for a complex query predicate.
void BM_ShapifyComplex(benchmark::State& state) {
    runBenchmark(query_benchmark_constants::kComplexPredicate,
                 query_benchmark_constants::kComplexProjection,
                 buildSortSpec(5),
                 5,
                 10,
                 state);
}

// Benchmark computing the query stats key and its hash for a very complex query predicate
// (inspired by change stream predicate).
void BM_ShapifyVeryComplex(benchmark::State& state) {
    runBenchmark(query_benchmark_constants::kChangeStreamPredicate,
                 query_benchmark_constants::kVeryComplexProjection,
                 buildSortSpec(10),
                 5,
                 10,
                 state);
}

BENCHMARK(BM_ShapfiyIDHack)->Threads(1);
BENCHMARK(BM_ShapfiyMildlyComplex)->Threads(1);
BENCHMARK(BM_ShapifyComplex)->Threads(1);
BENCHMARK(BM_ShapifyVeryComplex)->Threads(1);

}  // namespace
}  // namespace mongo
