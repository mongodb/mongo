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


#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bson_validate_gen.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <string>

#include <benchmark/benchmark.h>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

namespace mongo {
namespace {

// Returns number within a sensible range for human ages (measured in years) based on the given seed
// 'i'.
int pseudoRandomAge(uint64_t i) {
    return i * 8391 % 97 + 12;
}

// Returns a number in the range [10000, 99999] resembling a zip code based on the seed 'i'.
int pseudoRandomZipCode(uint64_t i) {
    return 10'000 + i * 316'731 % 90'000;
}

// Returns a string which resembles a 7 digit phone number, computed based on the seed 'i'.
std::string pseudoRandomPhoneNo(uint64_t i) {
    return fmt::format("{}-{:04d}", i * 2923 % 900 + 100, i * 32'347 % 9999);
}

// Returns a string whose prefix is a pseudorandom number calculated based on seed 'i', followed by
// 50 'a' characters in order to pad the string.
std::string pseudoRandomLongStr(uint64_t i) {
    return fmt::format("{}{:a<50s}", i * 234'397'31, "");
}

// Returns a pseudorandom number in the interval [0, 9,999,999] based on the seed 'i'.
int pseudoRandom7Digits(uint64_t i) {
    return i * 83'438 % 10'000'000;
}

BSONObj buildSampleObj(uint64_t i) {
    // clang-format off
    return BSON("_id" << OID::gen()
                << "name" << "Wile E. Coyote"
                << "age" << pseudoRandomAge(i)
                << "i" << static_cast<int>(i)
                << "address" << BSON(
                    "street" << "433 W 43rd St"
                    << "zip_code" << pseudoRandomZipCode(i)
                    << "city" << "New York")
                << "random" << pseudoRandom7Digits(i)
                << "phone_no" << pseudoRandomPhoneNo(i)
                << "long_string" << pseudoRandomLongStr(i));
    // clang-format on
}

BSONObj buildWideObj(uint64_t i, int numFields) {
    std::vector<std::variant<int, std::string>> possibleValues;
    possibleValues.reserve(6);
    possibleValues.emplace_back(pseudoRandomAge(i));
    possibleValues.emplace_back(static_cast<int>(i));
    possibleValues.emplace_back(pseudoRandomZipCode(i));
    possibleValues.emplace_back(pseudoRandom7Digits(i));
    possibleValues.emplace_back(pseudoRandomPhoneNo(i));
    possibleValues.emplace_back(pseudoRandomLongStr(i));

    BSONObjBuilder builder;
    for (int j = 0; j < numFields; ++j) {
        // Choose an 8-character field name based on 'j' by multiplying by a large prime number
        // and then representing the low-order bits as hex.
        uint64_t hash = (static_cast<uint64_t>(j) * 2654435761u) & 0xffffffff;
        std::string fieldName = fmt::format("{:08x}", hash);

        // Round robin through the list of possible values computed previously, dealing with the
        // fact that they may be either int or string.
        auto& value = possibleValues[j % possibleValues.size()];
        std::visit(
            OverloadedVisitor{
                [&builder, &fieldName](const auto& v) { builder.append(fieldName, v); },
            },
            value);
    }

    return builder.obj();
}

void BM_arrayBuilder(benchmark::State& state) {
    size_t totalBytes = 0;
    for (auto _ : state) {
        benchmark::ClobberMemory();
        BSONArrayBuilder array;
        for (auto j = 0; j < state.range(0); j++)
            array.append(j);
        totalBytes += array.len();
        benchmark::DoNotOptimize(array.done());
    }
    state.SetBytesProcessed(totalBytes);
}

void BM_arrayLookup(benchmark::State& state) {
    BSONArrayBuilder builder;
    auto len = state.range(0);
    auto totalBytes = len * 0;
    for (auto j = 0; j < len; j++)
        builder.append(j);
    BSONObj array = builder.done();

    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(array[len]);
        totalBytes += array.objsize();
    }
    state.SetBytesProcessed(totalBytes);
}


void BM_arrayStlIterate(benchmark::State& state) {
    BSONArrayBuilder builder;
    auto len = state.range(0);
    auto totalBytes = len * 0;
    for (auto j = 0; j < len; j++)
        builder.append(j);
    BSONObj array = builder.done();

    for (auto _ : state) {
        benchmark::ClobberMemory();
        long count = 0;
        for (auto&& e : array) {
            ++count;
            benchmark::DoNotOptimize(e);
        }
        invariant(count == len);
        totalBytes += array.objsize();
    }
    state.SetBytesProcessed(totalBytes);
}

void BM_arrayNonStlIterate(benchmark::State& state) {
    BSONArrayBuilder builder;
    auto len = state.range(0);
    auto totalBytes = len * 0;
    for (auto j = 0; j < len; j++)
        builder.append(j);
    BSONObj array = builder.done();

    for (auto _ : state) {
        benchmark::ClobberMemory();
        long count = 0;
        auto it = BSONObjIterator(array);
        while (it.more()) {
            auto e = it.next();
            ++count;
            benchmark::DoNotOptimize(e);
        }
        invariant(count == len);
        totalBytes += array.objsize();
    }
    state.SetBytesProcessed(totalBytes);
}

void BM_arrayStlIterateWithSize(benchmark::State& state) {
    BSONArrayBuilder builder;
    auto len = state.range(0);
    auto totalBytes = len * 0;
    for (auto j = 0; j < len; j++)
        builder.append(j);
    BSONObj array = builder.done();
    const auto emptyBSONObjSize = BSONObj().objsize();

    for (auto _ : state) {
        benchmark::ClobberMemory();
        long count = 0;
        auto objBytes = 0;
        for (auto&& e : array) {
            objBytes += e.size();
            ++count;
            benchmark::DoNotOptimize(e);
        }
        invariant(count == len);
        invariant(objBytes + emptyBSONObjSize == array.objsize());
        totalBytes += array.objsize();
    }
    state.SetBytesProcessed(totalBytes);
}

void BSONnFields(benchmark::State& state) {
    BSONArrayBuilder builder;
    auto len = state.range(0);
    auto totalBytes = len * 0;
    for (auto j = 0; j < len; j++)
        builder.append(j);
    BSONObj array = builder.done();

    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(array.nFields());
        totalBytes += array.objsize();
    }
    state.SetBytesProcessed(totalBytes);
}

void BM_bsonIteratorSortedConstruction(benchmark::State& state) {
    BSONArrayBuilder builder;
    auto len = state.range(0);
    auto totalBytes = len * 0;
    for (auto j = 0; j < len; j++)
        builder.append(j);
    BSONObj array = builder.done();

    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(BSONObjIteratorSorted(array));
        totalBytes += array.objsize();
    }
    state.SetBytesProcessed(totalBytes);
}

