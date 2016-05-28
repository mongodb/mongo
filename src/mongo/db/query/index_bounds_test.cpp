/**
 *    Copyright (C) 2013 10gen Inc.
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

/**
 * This file contains tests for mongo/db/query/index_bounds.cpp
 */

#include "mongo/platform/basic.h"

#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/text.h"

using namespace mongo;

namespace {

//
// Validation
//

TEST(IndexBoundsTest, ValidBasic) {
    OrderedIntervalList list("foo");
    list.intervals.push_back(Interval(BSON("" << 7 << "" << 20), true, true));
    IndexBounds bounds;
    bounds.fields.push_back(list);

    // Go forwards with data indexed forwards.
    ASSERT(bounds.isValidFor(BSON("foo" << 1), 1));
    // Go backwards with data indexed backwards.
    ASSERT(bounds.isValidFor(BSON("foo" << -1), -1));
    // Bounds are not oriented along the direction of traversal.
    ASSERT_FALSE(bounds.isValidFor(BSON("foo" << 1), -1));

    // Bounds must match the index exactly.
    ASSERT_FALSE(bounds.isValidFor(BSON("foo" << 1 << "bar" << 1), 1));
    ASSERT_FALSE(bounds.isValidFor(BSON("bar" << 1), 1));
}

TEST(IndexBoundsTest, ValidTwoFields) {
    OrderedIntervalList list("foo");
    list.intervals.push_back(Interval(BSON("" << 7 << "" << 20), true, true));
    IndexBounds bounds;
    bounds.fields.push_back(list);

    // Let's add another field
    OrderedIntervalList otherList("bar");
    otherList.intervals.push_back(Interval(BSON("" << 0 << "" << 3), true, true));
    bounds.fields.push_back(otherList);

    // These are OK.
    ASSERT(bounds.isValidFor(BSON("foo" << 1 << "bar" << 1), 1));
    ASSERT(bounds.isValidFor(BSON("foo" << -1 << "bar" << -1), -1));

    // Direction(s) don't match.
    ASSERT_FALSE(bounds.isValidFor(BSON("foo" << -1 << "bar" << 1), -1));
    ASSERT_FALSE(bounds.isValidFor(BSON("foo" << 1 << "bar" << -1), -1));

    // Index doesn't match.
    ASSERT_FALSE(bounds.isValidFor(BSON("foo" << 1), 1));
    ASSERT_FALSE(bounds.isValidFor(BSON("bar" << 1 << "foo" << 1), 1));
}

TEST(IndexBoundsTest, ValidIntervalsInOrder) {
    OrderedIntervalList list("foo");
    // Whether navigated forward or backward, there's no valid ordering for these two intervals.
    list.intervals.push_back(Interval(BSON("" << 7 << "" << 20), true, true));
    list.intervals.push_back(Interval(BSON("" << 0 << "" << 5), true, true));
    IndexBounds bounds;
    bounds.fields.push_back(list);
    ASSERT_FALSE(bounds.isValidFor(BSON("foo" << 1), 1));
    ASSERT_FALSE(bounds.isValidFor(BSON("foo" << -1), 1));
    ASSERT_FALSE(bounds.isValidFor(BSON("foo" << 1), -1));
    ASSERT_FALSE(bounds.isValidFor(BSON("foo" << -1), -1));
}

TEST(IndexBoundsTest, ValidNoOverlappingIntervals) {
    OrderedIntervalList list("foo");
    // overlapping intervals not allowed.
    list.intervals.push_back(Interval(BSON("" << 7 << "" << 20), true, true));
    list.intervals.push_back(Interval(BSON("" << 19 << "" << 25), true, true));
    IndexBounds bounds;
    bounds.fields.push_back(list);
    ASSERT_FALSE(bounds.isValidFor(BSON("foo" << 1), 1));
}

TEST(IndexBoundsTest, ValidOverlapOnlyWhenBothOpen) {
    OrderedIntervalList list("foo");
    list.intervals.push_back(Interval(BSON("" << 7 << "" << 20), true, false));
    list.intervals.push_back(Interval(BSON("" << 20 << "" << 25), false, true));
    IndexBounds bounds;
    bounds.fields.push_back(list);
    ASSERT(bounds.isValidFor(BSON("foo" << 1), 1));
}

//
// Tests for OrderedIntervalList::complement()
//

/**
 * Get a BSONObj which represents the interval from
 * MinKey to 'end'.
 */
BSONObj minKeyIntObj(int end) {
    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendNumber("", end);
    return bob.obj();
}

/**
 * Get a BSONObj which represents the interval from
 * 'start' to MaxKey.
 */
BSONObj maxKeyIntObj(int start) {
    BSONObjBuilder bob;
    bob.appendNumber("", start);
    bob.appendMaxKey("");
    return bob.obj();
}

/**
 * Get a BSONObj which represents the interval
 * [MinKey, MaxKey].
 */
BSONObj allValues() {
    BSONObjBuilder bob;
    bob.appendMinKey("");
    bob.appendMaxKey("");
    return bob.obj();
}

/**
 * Test that if we complement the OIL twice,
 * we get back the original OIL.
 */
void testDoubleComplement(const OrderedIntervalList* oil) {
    OrderedIntervalList clone;
    for (size_t i = 0; i < oil->intervals.size(); ++i) {
        clone.intervals.push_back(oil->intervals[i]);
    }

    clone.complement();
    clone.complement();

    ASSERT_EQUALS(oil->intervals.size(), clone.intervals.size());
    for (size_t i = 0; i < oil->intervals.size(); ++i) {
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil->intervals[i].compare(clone.intervals[i]));
    }
}

