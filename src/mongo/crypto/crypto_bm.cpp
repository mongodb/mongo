/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/base/data_range.h"
#include "mongo/base/data_type_endian.h"
#include "mongo/base/data_view.h"
#include "mongo/crypto/fle_crypto.h"
#include "mongo/crypto/fle_crypto_types.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/util/assert_util.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

namespace mongo {
namespace {

PrfBlock hash(ConstDataRange cdr) {
    auto block = SHA256Block::computeHash(cdr.data<uint8_t>(), cdr.length());
    return FLEUtil::blockToArray(block);
}

PrfBlock hash(uint64_t value) {
    std::array<char, sizeof(uint64_t)> bufValue;
    DataView(bufValue.data()).write<LittleEndian<uint64_t>>(value);
    return hash(bufValue);
}

std::vector<char> hmacKey = {0x63, 0x63, 0x66, 0x63, 0x38, 0x65, 0x61, 0x32, 0x66, 0x31, 0x30,
                             0x63, 0x38, 0x61, 0x35, 0x39, 0x38, 0x34, 0x35, 0x65, 0x63, 0x30,
                             0x31, 0x63, 0x39, 0x38, 0x38, 0x65, 0x37, 0x30, 0x35, 0x37};

void BM_HMAC_SHA256(benchmark::State& state) {
    // Perform setup here
    uint64_t i = 0;

    uint64_t N = state.range(0);

    for (auto _ : state) {

        // This code gets timed
        for (size_t j = 0; j < N; ++j) {
            HmacContext hmacCtx;
            FLEUtil::prf(&hmacCtx, hmacKey, ++i);
        }
    }
}

BENCHMARK(BM_HMAC_SHA256)->Arg(1)->Arg(100)->Arg(1000)->Arg(10000)->Unit(benchmark::kMillisecond);

void BM_HMAC_SHA256_High_Perf(benchmark::State& state) {
    // Perform setup here
    uint64_t i = 0;

    uint64_t N = state.range(0);

    HmacContext hmacCtx;
    for (auto _ : state) {
        // This code gets timed
        for (size_t j = 0; j < N; ++j) {
            FLEUtil::prf(&hmacCtx, hmacKey, ++i);
        }
    }
}

BENCHMARK(BM_HMAC_SHA256_High_Perf)
    ->Arg(1)
    ->Arg(100)
    ->Arg(1000)
    ->Arg(10000)
    ->Unit(benchmark::kMillisecond);


void BM_SHA256(benchmark::State& state) {
    // Perform setup here
    uint64_t i = 0;

    uint64_t N = state.range(0);

    for (auto _ : state) {

        // This code gets timed
        for (size_t j = 0; j < N; ++j) {
            hash(++i);
        }
    }
}

BENCHMARK(BM_SHA256)->Arg(1)->Arg(100)->Arg(1000)->Arg(10000)->Unit(benchmark::kMillisecond);

std::vector<uint8_t> aesKey = {0x83, 0xcf, 0x8e, 0x86, 0x46, 0xfd, 0x42, 0x31, 0x8a, 0x21, 0x13,
                               0xe1, 0x33, 0x3d, 0x51, 0x51, 0x63, 0xd5, 0xc0, 0xb2, 0x1d, 0xdb,
                               0xfe, 0xc3, 0x74, 0x10, 0x7b, 0x71, 0xec, 0xbe, 0xc1, 0x65};
std::vector<uint8_t> aesBlock = {
    0xe4, 0x1c, 0x6d, 0x48, 0x41, 0x08, 0x21, 0x91, 0x72, 0xde, 0x42, 0x1a, 0x42, 0x6b, 0xbe, 0x52,
    0x6f, 0xd6, 0x3f, 0xec, 0xa2, 0x46, 0xe2, 0x6f, 0x5f, 0x9b, 0x59, 0x38, 0x0e, 0x6b, 0x35, 0xf0,
    0x7c, 0x57, 0x2c, 0x9e, 0xb6, 0x20, 0x7f, 0x00, 0xfb, 0xe1, 0xbf, 0x3d, 0x4c, 0x01, 0xf4};

void BM_AES256(benchmark::State& state) {
    // Perform setup here
    uint64_t N = state.range(0);

    for (auto _ : state) {

        // This code gets timed
        for (size_t j = 0; j < N; ++j) {
            auto sw = FLEUtil::decryptData(aesKey, aesBlock);
            uassertStatusOK(sw);
        }
    }
}

BENCHMARK(BM_AES256)->Arg(1)->Arg(100)->Arg(1000)->Arg(10000)->Unit(benchmark::kMillisecond);

}  // namespace
}  // namespace mongo