void BM_validate(benchmark::State& state) {
    BSONArrayBuilder builder;
    auto len = state.range(0);
    size_t totalSize = 0;
    for (auto j = 0; j < len; j++)
        builder.append(buildSampleObj(j));
    BSONObj array = builder.done();

    const auto& elem = array[0].Obj();
    auto status = validateBSON(elem.objdata(), elem.objsize());
    if (!status.isOK())
        LOGV2(4440100, "Validate failed", "elem"_attr = elem, "status"_attr = status);
    invariant(status);

    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(validateBSON(array.objdata(), array.objsize()));
        totalSize += array.objsize();
    }
    state.SetBytesProcessed(totalSize);
}

void BM_validate_contents(benchmark::State& state) {
    BSONArrayBuilder builder;
    auto len = state.range(0);
    size_t totalSize = 0;
    for (auto j = 0; j < len; j++)
        builder.append(buildSampleObj(j));
    BSONObj array = builder.done();

    const auto& elem = array[0].Obj();
    auto status = validateBSON(elem.objdata(), elem.objsize(), BSONValidateModeEnum::kFull);
    if (!status.isOK())
        LOGV2(6752100, "Validate failed", "elem"_attr = elem, "status"_attr = status);
    invariant(status);

    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(
            validateBSON(array.objdata(), array.objsize(), BSONValidateModeEnum::kFull));
        totalSize += array.objsize();
    }
    state.SetBytesProcessed(totalSize);
}

