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

static constexpr mongo::int128_t add(mongo::int128_t lhs, mongo::int128_t rhs) {
    return static_cast<mongo::int128_t>(static_cast<mongo::uint128_t>(lhs) +
                                        static_cast<mongo::uint128_t>(rhs));
}

extern "C" int LLVMFuzzerTestOneInput(const char* Data, size_t Size) {
    using namespace mongo;

    // The size of the Simple8b buffer in bytes needs to be a multiple of 8.
    size_t bufferSize = (Size >> 3) << 3;

    // We need at least one simple8b block
    if (bufferSize < sizeof(uint64_t))
        return 0;

    // Verify block sum vs iterator based implementation.
    {
        auto oldSum = [&]() -> boost::optional<int128_t> {
            try {
                int128_t sum = 0;
                Simple8b<uint128_t> s8b(Data, bufferSize);
                for (auto&& val : s8b) {
                    if (val)
                        sum = add(sum, Simple8bTypeUtil::decodeInt(*val));
                }
                return sum;
            } catch (const DBException&) {
                return boost::none;
            }
        }();

        auto sum = [&]() -> boost::optional<int128_t> {
            try {
                uint64_t prev =
                    0xE;  // Previous value 0, this is one simple8b value containing a zero.
                return simple8b::sum<int128_t>(Data, bufferSize, prev);
            } catch (const DBException&) {
                return boost::none;
            }
        }();

        if (sum != oldSum) {
            LOGV2_DEBUG(8384500,
                        2,
                        "simple8b::sum is different compared to reference implementation",
                        "input"_attr = hexblob::encode(Data, bufferSize),
                        "sumLow"_attr = sum.map(absl::Int128Low64),
                        "sumHigh"_attr = sum.map(absl::Int128High64),
                        "oldSumLow"_attr = oldSum.map(absl::Int128Low64),
                        "oldSumHigh"_attr = oldSum.map(absl::Int128High64));
            return 0;
        }

        // This will effectively cause the fuzzer to find differences between both implementations
        // (as they'd lead to crashes), while using edge cases leading to interesting control flow
        // paths in both implementations.
        invariant(sum == oldSum);
    }


    return 0;
}
