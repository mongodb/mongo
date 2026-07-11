// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/change_stream_pre_image_id_util.h"

#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <limits>

namespace mongo {

namespace {
using namespace change_stream_pre_image_id_util;

const UUID kNsUUID = UUID::gen();

TEST(ChangeStreamPreImageIdUtilTest, GetPreImageTimestampAndApplyOsIndex) {
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

TEST(ChangeStreamPreImageIdUtilTest, TimestampAndApplyOpsNumberConversionEmpty) {
    const Timestamp ts = Timestamp();
    const int64_t applyOpsIndex = 0;

    auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex);
    auto converted = timestampAndApplyOpsIndexFromNumber(number);
    ASSERT_EQ(ts, converted.first);
    ASSERT_EQ(applyOpsIndex, converted.second);
}

TEST(ChangeStreamPreImageIdUtilTest, TimestampAndApplyOpsNumberConversionToNumber) {
    const Timestamp ts = {Seconds(992412), 1};
    const int64_t applyOpsIndex = 1;

    // Convert to number.
    auto number = timestampAndApplyOpsIndexToNumber(ts, applyOpsIndex);

    // Convert back from number.
    auto converted = timestampAndApplyOpsIndexFromNumber(number);
    ASSERT_EQ(ts, converted.first);
    ASSERT_EQ(applyOpsIndex, converted.second);
}

TEST(ChangeStreamPreImageIdUtilTest, TimestampAndApplyOpsNumericIncrements) {
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

TEST(ChangeStreamPreImageIdUtilTest, TimestampAndApplyOpsNumericDecrements) {
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

TEST(ChangeStreamPreImageIdUtilTest, TimestampAndApplyOpsIndexNumericOverflows) {
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

DEATH_TEST_REGEX(ChangeStreamPreImageIdUtilDeathTest,
                 TimestampAndApplyOpsNumberConversionNegativeApplyOpsIndex,
                 "invariant.*failure") {
    // Triggers an invariant failure.
    timestampAndApplyOpsIndexToNumber(Timestamp(), -1);
}

}  // namespace

}  // namespace mongo