// Complement of empty is [MinKey, MaxKey]
TEST(IndexBoundsTest, ComplementEmptyOil) {
    OrderedIntervalList oil;
    testDoubleComplement(&oil);
    oil.complement();
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(allValues(), true, true)));
}

// Complement of [MinKey, MaxKey] is empty
TEST(IndexBoundsTest, ComplementAllValues) {
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(allValues(), true, true));
    testDoubleComplement(&oil);
    oil.complement();
    ASSERT_EQUALS(oil.intervals.size(), 0U);
}

// Complement of [MinKey, 3), [5, MaxKey) is
// [3, 5), [MaxKey, MaxKey].
TEST(IndexBoundsTest, ComplementRanges) {
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(minKeyIntObj(3), true, false));
    oil.intervals.push_back(Interval(maxKeyIntObj(5), true, false));
    testDoubleComplement(&oil);
    oil.complement();
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(BSON("" << 3 << "" << 5), true, false)));

    // Make the interval [MaxKey, MaxKey].
    BSONObjBuilder bob;
    bob.appendMaxKey("");
    bob.appendMaxKey("");
    BSONObj maxKeyInt = bob.obj();

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(maxKeyInt, true, true)));
}

// Complement of (MinKey, 3), (3, MaxKey) is
// [MinKey, MinKey], [3, 3], [MaxKey, MaxKey].
TEST(IndexBoundsTest, ComplementRanges2) {
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(minKeyIntObj(3), false, false));
    oil.intervals.push_back(Interval(maxKeyIntObj(3), false, false));
    testDoubleComplement(&oil);
    oil.complement();
    ASSERT_EQUALS(oil.intervals.size(), 3U);

    // First interval is [MinKey, MinKey]
    BSONObjBuilder minBob;
    minBob.appendMinKey("");
    minBob.appendMinKey("");
    BSONObj minObj = minBob.obj();
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minObj, true, true)));

    // Second is [3, 3]
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(BSON("" << 3 << "" << 3), true, true)));

    // Third is [MaxKey, MaxKey]
    BSONObjBuilder maxBob;
    maxBob.appendMaxKey("");
    maxBob.appendMaxKey("");
    BSONObj maxObj = maxBob.obj();
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[2].compare(Interval(maxObj, true, true)));
}

//
// Equality
//

TEST(IndexBoundsTest, IndexBoundsEqual) {
    // Both sets of bounds are {a: [[1, 3), (4, 5]], b: [[1,1]]}
    OrderedIntervalList oil1;
    oil1.name = "a";
    oil1.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil1.intervals.push_back(Interval(BSON("" << 4 << "" << 5), false, true));

    OrderedIntervalList oil2;
    oil2.name = "b";
    oil2.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    IndexBounds bounds1;
    bounds1.fields.push_back(oil1);
    bounds1.fields.push_back(oil2);

    OrderedIntervalList oil3;
    oil3.name = "a";
    oil3.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil3.intervals.push_back(Interval(BSON("" << 4 << "" << 5), false, true));

    OrderedIntervalList oil4;
    oil4.name = "b";
    oil4.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    IndexBounds bounds2;
    bounds2.fields.push_back(oil3);
    bounds2.fields.push_back(oil4);

    ASSERT_TRUE(bounds1 == bounds2);
    ASSERT_FALSE(bounds1 != bounds2);
}

TEST(IndexBoundsTest, EmptyBoundsEqual) {
    IndexBounds bounds1;
    IndexBounds bounds2;
    ASSERT_TRUE(bounds1 == bounds2);
    ASSERT_FALSE(bounds1 != bounds2);
}

TEST(IndexBoundsTest, SimpleRangeBoundsEqual) {
    IndexBounds bounds1;
    bounds1.isSimpleRange = true;
    bounds1.startKey = BSON("" << 1 << "" << 3);
    bounds1.endKey = BSON("" << 2 << "" << 4);
    bounds1.endKeyInclusive = false;

    IndexBounds bounds2;
    bounds2.isSimpleRange = true;
    bounds2.startKey = BSON("" << 1 << "" << 3);
    bounds2.endKey = BSON("" << 2 << "" << 4);
    bounds2.endKeyInclusive = false;

    ASSERT_TRUE(bounds1 == bounds2);
    ASSERT_FALSE(bounds1 != bounds2);
}

