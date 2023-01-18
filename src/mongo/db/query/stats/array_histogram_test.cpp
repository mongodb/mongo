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
#include <limits>

#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/exec/sbe/values/value.h"
#include "mongo/db/query/stats/max_diff.h"
#include "mongo/db/query/stats/value_utils.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/time_support.h"

namespace mongo::stats {

TEST(ArrayHistogram, BSONEdgeValues) {
    const std::vector<SBEValue> values{
        SBEValue{sbe::value::TypeTags::Nothing, {}},

        makeInt32Value(std::numeric_limits<int32_t>::min()),
        makeInt32Value(std::numeric_limits<int32_t>::max()),

        makeInt64Value(std::numeric_limits<int>::min()),
        makeInt64Value(std::numeric_limits<int>::max()),

        makeDoubleValue(std::numeric_limits<double>::min()),
        makeDoubleValue(std::numeric_limits<double>::max()),
        makeDoubleValue(std::numeric_limits<double>::infinity()),
        // TODO SERVER-72807: Support NaN in histograms
        // makeDoubleValue(std::numeric_limits<double>::quiet_NaN()),
        // makeDoubleValue(std::numeric_limits<double>::signaling_NaN()),

        makeDateValue(Date_t::min()),
        makeDateValue(Date_t::max()),

        makeTimestampValue(Timestamp::min()),
        makeTimestampValue(Timestamp::max()),

        SBEValue{sbe::value::TypeTags::Boolean, true},
        SBEValue{sbe::value::TypeTags::Boolean, false},

        makeNullValue(),

        sbe::value::makeNewString(""),
        sbe::value::makeNewString("a"),
        sbe::value::makeNewString("aaaaaaaaaaaaaaaaaa"),  // Force heap-allocated string
        sbe::value::makeNewString(std::string(10000, 'a')),
        // TODO SERVER-72850: Support strings with Unicode characters in histograms
        // sbe::value::makeNewString("😊"),

        SBEValue{sbe::value::TypeTags::MinKey, {}},
        SBEValue{sbe::value::TypeTags::MaxKey, {}},

        sbe::value::makeCopyDecimal(Decimal128::kLargestNegative),
        sbe::value::makeCopyDecimal(Decimal128::kSmallestNegative),
        sbe::value::makeCopyDecimal(Decimal128::kSmallestPositive),
        sbe::value::makeCopyDecimal(Decimal128::kLargestPositive),
        sbe::value::makeCopyDecimal(Decimal128::kNormalizedZero),
        sbe::value::makeCopyDecimal(Decimal128::kLargestNegativeExponentZero),
        sbe::value::makeCopyDecimal(Decimal128::kNegativeInfinity),
        sbe::value::makeCopyDecimal(Decimal128::kPositiveInfinity),
        // TODO SERVER-72807: Support NaN in histograms
        // sbe::value::makeCopyDecimal(Decimal128::kNegativeNaN),
        // sbe::value::makeCopyDecimal(Decimal128::kPositiveNaN),

        sbe::value::makeNewArray(),
        sbe::value::makeCopyArray([] {
            sbe::value::Array largeArray;
            largeArray.reserve(10000);
            for (int i = 0; i < 10000; ++i) {
                largeArray.push_back(sbe::value::TypeTags::NumberInt64, i);
            }
            return largeArray;
        }()),
        sbe::value::makeNewObject(),
        sbe::value::makeNewObjectId(),
    };
    auto ah = createArrayEstimator(values, ScalarHistogram::kMaxBuckets);
    // We are relying on the fact that 'createArrayEstimator()' performs validation of the histogram
    // upon construction.

    TypeCounts expectedTypeCounts = {
        {sbe::value::TypeTags::Nothing, 1},
        {sbe::value::TypeTags::NumberInt32, 2},
        {sbe::value::TypeTags::NumberInt64, 2},
        {sbe::value::TypeTags::NumberDouble, 3},
        {sbe::value::TypeTags::Date, 2},
        {sbe::value::TypeTags::Timestamp, 2},
        {sbe::value::TypeTags::Boolean, 2},
        {sbe::value::TypeTags::Null, 1},
        {sbe::value::TypeTags::StringSmall, 2},
        {sbe::value::TypeTags::StringBig, 2},
        {sbe::value::TypeTags::MinKey, 1},
        {sbe::value::TypeTags::MaxKey, 1},
        {sbe::value::TypeTags::NumberDecimal, 8},
        {sbe::value::TypeTags::Array, 2},
        {sbe::value::TypeTags::Object, 1},
        {sbe::value::TypeTags::ObjectId, 1},
    };
    ASSERT_EQ(expectedTypeCounts, ah->getTypeCounts());

    // TODO SERVER-72997: Fix this test. We currently need to specify at least 20 buckets for this
    // test to pass.
    // Verify that we can build a histogram with the number of buckets equal to the number of
    // types in the value stream (numeric, date, timestamp, string, and objectId).
    // ah = createArrayEstimator(values, 5);

    // Ensure we fail to build a histrogram when we have more types than buckets.
    ASSERT_THROWS_CODE(createArrayEstimator(values, 4), DBException, 6660504);
}

}  // namespace mongo::stats