/**
 * Benchmark BSON validation for objects that have no nesting but many field names. The first range
 * argument (state.range(0)) indicates the number of wide objects to validate. The second range
 * argument (state.range(1)) specifies the number of fields that each of these objects should
 * contain.
 *
 * The template parameter 'M' specifies the validation mode (default, extended, or full).
 */
template <BSONValidateModeEnum M>
void BM_validateWideObj(benchmark::State& state) {
    auto arrayLen = state.range(0);
    auto numFields = state.range(1);

    BSONArrayBuilder builder;
    size_t totalSize = 0;
    for (auto i = 0; i < arrayLen; i++) {
        builder.append(buildWideObj(i, numFields));
    }
    BSONObj array = builder.done();

    const auto& elem = array[0].Obj();
    auto status = validateBSON(elem.objdata(), elem.objsize(), M);
    if (!status.isOK())
        LOGV2(10101800, "Validate failed", "elem"_attr = elem, "status"_attr = status);
    invariant(status);

    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(validateBSON(array.objdata(), array.objsize(), M));
        totalSize += array.objsize();
    }
    state.SetBytesProcessed(totalSize);
}

BENCHMARK(BM_arrayBuilder)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_arrayLookup)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_arrayNonStlIterate)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_arrayStlIterate)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_arrayStlIterateWithSize)->Ranges({{{1}, {100'000}}});
BENCHMARK(BSONnFields)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_bsonIteratorSortedConstruction)->Ranges({{{1}, {100'000}}});

// BSON validation benchmarks.
BENCHMARK(BM_validate)->Ranges({{{1}, {1'000}}});
BENCHMARK(BM_validate_contents)->Ranges({{{1}, {1'000}}});
BENCHMARK_TEMPLATE(BM_validateWideObj, BSONValidateModeEnum::kDefault)
    ->Ranges({{64, 512}, {50, 1'000}});
BENCHMARK_TEMPLATE(BM_validateWideObj, BSONValidateModeEnum::kExtended)
    ->Ranges({{64, 512}, {50, 1'000}});
BENCHMARK_TEMPLATE(BM_validateWideObj, BSONValidateModeEnum::kFull)
    ->Ranges({{64, 512}, {50, 1'000}});

void BM_objBuilderAppendInt(benchmark::State& state) {
    int n = state.range(0);
    int reps = 0;
    for (auto _ : state) {
        BSONObjBuilder bob;
        for (int i = 0; i < n; ++i) {
            bob.append("a"_sd, i);
        }
        benchmark::DoNotOptimize(bob.done());
        ++reps;
    }
    state.SetItemsProcessed(n * reps);
}

void BM_objBuilderAppendIntStreamOperator(benchmark::State& state) {
    int n = state.range(0);
    int reps = 0;
    for (auto _ : state) {
        BSONObjBuilder bob;
        for (int i = 0; i < n; ++i) {
            bob << "a" << i;
        }
        benchmark::DoNotOptimize(bob.done());
        ++reps;
    }
    state.SetItemsProcessed(n * reps);
}

void BM_objBuilderAppendStreamedValue(benchmark::State& state) {
    int n = state.range(0);
    int reps = 0;
    for (auto _ : state) {
        BSONObjBuilder bob;
        for (int i = 0; i < n; ++i) {
            bob << "a" << Value(i);
        }
        benchmark::DoNotOptimize(bob.done());
        ++reps;
    }
    state.SetItemsProcessed(n * reps);
}

BENCHMARK(BM_objBuilderAppendInt)->DenseRange(1, 8)->Range(9, 1 << 20);
BENCHMARK(BM_objBuilderAppendIntStreamOperator)->DenseRange(1, 8)->Range(9, 1 << 20);
BENCHMARK(BM_objBuilderAppendStreamedValue)->DenseRange(1, 8)->Range(9, 1 << 20);

}  // namespace
}  // namespace mongo
