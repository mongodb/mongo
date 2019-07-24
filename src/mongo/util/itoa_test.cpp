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

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/decimal_counter.h"
#include "mongo/util/itoa.h"

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
        ASSERT_EQ(StringData(ItoA{i}), std::to_string(i)) << ", i=" << i;
    }
}

}  // namespace
}  // namespace mongo
