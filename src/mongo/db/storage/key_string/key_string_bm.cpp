/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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


#include "mongo/db/storage/key_string/key_string.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/record_id.h"
#include "mongo/platform/decimal128.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/bufreader.h"

#include <cstring>
#include <random>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {
namespace {

std::mt19937_64 seedGen(1234);
const int kSampleSize = 500;
const int kStrLenMultiplier = 100;
const int kArrLenMultiplier = 40;

const Ordering ALL_ASCENDING = Ordering::make(BSONObj());

struct BsonsAndKeyStrings {
    int bsonSize = 0;
    int keystringSize = 0;
    BSONObj bsons[kSampleSize];
    std::vector<char> keystrings[kSampleSize];
    std::vector<char> typebits[kSampleSize];
};

enum BsonValueType {
    INT,
    DOUBLE,
    STRING,
    ARRAY,
    DECIMAL,
};

BSONObj generateBson(BsonValueType bsonValueType) {
    std::mt19937 gen(seedGen());
    std::exponential_distribution<double> expReal(1e-3);
    std::exponential_distribution<double> expDist(1.0);

    switch (bsonValueType) {
        case INT:
            return BSON("" << static_cast<int>(expReal(gen)));
        case DOUBLE:
            return BSON("" << expReal(gen));
        case STRING:
            return BSON("" << std::string(expDist(gen) * kStrLenMultiplier, 'x'));
        case ARRAY: {
            const int arrLen = expDist(gen) * kArrLenMultiplier;
            BSONArrayBuilder bab;
            for (int i = 0; i < arrLen; i++) {
                bab.append(expReal(gen));
            }
            return BSON("" << BSON("a" << bab.arr()));
        }
        case DECIMAL:
            return BSON("" << Decimal128(expReal(gen),
                                         Decimal128::kRoundTo34Digits,
                                         Decimal128::kRoundTiesToAway)
                                  .quantize(Decimal128("0.01", Decimal128::kRoundTiesToAway)));
    }
    MONGO_UNREACHABLE;
}

static BsonsAndKeyStrings generateBsonsAndKeyStrings(BsonValueType bsonValueType,
                                                     key_string::Version version) {
    BsonsAndKeyStrings result;
    result.bsonSize = 0;
    result.keystringSize = 0;
    for (int i = 0; i < kSampleSize; i++) {
        result.bsons[i] = generateBson(bsonValueType);
        result.bsonSize += result.bsons[i].objsize();
        key_string::Builder ks(version, result.bsons[i], ALL_ASCENDING);

        result.keystrings[i].assign(ks.getView().begin(), ks.getView().end());
        result.keystringSize += result.keystrings[i].size();

        result.typebits[i].assign(ks.getTypeBits().getView().begin(),
                                  ks.getTypeBits().getView().end());
    }
    return result;
}

void BM_BSONToKeyString(benchmark::State& state,
                        const key_string::Version version,
                        BsonValueType bsonType) {
    const BsonsAndKeyStrings bsonsAndKeyStrings = generateBsonsAndKeyStrings(bsonType, version);
    for (auto _ : state) {
        benchmark::ClobberMemory();
        for (const auto& bson : bsonsAndKeyStrings.bsons) {
            benchmark::DoNotOptimize(key_string::Builder(version, bson, ALL_ASCENDING));
        }
    }
    state.SetBytesProcessed(state.iterations() * bsonsAndKeyStrings.bsonSize);
    state.SetItemsProcessed(state.iterations() * kSampleSize);
}

void BM_KeyStringToBSON(benchmark::State& state,
                        const key_string::Version version,
                        BsonValueType bsonType) {
    const BsonsAndKeyStrings bsonsAndKeyStrings = generateBsonsAndKeyStrings(bsonType, version);
    for (auto _ : state) {
        benchmark::ClobberMemory();
        for (size_t i = 0; i < kSampleSize; i++) {
            auto& typeBits = bsonsAndKeyStrings.typebits[i];
            BufReader buf(typeBits.data(), typeBits.size());
            benchmark::DoNotOptimize(
                key_string::toBson(bsonsAndKeyStrings.keystrings[i],
                                   ALL_ASCENDING,
                                   key_string::TypeBits::fromBuffer(version, &buf)));
        }
    }
    state.SetBytesProcessed(state.iterations() * bsonsAndKeyStrings.bsonSize);
    state.SetItemsProcessed(state.iterations() * kSampleSize);
}

void BM_KeyStringValueAssign(benchmark::State& state, BsonValueType bsonType) {
    // The KeyString version does not matter for this test.
    const auto version = key_string::Version::V1;
    const BsonsAndKeyStrings bsonsAndKeyStrings = generateBsonsAndKeyStrings(bsonType, version);

    // Pre-construct the values.
    std::vector<key_string::Value> values;
    for (size_t i = 0; i < kSampleSize; i++) {
        key_string::HeapBuilder builder(version, bsonsAndKeyStrings.bsons[i], ALL_ASCENDING);
        values.emplace_back(builder.release());
    }

    for (auto _ : state) {
        benchmark::ClobberMemory();
        key_string::Value oldValue = values[0];
        for (size_t i = 1; i < kSampleSize; i++) {
            oldValue = values[i];
        }
    }
    state.SetBytesProcessed(state.iterations() * bsonsAndKeyStrings.keystringSize);
    state.SetItemsProcessed(state.iterations() * kSampleSize);
}

void BM_KeyStringHeapBuilderRelease(benchmark::State& state, BsonValueType bsonType) {
    // The KeyString version does not matter for this test.
    const auto version = key_string::Version::V1;
    const BsonsAndKeyStrings bsonsAndKeyStrings = generateBsonsAndKeyStrings(bsonType, version);

    for (auto _ : state) {
        benchmark::ClobberMemory();
        for (size_t i = 0; i < kSampleSize; i++) {
            key_string::HeapBuilder builder(version, bsonsAndKeyStrings.bsons[i], ALL_ASCENDING);
            benchmark::DoNotOptimize(builder.release());
        }
    }
    state.SetBytesProcessed(state.iterations() * bsonsAndKeyStrings.bsonSize);
    state.SetItemsProcessed(state.iterations() * kSampleSize);
}

void BM_KeyStringStackBuilderCopy(benchmark::State& state, BsonValueType bsonType) {
    // The KeyString version does not matter for this test.
    const auto version = key_string::Version::V1;
    const BsonsAndKeyStrings bsonsAndKeyStrings = generateBsonsAndKeyStrings(bsonType, version);

    for (auto _ : state) {
        benchmark::ClobberMemory();
        for (size_t i = 0; i < kSampleSize; i++) {
            key_string::Builder builder(version, bsonsAndKeyStrings.bsons[i], ALL_ASCENDING);
            benchmark::DoNotOptimize(builder.getValueCopy());
        }
    }
    state.SetBytesProcessed(state.iterations() * bsonsAndKeyStrings.bsonSize);
    state.SetItemsProcessed(state.iterations() * kSampleSize);
}

void BM_KeyStringRecordIdStrAppend(benchmark::State& state, const size_t size) {
    const auto buf = std::string(size, 'a');
    auto rid = RecordId(buf);
    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(key_string::Builder(key_string::Version::V1, rid));
    }
}

