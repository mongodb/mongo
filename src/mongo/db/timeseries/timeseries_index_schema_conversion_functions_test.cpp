/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/db/timeseries/timeseries_index_schema_conversion_functions.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/timeseries/timeseries_constants.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

const std::string kTimeseriesTimeFieldName("tm");
const std::string kTimeseriesMetaFieldName("mm");
const std::string kSubField1Name(".subfield1");
const std::string kSubField2Name(".subfield2");
const std::string kControlMinTimeFieldName(timeseries::kControlMinFieldNamePrefix +
                                           kTimeseriesTimeFieldName);
const std::string kControlMaxTimeFieldName(timeseries::kControlMaxFieldNamePrefix +
                                           kTimeseriesTimeFieldName);
const std::string kTimeseriesSomeDataFieldName("somedatafield");
const std::string kBucketsSomeDataFieldName(timeseries::kBucketDataFieldName + "." +
                                            kTimeseriesSomeDataFieldName);

/**
 * Constructs a TimeseriesOptions object for testing.
 */
TimeseriesOptions makeTimeseriesOptions() {
    TimeseriesOptions options(kTimeseriesTimeFieldName);
    options.setMetaField(StringData(kTimeseriesMetaFieldName));

    return options;
}

/**
 * Uses 'timeseriesOptions' to convert 'timeseriesIndexSpec' to 'bucketsIndexSpec' and vice versa.
 * If 'testShouldSucceed' is false, pivots to testing that conversion attempts fail.
 */
void testBothWaysIndexSpecConversion(const TimeseriesOptions& timeseriesOptions,
                                     const BSONObj& timeseriesIndexSpec,
                                     const BSONObj& bucketsIndexSpec,
                                     bool testShouldSucceed = true) {
    // Test time-series => buckets schema conversion.

    auto swBucketsIndexSpecs = timeseries::createBucketsIndexSpecFromTimeseriesIndexSpec(
        timeseriesOptions, timeseriesIndexSpec);

    if (testShouldSucceed) {
        ASSERT_OK(swBucketsIndexSpecs);
        ASSERT_BSONOBJ_EQ(bucketsIndexSpec, swBucketsIndexSpecs.getValue());
    } else {
        ASSERT_NOT_OK(swBucketsIndexSpecs);
    }

    // Test buckets => time-series schema conversion.

    auto timeseriesIndexSpecResult = timeseries::createTimeseriesIndexFromBucketsIndex(
        timeseriesOptions, BSON(timeseries::kKeyFieldName << bucketsIndexSpec));

    if (testShouldSucceed) {
        ASSERT(timeseriesIndexSpecResult);
        ASSERT_BSONOBJ_EQ(timeseriesIndexSpec,
                          timeseriesIndexSpecResult->getObjectField(timeseries::kKeyFieldName));
    } else {
        // A buckets collection index spec that does not conform to the supported time-series index
        // spec schema should be converted to an empty time-series index spec result.
        ASSERT(!timeseriesIndexSpecResult);
    }
}

TEST(TimeseriesIndexSchemaConversionTest, OriginalSpecFieldName) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();

    BSONObj bucketsIndexSpec =
        BSON(timeseries::kKeyFieldName << BSON("control.min.a" << 1 << "control.max.a" << 1)
                                       << timeseries::kOriginalSpecFieldName << BSON("abc" << 123));

    {
        // The "originalSpec" field is used when the time-series metric indexes feature flag is
        // enabled.
        RAIIServerParameterControllerForTest controller("featureFlagTimeseriesMetricIndexes", true);
        auto timeseriesIndexSpecResult =
            timeseries::createTimeseriesIndexFromBucketsIndex(timeseriesOptions, bucketsIndexSpec);
        ASSERT(timeseriesIndexSpecResult);
        ASSERT_BSONOBJ_EQ(*timeseriesIndexSpecResult, BSON("abc" << 123));
    }

    {
        // The "originalSpec" field is not used when the time-series metric indexes feature flag is
        // disabled.
        RAIIServerParameterControllerForTest controller("featureFlagTimeseriesMetricIndexes",
                                                        false);
        auto timeseriesIndexSpecResult =
            timeseries::createTimeseriesIndexFromBucketsIndex(timeseriesOptions, bucketsIndexSpec);
        ASSERT(!timeseriesIndexSpecResult);
    }
}

