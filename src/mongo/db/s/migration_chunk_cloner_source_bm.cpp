// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/s/migration_chunk_cloner_source.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"

#include <chrono>
#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

BSONObj createCollectionDocumentWithSize(int id, int size) {
    std::string repeatedValue(size, 'x');
    return BSON("_id" << id << "X" << repeatedValue);
}

void createModListWithDuplicates(std::list<BSONObj>* modList, int percentDuplicates, int size) {
    int numDuplicates = 2000 * percentDuplicates / 100;
    for (int i = 0; i < 2000; i++) {
        modList->push_back((i < numDuplicates)
                               ? createCollectionDocumentWithSize(numDuplicates, size)
                               : createCollectionDocumentWithSize(i, size));
    }
}

bool noopFn(BSONObj query, BSONObj* result) {
    *result = query;
    return true;
}

void BM_xferDeletes(benchmark::State& state) {
    int percentDup = state.range(0);
    int docSizeInBytes = state.range(1);
    for (auto _ : state) {
        BSONObjBuilder builder;
        BSONArrayBuilder arrDel(builder.subarrayStart("deleted"));
        std::list<BSONObj> deleteList;
        createModListWithDuplicates(&deleteList, percentDup, docSizeInBytes);
        auto start = std::chrono::high_resolution_clock::now();
        benchmark::DoNotOptimize(xferMods(&arrDel, &deleteList, 0, noopFn));
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed_seconds = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        state.SetIterationTime(elapsed_seconds.count());
        arrDel.done();
    }
}

BENCHMARK(BM_xferDeletes)->ArgsProduct({{0, 25, 50, 75, 100}, {1, 1024, 2048}});

}  // namespace
}  // namespace mongo