TEST(IndexBoundsTest, BoundsNotEqualDifferentFieldNames) {
    // First set of bounds: {a: [[1, 3), (4, 5]], b: [[1,1]]}
    OrderedIntervalList oil1;
    oil1.name = "a";
    oil1.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil1.intervals.push_back(Interval(BSON("" << 4 << "" << 5), false, true));

    OrderedIntervalList oil2;
    oil2.name = "b";
    oil2.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    IndexBounds bounds1;
    bounds1.fields.push_back(oil1);
    bounds1.fields.push_back(oil2);

    // Second set of bounds identical except for the second field name:
    //    {a: [[1, 3), (4, 5]], c: [[1,1]]}
    OrderedIntervalList oil3;
    oil3.name = "a";
    oil3.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil3.intervals.push_back(Interval(BSON("" << 4 << "" << 5), false, true));

    OrderedIntervalList oil4;
    oil4.name = "c";
    oil4.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    IndexBounds bounds2;
    bounds2.fields.push_back(oil3);
    bounds2.fields.push_back(oil4);

    ASSERT_FALSE(bounds1 == bounds2);
    ASSERT_TRUE(bounds1 != bounds2);
}

TEST(IndexBoundsTest, BoundsNotEqualOneRangeExclusive) {
    // First set of bounds: {a: [[1, 3), (4, 5]], b: [[1,1]]}
    OrderedIntervalList oil1;
    oil1.name = "a";
    oil1.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil1.intervals.push_back(Interval(BSON("" << 4 << "" << 5), false, true));

    OrderedIntervalList oil2;
    oil2.name = "b";
    oil2.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    IndexBounds bounds1;
    bounds1.fields.push_back(oil1);
    bounds1.fields.push_back(oil2);

    // Second set of bounds identical except for (4, 5] changed to (4, 5):
    //    {a: [[1, 3), (4, 5]], b: [[1,1]]}
    OrderedIntervalList oil3;
    oil3.name = "a";
    oil3.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil3.intervals.push_back(Interval(BSON("" << 4 << "" << 5), false, false));

    OrderedIntervalList oil4;
    oil4.name = "b";
    oil4.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    IndexBounds bounds2;
    bounds2.fields.push_back(oil3);
    bounds2.fields.push_back(oil4);

    ASSERT_FALSE(bounds1 == bounds2);
    ASSERT_TRUE(bounds1 != bounds2);
}

TEST(IndexBoundsTest, BoundsNotEqualExtraInterval) {
    // First set of bounds: {a: [[1, 3), (4, 5]], b: [[1,1]]}
    OrderedIntervalList oil1;
    oil1.name = "a";
    oil1.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil1.intervals.push_back(Interval(BSON("" << 4 << "" << 5), false, true));

    OrderedIntervalList oil2;
    oil2.name = "b";
    oil2.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    IndexBounds bounds1;
    bounds1.fields.push_back(oil1);
    bounds1.fields.push_back(oil2);

    // Second set of bounds has an additional interval for field 'a':
    //    {a: [[1, 3), (4, 5], (6, 7)], b: [[1,1]]}
    OrderedIntervalList oil3;
    oil3.name = "a";
    oil3.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil3.intervals.push_back(Interval(BSON("" << 4 << "" << 5), false, true));
    oil3.intervals.push_back(Interval(BSON("" << 6 << "" << 7), false, false));

    OrderedIntervalList oil4;
    oil4.name = "b";
    oil4.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    IndexBounds bounds2;
    bounds2.fields.push_back(oil3);
    bounds2.fields.push_back(oil4);

    ASSERT_FALSE(bounds1 == bounds2);
    ASSERT_TRUE(bounds1 != bounds2);
}

TEST(IndexBoundsTest, BoundsNotEqualExtraField) {
    // First set of bounds: {a: [[1, 3), (4, 5]], b: [[1,1]]}
    OrderedIntervalList oil1;
    oil1.name = "a";
    oil1.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil1.intervals.push_back(Interval(BSON("" << 4 << "" << 5), false, true));

    OrderedIntervalList oil2;
    oil2.name = "b";
    oil2.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    IndexBounds bounds1;
    bounds1.fields.push_back(oil1);
    bounds1.fields.push_back(oil2);

    // Second set of bounds has an additional field 'c':
    //    {a: [[1, 3), (4, 5]], b: [[1,1]], c: [[1]]}
    OrderedIntervalList oil3;
    oil3.name = "a";
    oil3.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil3.intervals.push_back(Interval(BSON("" << 4 << "" << 5), false, true));

    OrderedIntervalList oil4;
    oil4.name = "b";
    oil4.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    OrderedIntervalList oil5;
    oil4.name = "c";
    oil4.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));

    IndexBounds bounds2;
    bounds2.fields.push_back(oil3);
    bounds2.fields.push_back(oil4);
    bounds2.fields.push_back(oil5);

    ASSERT_FALSE(bounds1 == bounds2);
    ASSERT_TRUE(bounds1 != bounds2);
}

