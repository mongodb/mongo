// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/timeseries/bucket_catalog/measurement_map.h"

#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/timeseries/timeseries_gen.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/tracking/context.h"

#include <string>
#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo::timeseries::bucket_catalog {
class MeasurementMapTest : public unittest::Test {
public:
    MeasurementMapTest() : measurementMap(trackingContext) {}

protected:
    tracking::Context trackingContext;
    MeasurementMap measurementMap;
    static constexpr std::string_view _metaField = "meta";
    static constexpr std::string_view _timeField = "time";
    static constexpr std::string_view _metaValue = "a";
};

TEST_F(MeasurementMapTest, IterationBasicWithoutMeta) {
    // Insert measurement 1.
    const BSONObj m1 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "1"));                                 // 'a'
    measurementMap.insertOne(m1, /*metaField=*/boost::none);

    // Insert measurement 2.
    const BSONObj m2 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:31.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "5"));                                 // 'a'
    measurementMap.insertOne(m2, /*metaField=*/boost::none);

    // Two distinct fields: 'a' and '_timeField'.
    invariant(measurementMap.numFields() == 2);
}

TEST_F(MeasurementMapTest, IterationBasicWithMeta) {
    // Insert measurement 1.
    const BSONObj m1 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "1")                                   // 'a'
                        << _metaField << _metaValue);                                // '_metaField'
    measurementMap.insertOne(m1, _metaField);

    // Insert measurement 2.
    const BSONObj m2 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:31.000Z"))  // '_timeField'
                        << _metaField << _metaValue                                  // '_metaField'
                        << "a" << BSON("0" << "5"));                                 // 'a'
    measurementMap.insertOne(m2, _metaField);

    // Two distinct fields: 'a' and '_timeField'. We shouldn't include '_metaField'.
    invariant(measurementMap.numFields() == 2);
}

TEST_F(MeasurementMapTest, IterationBasicWithMixed) {
    // Insert measurement 1.
    const BSONObj m1 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << _metaField << _metaValue                                  // '_metaField'
                        << "a" << BSON("0" << "1"));                                 // 'a'
    measurementMap.insertOne(m1, _metaField);

    // Insert measurement 2. Doesn't have a '_metaField' field.
    const BSONObj m2 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:31.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "5"));                                 // 'a'
    measurementMap.insertOne(m2, _metaField);

    // Two distinct fields: 'a' and '_timeField'. We shouldn't include '_metaField'.
    invariant(measurementMap.numFields() == 2);
}

TEST_F(MeasurementMapTest, FillSkipsDifferentField) {
    const BSONObj m1 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "1")                                   // 'a'
                        << "b" << BSON("0" << "1"));                                 // 'b'

    const BSONObj m2 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:31.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "1")                                   // 'a'
                        << "b" << BSON("0" << "1"));                                 // 'b'

    const BSONObj mWithNewField =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:32.000Z"))  // '_timeField'
                        << "c" << BSON("4" << "5"));                                 // 'c'

    measurementMap.insertOne(m1, /*metaField=*/boost::none);
    measurementMap.insertOne(m2, /*metaField=*/boost::none);
    measurementMap.insertOne(mWithNewField, /*metaField=*/boost::none);
    invariant(measurementMap.numFields() == 4);
}

TEST_F(MeasurementMapTest, FillSkipsDifferentFieldWithMeta) {
    const BSONObj m1 =
        BSON(_metaField << _metaValue  // '_metaField'
                        << _timeField
                        << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "1")                                   // 'a'
                        << "b" << BSON("0" << "1"));                                 // 'b'

    const BSONObj m2 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:31.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "1")                                   // 'a'
                        << "b" << BSON("0" << "1")                                   // 'b'
                        << _metaField << _metaValue);                                // '_metaField'

    const BSONObj mWithNewField =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:32.000Z"))  // '_timeField'
                        << "c" << BSON("4" << "5"));                                 // 'c'

    measurementMap.insertOne(m1, _metaField);
    measurementMap.insertOne(m2, _metaField);
    measurementMap.insertOne(mWithNewField, _metaField);
    invariant(measurementMap.numFields() == 4);
}

TEST_F(MeasurementMapTest, FillSkipsAddField) {
    const BSONObj m1 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "1")                                   // 'a'
                        << "b" << BSON("0" << "1"));                                 // 'b'

    const BSONObj mWithNewField =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:35.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "4")                                   // 'a'
                        << "b" << BSON("0" << "1")                                   // 'b'
                        << "c" << BSON("0" << "1"));                                 // 'c'

    measurementMap.insertOne(m1, /*metaField=*/boost::none);
    measurementMap.insertOne(mWithNewField, /*metaField=*/boost::none);
    invariant(measurementMap.numFields() == 4);
}

