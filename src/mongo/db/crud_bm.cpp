// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/crud_bm_fixture.h"

#include <algorithm>

namespace mongo {
namespace {

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_FIND_ONE)
(benchmark::State& state) {
    _runFindOne(state);
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_UPDATE_ONE)
(benchmark::State& state) {
    _populateTestData(getGlobalServiceContext());
    runBenchmark(state, [updateValue = int64_t{0}]() mutable {
        // clang-format off
        return BSON(
            "update" << kCollection
            << "$db" << kDatabase
            << "updates" << BSON_ARRAY(
                BSON(
                    "q" << BSON(
                        "_id" << 1
                    )
                    << "u" << BSON(
                        "$set" << BSON(
                            "data" << ++updateValue
                        )
                    )
                    << "multi" << false
                    << "upsert" << false
                )
            )
        );
        // clang-format on
    });
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_FIND_AND_MODIFY)
(benchmark::State& state) {
    _populateTestData(getGlobalServiceContext());
    // Query on an extra "data" predicate to make the query express path ineligible.
    runBenchmark(state, [updateValue = int64_t{0}]() mutable {
        // clang-format off
        return BSON(
            "findAndModify" << kCollection
            << "$db" << kDatabase
            << "query" << BSON("_id" << 1 << "data" << "MongoDB")
            << "update" << BSON(
                "$set" << BSON(
                    "counter" << ++updateValue
                )
            )
            << "new" << true
            << "upsert" << false
        );
        // clang-format on
    });
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_INSERT_ONE)
(benchmark::State& state) {
    _runInsertOne(state);
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_INSERT_MANY)
(benchmark::State& state) {
    // Prepopulate the collection so we are not benchmarking collection creation time.
    _populateTestData(getGlobalServiceContext());
    BSONArrayBuilder documents;
    for (int i = 0; i < kBatchSize; ++i) {
        documents.append(BSON("data" << kDocumentData));
    }
    // clang-format off
    BSONObj cmd = BSON(
            "insert" << kCollection
            << "$db" << kDatabase
            << "documents" << documents.arr());
    // clang-format on
    runBenchmark(state, [=] { return cmd; });
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_AGG_MATCH_ONE)
(benchmark::State& state) {
    _populateTestData(getGlobalServiceContext());
    // clang-format off
    BSONObj cmd = BSON(
            "aggregate" << kCollection
            << "$db" << kDatabase
            << "pipeline" << BSON_ARRAY(
                BSON("$match" << BSON("_id" << 1))
                << BSON("$limit" << 1))
            << "cursor" << BSONObj());
    // clang-format on
    runBenchmark(state, [=] { return cmd; });
}

BENCHMARK_DEFINE_F(CrudBenchmarkFixture, BM_DELETE_ONE)
(benchmark::State& state) {
    // Load one document per iteration (kDeleteOneIterations). The extra kDeleteOneCollectionFloor
    // documents are never deleted, so every iteration deletes from a collection of at least that
    // size. Ids are partitioned by thread so concurrent benchmark threads never target the same
    // document.
    const auto baseId = (int64_t{state.thread_index} + 1) << 32;
    _populateDeleteTestData(
        getGlobalServiceContext(), baseId + 1, kDeleteOneIterations + kDeleteOneCollectionFloor);
    runBenchmark(state, [idValue = baseId]() mutable {
        // clang-format off
        return BSON(
            "delete" << kCollection
            << "$db" << kDatabase
            << "deletes" << BSON_ARRAY(
                BSON(
                    "q" << BSON(
                        "_id" << ++idValue
                    )
                    << "limit" << 1
                )
            )
        );
        // clang-format on
    });
}

BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_FIND_ONE)->Threads(1)->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_UPDATE_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_FIND_AND_MODIFY)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_INSERT_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_INSERT_MANY)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_DELETE_ONE)
    ->Iterations(CrudBenchmarkFixture::kDeleteOneIterations)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);
BENCHMARK_REGISTER_F(CrudBenchmarkFixture, BM_AGG_MATCH_ONE)
    ->Threads(1)
    ->Threads(kCommandBMMaxThreads);

}  // namespace
}  // namespace mongo