TEST(IndexBoundsTest, SimpleRangeBoundsNotEqualToRegularBounds) {
    IndexBounds bounds1;
    bounds1.isSimpleRange = true;
    bounds1.startKey = BSON("" << 1 << "" << 3);
    bounds1.endKey = BSON("" << 2 << "" << 4);
    bounds1.endKeyInclusive = false;

    IndexBounds bounds2;
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(BSON("" << 1 << "" << 1), true, true));
    bounds2.fields.push_back(oil);

    ASSERT_FALSE(bounds1 == bounds2);
    ASSERT_TRUE(bounds1 != bounds2);
}

TEST(IndexBoundsTest, SimpleRangeBoundsNotEqualDifferentStartKey) {
    IndexBounds bounds1;
    bounds1.isSimpleRange = true;
    bounds1.startKey = BSON("" << 1 << "" << 3);
    bounds1.endKey = BSON("" << 2 << "" << 4);
    bounds1.endKeyInclusive = false;

    IndexBounds bounds2;
    bounds1.isSimpleRange = true;
    bounds1.startKey = BSON("" << 1 << "" << 1);
    bounds1.endKey = BSON("" << 2 << "" << 4);
    bounds1.endKeyInclusive = false;

    ASSERT_FALSE(bounds1 == bounds2);
    ASSERT_TRUE(bounds1 != bounds2);
}

TEST(IndexBoundsTest, SimpleRangeBoundsNotEqualDifferentEndKey) {
    IndexBounds bounds1;
    bounds1.isSimpleRange = true;
    bounds1.startKey = BSON("" << 1 << "" << 3);
    bounds1.endKey = BSON("" << 2 << "" << 4);
    bounds1.endKeyInclusive = false;

    IndexBounds bounds2;
    bounds1.isSimpleRange = true;
    bounds1.startKey = BSON("" << 1 << "" << 3);
    bounds1.endKey = BSON("" << 2 << "" << 99);
    bounds1.endKeyInclusive = false;

    ASSERT_FALSE(bounds1 == bounds2);
    ASSERT_TRUE(bounds1 != bounds2);
}

TEST(IndexBoundsTest, SimpleRangeBoundsNotEqualDifferentEndKeyInclusive) {
    IndexBounds bounds1;
    bounds1.isSimpleRange = true;
    bounds1.startKey = BSON("" << 1 << "" << 3);
    bounds1.endKey = BSON("" << 2 << "" << 4);
    bounds1.endKeyInclusive = false;

    IndexBounds bounds2;
    bounds1.isSimpleRange = true;
    bounds1.startKey = BSON("" << 1 << "" << 3);
    bounds1.endKey = BSON("" << 2 << "" << 4);
    bounds1.endKeyInclusive = true;

    ASSERT_FALSE(bounds1 == bounds2);
    ASSERT_TRUE(bounds1 != bounds2);
}

//
// Iteration over
//

TEST(IndexBoundsCheckerTest, StartKey) {
    OrderedIntervalList fooList("foo");
    fooList.intervals.push_back(Interval(BSON("" << 7 << "" << 20), true, true));

    OrderedIntervalList barList("bar");
    barList.intervals.push_back(Interval(BSON("" << 0 << "" << 5), false, false));

    IndexBounds bounds;
    bounds.fields.push_back(fooList);
    bounds.fields.push_back(barList);
    IndexBoundsChecker it(&bounds, BSON("foo" << 1 << "bar" << 1), 1);

    IndexSeekPoint seekPoint;
    it.getStartSeekPoint(&seekPoint);

    ASSERT_EQUALS(seekPoint.keySuffix[0]->numberInt(), 7);
    ASSERT_EQUALS(seekPoint.suffixInclusive[0], true);
    ASSERT_EQUALS(seekPoint.keySuffix[1]->numberInt(), 0);
    ASSERT_EQUALS(seekPoint.suffixInclusive[1], false);
}

