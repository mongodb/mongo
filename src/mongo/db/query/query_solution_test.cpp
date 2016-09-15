/**
 *    Copyright (C) 2016 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/index_bounds_builder.h"
#include "mongo/db/query/index_entry.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/stdx/memory.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Min: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Max: {a: 1, b: 1, c: 1, d: 1, e: 1}
TEST(QuerySolutionTest, SimpleRangeAllEqual) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};
    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    node.bounds.endKey = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    node.computeProperties();

    // Expected sort orders
    ASSERT_EQUALS(node.getSort().size(), 9U);
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1)));
    ASSERT(node.getSort().count(BSON("b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("e" << 1)));
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Min: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Max: {a: 2, b: 2, c: 2, d: 2, e: 2}
TEST(QuerySolutionTest, SimpleRangeNoneEqual) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};
    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    node.bounds.endKey = BSON("a" << 2 << "b" << 2 << "c" << 2 << "d" << 2 << "e" << 2);
    node.computeProperties();

    // Expected sort orders
    ASSERT_EQUALS(node.getSort().size(), 5U);
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1)));
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Min: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Max: {a: 1, b: 1, c: 2, d: 2, e: 2}
TEST(QuerySolutionTest, SimpleRangeSomeEqual) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};
    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    node.bounds.endKey = BSON("a" << 1 << "b" << 1 << "c" << 2 << "d" << 2 << "e" << 2);
    node.computeProperties();

    // Expected sort orders
    ASSERT_EQUALS(node.getSort().size(), 9U);
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1)));
    ASSERT(node.getSort().count(BSON("b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("c" << 1 << "d" << 1)));
    ASSERT(node.getSort().count(BSON("c" << 1)));
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Intervals: a: [1,1], b: [1,1], c: [1,1], d: [1,1], e: [1,1]
TEST(QuerySolutionTest, IntervalListAllPoints) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(c);

    OrderedIntervalList d{};
    d.name = "d";
    d.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(d);

    OrderedIntervalList e{};
    e.name = "e";
    e.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(e);

    node.computeProperties();

    // Expected sort orders
    ASSERT_EQUALS(node.getSort().size(), 9U);
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1)));
    ASSERT(node.getSort().count(BSON("b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("e" << 1)));
}


// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Intervals: a: [1,2], b: [1,2], c: [1,2], d: [1,2], e: [1,2]
TEST(QuerySolutionTest, IntervalListNoPoints) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    OrderedIntervalList d{};
    d.name = "d";
    d.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(d);

    OrderedIntervalList e{};
    e.name = "e";
    e.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(e);

    node.computeProperties();

    // Expected sort orders
    ASSERT_EQUALS(node.getSort().size(), 5U);
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1)));
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Intervals: a: [1,1], b: [1,1], c: [1,2], d: [1,2], e: [1,2]
TEST(QuerySolutionTest, IntervalListSomePoints) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    OrderedIntervalList d{};
    d.name = "d";
    d.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(d);

    OrderedIntervalList e{};
    e.name = "e";
    e.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(e);

    node.computeProperties();

    // Expected sort orders
    ASSERT_EQUALS(node.getSort().size(), 9U);
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1 << "c" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1)));
    ASSERT(node.getSort().count(BSON("b" << 1 << "c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("c" << 1 << "d" << 1 << "e" << 1)));
    ASSERT(node.getSort().count(BSON("c" << 1 << "d" << 1)));
    ASSERT(node.getSort().count(BSON("c" << 1)));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesFieldsContainingStrings) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 1), BoundInclusion::kIncludeBothStartAndEndKeys));
    bounds.fields.push_back(oilA);

    OrderedIntervalList oilB{};
    oilB.name = "b";
    oilB.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << false << "" << true), BoundInclusion::kIncludeBothStartAndEndKeys));
    bounds.fields.push_back(oilB);

    OrderedIntervalList oilC{};
    oilC.name = "c";
    oilC.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON(""
                                                   << "a"
                                                   << ""
                                                   << "b"),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    bounds.fields.push_back(oilC);

    OrderedIntervalList oilD{};
    oilD.name = "d";
    oilD.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << BSON("foo" << 1) << "" << BSON("foo" << 2)),
        BoundInclusion::kIncludeBothStartAndEndKeys));
    bounds.fields.push_back(oilD);

    OrderedIntervalList oilE{};
    oilE.name = "e";
    oilE.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << BSON_ARRAY(1 << 2 << 3) << "" << BSON_ARRAY(2 << 3 << 4)),
        BoundInclusion::kIncludeBothStartAndEndKeys));
    bounds.fields.push_back(oilE);

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 3U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_FALSE(fields.count("b"));
    ASSERT_TRUE(fields.count("c"));
    ASSERT_TRUE(fields.count("d"));
    ASSERT_TRUE(fields.count("e"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsFromNonPointBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = BSON("a" << 1 << "b" << 2 << "c" << 3 << "d" << 4 << "e" << 5);
    bounds.endKey = BSON("a" << 1 << "b" << 2 << "c" << 3 << "d" << 5 << "e" << 5);
    bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_FALSE(fields.count("b"));
    ASSERT_FALSE(fields.count("c"));
    ASSERT_FALSE(fields.count("d"));
    ASSERT_TRUE(fields.count("e"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsFromStringTypePointBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1, 'b': 'a', 'c': 3, 'd': 4, 'e': 5}");
    bounds.endKey = bounds.startKey;
    bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 4U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_TRUE(fields.count("b"));
    ASSERT_TRUE(fields.count("c"));
    ASSERT_TRUE(fields.count("d"));
    ASSERT_TRUE(fields.count("e"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsFromArrayTypeBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1, 'b': [1,2], 'c': 3, 'd': 4, 'e': 5}");
    bounds.endKey = bounds.startKey;
    bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 4U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_TRUE(fields.count("b"));
    ASSERT_TRUE(fields.count("c"));
    ASSERT_TRUE(fields.count("d"));
    ASSERT_TRUE(fields.count("e"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsFromObjectTypeBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1, 'b': {'foo': 2}, 'c': 3, 'd': 4, 'e': 5}");
    bounds.endKey = bounds.startKey;
    bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 4U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_TRUE(fields.count("b"));
    ASSERT_TRUE(fields.count("c"));
    ASSERT_TRUE(fields.count("d"));
    ASSERT_TRUE(fields.count("e"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsWithExclusiveBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1, 'b': 1}");
    bounds.endKey = fromjson("{'a': 2, 'b': 2}");
    bounds.boundInclusion = BoundInclusion::kExcludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_FALSE(fields.count("a"));
    ASSERT_TRUE(fields.count("b"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsWithExclusiveBoundsOnBoundary) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1 << "b" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1, 'b': 1}");
    bounds.endKey = fromjson("{'a': '', 'b': 1}");
    bounds.boundInclusion = BoundInclusion::kExcludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 2U);
    ASSERT_TRUE(fields.count("a"));
    ASSERT_TRUE(fields.count("b"));
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesNoStringsWithEmptyExclusiveBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1}");
    bounds.endKey = fromjson("{'a': ''}");
    bounds.boundInclusion = BoundInclusion::kExcludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 0U);
}

TEST(QuerySolutionTest, GetFieldsWithStringBoundsIdentifiesStringsWithInclusiveBounds) {
    IndexBounds bounds;
    BSONObj keyPattern = BSON("a" << 1);
    bounds.isSimpleRange = true;
    bounds.startKey = fromjson("{'a': 1}");
    bounds.endKey = fromjson("{'a': ''}");
    bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    auto fields = IndexScanNode::getFieldsWithStringBounds(bounds, keyPattern);
    ASSERT_EQUALS(fields.size(), 1U);
    ASSERT_TRUE(fields.count("a"));
}

TEST(QuerySolutionTest, IndexScanNodeRemovesNonMatchingCollatedFieldsFromSortsOnSimpleBounds) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1);
    node.bounds.endKey = BSON("a" << 2 << "b" << 1);
    node.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    node.computeProperties();

    BSONObjSet sorts = node.getSort();
    ASSERT_EQUALS(sorts.size(), 1U);
    ASSERT_TRUE(sorts.count(BSON("a" << 1)));
}

TEST(QuerySolutionTest, IndexScanNodeGetFieldsWithStringBoundsCorrectlyHandlesEndKeyInclusive) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 1);
    node.bounds.endKey = BSON("a" << 1 << "b"
                                  << "");
    node.bounds.boundInclusion = BoundInclusion::kIncludeStartKeyOnly;

    node.computeProperties();

    BSONObjSet sorts = node.getSort();
    ASSERT_EQUALS(sorts.size(), 3U);
    ASSERT_TRUE(sorts.count(BSON("a" << 1)));
    ASSERT_TRUE(sorts.count(BSON("a" << 1 << "b" << 1)));
    ASSERT_TRUE(sorts.count(BSON("b" << 1)));

    node.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;

    node.computeProperties();

    sorts = node.getSort();
    ASSERT_EQUALS(sorts.size(), 1U);
    ASSERT_TRUE(sorts.count(BSON("a" << 1)));
}

// Index: {a: 1}
// Bounds: [MINKEY, MAXKEY]
TEST(QuerySolutionTest, IndexScanNodeRemovesCollatedFieldsFromSortsIfCollationDifferent) {
    IndexScanNode node{IndexEntry(BSON("a" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    node.computeProperties();

    BSONObjSet sorts = node.getSort();
    ASSERT_EQUALS(sorts.size(), 0U);
}

TEST(QuerySolutionTest, IndexScanNodeDoesNotRemoveCollatedFieldsFromSortsIfCollationMatches) {
    IndexScanNode node{IndexEntry(BSON("a" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    node.computeProperties();

    BSONObjSet sorts = node.getSort();
    ASSERT_EQUALS(sorts.size(), 1U);
    ASSERT_TRUE(sorts.count(BSON("a" << 1)));
}

// Index: {a: 1, b: 1, c: 1, d: 1, e: 1}
// Intervals: a: [1,1], b: [1,1], c: [MinKey, MaxKey], d: [1,2], e: [1,2]
TEST(QuerySolutionTest, CompoundIndexWithNonMatchingCollationFiltersAllSortsWithCollatedField) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    node.index = IndexEntry(BSON("a" << 1 << "b" << 1 << "c" << 1 << "d" << 1 << "e" << 1));

    OrderedIntervalList a{};
    a.name = "a";
    a.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(a);

    OrderedIntervalList b{};
    b.name = "b";
    b.intervals.push_back(IndexBoundsBuilder::makePointInterval(BSON("" << 1)));
    node.bounds.fields.push_back(b);

    OrderedIntervalList c{};
    c.name = "c";
    c.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << MINKEY << "" << MAXKEY), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(c);

    OrderedIntervalList d{};
    d.name = "d";
    d.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(d);

    OrderedIntervalList e{};
    e.name = "e";
    e.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(e);

    node.computeProperties();

    // Expected sort orders
    ASSERT_EQUALS(node.getSort().size(), 2U);
    ASSERT(node.getSort().count(BSON("a" << 1 << "b" << 1)));
    ASSERT(node.getSort().count(BSON("a" << 1)));
}

// Index: {a : 1}
// Bounds: [{}, {}]
TEST(QuerySolutionTest, IndexScanNodeWithNonMatchingCollationFiltersObjectField) {
    IndexScanNode node{IndexEntry(BSON("a" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << BSON("foo" << 1) << "" << BSON("foo" << 2)),
        BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    node.computeProperties();

    BSONObjSet sorts = node.getSort();
    ASSERT_EQUALS(sorts.size(), 0U);
}

// Index: {a : 1}
// Bounds: [[], []]
TEST(QuerySolutionTest, IndexScanNodeWithNonMatchingCollationFiltersArrayField) {
    IndexScanNode node{IndexEntry(BSON("a" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON("" << BSON_ARRAY(1) << "" << BSON_ARRAY(2)),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    node.computeProperties();

    BSONObjSet sorts = node.getSort();
    ASSERT_EQUALS(sorts.size(), 0U);
}

TEST(QuerySolutionTest, WithNonMatchingCollatorAndNoEqualityPrefixSortsAreNotDuplicated) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1))};
    CollatorInterfaceMock queryCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.queryCollator = &queryCollator;

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    OrderedIntervalList oilB{};
    oilB.name = "b";
    oilB.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON(""
                                                   << "a"
                                                   << ""
                                                   << "b"),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilB);

    node.computeProperties();

    // Expected sort orders
    ASSERT_EQUALS(node.getSort().size(), 1U);
    ASSERT(node.getSort().count(BSON("a" << 1)));
}

TEST(QuerySolutionTest, IndexScanNodeHasFieldIncludesStringFieldWhenNoCollator) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1))};

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON(""
                                                   << "str"
                                                   << ""
                                                   << "str"),
                                              BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    OrderedIntervalList oilB{};
    oilB.name = "b";
    oilB.intervals.push_back(IndexBoundsBuilder::allValues());
    node.bounds.fields.push_back(oilB);

    ASSERT_TRUE(node.hasField("a"));
    ASSERT_TRUE(node.hasField("b"));
}

TEST(QuerySolutionTest, IndexScanNodeHasFieldIncludesSimpleBoundsStringFieldWhenNoCollator) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1))};

    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 2);
    node.bounds.endKey = BSON("a" << 2 << "b" << 1);
    node.bounds.boundInclusion = BoundInclusion::kIncludeStartKeyOnly;

    ASSERT_TRUE(node.hasField("a"));
    ASSERT_TRUE(node.hasField("b"));
}

TEST(QuerySolutionTest, IndexScanNodeHasFieldExcludesStringFieldWhenIndexHasCollator) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1))};
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.index.collator = &indexCollator;

    OrderedIntervalList oilA{};
    oilA.name = "a";
    oilA.intervals.push_back(IndexBoundsBuilder::makeRangeInterval(
        BSON("" << 1 << "" << 2), BoundInclusion::kIncludeBothStartAndEndKeys));
    node.bounds.fields.push_back(oilA);

    OrderedIntervalList oilB{};
    oilB.name = "b";
    oilB.intervals.push_back(
        IndexBoundsBuilder::makeRangeInterval(BSON(""
                                                   << "bar"
                                                   << ""
                                                   << "foo"),
                                              BoundInclusion::kIncludeStartKeyOnly));
    node.bounds.fields.push_back(oilB);

    ASSERT_TRUE(node.hasField("a"));
    ASSERT_FALSE(node.hasField("b"));
}

TEST(QuerySolutionTest, IndexScanNodeHasFieldExcludesSimpleBoundsStringFieldWhenIndexHasCollator) {
    IndexScanNode node{IndexEntry(BSON("a" << 1 << "b" << 1))};
    CollatorInterfaceMock indexCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.index.collator = &indexCollator;

    node.bounds.isSimpleRange = true;
    node.bounds.startKey = BSON("a" << 1 << "b" << 2);
    node.bounds.endKey = BSON("a" << 2 << "b" << 1);
    node.bounds.boundInclusion = BoundInclusion::kIncludeStartKeyOnly;

    ASSERT_TRUE(node.hasField("a"));
    ASSERT_FALSE(node.hasField("b"));
}

std::unique_ptr<ParsedProjection> createParsedProjection(const BSONObj& query,
                                                         const BSONObj& projObj) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression queryMatchExpr =
        MatchExpressionParser::parse(query, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT(queryMatchExpr.isOK());
    ParsedProjection* out = nullptr;
    Status status = ParsedProjection::make(
        projObj, queryMatchExpr.getValue().get(), &out, ExtensionsCallbackDisallowExtensions());
    if (!status.isOK()) {
        FAIL(mongoutils::str::stream() << "failed to parse projection " << projObj << " (query: "
                                       << query
                                       << "): "
                                       << status.toString());
    }
    ASSERT(out);
    return std::unique_ptr<ParsedProjection>(out);
}

TEST(QuerySolutionTest, InclusionProjectionPreservesSort) {
    IndexEntry index(BSON("a" << 1));
    auto node = stdx::make_unique<IndexScanNode>(index);

    BSONObj projection = BSON("a" << 1);
    BSONObj match;

    auto parsedProjection = createParsedProjection(match, projection);

    ProjectionNode proj{*parsedProjection};
    proj.children.push_back(node.release());
    proj.computeProperties();

    ASSERT_EQ(proj.getSort().size(), 1U);
    ASSERT(proj.getSort().count(BSON("a" << 1)));
}

TEST(QuerySolutionTest, ExclusionProjectionDoesNotPreserveSort) {
    IndexEntry index(BSON("a" << 1));
    auto node = stdx::make_unique<IndexScanNode>(index);

    BSONObj projection = BSON("a" << 0);
    BSONObj match;

    auto parsedProjection = createParsedProjection(match, projection);

    ProjectionNode proj{*parsedProjection};
    proj.children.push_back(node.release());
    proj.computeProperties();

    ASSERT_EQ(proj.getSort().size(), 0U);
}

TEST(QuerySolutionTest, InclusionProjectionTruncatesSort) {
    auto node = stdx::make_unique<IndexScanNode>(IndexEntry(BSON("a" << 1 << "b" << 1)));

    BSONObj projection = BSON("a" << 1);
    BSONObj match;

    auto parsedProjection = createParsedProjection(match, projection);

    ProjectionNode proj{*parsedProjection};
    proj.children.push_back(node.release());
    proj.computeProperties();

    ASSERT_EQ(proj.getSort().size(), 1U);
    ASSERT(proj.getSort().count(BSON("a" << 1)));
}

TEST(QuerySolutionTest, ExclusionProjectionTruncatesSort) {
    auto node = stdx::make_unique<IndexScanNode>(IndexEntry(BSON("a" << 1 << "b" << 1)));

    BSONObj projection = BSON("b" << 0);
    BSONObj match;

    auto parsedProjection = createParsedProjection(match, projection);

    ProjectionNode proj{*parsedProjection};
    proj.children.push_back(node.release());
    proj.computeProperties();

    ASSERT_EQ(proj.getSort().size(), 1U);
    ASSERT(proj.getSort().count(BSON("a" << 1)));
}

}  // namespace