void BM_KeyStringRecordIdStrDecode(benchmark::State& state, const size_t size) {
    const auto buf = std::string(size, 'a');
    key_string::Builder ks(key_string::Version::V1, RecordId(buf));
    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(key_string::decodeRecordIdStrAtEnd(ks.getView()));
    }
}

BENCHMARK_CAPTURE(BM_KeyStringValueAssign, Int, INT);
BENCHMARK_CAPTURE(BM_KeyStringValueAssign, Double, DOUBLE);
BENCHMARK_CAPTURE(BM_KeyStringValueAssign, Decimal, DECIMAL);
BENCHMARK_CAPTURE(BM_KeyStringValueAssign, String, STRING);
BENCHMARK_CAPTURE(BM_KeyStringValueAssign, Array, ARRAY);

BENCHMARK_CAPTURE(BM_KeyStringHeapBuilderRelease, Int, INT);
BENCHMARK_CAPTURE(BM_KeyStringHeapBuilderRelease, Double, DOUBLE);
BENCHMARK_CAPTURE(BM_KeyStringHeapBuilderRelease, Decimal, DECIMAL);
BENCHMARK_CAPTURE(BM_KeyStringHeapBuilderRelease, String, STRING);
BENCHMARK_CAPTURE(BM_KeyStringHeapBuilderRelease, Array, ARRAY);

