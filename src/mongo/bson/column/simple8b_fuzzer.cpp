// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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

struct LastResult {
    boost::optional<mongo::uint128_t> encoded;
    boost::optional<mongo::int128_t> decoded;
};

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

        auto oldLast = [&]() -> boost::optional<LastResult> {
            try {
                boost::optional<uint128_t> last = uint128_t{0};
                Simple8b<uint128_t> s8b(Data, bufferSize);
                for (auto&& val : s8b) {
                    last = val;
                }
                if (last) {
                    return LastResult{last, Simple8bTypeUtil::decodeInt(*last)};
                } else {
                    return LastResult{boost::optional<uint128_t>(boost::none),
                                      boost::optional<int128_t>(boost::none)};
                }

            } catch (const DBException&) {
                return boost::none;
            }
        }();

        auto sum = [&]() -> boost::optional<int128_t> {
            try {
                uint64_t prev = simple8b::kSingleZero;
                return simple8b::sum<int128_t>(Data, bufferSize, prev);
            } catch (const DBException&) {
                return boost::none;
            }
        }();

        auto last = [&]() -> boost::optional<LastResult> {
            try {
                uint64_t prev1 = simple8b::kSingleZero;
                uint64_t prev2 = simple8b::kSingleZero;
                return LastResult{simple8b::last<uint128_t>(Data, bufferSize, prev1),
                                  simple8b::last<int128_t>(Data, bufferSize, prev2)};
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
        // simple8b::last is not required to decode everything so an invalid binary might not throw.
        if (last && oldLast) {
            invariant(last->encoded == oldLast->encoded);
            invariant(last->decoded == oldLast->decoded);
        }
    }

    // Verify simple8b::dense agrees with iterator-based missing detection.
    {
        auto iteratorHasMissing = [&]() -> boost::optional<bool> {
            try {
                Simple8b<uint128_t> s8b(Data, bufferSize);
                for (auto&& val : s8b) {
                    if (!val)
                        return true;
                }
                return false;
            } catch (const DBException&) {
                return boost::none;
            }
        }();

        auto blockDense = [&]() -> boost::optional<bool> {
            try {
                return simple8b::dense(Data, bufferSize);
            } catch (const DBException&) {
                return boost::none;
            }
        }();

        invariant(iteratorHasMissing.has_value() == blockDense.has_value());

        if (iteratorHasMissing && blockDense) {
            invariant(*blockDense != *iteratorHasMissing);
        }
    }

    return 0;
}
