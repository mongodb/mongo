/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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
#include <chrono>
#include <cstdint>
#include <list>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/s/migration_chunk_cloner_source.h"

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
        auto start = mongo::stdx::chrono::high_resolution_clock::now();
        benchmark::DoNotOptimize(xferMods(&arrDel, &deleteList, 0, noopFn));
        auto end = mongo::stdx::chrono::high_resolution_clock::now();
        auto elapsed_seconds =
            mongo::stdx::chrono::duration_cast<mongo::stdx::chrono::nanoseconds>(end - start);
        state.SetIterationTime(elapsed_seconds.count());
        arrDel.done();
    }
}

BENCHMARK(BM_xferDeletes)->ArgsProduct({{0, 25, 50, 75, 100}, {1, 1024, 2048}});

}  // namespace
}  // namespace mongo
