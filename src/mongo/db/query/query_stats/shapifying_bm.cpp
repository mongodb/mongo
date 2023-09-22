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


#include <benchmark/benchmark.h>
#include <climits>
#include <memory>

#include "mongo/bson/json.h"
#include "mongo/db/matcher/expression_leaf.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/query_shape/query_shape.h"
#include "mongo/db/query/query_stats/find_key_generator.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/query_stats/rate_limiting.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/rpc/metadata/client_metadata.h"
#include "mongo/util/duration.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/testing_proctor.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {

static const NamespaceStringOrUUID kDefaultTestNss =
    NamespaceStringOrUUID{NamespaceString::createNamespaceString_forTest("testDB.testColl")};

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

auto makeFindKeyGenerator(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          const ParsedFindCommand& parsedFind) {
    return std::make_unique<const query_stats::FindKeyGenerator>(
        expCtx, parsedFind, kCollectionType);
}

int shapifyAndHashRequest(const boost::intrusive_ptr<ExpressionContext>& expCtx,
                          const ParsedFindCommand& parsedFind) {
    auto keyGenerator = makeFindKeyGenerator(expCtx, parsedFind);
    [[maybe_unused]] auto hash = absl::HashOf(keyGenerator);
    return 0;
}

// Benchmark the performance of computing and hashing the query stats key for an IDHACK query.
void BM_ShapfiyIDHack(benchmark::State& state) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("query_test");

    auto opCtx = client->makeOperationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get());
    auto fcr = std::make_unique<FindCommandRequest>(expCtx->ns);
    fcr->setFilter(fromjson("{_id: 4}"));
    ClientMetadata::setFromMetadata(opCtx->getClient(), kMockClientMetadataElem, false);
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, std::move(fcr)));

    // Run the benchmark.
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(shapifyAndHashRequest(expCtx, *parsedFind));
    }
}

// Benchmark computing the query stats key and its hash for a mildly complex query predicate.
void BM_ShapfiyMildlyComplex(benchmark::State& state) {
    auto serviceCtx = ServiceContext::make();
    auto client = serviceCtx->makeClient("query_test");

    auto opCtx = client->makeOperationContext();
    auto expCtx = make_intrusive<ExpressionContextForTest>(opCtx.get());
    auto fcr = std::make_unique<FindCommandRequest>(expCtx->ns);
    fcr->setFilter(fromjson(R"({
        clientId: {$nin: ["432345", "4386945", "111111"]},
        nEmployees: {$gte: 4, $lt: 20},
        deactivated: false,
        region: "US",
        yearlySpend: {$lte: 1000}
    })"));
    ClientMetadata::setFromMetadata(opCtx->getClient(), kMockClientMetadataElem, false);
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, std::move(fcr)));

    // Run the benchmark.
    for (auto keepRunning : state) {
        benchmark::DoNotOptimize(shapifyAndHashRequest(expCtx, *parsedFind));
    }
}

BENCHMARK(BM_ShapfiyIDHack)->Threads(1);
BENCHMARK(BM_ShapfiyMildlyComplex)->Threads(1);

}  // namespace
}  // namespace mongo
