// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/unittest/unittest.h"

#include <vector>

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

TEST(IndexBoundsBuilderIntervalTest, SingleFieldEqualityInterval) {
    // Equality on a single field is a single interval.
    OrderedIntervalList oil("a");
    IndexBounds bounds;
    oil.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    bounds.fields.push_back(oil);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderIntervalTest, SingleIntervalSingleFieldInterval) {
    // Single interval on a single field is a single interval.
    OrderedIntervalList oil("a");
    IndexBounds bounds;
    oil.intervals.push_back(Interval(fromjson("{ '':5, '':Infinity }"), true, true));
    bounds.fields.push_back(oil);
    ASSERT(testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderIntervalTest, MultipleIntervalsSingleFieldInterval) {
    // Multiple intervals on a single field is not a single interval.
    OrderedIntervalList oil("a");
    IndexBounds bounds;
    oil.intervals.push_back(Interval(fromjson("{ '':4, '':5 }"), true, true));
    oil.intervals.push_back(Interval(fromjson("{ '':7, '':Infinity }"), true, true));
    bounds.fields.push_back(oil);
    ASSERT(!testSingleInterval(bounds));
}

TEST(IndexBoundsBuilderIntervalTest, EqualityTwoFieldsInterval) {
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

TEST(IndexBoundsBuilderIntervalTest, EqualityFirstFieldSingleIntervalSecondFieldInterval) {
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

TEST(IndexBoundsBuilderIntervalTest, SingleIntervalFirstAndSecondFieldsInterval) {
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

TEST(IndexBoundsBuilderIntervalTest, MultipleIntervalsTwoFieldsInterval) {
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

TEST(IndexBoundsBuilderIntervalTest, MissingSecondFieldInterval) {
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

TEST(IndexBoundsBuilderIntervalTest, EqualityTwoFieldsIntervalThirdInterval) {
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

TEST(IndexBoundsBuilderIntervalTest, EqualitySingleIntervalMissingInterval) {
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

TEST(IndexBoundsBuilderIntervalTest, EqualitySingleMissingMissingInterval) {
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

TEST(IndexBoundsBuilderIntervalTest, EqualitySingleMissingMissingMixedInterval) {
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

TEST(IndexBoundsBuilderIntervalTest, EqualitySingleMissingSingleInterval) {
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
