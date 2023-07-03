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

#include <array>
#include <benchmark/benchmark.h>
#include <cmath>
#include <cstdint>
#include <fmt/format.h>
#include <random>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/bson/util/bsoncolumn.h"
#include "mongo/bson/util/bsoncolumnbuilder.h"
#include "mongo/util/base64.h"
#include "mongo/util/time_support.h"

namespace mongo {
namespace {
std::mt19937_64 seedGen(1337);

std::vector<BSONObj> generateIntegers(int num, int skipPercentage) {
    std::mt19937 gen(seedGen());
    std::normal_distribution<> d(100, 10);
    std::uniform_int_distribution skip(1, 100);

    std::vector<BSONObj> ints;

    for (int i = 0; i < num; ++i) {
        if (skip(gen) <= skipPercentage) {
            ints.push_back(BSONObj());
        } else {
            BSONObjBuilder builder;
            int32_t value = std::lround(d(gen));
            builder.append(""_sd, value);
            ints.push_back(builder.obj());
        }
    }

    return ints;
}

std::vector<BSONObj> generateDoubles(int num, int skipPercentage, int decimals) {
    std::mt19937 gen(seedGen());
    std::normal_distribution<> d(100, 10);
    std::uniform_int_distribution skip(1, 100);

    std::vector<BSONObj> doubles;

    for (int i = 0; i < num; ++i) {
        if (skip(gen) <= skipPercentage) {
            doubles.push_back(BSONObj());
        } else {
            constexpr std::array<double, 5> factors = {1.0, 10.0, 100.0, 1000.0, 10000.0};

            BSONObjBuilder builder;


            double generated = std::llround(d(gen) * factors[decimals]) / factors[decimals];
            builder.append(""_sd, generated);
            doubles.push_back(builder.obj());
        }
    }

    return doubles;
}

std::vector<BSONObj> generateTimestamps(int num, int skipPercentage, double mean, double stddev) {
    std::mt19937 gen(seedGen());
    std::normal_distribution<> d(mean, stddev);
    std::uniform_int_distribution skip(1, 100);

    std::vector<BSONObj> timestamps;

    auto now = Date_t::now().toULL();

    for (int i = 0; i < num; ++i) {
        if (skip(gen) <= skipPercentage) {
            timestamps.push_back(BSONObj());
        } else {
            BSONObjBuilder builder;
            builder.append(""_sd, Timestamp(std::llround(now + d(gen))));
            timestamps.push_back(builder.obj());
        }
    }

    return timestamps;
}

std::vector<BSONObj> generateObjectIds(int num, int skipPercentage) {
    std::mt19937 gen(seedGen());
    std::uniform_int_distribution skip(1, 100);

    std::vector<BSONObj> timestamps;

    for (int i = 0; i < num; ++i) {
        if (skip(gen) <= skipPercentage) {
            timestamps.push_back(BSONObj());
        } else {
            BSONObjBuilder builder;
            builder.append(""_sd, OID::gen());
            timestamps.push_back(builder.obj());
        }
    }

    return timestamps;
}

BSONObj buildCompressed(const std::vector<BSONObj>& elems) {
    BSONColumnBuilder col;
    for (auto&& elem : elems) {
        if (!elem.isEmpty()) {
            col.append(elem.firstElement());
        } else {
            col.skip();
        }
    }
    auto binData = col.finalize();
    BSONObjBuilder objBuilder;
    objBuilder.append(""_sd, binData);
    return objBuilder.obj();
}

BSONObj getCompressedFTDC() {
// The large literal emits this on Visual Studio: Fatal error C1091: compiler limit: string exceeds
// 65535 bytes in length
#if !defined(_MSC_VER)
    StringData compressedBase64Encoded = {};

    std::string compressed = base64::decode(compressedBase64Encoded);
    BSONObjBuilder builder;
    builder.appendBinData("data"_sd, compressed.size(), BinDataType::Column, compressed.data());
    return builder.obj();
#else
    return BSONObj();
#endif
}

void benchmarkDecompression(benchmark::State& state,
                            const BSONElement& compressedElement,
                            int skipSize) {
    uint64_t totalElements = 0;
    uint64_t totalBytes = 0;
    for (auto _ : state) {
        BSONColumn col(compressedElement);
        for (auto&& decompressed : col) {
            totalBytes += decompressed.size();
            ++totalElements;
        }
    }
    state.SetItemsProcessed(totalElements);
    state.SetBytesProcessed(totalBytes);

    uint64_t uncompressedSize = 0;
    BSONColumn col(compressedElement);
    for (auto&& decompressed : col) {
        if (decompressed.eoo())
            uncompressedSize += skipSize;
        else {
            uncompressedSize += decompressed.valuesize();
        }
    }
    state.SetLabel(
        fmt::format("compress:{:.1f}%",
                    100.0 * (1 - ((double)compressedElement.valuesize() / uncompressedSize))));
}

void benchmarkCompression(benchmark::State& state,
                          const BSONElement& compressedElement,
                          int skipSize) {
    BSONColumn col(compressedElement);
    // Iterate over BSONColumn once to fully decompress it so when we are benchmarking below we
    // don't have to pay decompression cost. Also calculate uncompressed size.
    uint64_t uncompressedSize = 0;
    for (auto&& decompressed : col) {
        if (decompressed.eoo())
            uncompressedSize += skipSize;
        else {
            uncompressedSize += decompressed.valuesize();
        }
    }

    uint64_t totalElements = 0;
    uint64_t totalBytes = 0;
    for (auto _ : state) {
        BSONColumnBuilder columnBuilder;
        for (auto&& decompressed : col) {
            columnBuilder.append(decompressed);
            totalBytes += decompressed.size();
            ++totalElements;
        }
        benchmark::DoNotOptimize(columnBuilder.finalize());
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(totalElements);
    state.SetBytesProcessed(totalBytes);
    state.SetLabel(
        fmt::format("compress:{:.1f}%",
                    100.0 * (1 - ((double)compressedElement.valuesize() / uncompressedSize))));
}

}  // namespace

void BM_decompressIntegers(benchmark::State& state, int skipPercentage) {
    BSONObj compressed = buildCompressed(generateIntegers(10000, skipPercentage));
    benchmarkDecompression(state, compressed.firstElement(), sizeof(int32_t));
}

void BM_decompressDoubles(benchmark::State& state, int decimals, int skipPercentage) {
    auto doubles = generateDoubles(10000, skipPercentage, decimals);
    BSONObj compressed = buildCompressed(doubles);
    benchmarkDecompression(state, compressed.firstElement(), sizeof(double));
}

void BM_decompressTimestamps(benchmark::State& state,
                             double mean,
                             double stddev,
                             int skipPercentage) {
    BSONObj compressed = buildCompressed(generateTimestamps(10000, skipPercentage, mean, stddev));
    benchmarkDecompression(state, compressed.firstElement(), sizeof(Timestamp));
}

void BM_decompressObjectIds(benchmark::State& state, int skipPercentage) {
    BSONObj compressed = buildCompressed(generateObjectIds(10000, skipPercentage));
    benchmarkDecompression(state, compressed.firstElement(), sizeof(OID));
}

void BM_decompressFTDC(benchmark::State& state) {
    BSONObj compressed = getCompressedFTDC();
    benchmarkDecompression(state, compressed.firstElement(), 0);
}

void BM_compressIntegers(benchmark::State& state, int skipPercentage) {
    BSONObj compressed = buildCompressed(generateIntegers(10000, skipPercentage));
    benchmarkCompression(state, compressed.firstElement(), sizeof(int32_t));
}

void BM_compressDoubles(benchmark::State& state, int decimals, int skipPercentage) {
    BSONObj compressed = buildCompressed(generateDoubles(10000, skipPercentage, decimals));
    benchmarkCompression(state, compressed.firstElement(), sizeof(double));
}

void BM_compressTimestamps(benchmark::State& state,
                           double mean,
                           double stddev,
                           int skipPercentage) {
    BSONObj compressed = buildCompressed(generateTimestamps(10000, skipPercentage, mean, stddev));
    benchmarkCompression(state, compressed.firstElement(), sizeof(Timestamp));
}

void BM_compressObjectIds(benchmark::State& state, int skipPercentage) {
    BSONObj compressed = buildCompressed(generateObjectIds(10000, skipPercentage));
    benchmarkCompression(state, compressed.firstElement(), sizeof(OID));
}

void BM_compressFTDC(benchmark::State& state) {
    BSONObj compressed = getCompressedFTDC();
    benchmarkCompression(state, compressed.firstElement(), 0);
}

BENCHMARK_CAPTURE(BM_decompressIntegers, Skip = 0 %, 0);
BENCHMARK_CAPTURE(BM_decompressIntegers, Skip = 10 %, 10);
BENCHMARK_CAPTURE(BM_decompressIntegers, Skip = 50 %, 50);
BENCHMARK_CAPTURE(BM_decompressIntegers, Skip = 90 %, 90);
BENCHMARK_CAPTURE(BM_decompressIntegers, Skip = 99 %, 99);

BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 0 / Skip = 0 %, 0, 0);
BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 1 / Skip = 0 %, 1, 0);
BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 2 / Skip = 0 %, 2, 0);
BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 4 / Skip = 0 %, 4, 0);

BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 0 / Skip = 10 %, 0, 10);
BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 1 / Skip = 10 %, 1, 10);
BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 2 / Skip = 10 %, 2, 10);
BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 4 / Skip = 10 %, 4, 10);

BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 0 / Skip = 90 %, 0, 90);
BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 1 / Skip = 90 %, 1, 90);
BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 2 / Skip = 90 %, 2, 90);
BENCHMARK_CAPTURE(BM_decompressDoubles, Decimals = 4 / Skip = 90 %, 4, 90);

BENCHMARK_CAPTURE(BM_decompressTimestamps, Mean = 1 / Stddev = 0 / Skip = 0 %, 0, 1, 0);
BENCHMARK_CAPTURE(BM_decompressTimestamps, Mean = 5 / Stddev = 2 / Skip = 0 %, 0, 1, 0);
BENCHMARK_CAPTURE(BM_decompressTimestamps, Mean = 1 / Stddev = 0 / Skip = 10 %, 0, 1, 10);
BENCHMARK_CAPTURE(BM_decompressTimestamps, Mean = 5 / Stddev = 2 / Skip = 10 %, 0, 1, 10);
BENCHMARK_CAPTURE(BM_decompressTimestamps, Mean = 1 / Stddev = 0 / Skip = 90 %, 0, 1, 90);
BENCHMARK_CAPTURE(BM_decompressTimestamps, Mean = 5 / Stddev = 2 / Skip = 90 %, 0, 1, 90);