BENCHMARK_CAPTURE(BM_KeyStringStackBuilderCopy, Int, INT);
BENCHMARK_CAPTURE(BM_KeyStringStackBuilderCopy, Double, DOUBLE);
BENCHMARK_CAPTURE(BM_KeyStringStackBuilderCopy, Decimal, DECIMAL);
BENCHMARK_CAPTURE(BM_KeyStringStackBuilderCopy, String, STRING);
BENCHMARK_CAPTURE(BM_KeyStringStackBuilderCopy, Array, ARRAY);

BENCHMARK_CAPTURE(BM_BSONToKeyString, V0_Int, key_string::Version::V0, INT);
BENCHMARK_CAPTURE(BM_BSONToKeyString, V1_Int, key_string::Version::V1, INT);
BENCHMARK_CAPTURE(BM_BSONToKeyString, V0_Double, key_string::Version::V0, DOUBLE);
BENCHMARK_CAPTURE(BM_BSONToKeyString, V1_Double, key_string::Version::V1, DOUBLE);
BENCHMARK_CAPTURE(BM_BSONToKeyString, V1_Decimal, key_string::Version::V1, DECIMAL);
BENCHMARK_CAPTURE(BM_BSONToKeyString, V0_String, key_string::Version::V0, STRING);
BENCHMARK_CAPTURE(BM_BSONToKeyString, V1_String, key_string::Version::V1, STRING);
BENCHMARK_CAPTURE(BM_BSONToKeyString, V0_Array, key_string::Version::V0, ARRAY);
BENCHMARK_CAPTURE(BM_BSONToKeyString, V1_Array, key_string::Version::V1, ARRAY);

BENCHMARK_CAPTURE(BM_KeyStringToBSON, V0_Int, key_string::Version::V0, INT);
BENCHMARK_CAPTURE(BM_KeyStringToBSON, V1_Int, key_string::Version::V1, INT);
BENCHMARK_CAPTURE(BM_KeyStringToBSON, V0_Double, key_string::Version::V0, DOUBLE);
BENCHMARK_CAPTURE(BM_KeyStringToBSON, V1_Double, key_string::Version::V1, DOUBLE);
BENCHMARK_CAPTURE(BM_KeyStringToBSON, V1_Decimal, key_string::Version::V1, DECIMAL);
BENCHMARK_CAPTURE(BM_KeyStringToBSON, V0_String, key_string::Version::V0, STRING);
BENCHMARK_CAPTURE(BM_KeyStringToBSON, V1_String, key_string::Version::V1, STRING);
BENCHMARK_CAPTURE(BM_KeyStringToBSON, V0_Array, key_string::Version::V0, ARRAY);
BENCHMARK_CAPTURE(BM_KeyStringToBSON, V1_Array, key_string::Version::V1, ARRAY);

BENCHMARK_CAPTURE(BM_KeyStringRecordIdStrAppend, 16B, 16);
BENCHMARK_CAPTURE(BM_KeyStringRecordIdStrAppend, 512B, 512);
BENCHMARK_CAPTURE(BM_KeyStringRecordIdStrAppend, 1kB, 1024);
BENCHMARK_CAPTURE(BM_KeyStringRecordIdStrAppend, 1MB, 1024 * 1024);
BENCHMARK_CAPTURE(BM_KeyStringRecordIdStrDecode, 16B, 16);
BENCHMARK_CAPTURE(BM_KeyStringRecordIdStrDecode, 512B, 512);
BENCHMARK_CAPTURE(BM_KeyStringRecordIdStrDecode, 1kB, 1024);
BENCHMARK_CAPTURE(BM_KeyStringRecordIdStrDecode, 1MB, 1024 * 1024);

}  // namespace
}  // namespace mongo