TEST(IndexBoundsCheckerTest, CheckEnd) {
    OrderedIntervalList fooList("foo");
    fooList.intervals.push_back(Interval(BSON("" << 7 << "" << 20), true, true));
    fooList.intervals.push_back(Interval(BSON("" << 21 << "" << 30), true, false));

    OrderedIntervalList barList("bar");
    barList.intervals.push_back(Interval(BSON("" << 0 << "" << 5), false, false));

    IndexBounds bounds;
    bounds.fields.push_back(fooList);
    bounds.fields.push_back(barList);
    IndexBoundsChecker it(&bounds, BSON("foo" << 1 << "bar" << 1), 1);

    IndexSeekPoint seekPoint;
    IndexBoundsChecker::KeyState state;

    // Start at something in our range.
    state = it.checkKey(BSON("" << 7 << "" << 1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // Second field moves past the end, but we're not done, since there's still an interval in
    // the previous field that the key hasn't advanced to.
    state = it.checkKey(BSON("" << 20 << "" << 5), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 1);
    ASSERT(seekPoint.prefixExclusive);

    // The next index key is in the second interval for 'foo' and there is a valid interval for
    // 'bar'.
    state = it.checkKey(BSON("" << 22 << "" << 1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // The next index key is very close to the end of the open interval for foo, and it's past
    // the interval for 'bar'.  Since the interval for foo is open, we are asked to move
    // forward, since we possibly could.
    state = it.checkKey(BSON("" << 29.9 << "" << 5), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 1);
    ASSERT(seekPoint.prefixExclusive);
}

TEST(IndexBoundsCheckerTest, MoveIntervalForwardToNextInterval) {
    OrderedIntervalList fooList("foo");
    fooList.intervals.push_back(Interval(BSON("" << 7 << "" << 20), true, true));
    fooList.intervals.push_back(Interval(BSON("" << 21 << "" << 30), true, false));

    OrderedIntervalList barList("bar");
    barList.intervals.push_back(Interval(BSON("" << 0 << "" << 5), false, false));

    IndexBounds bounds;
    bounds.fields.push_back(fooList);
    bounds.fields.push_back(barList);
    IndexBoundsChecker it(&bounds, BSON("foo" << 1 << "bar" << 1), 1);

    IndexSeekPoint seekPoint;
    IndexBoundsChecker::KeyState state;

    // Start at something in our range.
    state = it.checkKey(BSON("" << 7 << "" << 1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // "foo" moves between two intervals.
    state = it.checkKey(BSON("" << 20.5 << "" << 1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 0);
    // Should be told to move exactly to the next interval's beginning.
    ASSERT_EQUALS(seekPoint.prefixExclusive, false);
    ASSERT_EQUALS(seekPoint.keySuffix[0]->numberInt(), 21);
    ASSERT_EQUALS(seekPoint.suffixInclusive[0], true);
    ASSERT_EQUALS(seekPoint.keySuffix[1]->numberInt(), 0);
    ASSERT_EQUALS(seekPoint.suffixInclusive[1], false);
}

TEST(IndexBoundsCheckerTest, MoveIntervalForwardManyIntervals) {
    OrderedIntervalList fooList("foo");
    fooList.intervals.push_back(Interval(BSON("" << 7 << "" << 20), true, true));
    fooList.intervals.push_back(Interval(BSON("" << 21 << "" << 30), true, false));
    fooList.intervals.push_back(Interval(BSON("" << 31 << "" << 40), true, false));
    fooList.intervals.push_back(Interval(BSON("" << 41 << "" << 50), true, false));

    IndexBounds bounds;
    bounds.fields.push_back(fooList);
    IndexBoundsChecker it(&bounds, BSON("foo" << 1), 1);

    IndexSeekPoint seekPoint;
    IndexBoundsChecker::KeyState state;

    // Start at something in our range.
    state = it.checkKey(BSON("" << 7), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // "foo" moves forward a few intervals.
    state = it.checkKey(BSON("" << 42), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);
}

TEST(IndexBoundsCheckerTest, SimpleCheckKey) {
    OrderedIntervalList fooList("foo");
    fooList.intervals.push_back(Interval(BSON("" << 7 << "" << 20), true, true));

    OrderedIntervalList barList("bar");
    barList.intervals.push_back(Interval(BSON("" << 0 << "" << 5), false, true));

    IndexBounds bounds;
    bounds.fields.push_back(fooList);
    bounds.fields.push_back(barList);
    IndexBoundsChecker it(&bounds, BSON("foo" << 1 << "bar" << 1), 1);

    IndexSeekPoint seekPoint;
    IndexBoundsChecker::KeyState state;

    // Start at something in our range.
    state = it.checkKey(BSON("" << 7 << "" << 1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // The rightmost key is past the range.  We should be told to move past the key before the
    // one whose interval we exhausted.
    state = it.checkKey(BSON("" << 7 << "" << 5.00001), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 1);
    ASSERT_EQUALS(seekPoint.prefixExclusive, true);

    // Move a little forward, but note that the rightmost key isn't in the interval yet.
    state = it.checkKey(BSON("" << 7.2 << "" << 0), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 1);
    ASSERT_EQUALS(seekPoint.prefixExclusive, false);
    ASSERT_EQUALS(seekPoint.keySuffix[1]->numberInt(), 0);
    ASSERT_EQUALS(seekPoint.suffixInclusive[1], false);

    // Move to the edge of both intervals, 20,5
    state = it.checkKey(BSON("" << 20 << "" << 5), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // And a little beyond.
    state = it.checkKey(BSON("" << 20 << "" << 5.1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::DONE);
}

TEST(IndexBoundsCheckerTest, FirstKeyMovedIsOKSecondKeyMustMove) {
    OrderedIntervalList fooList("foo");
    fooList.intervals.push_back(Interval(BSON("" << 0 << "" << 9), true, true));
    fooList.intervals.push_back(Interval(BSON("" << 10 << "" << 20), true, true));

    OrderedIntervalList barList("bar");
    barList.intervals.push_back(Interval(BSON("" << 0 << "" << 5), false, true));

    IndexBounds bounds;
    bounds.fields.push_back(fooList);
    bounds.fields.push_back(barList);
    IndexBoundsChecker it(&bounds, BSON("foo" << 1 << "bar" << 1), 1);

    IndexSeekPoint seekPoint;
    IndexBoundsChecker::KeyState state;

    // Start at something in our range.
    state = it.checkKey(BSON("" << 0 << "" << 1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // First key moves to next interval, second key needs to be advanced.
    state = it.checkKey(BSON("" << 10 << "" << -1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 1);
    ASSERT_EQUALS(seekPoint.prefixExclusive, false);
    ASSERT_EQUALS(seekPoint.keySuffix[1]->numberInt(), 0);
    ASSERT_EQUALS(seekPoint.suffixInclusive[1], false);
}

TEST(IndexBoundsCheckerTest, SecondIntervalMustRewind) {
    OrderedIntervalList first("first");
    first.intervals.push_back(Interval(BSON("" << 25 << "" << 30), true, true));

    OrderedIntervalList second("second");
    second.intervals.push_back(Interval(BSON("" << 0 << "" << 0), true, true));
    second.intervals.push_back(Interval(BSON("" << 9 << "" << 9), true, true));

    IndexBounds bounds;
    bounds.fields.push_back(first);
    bounds.fields.push_back(second);

    BSONObj idx = BSON("first" << 1 << "second" << 1);
    ASSERT(bounds.isValidFor(idx, 1));
    IndexBoundsChecker it(&bounds, idx, 1);

    IndexSeekPoint seekPoint;
    IndexBoundsChecker::KeyState state;

    state = it.checkKey(BSON("" << 25 << "" << 0), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    state = it.checkKey(BSON("" << 25 << "" << 1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 1);
    ASSERT_EQUALS(seekPoint.prefixExclusive, false);
    ASSERT_EQUALS(seekPoint.keySuffix[1]->numberInt(), 9);
    ASSERT_EQUALS(seekPoint.suffixInclusive[1], true);

    state = it.checkKey(BSON("" << 25 << "" << 9), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // First key moved forward.  The second key moved back to a valid state but it's behind
    // the interval that the checker thought it was in.
    state = it.checkKey(BSON("" << 26 << "" << 0), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);
}

TEST(IndexBoundsCheckerTest, SimpleCheckKeyBackwards) {
    OrderedIntervalList fooList("foo");
    fooList.intervals.push_back(Interval(BSON("" << 20 << "" << 7), true, true));

    OrderedIntervalList barList("bar");
    barList.intervals.push_back(Interval(BSON("" << 5 << "" << 0), true, false));

    IndexBounds bounds;
    bounds.fields.push_back(fooList);
    bounds.fields.push_back(barList);

    BSONObj idx = BSON("foo" << -1 << "bar" << -1);
    ASSERT(bounds.isValidFor(idx, 1));
    IndexBoundsChecker it(&bounds, idx, 1);

    IndexSeekPoint seekPoint;
    IndexBoundsChecker::KeyState state;

    // Start at something in our range.
    state = it.checkKey(BSON("" << 20 << "" << 5), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // The rightmost key is past the range.  We should be told to move past the key before the
    // one whose interval we exhausted.
    state = it.checkKey(BSON("" << 20 << "" << 0), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 1);
    ASSERT_EQUALS(seekPoint.prefixExclusive, true);

    // Move a little forward, but note that the rightmost key isn't in the interval yet.
    state = it.checkKey(BSON("" << 19 << "" << 6), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 1);
    ASSERT_EQUALS(seekPoint.prefixExclusive, false);
    ASSERT_EQUALS(seekPoint.keySuffix[1]->numberInt(), 5);
    ASSERT_EQUALS(seekPoint.suffixInclusive[1], true);

    // Move to the edge of both intervals
    state = it.checkKey(BSON("" << 7 << "" << 0.01), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // And a little beyond.
    state = it.checkKey(BSON("" << 7 << "" << 0), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::DONE);
}

TEST(IndexBoundsCheckerTest, CheckEndBackwards) {
    OrderedIntervalList fooList("foo");
    fooList.intervals.push_back(Interval(BSON("" << 30 << "" << 21), true, true));
    fooList.intervals.push_back(Interval(BSON("" << 20 << "" << 7), true, false));

    OrderedIntervalList barList("bar");
    barList.intervals.push_back(Interval(BSON("" << 0 << "" << 5), false, false));

    IndexBounds bounds;
    bounds.fields.push_back(fooList);
    bounds.fields.push_back(barList);

    BSONObj idx = BSON("foo" << 1 << "bar" << -1);
    ASSERT(bounds.isValidFor(idx, -1));
    IndexBoundsChecker it(&bounds, idx, -1);

    IndexSeekPoint seekPoint;
    IndexBoundsChecker::KeyState state;

    // Start at something in our range.
    state = it.checkKey(BSON("" << 30 << "" << 1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // Second field moves past the end, but we're not done, since there's still an interval in
    // the previous field that the key hasn't advanced to.
    state = it.checkKey(BSON("" << 30 << "" << 5), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 1);
    ASSERT(seekPoint.prefixExclusive);

    // The next index key is in the second interval for 'foo' and there is a valid interval for
    // 'bar'.
    state = it.checkKey(BSON("" << 20 << "" << 1), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

    // The next index key is very close to the end of the open interval for foo, and it's past
    // the interval for 'bar'.  Since the interval for foo is open, we are asked to move
    // forward, since we possibly could.
    state = it.checkKey(BSON("" << 7.001 << "" << 5), &seekPoint);
    ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
    ASSERT_EQUALS(seekPoint.prefixLen, 1);
    ASSERT(seekPoint.prefixExclusive);
}

//
// IndexBoundsChecker::findIntervalForField
//

/**
 * Returns string representation of IndexBoundsChecker::Location.
 */
std::string toString(IndexBoundsChecker::Location location) {
    switch (location) {
        case IndexBoundsChecker::BEHIND:
            return "BEHIND";
        case IndexBoundsChecker::WITHIN:
            return "WITHIN";
        case IndexBoundsChecker::AHEAD:
            return "AHEAD";
    }
    invariant(0);
}

/**
 * Test function for findIntervalForField.
 * Constructs a list of point intervals from 'points' and searches for 'key'
 * using findIntervalForField(). Verifies expected location and index (if expectedLocation
 * is BEHIND or WITHIN).
 * 'points' is provided in BSON format: {points: [pt1, pt2, pt4, ...]
 */
void testFindIntervalForField(int key,
                              const BSONObj& pointsObj,
                              const int expectedDirection,
                              IndexBoundsChecker::Location expectedLocation,
                              size_t expectedIntervalIndex) {
    // Create key BSONElement.
    BSONObj keyObj = BSON("" << key);
    BSONElement keyElt = keyObj.firstElement();

    // Construct point intervals.
    OrderedIntervalList oil("foo");
    BSONObjIterator i(pointsObj.getObjectField("points"));
    while (i.more()) {
        BSONElement e = i.next();
        int j = e.numberInt();
        oil.intervals.push_back(Interval(BSON("" << j << "" << j), true, true));
    }
    size_t intervalIndex = 0;
    IndexBoundsChecker::Location location =
        IndexBoundsChecker::findIntervalForField(keyElt, oil, expectedDirection, &intervalIndex);
    if (expectedLocation != location) {
        mongoutils::str::stream ss;
        ss << "Unexpected location from findIntervalForField: key=" << keyElt
           << "; intervals=" << oil.toString() << "; direction=" << expectedDirection
           << ". Expected: " << toString(expectedLocation) << ". Actual: " << toString(location);
        FAIL(ss);
    }
    // Check interval index if location is BEHIND or WITHIN.
    if ((IndexBoundsChecker::BEHIND == expectedLocation ||
         IndexBoundsChecker::WITHIN == expectedLocation) &&
        expectedIntervalIndex != intervalIndex) {
        mongoutils::str::stream ss;
        ss << "Unexpected interval index from findIntervalForField: key=" << keyElt
           << "; intervals=" << oil.toString() << "; direction=" << expectedDirection
           << "; location= " << toString(location) << ". Expected: " << expectedIntervalIndex
           << ". Actual: " << intervalIndex;
        FAIL(ss);
    }
}

TEST(IndexBoundsCheckerTest, FindIntervalForField) {
    // No intervals
    BSONObj pointsObj = fromjson("{points: []}");
    testFindIntervalForField(5, pointsObj, 1, IndexBoundsChecker::AHEAD, 0U);
    testFindIntervalForField(5, pointsObj, -1, IndexBoundsChecker::AHEAD, 0U);

    // One interval
    pointsObj = fromjson("{points: [5]}");
    testFindIntervalForField(4, pointsObj, 1, IndexBoundsChecker::BEHIND, 0U);
    testFindIntervalForField(5, pointsObj, 1, IndexBoundsChecker::WITHIN, 0U);
    testFindIntervalForField(6, pointsObj, 1, IndexBoundsChecker::AHEAD, 0U);

    // One interval - reverse direction
    pointsObj = fromjson("{points: [5]}");
    testFindIntervalForField(6, pointsObj, -1, IndexBoundsChecker::BEHIND, 0U);
    testFindIntervalForField(5, pointsObj, -1, IndexBoundsChecker::WITHIN, 0U);
    testFindIntervalForField(4, pointsObj, -1, IndexBoundsChecker::AHEAD, 0U);

    // Two intervals
    // Verifies off-by-one handling in upper bound of binary search.
    pointsObj = fromjson("{points: [5, 7]}");
    testFindIntervalForField(4, pointsObj, 1, IndexBoundsChecker::BEHIND, 0U);
    testFindIntervalForField(5, pointsObj, 1, IndexBoundsChecker::WITHIN, 0U);
    testFindIntervalForField(6, pointsObj, 1, IndexBoundsChecker::BEHIND, 1U);
    testFindIntervalForField(7, pointsObj, 1, IndexBoundsChecker::WITHIN, 1U);
    testFindIntervalForField(8, pointsObj, 1, IndexBoundsChecker::AHEAD, 0U);

    // Two intervals - reverse direction
    // Verifies off-by-one handling in upper bound of binary search.
    pointsObj = fromjson("{points: [7, 5]}");
    testFindIntervalForField(8, pointsObj, -1, IndexBoundsChecker::BEHIND, 0U);
    testFindIntervalForField(7, pointsObj, -1, IndexBoundsChecker::WITHIN, 0U);
    testFindIntervalForField(6, pointsObj, -1, IndexBoundsChecker::BEHIND, 1U);
    testFindIntervalForField(5, pointsObj, -1, IndexBoundsChecker::WITHIN, 1U);
    testFindIntervalForField(4, pointsObj, -1, IndexBoundsChecker::AHEAD, 0U);

    // Multiple intervals - odd number of intervals.
    pointsObj = fromjson("{points: [1, 3, 5, 7, 9]}");
    testFindIntervalForField(0, pointsObj, 1, IndexBoundsChecker::BEHIND, 0U);
    testFindIntervalForField(1, pointsObj, 1, IndexBoundsChecker::WITHIN, 0U);
    testFindIntervalForField(2, pointsObj, 1, IndexBoundsChecker::BEHIND, 1U);
    testFindIntervalForField(3, pointsObj, 1, IndexBoundsChecker::WITHIN, 1U);
    testFindIntervalForField(4, pointsObj, 1, IndexBoundsChecker::BEHIND, 2U);
    testFindIntervalForField(5, pointsObj, 1, IndexBoundsChecker::WITHIN, 2U);
    testFindIntervalForField(6, pointsObj, 1, IndexBoundsChecker::BEHIND, 3U);
    testFindIntervalForField(7, pointsObj, 1, IndexBoundsChecker::WITHIN, 3U);
    testFindIntervalForField(8, pointsObj, 1, IndexBoundsChecker::BEHIND, 4U);
    testFindIntervalForField(9, pointsObj, 1, IndexBoundsChecker::WITHIN, 4U);
    testFindIntervalForField(10, pointsObj, 1, IndexBoundsChecker::AHEAD, 0U);

    // Multiple intervals - even number of intervals, reverse direction
    // Interval order has to match direction.
    pointsObj = fromjson("{points: [7, 5, 3, 1]}");
    testFindIntervalForField(8, pointsObj, -1, IndexBoundsChecker::BEHIND, 0U);
    testFindIntervalForField(7, pointsObj, -1, IndexBoundsChecker::WITHIN, 0U);
    testFindIntervalForField(6, pointsObj, -1, IndexBoundsChecker::BEHIND, 1U);
    testFindIntervalForField(5, pointsObj, -1, IndexBoundsChecker::WITHIN, 1U);
    testFindIntervalForField(4, pointsObj, -1, IndexBoundsChecker::BEHIND, 2U);
    testFindIntervalForField(3, pointsObj, -1, IndexBoundsChecker::WITHIN, 2U);
    testFindIntervalForField(2, pointsObj, -1, IndexBoundsChecker::BEHIND, 3U);
    testFindIntervalForField(1, pointsObj, -1, IndexBoundsChecker::WITHIN, 3U);
    testFindIntervalForField(0, pointsObj, -1, IndexBoundsChecker::AHEAD, 0U);
}

}  // namespace
