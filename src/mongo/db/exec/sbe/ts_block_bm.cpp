/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/exec/sbe/values/ts_block.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/column/bsoncolumn.h"
#include "mongo/db/exec/sbe/values/bson.h"
#include "mongo/db/timeseries/bucket_compression.h"
#include "mongo/db/timeseries/timeseries_constants.h"

#include <cstdint>
#include <random>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo::sbe {
namespace {

constexpr int kMeasurementsPerBucket = 1000;

// Generates a v1 (uncompressed) timeseries bucket matching the MatchGroupSort genny workload data
// shape: timestamps incrementing by 100ms, nested obj with measurement1 (int 0-10),
// measurement2 (double 0-100), measurement3 (double 0-1000).
BSONObj generateUncompressedBucket(int numMeasurements, std::mt19937& gen) {
    std::uniform_int_distribution<int> intDist(0, 10);
    std::uniform_real_distribution<double> doubleDist2(0.0, 100.0);
    std::uniform_real_distribution<double> doubleDist3(0.0, 1000.0);

    auto startTime = Date_t::fromMillisSinceEpoch(1640995200000LL);  // 2022-01-01

    BSONObjBuilder bucket;
    bucket.append("_id", OID::gen());

    // Control section.
    {
        BSONObjBuilder control(bucket.subobjStart("control"));
        control.append("version", 1);

        // Min/max for time.
        {
            BSONObjBuilder minBuilder(control.subobjStart("min"));
            minBuilder.append("time", startTime);
            minBuilder.append(
                "obj", BSON("measurement1" << 0 << "measurement2" << 0.0 << "measurement3" << 0.0));
        }
        {
            BSONObjBuilder maxBuilder(control.subobjStart("max"));
            maxBuilder.append("time", startTime + Milliseconds(100 * (numMeasurements - 1)));
            maxBuilder.append(
                "obj",
                BSON("measurement1" << 10 << "measurement2" << 100.0 << "measurement3" << 1000.0));
        }
    }

    bucket.append("meta", "AAAAAA");

    // Data section (v1 format: field -> {0: val, 1: val, ...}).
    {
        BSONObjBuilder data(bucket.subobjStart("data"));

        // Time field.
        {
            BSONObjBuilder timeBuilder(data.subobjStart("time"));
            for (int i = 0; i < numMeasurements; ++i) {
                timeBuilder.append(std::to_string(i), startTime + Milliseconds(100 * i));
            }
        }

        // Nested obj field with measurement1/2/3.
        {
            BSONObjBuilder objBuilder(data.subobjStart("obj"));
            for (int i = 0; i < numMeasurements; ++i) {
                objBuilder.append(std::to_string(i),
                                  BSON("measurement1" << intDist(gen) << "measurement2"
                                                      << doubleDist2(gen) << "measurement3"
                                                      << doubleDist3(gen)));
            }
        }
    }

    return bucket.obj();
}

// Compress a v1 bucket into v2 format.
BSONObj compressBucket(const BSONObj& uncompressed) {
    auto result = timeseries::compressBucket(uncompressed, "time"_sd, {}, false);
    invariant(result.compressedBucket);
    return *result.compressedBucket;
}

int getBucketVersion(const BSONObj& bucket) {
    return bucket[timeseries::kBucketControlFieldName].embeddedObject().getIntField(
        timeseries::kBucketControlVersionFieldName);
}

// Construct a TsBlock from a bucket for a given field. The bucket must stay alive.
std::unique_ptr<value::TsBlock> makeTsBlock(const BSONObj& bucket,
                                            StringData fieldName,
                                            int count) {
    auto bucketElem = bucket["data"][fieldName];
    auto [columnTag, columnVal] = bson::convertToView(bucketElem);

    auto minElem = bucket["control"]["min"][fieldName];
    auto maxElem = bucket["control"]["max"][fieldName];

    auto min = minElem ? bson::convertToView(minElem) : value::TagValueView{};
    auto max = maxElem ? bson::convertToView(maxElem) : value::TagValueView{};

    return std::make_unique<value::TsBlock>(count,
                                            false /* owned */,
                                            columnTag,
                                            columnVal,
                                            getBucketVersion(bucket),
                                            fieldName == "time",
                                            min,
                                            max);
}

class TsBlockDeblockFixture : public benchmark::Fixture {
public:
    void SetUp(benchmark::State&) override {
        std::mt19937 gen(42);
        _uncompressedBucket = generateUncompressedBucket(kMeasurementsPerBucket, gen);
        _compressedBucket = compressBucket(_uncompressedBucket);
    }

    const BSONObj& uncompressedBucket() const {
        return _uncompressedBucket;
    }

    const BSONObj& compressedBucket() const {
        return _compressedBucket;
    }

private:
    BSONObj _uncompressedBucket;
    BSONObj _compressedBucket;
};

// Scalar fast path: timestamp column (BSONColumn, no arrays/objects).
BENCHMARK_DEFINE_F(TsBlockDeblockFixture, BM_DeblockTimestampColumn)(benchmark::State& state) {
    uint64_t totalElements = 0;
    for (auto _ : state) {
        auto tsBlock = makeTsBlock(compressedBucket(), "time", kMeasurementsPerBucket);
        boost::optional<value::DeblockedTagValStorage> storage;
        benchmark::DoNotOptimize(tsBlock->deblock(storage));
        totalElements += kMeasurementsPerBucket;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(totalElements);
}

// Interleaved object slow path: nested obj column (BSONColumn with objects).
BENCHMARK_DEFINE_F(TsBlockDeblockFixture, BM_DeblockInterleavedObjectColumn)
(benchmark::State& state) {
    uint64_t totalElements = 0;
    for (auto _ : state) {
        auto tsBlock = makeTsBlock(compressedBucket(), "obj", kMeasurementsPerBucket);
        boost::optional<value::DeblockedTagValStorage> storage;
        benchmark::DoNotOptimize(tsBlock->deblock(storage));
        totalElements += kMeasurementsPerBucket;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(totalElements);
}

// V1 uncompressed path: BSONObj-based deblock.
BENCHMARK_DEFINE_F(TsBlockDeblockFixture, BM_DeblockBsonObjColumn)(benchmark::State& state) {
    uint64_t totalElements = 0;
    for (auto _ : state) {
        auto tsBlock = makeTsBlock(uncompressedBucket(), "time", kMeasurementsPerBucket);
        boost::optional<value::DeblockedTagValStorage> storage;
        benchmark::DoNotOptimize(tsBlock->deblock(storage));
        totalElements += kMeasurementsPerBucket;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(totalElements);
}

BENCHMARK_REGISTER_F(TsBlockDeblockFixture, BM_DeblockTimestampColumn);
BENCHMARK_REGISTER_F(TsBlockDeblockFixture, BM_DeblockInterleavedObjectColumn);
BENCHMARK_REGISTER_F(TsBlockDeblockFixture, BM_DeblockBsonObjColumn);

}  // namespace
}  // namespace mongo::sbe