BENCHMARK_CAPTURE(BM_decompressObjectIds, Skip = 0 %, 0);
BENCHMARK_CAPTURE(BM_decompressObjectIds, Skip = 10 %, 10);
BENCHMARK_CAPTURE(BM_decompressObjectIds, Skip = 90 %, 90);

// The large literal emits this on Visual Studio: Fatal error C1091: compiler limit: string exceeds
// 65535 bytes in length
#if !defined(_MSC_VER)
BENCHMARK(BM_decompressFTDC);
#endif

BENCHMARK_CAPTURE(BM_compressIntegers, Skip = 0 %, 0);
BENCHMARK_CAPTURE(BM_compressIntegers, Skip = 10 %, 10);
BENCHMARK_CAPTURE(BM_compressIntegers, Skip = 50 %, 50);
BENCHMARK_CAPTURE(BM_compressIntegers, Skip = 90 %, 90);
BENCHMARK_CAPTURE(BM_compressIntegers, Skip = 99 %, 99);

BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 1 / Skip = 0 %, 0, 0);
BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 10 / Skip = 0 %, 1, 0);
BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 100 / Skip = 0 %, 2, 0);
BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 10000 / Skip = 0 %, 4, 0);

BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 1 / Skip = 10 %, 0, 10);
BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 10 / Skip = 10 %, 1, 10);
BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 100 / Skip = 10 %, 2, 10);
BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 10000 / Skip = 10 %, 4, 10);

BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 1 / Skip = 90 %, 0, 90);
BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 10 / Skip = 90 %, 1, 90);
BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 100 / Skip = 90 %, 2, 90);
BENCHMARK_CAPTURE(BM_compressDoubles, Scale = 10000 / Skip = 90 %, 4, 90);

BENCHMARK_CAPTURE(BM_compressTimestamps, Mean = 1 / Stddev = 0 / Skip = 0 %, 0, 1, 0);
BENCHMARK_CAPTURE(BM_compressTimestamps, Mean = 5 / Stddev = 2 / Skip = 0 %, 0, 1, 0);
BENCHMARK_CAPTURE(BM_compressTimestamps, Mean = 1 / Stddev = 0 / Skip = 10 %, 0, 1, 10);
BENCHMARK_CAPTURE(BM_compressTimestamps, Mean = 5 / Stddev = 2 / Skip = 10 %, 0, 1, 10);
BENCHMARK_CAPTURE(BM_compressTimestamps, Mean = 1 / Stddev = 0 / Skip = 90 %, 0, 1, 90);
BENCHMARK_CAPTURE(BM_compressTimestamps, Mean = 5 / Stddev = 2 / Skip = 90 %, 0, 1, 90);

BENCHMARK_CAPTURE(BM_compressObjectIds, Skip = 0 %, 0);
BENCHMARK_CAPTURE(BM_compressObjectIds, Skip = 10 %, 10);
BENCHMARK_CAPTURE(BM_compressObjectIds, Skip = 90 %, 90);

// The large literal emits this on Visual Studio: Fatal error C1091: compiler limit: string exceeds
// 65535 bytes in length
#if !defined(_MSC_VER)
BENCHMARK(BM_compressFTDC);
#endif

}  // namespace mongo
