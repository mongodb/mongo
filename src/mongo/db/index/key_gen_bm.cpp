/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include <benchmark/benchmark.h>
#include <random>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/index/btree_key_generator.h"

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
                                nullptr,
                                KeyString::Version::kLatestVersion,
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
                                nullptr,
                                KeyString::Version::kLatestVersion,
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
                                nullptr,
                                KeyString::Version::kLatestVersion,
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
                                nullptr,
                                KeyString::Version::kLatestVersion,
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

}  // namespace
}  // namespace mongo
