// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/timeseries_bucket_validation.h"

#include "mongo/bson/json.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

#include <string_view>

namespace mongo::timeseries {
namespace {
using namespace std::literals::string_view_literals;

class TimeseriesBucketValidationTest : public unittest::Test {
protected:
    BucketConsistencyViolation validate(const BSONObj& data, const BSONObj& max) {
        TimeseriesOptions options;
        options.setTimeField("time"sv);
        const auto control = BSON("count" << 3);
        const auto min = fromjson(R"({"time": {"$date": "1970-01-01T00:01:00Z"}, "temp": 1})");
        return validateBucketData(options,
                                  /*collator=*/nullptr,
                                  kTimeseriesControlUncompressedVersion,
                                  control["count"],
                                  min,
                                  max,
                                  data,
                                  /*criticalValidationOnly=*/false);
    }

    static BSONObj makeData(std::string_view tempIdx0,
                            std::string_view tempIdx1,
                            std::string_view tempIdx2) {
        return fromjson(str::stream() << R"({"time": {"0": {"$date": "1970-01-01T00:01:00Z"},
                                        "1": {"$date": "1970-01-01T00:01:01Z"},
                                        "2": {"$date": "1970-01-01T00:01:02Z"}},
                               "temp": {")"
                                      << tempIdx0 << R"(": 1, ")" << tempIdx1 << R"(": 2, ")"
                                      << tempIdx2 << R"(": 3}})");
    }

    const BSONObj max = fromjson(R"({"time": {"$date": "1970-01-01T00:01:02Z"}, "temp": 3})");
};

TEST_F(TimeseriesBucketValidationTest, AcceptsLargestLegalIndex) {
    ASSERT_EQ(validate(makeData("0", "1", "2"), max), BucketConsistencyViolation::kNone);
}

TEST_F(TimeseriesBucketValidationTest, RejectsIndexEqualToMeasurementCount) {
    ASSERT_EQ(validate(makeData("0", "1", "3"), max), BucketConsistencyViolation::kIndexOutOfRange);
}

TEST_F(TimeseriesBucketValidationTest, RejectsIndexGreaterThanMeasurementCount) {
    ASSERT_EQ(validate(makeData("0", "1", "10"), max),
              BucketConsistencyViolation::kIndexOutOfRange);
}

TEST_F(TimeseriesBucketValidationTest, RejectsNegativeIndex) {
    ASSERT_EQ(validate(makeData("-1", "1", "2"), max), BucketConsistencyViolation::kIndexBadValue);
}

}  // namespace
}  // namespace mongo::timeseries