// {} <=> {}
TEST(TimeseriesIndexSchemaConversionTest, EmptyTimeseriesIndexSpecDoesNothing) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj emptyIndexSpec = {};

    testBothWaysIndexSpecConversion(timeseriesOptions, emptyIndexSpec, emptyIndexSpec);
}

// {tm: 1} <=> {control.min.tm: 1, control.max.tm: 1}
TEST(TimeseriesIndexSchemaConversionTest, AscendingTimeIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesTimeFieldName << 1);
    BSONObj bucketsIndexSpec = BSON(kControlMinTimeFieldName << 1 << kControlMaxTimeFieldName << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {tm: -1} <=> {control.max.tm: -1, control.min.tm: -1}
TEST(TimeseriesIndexSchemaConversionTest, DescendingTimeIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesTimeFieldName << -1);
    BSONObj bucketsIndexSpec =
        BSON(kControlMaxTimeFieldName << -1 << kControlMinTimeFieldName << -1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {tm.subfield1: 1} <=> {tm.subfield1: 1}
TEST(TimeseriesIndexSchemaConversionTest, TimeSubFieldIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesTimeFieldName + kSubField1Name << 1);
    BSONObj bucketsIndexSpec = BSON(kTimeseriesTimeFieldName + kSubField1Name << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {mm: 1} <=> {meta: 1}
TEST(TimeseriesIndexSchemaConversionTest, AscendingMetadataIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName << 1);
    BSONObj bucketsIndexSpec = BSON(timeseries::kBucketMetaFieldName << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {mm: -1} <=> {meta: -1}
TEST(TimeseriesIndexSchemaConversionTest, DescendingMetadataIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName << -1);
    BSONObj bucketsIndexSpec = BSON(timeseries::kBucketMetaFieldName << -1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {mm.subfield1: 1, mm.subfield2: 1} <=> {meta.subfield1: 1, mm.subfield2: 1}
TEST(TimeseriesIndexSchemaConversionTest, MetadataCompoundIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName + kSubField1Name
                                       << 1 << kTimeseriesMetaFieldName + kSubField2Name << 1);
    BSONObj bucketsIndexSpec = BSON(timeseries::kBucketMetaFieldName + kSubField1Name
                                    << 1 << timeseries::kBucketMetaFieldName + kSubField2Name << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {tm: 1, mm.subfield1: 1} <=> {control.min.tm: 1, control.max.tm: 1, meta.subfield1: 1}
TEST(TimeseriesIndexSchemaConversionTest, TimeAndMetadataCompoundIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesTimeFieldName << 1 << kTimeseriesMetaFieldName + kSubField1Name << 1);
    BSONObj bucketsIndexSpec =
        BSON(kControlMinTimeFieldName << 1 << kControlMaxTimeFieldName << 1
                                      << timeseries::kBucketMetaFieldName + kSubField1Name << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {mm.subfield1: 1, tm: 1} <=> {meta.subfield1: 1, control.min.tm: 1, control.max.tm: 1}
TEST(TimeseriesIndexSchemaConversionTest, MetadataAndTimeCompoundIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesMetaFieldName + kSubField1Name << 1 << kTimeseriesTimeFieldName << 1);
    BSONObj bucketsIndexSpec =
        BSON(timeseries::kBucketMetaFieldName + kSubField1Name << 1 << kControlMinTimeFieldName << 1
                                                               << kControlMaxTimeFieldName << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {somedatafield: 1} <=> {data.somedatafield: 1}
TEST(TimeseriesIndexSchemaConversionTest, DataIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesSomeDataFieldName << 1);
    BSONObj bucketsIndexSpec = BSON(kBucketsSomeDataFieldName << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {tm: 1, somedatafield: 1} <=> {control.min.tm: 1, control.max.tm: 1, data.somedatafield: 1}
TEST(TimeseriesIndexSchemaConversionTest, TimeAndDataCompoundIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesTimeFieldName << 1 << kTimeseriesSomeDataFieldName << 1);
    BSONObj bucketsIndexSpec = BSON(kControlMinTimeFieldName << 1 << kControlMaxTimeFieldName << 1
                                                             << kBucketsSomeDataFieldName << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {somedatafield: 1, tm: 1} <=> {data.somedatafield: 1, control.min.tm: 1, control.max.tm: 1}
TEST(TimeseriesIndexSchemaConversionTest, DataAndTimeCompoundIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesSomeDataFieldName << 1 << kTimeseriesTimeFieldName << 1);
    BSONObj bucketsIndexSpec = BSON(kBucketsSomeDataFieldName << 1 << kControlMinTimeFieldName << 1
                                                              << kControlMaxTimeFieldName << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {mm: 1, somedatafield: 1} <=> {meta: 1, data.somedatafield: 1}
TEST(TimeseriesIndexSchemaConversionTest, MetadataAndDataCompoundIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesMetaFieldName << 1 << kTimeseriesSomeDataFieldName << 1);
    BSONObj bucketsIndexSpec =
        BSON(timeseries::kBucketMetaFieldName << 1 << kBucketsSomeDataFieldName << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {somedatafield: 1, mm: 1} <=> {data.somedatafield: 1, meta: 1}
TEST(TimeseriesIndexSchemaConversionTest, DataAndMetadataCompoundIndexSpecConversionFails) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesSomeDataFieldName << 1 << kTimeseriesMetaFieldName << 1);
    BSONObj bucketsIndexSpec =
        BSON(kBucketsSomeDataFieldName << 1 << timeseries::kBucketMetaFieldName << 1);

    testBothWaysIndexSpecConversion(
        timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec, false /* testShouldSucceed */);
}

// {mm.subfield1: 1, mm.subfield2: 1, mm.foo:1, mm.bar: 1, mm.baz: 1, tm: 1} <=>
// {meta.subfield1: 1, meta.subfield2: 1, meta.foo: 1, meta.bar: 1, meta.baz: 1, control.min.tm: 1,
// control.max.tm: 1}
TEST(TimeseriesIndexSchemaConversionTest, ManyFieldCompoundIndexSpecConversion) {
    const auto kMetaFieldName = timeseries::kBucketMetaFieldName;

    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec =
        BSON(kTimeseriesMetaFieldName + kSubField1Name
             << 1 << kTimeseriesMetaFieldName + kSubField2Name << 1
             << kTimeseriesMetaFieldName + ".foo" << 1 << kTimeseriesMetaFieldName + ".bar" << 1
             << kTimeseriesMetaFieldName + ".baz" << 1 << kTimeseriesTimeFieldName << 1);
    BSONObj bucketsIndexSpec =
        BSON(kMetaFieldName + kSubField1Name
             << 1 << kMetaFieldName + kSubField2Name << 1 << kMetaFieldName + ".foo" << 1
             << kMetaFieldName + ".bar" << 1 << kMetaFieldName + ".baz" << 1
             << kControlMinTimeFieldName << 1 << kControlMaxTimeFieldName << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {mm: "hashed"} <=> {meta: "hashed"}
TEST(TimeseriesIndexSchemaConversionTest, HashedMetadataIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName << "hashed");
    BSONObj bucketsIndexSpec = BSON(timeseries::kBucketMetaFieldName << "hashed");

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {"mm.$**": 1} <=> {"meta.$**": 1}
TEST(TimeseriesIndexSchemaConversionTest, WildcardMetadataIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName + ".$**" << 1);
    BSONObj bucketsIndexSpec = BSON(timeseries::kBucketMetaFieldName + ".$**" << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {"mm.subfield1.$**": 1} <=> {"meta.subfield1.$**": 1}
TEST(TimeseriesIndexSchemaConversionTest, WildcardMetadataSubfieldIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName + kSubField1Name + ".$**" << 1);
    BSONObj bucketsIndexSpec =
        BSON(timeseries::kBucketMetaFieldName + kSubField1Name + ".$**" << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {mm: "text"} <=> {meta: "text"}
TEST(TimeseriesIndexSchemaConversionTest, TextMetadataIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName << "text");
    BSONObj bucketsIndexSpec = BSON(timeseries::kBucketMetaFieldName << "text");

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {mm: "2d"} <=> {meta: "2d"}
TEST(TimeseriesIndexSchemaConversionTest, 2dTextMetadataIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName << "2d");
    BSONObj bucketsIndexSpec = BSON(timeseries::kBucketMetaFieldName << "2d");

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {mm: "2dsphere"} <=> {meta: "2dsphere"}
TEST(TimeseriesIndexSchemaConversionTest, 2dsphereMetadataIndexSpecConversion) {
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON(kTimeseriesMetaFieldName << "2dsphere");
    BSONObj bucketsIndexSpec = BSON(timeseries::kBucketMetaFieldName << "2dsphere");

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {a: 1} <=> {control.min.a: 1, control.max.a: 1}
TEST(TimeseriesIndexSchemaConversionTest, AscendingMeasurementIndexSpecConversion) {
    RAIIServerParameterControllerForTest controller("featureFlagTimeseriesMetricIndexes", true);
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON("a" << 1);
    BSONObj bucketsIndexSpec = BSON("control.min.a" << 1 << "control.max.a" << 1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {a: -1} <=> {control.max.a: -1, control.min.a: -1}
TEST(TimeseriesIndexSchemaConversionTest, DescendingMeasurementIndexSpecConversion) {
    RAIIServerParameterControllerForTest controller("featureFlagTimeseriesMetricIndexes", true);
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON("a" << -1);
    BSONObj bucketsIndexSpec = BSON("control.max.a" << -1 << "control.min.a" << -1);

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {a: 1, b: -1, c: 1, d: "2dsphere"} <=> {control.min.a: 1, control.max.a: 1,
//                                         control.max.b: -1, control.min.b: -1,
//                                         control.min.c: 1, control.max.c: 1,
//                                         data.d: "2dsphere_bucket"}
TEST(TimeseriesIndexSchemaConversionTest, MixedCompoundMeasurementIndexSpecConversion) {
    RAIIServerParameterControllerForTest controller("featureFlagTimeseriesMetricIndexes", true);
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON("a" << 1 << "b" << -1 << "c" << 1 << "d"
                                           << "2dsphere");
    BSONObj bucketsIndexSpec = BSON(
        "control.min.a" << 1 << "control.max.a" << 1 << "control.max.b" << -1 << "control.min.b"
                        << -1 << "control.min.c" << 1 << "control.max.c" << 1 << "data.d"
                        << "2dsphere_bucket");

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

// {a: "2sphere"} <=> {data.a: "2dsphere_bucket"}
TEST(TimeseriesIndexSchemaConversionTest, 2dsphereMeasurementIndexSpecConversion) {
    RAIIServerParameterControllerForTest controller("featureFlagTimeseriesMetricIndexes", true);
    TimeseriesOptions timeseriesOptions = makeTimeseriesOptions();
    BSONObj timeseriesIndexSpec = BSON("a"
                                       << "2dsphere");
    BSONObj bucketsIndexSpec = BSON("data.a"
                                    << "2dsphere_bucket");

    testBothWaysIndexSpecConversion(timeseriesOptions, timeseriesIndexSpec, bucketsIndexSpec);
}

}  // namespace
}  // namespace mongo
