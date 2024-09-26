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

#include "mongo/base/data_view.h"
#include "mongo/bson/column/simple8b.h"
#include "mongo/bson/column/simple8b_type_util.h"
#include "mongo/logv2/log.h"
#include "mongo/util/hex.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

static constexpr int128_t add(int128_t lhs, int128_t rhs) {
    return static_cast<int128_t>(static_cast<uint128_t>(lhs) + static_cast<uint128_t>(rhs));
}

extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    using namespace mongo;

    // We need at least 8 bytes
    if (Size < sizeof(uint64_t))
        return 0;

    // Find a contiguous buffer containing valid simple8b selectors
    size_t i = 0;
    for (; i < Size - sizeof(uint64_t); i += sizeof(uint64_t)) {
        uint64_t encoded = mongo::ConstDataView(Data + i).read<LittleEndian<uint64_t>>();
        auto selector = encoded & simple8b_internal::kBaseSelectorMask;
        // Selector 0 is not valid
        if (selector == 0) {
            break;
        }

        // Validate extended selectors for selector 7 and 8
        if (selector == 7) {
            encoded >>= 4;
            selector = encoded & simple8b_internal::kBaseSelectorMask;
            if (selector > 9)
                break;
        } else if (selector == 8) {
            encoded >>= 4;
            selector = encoded & simple8b_internal::kBaseSelectorMask;
            if (selector > 13)
                break;
        }
    }

    // We need at least one simple8b block
    if (i < sizeof(uint64_t))
        return 0;

    // Verify block sum vs iterator based implementation.
    {
        Simple8b<uint128_t> s8b(Data, i);
        int128_t oldSum = 0;
        for (auto&& val : s8b) {
            if (val) {
                oldSum = add(oldSum, Simple8bTypeUtil::decodeInt(*val));
            }
        }

        uint64_t prev = 0xE;  // Previous value 0, this is one simple8b value containing a zero.
        int128_t sum = simple8b::sum<int128_t>(Data, i, prev);

        if (sum != oldSum) {
            LOGV2_DEBUG(8384500,
                        2,
                        "simple8b::sum is different compared to reference implementation",
                        "input"_attr = hexblob::encode(Data, i),
                        "sumLow"_attr = absl::Int128Low64(sum),
                        "sumHigh"_attr = absl::Int128High64(sum),
                        "oldSumLow"_attr = absl::Int128Low64(oldSum),
                        "oldSumHigh"_attr = absl::Int128High64(oldSum));
        }

        // This will effectively cause the fuzzer to find differences between both implementations
        // (as they'd lead to crashes), while using edge cases leading to interesting control flow
        // paths in both implementations.
        invariant(sum == oldSum);
    }


    return 0;
}
