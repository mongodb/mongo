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

#include "mongo/db/query/index_bounds.h"
#include "mongo/db/json.h"
#include "mongo/db/jsobj.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

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

        vector<const BSONElement*> elt(2);
        vector<bool> inc(2);

        it.getStartKey(&elt, &inc);

        ASSERT_EQUALS(elt[0]->numberInt(), 7);
        ASSERT_EQUALS(inc[0], true);
        ASSERT_EQUALS(elt[1]->numberInt(), 0);
        ASSERT_EQUALS(inc[1], false);
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

        int keyEltsToUse;
        bool movePastKeyElts;
        vector<const BSONElement*> elt(2);
        vector<bool> inc(2);

        IndexBoundsChecker::KeyState state;

        // Start at something in our range.
        state = it.checkKey(BSON("" << 7 << "" << 1),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // Second field moves past the end, but we're not done, since there's still an interval in
        // the previous field that the key hasn't advanced to.
        state = it.checkKey(BSON("" << 20 << "" << 5),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
        ASSERT_EQUALS(keyEltsToUse, 1);
        ASSERT(movePastKeyElts);

        // The next index key is in the second interval for 'foo' and there is a valid interval for
        // 'bar'.
        state = it.checkKey(BSON("" << 22 << "" << 1),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // The next index key is very close to the end of the open interval for foo, and it's past
        // the interval for 'bar'.  Since the interval for foo is open, we are asked to move
        // forward, since we possibly could.
        state = it.checkKey(BSON("" << 29.9 << "" << 5),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
        ASSERT_EQUALS(keyEltsToUse, 1);
        ASSERT(movePastKeyElts);
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

        int keyEltsToUse;
        bool movePastKeyElts;
        vector<const BSONElement*> elt(2);
        vector<bool> inc(2);

        IndexBoundsChecker::KeyState state;

        // Start at something in our range.
        state = it.checkKey(BSON("" << 7 << "" << 1),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // "foo" moves between two intervals.
        state = it.checkKey(BSON("" << 20.5 << "" << 1),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
        ASSERT_EQUALS(keyEltsToUse, 0);
        // Should be told to move exactly to the next interval's beginning.
        ASSERT_EQUALS(movePastKeyElts, false);
        ASSERT_EQUALS(elt[0]->numberInt(), 21);
        ASSERT_EQUALS(inc[0], true);
        ASSERT_EQUALS(elt[1]->numberInt(), 0);
        ASSERT_EQUALS(inc[1], false);
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

        int keyEltsToUse;
        bool movePastKeyElts;
        vector<const BSONElement*> elt(1);
        vector<bool> inc(1);

        IndexBoundsChecker::KeyState state;

        // Start at something in our range.
        state = it.checkKey(BSON("" << 7),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // "foo" moves forward a few intervals.
        state = it.checkKey(BSON("" << 42),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
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

        int keyEltsToUse;
        bool movePastKeyElts;
        vector<const BSONElement*> elt(2);
        vector<bool> inc(2);

        IndexBoundsChecker::KeyState state;

        // Start at something in our range.
        state = it.checkKey(BSON("" << 7 << "" << 1),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // The rightmost key is past the range.  We should be told to move past the key before the
        // one whose interval we exhausted.
        state = it.checkKey(BSON("" << 7 << "" << 5.00001),
                            &keyEltsToUse,
                            &movePastKeyElts,
                            &elt,
                            &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
        ASSERT_EQUALS(keyEltsToUse, 1);
        ASSERT_EQUALS(movePastKeyElts, true);

        // Move a little forward, but note that the rightmost key isn't in the interval yet.
        state = it.checkKey(BSON("" << 7.2 << "" << 0),
                            &keyEltsToUse,
                            &movePastKeyElts,
                            &elt,
                            &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
        ASSERT_EQUALS(keyEltsToUse, 1);
        ASSERT_EQUALS(movePastKeyElts, false);
        ASSERT_EQUALS(elt[1]->numberInt(), 0);
        ASSERT_EQUALS(inc[1], false);

        // Move to the edge of both intervals, 20,5
        state = it.checkKey(BSON("" << 20 << "" << 5),
                            &keyEltsToUse,
                            &movePastKeyElts,
                            &elt,
                            &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // And a little beyond.
        state = it.checkKey(BSON("" << 20 << "" << 5.1),
                            &keyEltsToUse,
                            &movePastKeyElts,
                            &elt,
                            &inc);
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

        int keyEltsToUse;
        bool movePastKeyElts;
        vector<const BSONElement*> elt(2);
        vector<bool> inc(2);

        IndexBoundsChecker::KeyState state;

        // Start at something in our range.
        state = it.checkKey(BSON("" << 0 << "" << 1),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // First key moves to next interval, second key needs to be advanced.
        state = it.checkKey(BSON("" << 10 << "" << -1),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
        ASSERT_EQUALS(keyEltsToUse, 1);
        ASSERT_EQUALS(movePastKeyElts, false);
        ASSERT_EQUALS(elt[1]->numberInt(), 0);
        ASSERT_EQUALS(inc[1], false);
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

        int keyEltsToUse;
        bool movePastKeyElts;
        vector<const BSONElement*> elt(2);
        vector<bool> inc(2);

        IndexBoundsChecker::KeyState state;

        // Start at something in our range.
        state = it.checkKey(BSON("" << 20 << "" << 5),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // The rightmost key is past the range.  We should be told to move past the key before the
        // one whose interval we exhausted.
        state = it.checkKey(BSON("" << 20 << "" << 0),
                            &keyEltsToUse,
                            &movePastKeyElts,
                            &elt,
                            &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
        ASSERT_EQUALS(keyEltsToUse, 1);
        ASSERT_EQUALS(movePastKeyElts, true);

        // Move a little forward, but note that the rightmost key isn't in the interval yet.
        state = it.checkKey(BSON("" << 19 << "" << 6),
                            &keyEltsToUse,
                            &movePastKeyElts,
                            &elt,
                            &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
        ASSERT_EQUALS(keyEltsToUse, 1);
        ASSERT_EQUALS(movePastKeyElts, false);
        ASSERT_EQUALS(elt[1]->numberInt(), 5);
        ASSERT_EQUALS(inc[1], true);

        // Move to the edge of both intervals
        state = it.checkKey(BSON("" << 7 << "" << 0.01),
                            &keyEltsToUse,
                            &movePastKeyElts,
                            &elt,
                            &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // And a little beyond.
        state = it.checkKey(BSON("" << 7 << "" << 0),
                            &keyEltsToUse,
                            &movePastKeyElts,
                            &elt,
                            &inc);
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

        int keyEltsToUse;
        bool movePastKeyElts;
        vector<const BSONElement*> elt(2);
        vector<bool> inc(2);

        IndexBoundsChecker::KeyState state;

        // Start at something in our range.
        state = it.checkKey(BSON("" << 30 << "" << 1),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // Second field moves past the end, but we're not done, since there's still an interval in
        // the previous field that the key hasn't advanced to.
        state = it.checkKey(BSON("" << 30 << "" << 5),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
        ASSERT_EQUALS(keyEltsToUse, 1);
        ASSERT(movePastKeyElts);

        // The next index key is in the second interval for 'foo' and there is a valid interval for
        // 'bar'.
        state = it.checkKey(BSON("" << 20 << "" << 1),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::VALID);

        // The next index key is very close to the end of the open interval for foo, and it's past
        // the interval for 'bar'.  Since the interval for foo is open, we are asked to move
        // forward, since we possibly could.
        state = it.checkKey(BSON("" << 7.001 << "" << 5),
                             &keyEltsToUse,
                             &movePastKeyElts,
                             &elt,
                             &inc);
        ASSERT_EQUALS(state, IndexBoundsChecker::MUST_ADVANCE);
        ASSERT_EQUALS(keyEltsToUse, 1);
        ASSERT(movePastKeyElts);
    }

}  // namespace
