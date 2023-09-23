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


#include <benchmark/benchmark.h>
#include <cstddef>
#include <cstdint>
#include <fmt/format.h>
#include <string>
#include <utility>


#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bson_validate.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/util/assert_util_core.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault


namespace mongo {
namespace {

BSONObj buildSampleObj(long long unsigned int i) {

    int age = i * 8391 % 97 + 12;
    int istr = (1 << 16) + i;
    int zip_code = 10'000 + i * 316'731 % 90'000;
    int random = i * 83'438 % 9'999'999;
    std::string phone_no = fmt::format("{}-{:04d}", i * 2923 % 900 + 100, i * 32'347 % 9999);
    std::string long_string = fmt::format("{}{:a<50s}", i * 234'397'31, "");

    return BSON(GENOID << "name"
                       << "Wile E. Coyote"
                       << "age" << age << "i" << istr << "address"
                       << BSON("street"
                               << "433 W 43rd St"
                               << "zip_code" << zip_code << "city"
                               << "New York")
                       << "random" << random << "phone_no" << phone_no << "long_string"
                       << long_string);
}
}  // namespace

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
    auto totalLen = len * 0;
    for (auto j = 0; j < len; j++)
        builder.append(j);
    BSONObj array = builder.done();

    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(array[len]);
        totalLen += len;
    }
    state.SetItemsProcessed(totalLen);
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
    invariant(status.isOK());

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
    auto status = validateBSON(elem.objdata(), elem.objsize(), BSONValidateMode::kFull);
    if (!status.isOK())
        LOGV2(6752100, "Validate failed", "elem"_attr = elem, "status"_attr = status);
    invariant(status.isOK());

    for (auto _ : state) {
        benchmark::ClobberMemory();
        benchmark::DoNotOptimize(
            validateBSON(array.objdata(), array.objsize(), BSONValidateMode::kFull));
        totalSize += array.objsize();
    }
    state.SetBytesProcessed(totalSize);
}

BENCHMARK(BM_arrayBuilder)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_arrayLookup)->Ranges({{{1}, {100'000}}});
BENCHMARK(BM_validate)->Ranges({{{1}, {1'000}}});
BENCHMARK(BM_validate_contents)->Ranges({{{1}, {1'000}}});

}  // namespace mongo
