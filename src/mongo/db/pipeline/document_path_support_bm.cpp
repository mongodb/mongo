// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/pipeline/document_path_support.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/query/compiler/dependency_analysis/dependencies.h"

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

struct TestData {
    Document doc;
    OrderedPathSet paths;
};

TestData buildTestData(int numPrefixes) {
    BSONObjBuilder bob;
    OrderedPathSet paths;
    for (int i = 0; i < numPrefixes; ++i) {
        std::string prefix = "field_" + std::to_string(i);
        bob.append(prefix, BSON("x" << i));
        if (i == 0) {
            paths.insert(prefix + ".x");
        }
        paths.insert(prefix);
    }
    return {Document{bob.obj()}, std::move(paths)};
}

void BM_DocumentToBsonWithPaths(benchmark::State& state) {
    auto [doc, paths] = buildTestData(state.range(0));
    for (auto _ : state) {
        benchmark::DoNotOptimize(
            document_path_support::documentToBsonWithPaths<BSONObj::LargeSizeTrait, false>(doc,
                                                                                           paths));
    }
}

BENCHMARK(BM_DocumentToBsonWithPaths)->Arg(3)->Arg(10)->Arg(50)->Arg(100)->Arg(200)->Arg(500);

}  // namespace
}  // namespace mongo
