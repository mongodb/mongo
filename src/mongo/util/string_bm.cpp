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

#include <algorithm>
#include <cctype>  // NOLINT
#include <functional>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include <benchmark/benchmark.h>

#include "mongo/base/simple_string_data_comparator.h"
#include "mongo/base/string_data.h"
#include "mongo/util/ctype.h"

// Verify the performance of our string processing algorithms.
// This can include StringData, util/str utilities, etc.

namespace mongo {
namespace {

std::string makeString(size_t size) {
    StringData fill = "The quick brown fox jumped over the lazy dog. ";
    std::string s;
    while (s.size() < size) {
        size_t avail = size - s.size();
        StringData fillSub = fill.substr(0, std::min(avail, fill.size()));
        s.append(fillSub.begin(), fillSub.end());
    }
    return s;
}

void BM_StringDataEqualCaseInsensitive(benchmark::State& state) {
    std::uint64_t items = 0;
    std::string s1 = makeString(1000);
    std::string s2 = s1;
    StringData sd1 = s1;
    for (auto _ : state) {
        benchmark::DoNotOptimize(sd1.equalCaseInsensitive(s2));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_StringDataEqualCaseInsensitive);

void BM_StdToLower(benchmark::State& state) {
    std::uint64_t items = 0;
    std::string s1 = makeString(1000);
    for (auto _ : state) {
        for (char& c : s1)
            benchmark::DoNotOptimize(c = std::tolower(c));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_StdToLower);

void BM_MongoCtypeToLower(benchmark::State& state) {
    std::uint64_t items = 0;
    std::string s1 = makeString(1000);
    for (auto _ : state) {
        for (char& c : s1)
            benchmark::DoNotOptimize(c = ctype::toLower(c));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_MongoCtypeToLower);

void BM_StdIsAlpha(benchmark::State& state) {
    std::uint64_t items = 0;
    std::string s1 = makeString(1000);
    for (auto _ : state) {
        for (char& c : s1)
            benchmark::DoNotOptimize(std::isalpha(c));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_StdIsAlpha);

void BM_MongoCtypeIsAlpha(benchmark::State& state) {
    std::uint64_t items = 0;
    std::string s1 = makeString(1000);
    for (auto _ : state) {
        for (char& c : s1)
            benchmark::DoNotOptimize(ctype::isAlpha(c));
        ++items;
    }
    state.SetItemsProcessed(items);
}
BENCHMARK(BM_MongoCtypeIsAlpha);

}  // namespace
}  // namespace mongo
