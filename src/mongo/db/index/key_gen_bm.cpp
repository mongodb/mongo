// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/index/btree_key_generator.h"
#include "mongo/db/index/multikey_paths.h"
#include "mongo/db/index/sort_key_generator.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/logical_model/sort_pattern/sort_pattern.h"
#include "mongo/db/storage/key_string/key_string.h"
#include "mongo/util/shared_buffer_fragment.h"

#include <cstddef>
#include <cstdint>
#include <random>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

std::mt19937_64 numGen(1234);
constexpr const char* kFieldName = "a";
constexpr size_t kMemBlockSize = 16 * 1024;

Ordering makeOrdering(const char* fieldName) {
    return Ordering::make((BSONObjBuilder() << fieldName << 1).obj());
}

void BM_KeyGenBasic(benchmark::State& state, bool skipMultikey) {
    BSONObjBuilder builder;
    builder.append(kFieldName, 1);
    BSONObj obj = builder.obj();

    BtreeKeyGenerator generator({kFieldName},
                                {BSONElement{}},
                                false,
                                key_string::Version::kLatestVersion,
                                makeOrdering(kFieldName));

    SharedBufferFragmentBuilder allocator(kMemBlockSize,
                                          SharedBufferFragmentBuilder::ConstantGrowStrategy());
    KeyStringSet keys;
    MultikeyPaths multikeyPaths;

    for (auto _ : state) {
        generator.getKeys(allocator, obj, skipMultikey, &keys, &multikeyPaths);
        benchmark::ClobberMemory();
        keys.clear();
        multikeyPaths.clear();
    }
}

void BM_KeyGenArray(benchmark::State& state, int32_t elements) {
    std::mt19937 gen(numGen());

    BSONObjBuilder builder;
    BSONArrayBuilder arrBuilder(builder.subarrayStart(kFieldName));
    for (int32_t i = 0; i < elements; ++i) {
        arrBuilder.append(static_cast<int32_t>(gen()));
    }
    arrBuilder.done();
    BSONObj obj = builder.obj();

    BtreeKeyGenerator generator({kFieldName},
                                {BSONElement{}},
                                false,
                                key_string::Version::kLatestVersion,
                                makeOrdering(kFieldName));

    SharedBufferFragmentBuilder allocator(kMemBlockSize,
                                          SharedBufferFragmentBuilder::ConstantGrowStrategy());
    KeyStringSet keys;
    MultikeyPaths multikeyPaths;

    for (auto _ : state) {
        generator.getKeys(allocator, obj, false, &keys, &multikeyPaths);
        benchmark::ClobberMemory();
        keys.clear();
        multikeyPaths.clear();
    }
}

void BM_KeyGenArrayZero(benchmark::State& state, int32_t elements) {
    BSONObjBuilder builder;
    BSONArrayBuilder arrBuilder(builder.subarrayStart(kFieldName));
    for (int32_t i = 0; i < elements; ++i) {
        arrBuilder.append(0);
    }
    arrBuilder.done();
    BSONObj obj = builder.obj();

    BtreeKeyGenerator generator({kFieldName},
                                {BSONElement{}},
                                false,
                                key_string::Version::kLatestVersion,
                                makeOrdering(kFieldName));

    SharedBufferFragmentBuilder allocator(kMemBlockSize,
                                          SharedBufferFragmentBuilder::ConstantGrowStrategy());
    KeyStringSet keys;
    MultikeyPaths multikeyPaths;

    for (auto _ : state) {
        generator.getKeys(allocator, obj, false, &keys, &multikeyPaths);
        benchmark::ClobberMemory();
        keys.clear();
        multikeyPaths.clear();
    }
}

void BM_KeyGenArrayOfArray(benchmark::State& state, int32_t elements) {
    std::mt19937 gen(numGen());

    BSONObjBuilder builder;
    BSONArrayBuilder arrBuilder(builder.subarrayStart(kFieldName));
    for (int32_t i = 0; i < elements; ++i) {
        BSONArrayBuilder innerArrBuilder(arrBuilder.subarrayStart());
        for (int32_t j = 0; j < elements; ++j) {
            innerArrBuilder.append(static_cast<int32_t>(gen()));
        }
    }
    arrBuilder.done();
    BSONObj obj = builder.obj();

    BtreeKeyGenerator generator({kFieldName},
                                {BSONElement{}},
                                false,
                                key_string::Version::kLatestVersion,
                                makeOrdering(kFieldName));

    SharedBufferFragmentBuilder allocator(kMemBlockSize,
                                          SharedBufferFragmentBuilder::ConstantGrowStrategy());
    KeyStringSet keys;
    MultikeyPaths multikeyPaths;

    for (auto _ : state) {
        generator.getKeys(allocator, obj, false, &keys, &multikeyPaths);
        benchmark::ClobberMemory();
        keys.clear();
        multikeyPaths.clear();
    }
}

void BM_SortKeyGen(benchmark::State& state, int32_t elements) {
    std::mt19937 gen(numGen());

    QueryTestServiceContext testServiceContext;
    auto opCtx = testServiceContext.makeOperationContext();

    const auto kNss = NamespaceString::createNamespaceString_forTest("sort_key_gen.bm");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx =
        new ExpressionContextForTest(opCtx.get(), kNss);

    // Create a Document from backing BSON.
    BSONObjBuilder builder;
    BSONObjBuilder specBuilder;
    for (int32_t i = 0; i < elements; ++i) {
        builder.append(kFieldName + std::to_string(i), static_cast<int32_t>(gen()));
        specBuilder.append(kFieldName + std::to_string(i), 1);
    }
    BSONObj bsonDoc = builder.obj();

    auto doc = Document(bsonDoc);
    auto spec = specBuilder.obj();
    SortPattern sortPattern{spec, expCtx};
    SortKeyGenerator generator(std::move(sortPattern), nullptr);
    for (auto _ : state) {
        [[maybe_unused]] auto result = generator.computeSortKeyFromDocument(doc);
        benchmark::ClobberMemory();
    }
}

BENCHMARK_CAPTURE(BM_KeyGenBasic, Generic, false);
BENCHMARK_CAPTURE(BM_KeyGenBasic, SkipMultikey, true);

BENCHMARK_CAPTURE(BM_KeyGenArray, 1K, 1000);
BENCHMARK_CAPTURE(BM_KeyGenArray, 10K, 10000);
BENCHMARK_CAPTURE(BM_KeyGenArray, 100K, 100000);

BENCHMARK_CAPTURE(BM_KeyGenArrayZero, 1K, 1000);
BENCHMARK_CAPTURE(BM_KeyGenArrayZero, 10K, 10000);
BENCHMARK_CAPTURE(BM_KeyGenArrayZero, 100K, 100000);

BENCHMARK_CAPTURE(BM_KeyGenArrayOfArray, 10x10, 10);
BENCHMARK_CAPTURE(BM_KeyGenArrayOfArray, 100x100, 100);
BENCHMARK_CAPTURE(BM_KeyGenArrayOfArray, 1Kx1K, 1000);

BENCHMARK_CAPTURE(BM_SortKeyGen, 1, 1);
BENCHMARK_CAPTURE(BM_SortKeyGen, 10, 10);
}  // namespace
}  // namespace mongo
