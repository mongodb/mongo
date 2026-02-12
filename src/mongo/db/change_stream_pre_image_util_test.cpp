/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/db/change_stream_pre_image_util.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <limits>

namespace mongo {

namespace {
using namespace change_stream_pre_image_util;

const UUID kNsUUID = UUID::gen();

TEST(ChangeStreamPreImageUtilTest, GetPreImageTimestampAndApplyOsIndex) {
    {
        const Timestamp ts = Timestamp(1, 1);
        const int64_t applyOpsIndex = 0;
        const auto preImage = ChangeStreamPreImageId{kNsUUID, ts, applyOpsIndex};

        auto recordId = toRecordId(preImage);
        auto parts = getPreImageTimestampAndApplyOpsIndex(recordId);
        ASSERT_EQ(ts, parts.first);
        ASSERT_EQ(applyOpsIndex, parts.second);

        ASSERT_EQ(ts, getPreImageTimestamp(recordId));
    }

    {
        const Timestamp ts = Timestamp(69492567, 1523);
        const int64_t applyOpsIndex = 593972;
        const auto preImage = ChangeStreamPreImageId{kNsUUID, ts, applyOpsIndex};

        auto recordId = toRecordId(preImage);
        auto parts = getPreImageTimestampAndApplyOpsIndex(recordId);
        ASSERT_EQ(ts, parts.first);
        ASSERT_EQ(applyOpsIndex, parts.second);

        ASSERT_EQ(ts, getPreImageTimestamp(recordId));
    }
}

TEST(ChangeStreamPreImageUtilTest, TimestampAndApplyOpsNumberConversionEmpty) {
    const Timestamp ts = Timestamp();
    const int64_t applyOpsIndex = 0;

    auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex);
    auto converted = timestampAndApplyOpsIndexFromNumber(number);
    ASSERT_EQ(ts, converted.first);
    ASSERT_EQ(applyOpsIndex, converted.second);
}

TEST(ChangeStreamPreImageUtilTest, TimestampAndApplyOpsNumberConversionToNumber) {
    const Timestamp ts = {Seconds(992412), 1};
    const int64_t applyOpsIndex = 1;

    // Convert to number.
    auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex);

    // Convert back from number.
    auto converted = timestampAndApplyOpsIndexFromNumber(number);
    ASSERT_EQ(ts, converted.first);
    ASSERT_EQ(applyOpsIndex, converted.second);
}

TEST(ChangeStreamPreImageUtilTest, TimestampAndApplyOpsNumericIncrements) {
    const Timestamp ts = {Seconds(992412), 1};
    const int64_t applyOpsIndex = 1;

    {
        auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) + 1;
        auto converted = timestampAndApplyOpsIndexFromNumber(number);
        ASSERT_EQ(ts, converted.first);
        ASSERT_EQ(applyOpsIndex + 1, converted.second);
    }

    // Increment by more than 1.
    {
        auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) + 10;
        auto converted = timestampAndApplyOpsIndexFromNumber(number);
        ASSERT_EQ(ts, converted.first);
        ASSERT_EQ(applyOpsIndex + 10, converted.second);
    }

    // Increment by a larger value.
    {
        auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) + (uint128_t(1) << 65);
        auto converted = timestampAndApplyOpsIndexFromNumber(number);
        const Timestamp expected = Timestamp{Seconds(992412), 5};
        ASSERT_EQ(expected, converted.first);
        ASSERT_EQ(1, converted.second);
    }
}

TEST(ChangeStreamPreImageUtilTest, TimestampAndApplyOpsNumericDecrements) {
    const Timestamp ts = {Seconds(992412), 1};
    const int64_t applyOpsIndex = 1;

    {
        auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) - 1;
        auto converted = timestampAndApplyOpsIndexFromNumber(number);
        ASSERT_EQ(ts, converted.first);
        ASSERT_EQ(applyOpsIndex - 1, converted.second);
    }

    // Decrement by more than 1.
    {
        auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) - 10;
        auto converted = timestampAndApplyOpsIndexFromNumber(number);
        const Timestamp expected = Timestamp{Seconds(992412), 0};
        ASSERT_EQ(expected, converted.first);
        ASSERT_EQ(9223372036854775799LL, converted.second);
    }

    // Decrement by a larger value.
    {
        auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) - (uint128_t(1) << 65);
        auto converted = timestampAndApplyOpsIndexFromNumber(number);
        const Timestamp expected = Timestamp{Seconds(992411), 4294967293ULL};
        ASSERT_EQ(expected, converted.first);
        ASSERT_EQ(1, converted.second);
    }
}

TEST(ChangeStreamPreImageUtilTest, TimestampAndApplyOpsIndexNumericOverflows) {
    // Use maximum value for Timestamp seconds part and do arithmetic.
    {
        const Timestamp ts = Timestamp{Seconds(std::numeric_limits<uint32_t>::max()), 123};
        const int64_t applyOpsIndex = 42;

        // No overflow yet.
        auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) + 1;
        auto converted = timestampAndApplyOpsIndexFromNumber(number);
        ASSERT_EQ(ts, converted.first);
        ASSERT_EQ(applyOpsIndex + 1, converted.second);

        number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) + (uint128_t(1) << 63);
        converted = timestampAndApplyOpsIndexFromNumber(number);
        const Timestamp expected = Timestamp{std::numeric_limits<uint32_t>::max(), 124};
        ASSERT_EQ(expected, converted.first);
        ASSERT_EQ(applyOpsIndex, converted.second);
    }

    // Use maximum value for Timestamp increment part and overflow it.
    {
        const Timestamp ts = Timestamp{Seconds(12345), std::numeric_limits<uint32_t>::max()};
        const int64_t applyOpsIndex = 5;

        // No overflow yet.
        auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) + 1;
        auto converted = timestampAndApplyOpsIndexFromNumber(number);
        ASSERT_EQ(ts, converted.first);
        ASSERT_EQ(applyOpsIndex + 1, converted.second);

        // Overflow it into the next second.
        number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) + (uint128_t(1) << 63);
        converted = timestampAndApplyOpsIndexFromNumber(number);
        const Timestamp expected = Timestamp{Seconds(12346), 0};
        ASSERT_EQ(expected, converted.first);
        ASSERT_EQ(applyOpsIndex, converted.second);
    }

    // Use maximum value for applyOpsIndex part and overflow it.
    {
        const Timestamp ts = Timestamp{Seconds(12345), 12345};
        const int64_t applyOpsIndex = std::numeric_limits<int64_t>::max();

        auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex) + 1;
        auto converted = timestampAndApplyOpsIndexFromNumber(number);
        const Timestamp expected = Timestamp{Seconds(12345), 12346};
        ASSERT_EQ(expected, converted.first);
        ASSERT_EQ(0, converted.second);
    }
}

DEATH_TEST_REGEX(ChangeStreamPreImageUtilDeathTest,
                 TimestampAndApplyOpsNumberConversionNegativeApplyOpsIndex,
                 "invariant.*failure") {
    // Triggers an invariant failure.
    timestampAndApplyOpsIndexToNumber(Timestamp(), -1);
}

}  // namespace

}  // namespace mongo
