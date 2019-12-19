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

#include "mongo/platform/basic.h"
#include "mongo/util/base64.h"
#include <benchmark/benchmark.h>
#include <fmt/format.h>

namespace mongo {
namespace {


void BM_Base64EncodeString(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in(len, 'x');
    std::string out;

    for (auto _ : state) {
        benchmark::DoNotOptimize(out = base64::encode(in));
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64EncodeString)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);

void BM_Base64EncodeStringStream(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in(len, 'x');

    for (auto _ : state) {
        std::stringstream out;
        base64::encode(out, in);
        benchmark::DoNotOptimize(out.str());
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64EncodeStringStream)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);

void BM_Base64EncodeFmtMemoryBuffer(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in(len, 'x');

    for (auto _ : state) {
        fmt::memory_buffer buf;
        base64::encode(buf, in);
        benchmark::DoNotOptimize(fmt::to_string(buf));
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64EncodeFmtMemoryBuffer)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);


void BM_Base64DecodeString(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in = base64::encode(std::string(len, 'x'));
    for (auto _ : state) {
        benchmark::DoNotOptimize(base64::decode(in));
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64DecodeString)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);

void BM_Base64DecodeStringStream(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in = base64::encode(std::string(len, 'x'));
    for (auto _ : state) {
        std::stringstream out;
        base64::decode(out, in);
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64DecodeStringStream)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);

void BM_Base64DecodeFmtMemoryBuffer(benchmark::State& state) {
    size_t len = state.range(0);
    size_t items = 0;
    std::string in = base64::encode(std::string(len, 'x'));
    for (auto _ : state) {
        fmt::memory_buffer buffer;
        base64::decode(buffer, in);
        items += in.size();
    }
    state.SetItemsProcessed(items);
}

BENCHMARK(BM_Base64DecodeFmtMemoryBuffer)
    ->Arg(1)
    ->Arg(16)
    ->Arg(1 << 10)
    ->Arg(2 << 10)
    ->Arg(3 << 10)
    ->Arg(4 << 10);

}  // namespace
}  // namespace mongo
