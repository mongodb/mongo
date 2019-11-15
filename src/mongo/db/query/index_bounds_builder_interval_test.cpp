/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/query/index_bounds_builder_test.h"

namespace mongo {
namespace {

/**
 * run isSingleInterval and return the result to calling test.
 */
bool testSingleInterval(IndexBounds bounds) {
    BSONObj startKey;
    bool startKeyIn;
    BSONObj endKey;
    bool endKeyIn;
    return IndexBoundsBuilder::isSingleInterval(bounds, &startKey, &startKeyIn, &endKey, &endKeyIn);
}

TEST(IndexBoundsBuilderTest, SingleFieldEqualityInterval) {
    // Equality on a single field is a single interval.
    OrderedIntervalList oil("a");
    IndexBounds bounds;
    oil.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    bounds.fields.push_back(oil);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, SingleIntervalSingleFieldInterval) {
    // Single interval on a single field is a single interval.
    OrderedIntervalList oil("a");
    IndexBounds bounds;
    oil.intervals.push_back(Interval(fromjson("{ '':5, '':Infinity }"), true, true));
    bounds.fields.push_back(oil);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, MultipleIntervalsSingleFieldInterval) {
    // Multiple intervals on a single field is not a single interval.
    OrderedIntervalList oil("a");
    IndexBounds bounds;
    oil.intervals.push_back(Interval(fromjson("{ '':4, '':5 }"), true, true));
    oil.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    bounds.fields.push_back(oil);
    ASSERT(!testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualityTwoFieldsInterval) {
    // Equality on two fields is a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(BSON("" << 6 << "" << 6), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualityFirstFieldSingleIntervalSecondFieldInterval) {
    // Equality on first field and single interval on second field
    // is a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':6, '':Infinity }"), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, SingleIntervalFirstAndSecondFieldsInterval) {
    // Single interval on first field and single interval on second field is
    // not a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(fromjson("{ '':-Infinity, '':5 }"), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':6, '':Infinity }"), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    ASSERT(!testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, MultipleIntervalsTwoFieldsInterval) {
    // Multiple intervals on two fields is not a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 4 << "" << 4), true, true));
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(BSON("" << 7 << "" << 7), true, true));
    oil_b.intervals.push_back(Interval(BSON("" << 8 << "" << 8), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    ASSERT(!testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, MissingSecondFieldInterval) {
    // when second field is not specified, still a compound single interval
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(IndexBoundsBuilder::allValues());
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualityTwoFieldsIntervalThirdInterval) {
    // Equality on first two fields and single interval on third is a
    // compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    OrderedIntervalList oil_c("c");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(BSON("" << 6 << "" << 6), true, true));
    oil_c.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    bounds.fields.push_back(oil_c);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualitySingleIntervalMissingInterval) {
    // Equality, then Single Interval, then missing is a compound single interval
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    OrderedIntervalList oil_c("c");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    oil_c.intervals.push_back(IndexBoundsBuilder::allValues());
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    bounds.fields.push_back(oil_c);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualitySingleMissingMissingInterval) {
    // Equality, then single interval, then missing, then missing,
    // is a compound single interval
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    OrderedIntervalList oil_c("c");
    OrderedIntervalList oil_d("d");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    oil_c.intervals.push_back(IndexBoundsBuilder::allValues());
    oil_d.intervals.push_back(IndexBoundsBuilder::allValues());
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    bounds.fields.push_back(oil_c);
    bounds.fields.push_back(oil_d);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualitySingleMissingMissingMixedInterval) {
    // Equality, then single interval, then missing, then missing, with mixed order
    // fields is a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    OrderedIntervalList oil_c("c");
    OrderedIntervalList oil_d("d");
    IndexBounds bounds;
    Interval allValues = IndexBoundsBuilder::allValues();
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    oil_c.intervals.push_back(allValues);
    IndexBoundsBuilder::reverseInterval(&allValues);
    oil_d.intervals.push_back(allValues);
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    bounds.fields.push_back(oil_c);
    bounds.fields.push_back(oil_d);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderTest, EqualitySingleMissingSingleInterval) {
    // Equality, then single interval, then missing, then single interval is not
    // a compound single interval.
    OrderedIntervalList oil_a("a");
    OrderedIntervalList oil_b("b");
    OrderedIntervalList oil_c("c");
    OrderedIntervalList oil_d("d");
    IndexBounds bounds;
    oil_a.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil_b.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    oil_c.intervals.push_back(IndexBoundsBuilder::allValues());
    oil_d.intervals.push_back(Interval(fromjson("{ '':1, '':Infinity }"), true, true));
    bounds.fields.push_back(oil_a);
    bounds.fields.push_back(oil_b);
    bounds.fields.push_back(oil_c);
    bounds.fields.push_back(oil_d);
    ASSERT(!testSingleInterval(bounds));
}

}  // namespace
}  // namespace mongo
