// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/storage/key_string/key_string.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/ordering.h"
#include "mongo/bson/util/builder.h"
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

class NoOpBuilderImpl : public key_string::BuilderInterface {
public:
    void append(const MinKeyLabeler& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const MaxKeyLabeler& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const NullLabeler& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const UndefinedLabeler& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(bool v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const Date_t& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const Timestamp& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const OID& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const std::string& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(std::string_view v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const BSONSymbol& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const BSONCode& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const BSONCodeWScope& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const BSONBinData& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const BSONRegEx& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const BSONDBRef& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(double v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(const Decimal128& v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(long long v) override {
        benchmark::DoNotOptimize(v);
    }
    void append(int32_t v) override {
        benchmark::DoNotOptimize(v);
    }

    BufBuilder& subobjStart() override {
        return _builder;
    }
    BufBuilder& subarrayStart() override {
        return _builder;
    }

private:
    BufBuilder _builder{0};
};

void BM_KeyStringReadValue(benchmark::State& state,
                           const key_string::Version version,
                           BsonValueType bsonType) {
    const BsonsAndKeyStrings bsonsAndKeyStrings = generateBsonsAndKeyStrings(bsonType, version);
    for (auto _ : state) {
        benchmark::ClobberMemory();
        for (size_t i = 0; i < kSampleSize; i++) {
            auto& typeBits = bsonsAndKeyStrings.typebits[i];
            BufReader typeBitsBuf(typeBits.data(), typeBits.size());
            auto typeBitsReader = key_string::TypeBits::getReaderFromBuffer(version, &typeBitsBuf);

            BufReader reader(bsonsAndKeyStrings.keystrings[i].data(),
                             bsonsAndKeyStrings.keystrings[i].size());
            NoOpBuilderImpl stream;
            while (key_string::readValue(
                &reader, &typeBitsReader, /*inverted=*/false, version, &stream)) {
            }
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

BENCHMARK_CAPTURE(BM_KeyStringReadValue, V0_Int, key_string::Version::V0, INT);
BENCHMARK_CAPTURE(BM_KeyStringReadValue, V1_Int, key_string::Version::V1, INT);
BENCHMARK_CAPTURE(BM_KeyStringReadValue, V0_Double, key_string::Version::V0, DOUBLE);
BENCHMARK_CAPTURE(BM_KeyStringReadValue, V1_Double, key_string::Version::V1, DOUBLE);
BENCHMARK_CAPTURE(BM_KeyStringReadValue, V1_Decimal, key_string::Version::V1, DECIMAL);
BENCHMARK_CAPTURE(BM_KeyStringReadValue, V0_String, key_string::Version::V0, STRING);
BENCHMARK_CAPTURE(BM_KeyStringReadValue, V1_String, key_string::Version::V1, STRING);
BENCHMARK_CAPTURE(BM_KeyStringReadValue, V0_Array, key_string::Version::V0, ARRAY);
BENCHMARK_CAPTURE(BM_KeyStringReadValue, V1_Array, key_string::Version::V1, ARRAY);

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