TEST_F(MeasurementMapTest, FillSkipsAddFieldWithMeta) {
    const BSONObj m1 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << _metaField << _metaValue                                  // '_metaField'
                        << "a" << BSON("0" << "1")                                   // 'a'
                        << "b" << BSON("0" << "1"));                                 // 'b'

    const BSONObj mWithNewField =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:35.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "4")                                   // 'a'
                        << "b" << BSON("0" << "1")                                   // 'b'
                        << "c" << BSON("0" << "1")                                   // 'c'
                        << _metaField << _metaValue);                                // '_metaField'

    measurementMap.insertOne(m1, _metaField);
    measurementMap.insertOne(mWithNewField, _metaField);
    invariant(measurementMap.numFields() == 4);
}

TEST_F(MeasurementMapTest, FillSkipsRemoveField) {
    const BSONObj m1 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "1")                                   // 'a'
                        << "b" << BSON("0" << "1"));                                 // 'b'

    const BSONObj mWithoutField =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << "a" << BSON("0" << "4"));                                 // 'a'

    measurementMap.insertOne(m1, /*metaField=*/boost::none);
    measurementMap.insertOne(mWithoutField, /*metaField=*/boost::none);
    invariant(measurementMap.numFields() == 3);
}

TEST_F(MeasurementMapTest, FillSkipsRemoveFieldWithMeta) {
    const BSONObj m1 =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << _metaField << _metaValue                                  // '_metaField'
                        << "a" << BSON("0" << "1")                                   // 'a'
                        << "b" << BSON("0" << "1"));                                 // 'b'

    const BSONObj mWithoutField =
        BSON(_timeField << BSON("0" << BSON("$date" << "2022-06-06T15:34:30.000Z"))  // '_timeField'
                        << _metaField << _metaValue                                  // '_metaField'
                        << "a" << BSON("0" << "4"));                                 // 'a'

    measurementMap.insertOne(m1, _metaField);
    measurementMap.insertOne(mWithoutField, _metaField);
    invariant(measurementMap.numFields() == 3);
}

TEST_F(MeasurementMapTest, InitBuilders) {
    BSONObj bucketDataDoc;
    BSONObjBuilder bucket;
    BSONObjBuilder dataBuilder = bucket.subobjStart("data");
    BSONColumnBuilder timeColumn;

    BSONObjBuilder bob1;
    bob1.appendTimestamp("$date", 0);
    BSONObjBuilder bob2;
    bob2.appendTimestamp("$date", 1);
    BSONObjBuilder bob3;
    bob3.appendTimestamp("$date", 2);
    timeColumn.append(bob1.done().firstElement());
    timeColumn.append(bob2.done().firstElement());
    timeColumn.append(bob3.done().firstElement());
    BSONBinData timeBinary = timeColumn.finalize();

    BSONColumnBuilder f1Column;
    BSONObj f1m1 = BSON("0" << "1");
    BSONObj f1m2 = BSON("1" << "2");
    BSONObj f1m3 = BSON("2" << "3");
    f1Column.append(f1m1.firstElement());
    f1Column.append(f1m2.firstElement());
    f1Column.append(f1m3.firstElement());
    BSONBinData f1Binary = f1Column.finalize();

    BSONColumnBuilder f2Column;
    BSONObj f2m1 = BSON("0" << "1");
    BSONObj f2m2 = BSON("1" << "1");
    BSONObj f2m3 = BSON("2" << "1");
    f2Column.append(f2m1.firstElement());
    f2Column.append(f2m2.firstElement());
    f2Column.append(f2m3.firstElement());
    BSONBinData f2Binary = f2Column.finalize();

    dataBuilder.append(_timeField, timeBinary);
    dataBuilder.append("a", f1Binary);
    dataBuilder.append("b", f2Binary);

    measurementMap.initBuilders(dataBuilder.done(), 3);
    invariant(measurementMap.numFields() == 3);
}

TEST_F(MeasurementMapTest, DuplicateFieldNameThrows) {
    BSONObjBuilder builder;
    builder.append("a", 1);
    builder.append("a", 2);

    ASSERT_THROWS_CODE(
        measurementMap.insertOne(builder.obj(), /*metaField=*/boost::none), DBException, 12602102);
}

TEST_F(MeasurementMapTest, DuplicateFieldNameInSubsequentThrows) {
    const BSONObj m = BSON("a" << 1);
    measurementMap.insertOne(m, /*metaField=*/boost::none);

    BSONObjBuilder builder;
    builder.append("a", 2);
    builder.append("a", 3);

    ASSERT_THROWS_CODE(
        measurementMap.insertOne(builder.obj(), /*metaField=*/boost::none), DBException, 12602102);
}

using MeasurementMapTestDeathTest = MeasurementMapTest;
DEATH_TEST_REGEX_F(MeasurementMapTestDeathTest, GetTimeForNonexistentField, "Invariant failure.*") {
    measurementMap.timeOfLastMeasurement(_timeField);
}

}  // namespace mongo::timeseries::bucket_catalog
