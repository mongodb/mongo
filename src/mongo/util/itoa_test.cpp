// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/itoa.h"

#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <iterator>
#include <limits>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace mongo {
namespace {

TEST(ItoA, StringDataEquality) {
    std::vector<uint64_t> cases;
    auto caseInsert = std::back_inserter(cases);
    static constexpr auto kMax = std::numeric_limits<uint64_t>::max();
    {
        // Manually-specified basics.
        const uint64_t interesting[]{
            0,
            1,
            10,
            11,
            12,
            99,
            100,
            101,
            110,
            133,
            1446,
            17789,
            192923,
            2389489,
            29313479,
            1928127389,
            kMax - 1,
            kMax,
        };
        cases.insert(cases.end(), std::begin(interesting), std::end(interesting));
    }

    {
        // Test the neighborhood of several powers of ten.
        uint64_t tenPower = 10;
        for (int i = 0; i < 10; ++i) {
            *caseInsert++ = tenPower - 1;
            *caseInsert++ = tenPower;
            *caseInsert++ = tenPower + 1;
            tenPower *= 10;
        }
    }

    static constexpr uint64_t kRampTop = 100'000;

    // Ramp of first several thousand values.
    for (uint64_t i = 0; i < kRampTop; ++i) {
        *caseInsert++ = i;
    }

    {
        // Large # of pseudorandom integers, spread over the remaining powers of ten.
        std::mt19937 gen(0);  // deterministic seed 0
        for (uint64_t i = kRampTop;; i *= 10) {
            auto upper = (i >= kMax / 10) ? kMax : 10 * i;
            std::uniform_int_distribution<uint64_t> dis(i, upper);
            for (uint64_t i = 0; i < 100'000; ++i) {
                *caseInsert++ = dis(gen);
            }
            if (upper == kMax)
                break;
        }
    }

    for (const auto& i : cases) {
        ItoA a{i};
        std::string expected = std::to_string(i);
        ASSERT_EQ(std::string_view(a), expected) << ", i=" << i;
        ASSERT_EQ(a.toStringData(), expected) << ", i=" << i;
    }
}

}  // namespace
}  // namespace mongo
