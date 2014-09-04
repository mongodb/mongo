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
 * This file contains tests for mongo/db/query/index_bounds_builder.cpp
 */

#include "mongo/db/query/index_bounds_builder.h"

#include <limits>
#include <memory>
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    using std::auto_ptr;

    double numberMin = -numeric_limits<double>::max();
    double numberMax = numeric_limits<double>::max();
    double negativeInfinity = -numeric_limits<double>::infinity();
    double positiveInfinity = numeric_limits<double>::infinity();

    /**
     * Utility function to create MatchExpression
     */
    MatchExpression* parseMatchExpression(const BSONObj& obj) {
        StatusWithMatchExpression status = MatchExpressionParser::parse(obj);
        ASSERT_TRUE(status.isOK());
        MatchExpression* expr(status.getValue());
        return expr;
    }

    /**
     * Given a list of queries in 'toUnion', translate into index bounds and return
     * the union of these bounds in the out-parameter 'oilOut'.
     */
    void testTranslateAndUnion(const vector<BSONObj>& toUnion, OrderedIntervalList* oilOut,
                               IndexBoundsBuilder::BoundsTightness* tightnessOut) {

        IndexEntry testIndex = IndexEntry(BSONObj());

        for (vector<BSONObj>::const_iterator it = toUnion.begin();
             it != toUnion.end();
             ++it) {
            auto_ptr<MatchExpression> expr(parseMatchExpression(*it));
            BSONElement elt = it->firstElement();
            if (toUnion.begin() == it) {
                IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
            }
            else {
                IndexBoundsBuilder::translateAndUnion(expr.get(), elt, testIndex, oilOut, tightnessOut);
            }
        }
    }

    /**
     * Given a list of queries in 'toUnion', translate into index bounds and return
     * the intersection of these bounds in the out-parameter 'oilOut'.
     */
    void testTranslateAndIntersect(const vector<BSONObj>& toIntersect, OrderedIntervalList* oilOut,
                                   IndexBoundsBuilder::BoundsTightness* tightnessOut) {

        IndexEntry testIndex = IndexEntry(BSONObj());

        for (vector<BSONObj>::const_iterator it = toIntersect.begin();
             it != toIntersect.end();
             ++it) {
            auto_ptr<MatchExpression> expr(parseMatchExpression(*it));
            BSONElement elt = it->firstElement();
            if (toIntersect.begin() == it) {
                IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
            }
            else {
                IndexBoundsBuilder::translateAndIntersect(expr.get(), elt, testIndex, oilOut, tightnessOut);
            }
        }
    }

    /**
     * 'constraints' is a vector of BSONObj's representing match expressions, where
     * each filter is paired with a boolean. If the boolean is true, then the filter's
     * index bounds should be intersected with the other constraints; if false, then
     * they should be unioned. The resulting bounds are returned in the
     * out-parameter 'oilOut'.
     */
    void testTranslate(const vector< std::pair<BSONObj, bool> >& constraints,
                       OrderedIntervalList* oilOut,
                       IndexBoundsBuilder::BoundsTightness* tightnessOut) {

        IndexEntry testIndex = IndexEntry(BSONObj());

        for (vector< std::pair<BSONObj, bool> >::const_iterator it = constraints.begin();
             it != constraints.end();
             ++it) {
            BSONObj obj = it->first;
            bool isIntersect = it->second;
            auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
            BSONElement elt = obj.firstElement();
            if (constraints.begin() == it) {
                IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
            }
            else if (isIntersect) {
                IndexBoundsBuilder::translateAndIntersect(expr.get(), elt, testIndex, oilOut, tightnessOut);
            }
            else {
                IndexBoundsBuilder::translateAndUnion(expr.get(), elt, testIndex, oilOut, tightnessOut);
            }
        }
    }

    /**
     * run isSingleInterval and return the result to calling test.
     */
    bool testSingleInterval(IndexBounds bounds) {
        BSONObj startKey;
        bool startKeyIn;
        BSONObj endKey;
        bool endKeyIn;
        return IndexBoundsBuilder::isSingleInterval( bounds,
                                                     &startKey,
                                                     &startKeyIn,
                                                     &endKey,
                                                     &endKeyIn );
    }

    //
    // $elemMatch value
    // Example: {a: {$elemMatch: {$gt: 2}}}
    //

    TEST(IndexBoundsBuilderTest, TranslateElemMatchValue) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        // Bounds generated should be the same as the embedded expression
        // except for the tightness.
        BSONObj obj = fromjson("{a: {$elemMatch: {$gt: 2}}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 2, '': Infinity}"), false, true)));
        ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    }

    //
    // Comparison operators ($lte, $lt, $gt, $gte, $eq)
    //

    TEST(IndexBoundsBuilderTest, TranslateLteNumber) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$lte: 1}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': -Infinity, '': 1}"), true, true)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateLteNumberMin) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = BSON("a" << BSON("$lte" << numberMin));
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(BSON("" << negativeInfinity << "" << numberMin), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateLteNegativeInfinity) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$lte: -Infinity}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': -Infinity, '': -Infinity}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateLtNumber) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$lt: 1}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': -Infinity, '': 1}"), true, false)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateLtNumberMin) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = BSON("a" << BSON("$lt" << numberMin));
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(BSON("" << negativeInfinity << "" << numberMin), true, false)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateLtNegativeInfinity) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$lt: -Infinity}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 0U);
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateLtDate) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = BSON("a" << LT << Date_t(5000));
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': true, '': new Date(5000)}"), false, false)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGtNumber) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$gt: 1}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 1, '': Infinity}"), false, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGtNumberMax) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = BSON("a" << BSON("$gt" << numberMax));
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(BSON("" << numberMax << "" << positiveInfinity), false, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGtPositiveInfinity) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$gt: Infinity}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 0U);
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGteNumber) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$gte: 1}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 1, '': Infinity}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGteNumberMax) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = BSON("a" << BSON("$gte" << numberMax));
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(BSON("" << numberMax << "" << positiveInfinity), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGtePositiveInfinity) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$gte: Infinity}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': Infinity, '': Infinity}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGtString) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$gt: 'abc'}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 'abc', '': {}}"), false, false)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateEqual) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = BSON("a" << 4);
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 4, '': 4}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateArrayEqualBasic) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: [1, 2, 3]}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 2U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 1, '': 1}"), true, true)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[1].compare(
            Interval(fromjson("{'': [1, 2, 3], '': [1, 2, 3]}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    }

    TEST(IndexBoundsBuilderTest, TranslateIn) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$in: [8, 44, -1, -3]}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 4U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': -3, '': -3}"), true, true)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[1].compare(
            Interval(fromjson("{'': -1, '': -1}"), true, true)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[2].compare(
            Interval(fromjson("{'': 8, '': 8}"), true, true)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[3].compare(
            Interval(fromjson("{'': 44, '': 44}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateInArray) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$in: [[1], 2]}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 3U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 1, '': 1}"), true, true)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[1].compare(
            Interval(fromjson("{'': 2, '': 2}"), true, true)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[2].compare(
            Interval(fromjson("{'': [1], '': [1]}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    }

    //
    // $exists tests
    //

    TEST(IndexBoundsBuilderTest, ExistsTrue) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$exists: true}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                      oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    }

    TEST(IndexBoundsBuilderTest, ExistsFalse) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$exists: false}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': null, '': null}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    }

    TEST(IndexBoundsBuilderTest, ExistsTrueSparse) {
        IndexEntry testIndex = IndexEntry(BSONObj(),
                                          false,
                                          true,
                                          "exists_true_sparse",
                                          BSONObj());
        BSONObj obj = fromjson("{a: {$exists: true}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                      oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    //
    // Union tests
    //

    TEST(IndexBoundsBuilderTest, UnionTwoLt) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toUnion;
        toUnion.push_back(fromjson("{a: {$lt: 1}}"));
        toUnion.push_back(fromjson("{a: {$lt: 5}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndUnion(toUnion, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': -Infinity, '': 5}"), true, false)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, UnionDupEq) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toUnion;
        toUnion.push_back(fromjson("{a: 1}"));
        toUnion.push_back(fromjson("{a: 5}"));
        toUnion.push_back(fromjson("{a: 1}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndUnion(toUnion, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 2U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 1, '': 1}"), true, true)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[1].compare(
            Interval(fromjson("{'': 5, '': 5}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, UnionGtLt) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toUnion;
        toUnion.push_back(fromjson("{a: {$gt: 1}}"));
        toUnion.push_back(fromjson("{a: {$lt: 3}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndUnion(toUnion, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': -Infinity, '': Infinity}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, UnionTwoEmptyRanges) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector< std::pair<BSONObj, bool> > constraints;
        constraints.push_back(std::make_pair(fromjson("{a: {$gt: 1}}"), true));
        constraints.push_back(std::make_pair(fromjson("{a: {$lte: 0}}"), true));
        constraints.push_back(std::make_pair(fromjson("{a: {$in:[]}}"), false));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslate(constraints, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 0U);
    }

    //
    // Intersection tests
    //

    TEST(IndexBoundsBuilderTest, IntersectTwoLt) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toIntersect;
        toIntersect.push_back(fromjson("{a: {$lt: 1}}"));
        toIntersect.push_back(fromjson("{a: {$lt: 5}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndIntersect(toIntersect, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': -Infinity, '': 1}"), true, false)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, IntersectEqGte) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toIntersect;
        toIntersect.push_back(fromjson("{a: 1}}"));
        toIntersect.push_back(fromjson("{a: {$gte: 1}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndIntersect(toIntersect, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 1, '': 1}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, IntersectGtLte) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toIntersect;
        toIntersect.push_back(fromjson("{a: {$gt: 0}}"));
        toIntersect.push_back(fromjson("{a: {$lte: 10}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndIntersect(toIntersect, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 0, '': 10}"), false, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, IntersectGtIn) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toIntersect;
        toIntersect.push_back(fromjson("{a: {$gt: 4}}"));
        toIntersect.push_back(fromjson("{a: {$in: [1,2,3,4,5,6]}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndIntersect(toIntersect, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 2U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 5, '': 5}"), true, true)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[1].compare(
            Interval(fromjson("{'': 6, '': 6}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, IntersectionIsPointInterval) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toIntersect;
        toIntersect.push_back(fromjson("{a: {$gte: 1}}"));
        toIntersect.push_back(fromjson("{a: {$lte: 1}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndIntersect(toIntersect, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 1, '': 1}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, IntersectFullyContained) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toIntersect;
        toIntersect.push_back(fromjson("{a: {$gt: 5}}"));
        toIntersect.push_back(fromjson("{a: {$lt: 15}}"));
        toIntersect.push_back(fromjson("{a: {$gte: 6}}"));
        toIntersect.push_back(fromjson("{a: {$lte: 13}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndIntersect(toIntersect, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 6, '': 13}"), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, EmptyIntersection) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toIntersect;
        toIntersect.push_back(fromjson("{a: 1}}"));
        toIntersect.push_back(fromjson("{a: {$gte: 2}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndIntersect(toIntersect, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 0U);
    }

    //
    // $mod
    //

    TEST(IndexBoundsBuilderTest, TranslateMod) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$mod: [2, 0]}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(BSON("" << numberMin << "" << numberMax), true, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
    }

    //
    // Test simpleRegex
    //

    TEST(SimpleRegexTest, RootedLine) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex("^foo", "", &tightness);
        ASSERT_EQUALS(prefix, "foo");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(SimpleRegexTest, RootedString) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "", &tightness);
        ASSERT_EQUALS(prefix, "foo");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(SimpleRegexTest, RootedOptionalFirstChar) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex("^f?oo", "", &tightness);
        ASSERT_EQUALS(prefix, "");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
    }

    TEST(SimpleRegexTest, RootedOptionalSecondChar) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex("^fz?oo", "", &tightness);
        ASSERT_EQUALS(prefix, "f");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
    }

    TEST(SimpleRegexTest, RootedMultiline) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex("^foo", "m", &tightness);
        ASSERT_EQUALS(prefix, "");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
    }

    TEST(SimpleRegexTest, RootedStringMultiline) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "m", &tightness);
        ASSERT_EQUALS(prefix, "foo");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(SimpleRegexTest, RootedCaseInsensitiveMulti) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "mi", &tightness);
        ASSERT_EQUALS(prefix, "");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
    }

    TEST(SimpleRegexTest, RootedComplex) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex(
                "\\Af \t\vo\n\ro  \\ \\# #comment", "mx", &tightness);
        ASSERT_EQUALS(prefix, "foo #");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
    }

    TEST(SimpleRegexTest, RootedLiteral) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex(
            "^\\Qasdf\\E", "", &tightness);
        ASSERT_EQUALS(prefix, "asdf");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(SimpleRegexTest, RootedLiteralWithExtra) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex(
            "^\\Qasdf\\E.*", "", &tightness);
        ASSERT_EQUALS(prefix, "asdf");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
    }

    TEST(SimpleRegexTest, RootedLiteralNoEnd) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex(
            "^\\Qasdf", "", &tightness);
        ASSERT_EQUALS(prefix, "asdf");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(SimpleRegexTest, RootedLiteralBackslash) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex(
            "^\\Qasdf\\\\E", "", &tightness);
        ASSERT_EQUALS(prefix, "asdf\\");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(SimpleRegexTest, RootedLiteralDotStar) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex(
            "^\\Qas.*df\\E", "", &tightness);
        ASSERT_EQUALS(prefix, "as.*df");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(SimpleRegexTest, RootedLiteralNestedEscape) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex(
            "^\\Qas\\Q[df\\E", "", &tightness);
        ASSERT_EQUALS(prefix, "as\\Q[df");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(SimpleRegexTest, RootedLiteralNestedEscapeEnd) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex(
            "^\\Qas\\E\\\\E\\Q$df\\E", "", &tightness);
        ASSERT_EQUALS(prefix, "as\\E$df");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    //
    // Regex bounds
    //

    TEST(IndexBoundsBuilderTest, SimpleNonPrefixRegex) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: /foo/}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.intervals.size(), 2U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': '', '': {}}"), true, false)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[1].compare(
            Interval(fromjson("{'': /foo/, '': /foo/}"), true, true)));
        ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
    }

    TEST(IndexBoundsBuilderTest, SimplePrefixRegex) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: /^foo/}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.intervals.size(), 2U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 'foo', '': 'fop'}"), true, false)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[1].compare(
            Interval(fromjson("{'': /^foo/, '': /^foo/}"), true, true)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    //
    // isSingleInterval
    //

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
        oil.intervals.push_back(Interval(fromjson( "{ '':4, '':5 }" ), true, true));
        oil.intervals.push_back(Interval(fromjson( "{ '':7, '':Infinity }" ), true, true));
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
        oil_b.intervals.push_back(Interval(fromjson( "{ '':6, '':Infinity }" ), true, true));
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
        oil_a.intervals.push_back(Interval(fromjson( "{ '':-Infinity, '':5 }" ), true, true));
        oil_b.intervals.push_back(Interval(fromjson( "{ '':6, '':Infinity }" ), true, true));
        bounds.fields.push_back(oil_a);
        bounds.fields.push_back(oil_b);
        ASSERT(!testSingleInterval(bounds));
    }

    TEST(IndexBoundsBuilderTest, MultipleIntervalsTwoFieldsInterval) {
        // Multiple intervals on two fields is not a compound single interval.
        OrderedIntervalList oil_a("a");
        OrderedIntervalList oil_b("b");
        IndexBounds bounds;
        oil_a.intervals.push_back(Interval(BSON( "" << 4 << "" << 4 ), true, true));
        oil_a.intervals.push_back(Interval(BSON( "" << 5 << "" << 5 ), true, true));
        oil_b.intervals.push_back(Interval(BSON( "" << 7 << "" << 7 ), true, true));
        oil_b.intervals.push_back(Interval(BSON( "" << 8 << "" << 8 ), true, true));
        bounds.fields.push_back(oil_a);
        bounds.fields.push_back(oil_b);
        ASSERT(!testSingleInterval(bounds));
    }

    TEST(IndexBoundsBuilderTest, MissingSecondFieldInterval) {
        // when second field is not specified, still a compound single interval
        OrderedIntervalList oil_a("a");
        OrderedIntervalList oil_b("b");
        IndexBounds bounds;
        oil_a.intervals.push_back(Interval(BSON( "" << 5 << "" << 5 ), true, true));
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
        oil_a.intervals.push_back(Interval(BSON( "" << 5 << "" << 5 ), true, true));
        oil_b.intervals.push_back(Interval(BSON( "" << 6 << "" << 6 ), true, true));
        oil_c.intervals.push_back(Interval(fromjson( "{ '':7, '':Infinity }" ), true, true));
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
        oil_a.intervals.push_back(Interval(BSON( "" << 5 << "" << 5 ), true, true));
        oil_b.intervals.push_back(Interval(fromjson( "{ '':7, '':Infinity }" ), true, true));
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
        oil_a.intervals.push_back(Interval(BSON( "" << 5 << "" << 5 ), true, true));
        oil_b.intervals.push_back(Interval(fromjson( "{ '':7, '':Infinity }" ), true, true));
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
        oil_a.intervals.push_back(Interval(BSON( "" << 5 << "" << 5 ), true, true));
        oil_b.intervals.push_back(Interval(fromjson( "{ '':7, '':Infinity }" ), true, true));
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
        oil_a.intervals.push_back(Interval(BSON( "" << 5 << "" << 5 ), true, true));
        oil_b.intervals.push_back(Interval(fromjson( "{ '':7, '':Infinity }" ), true, true));
        oil_c.intervals.push_back(IndexBoundsBuilder::allValues());
        oil_d.intervals.push_back(Interval(fromjson( "{ '':1, '':Infinity }" ), true, true));
        bounds.fields.push_back(oil_a);
        bounds.fields.push_back(oil_b);
        bounds.fields.push_back(oil_c);
        bounds.fields.push_back(oil_d);
        ASSERT(!testSingleInterval(bounds));
    }

    //
    // Complementing bounds for negations
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

    // Expected oil: [MinKey, 3), (3, MaxKey]
    TEST(IndexBoundsBuilderTest, SimpleNE) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = BSON("a" << BSON("$ne" << 3));
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 2U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(minKeyIntObj(3), true, false)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[1].compare(
            Interval(maxKeyIntObj(3), false, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, IntersectWithNE) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toIntersect;
        toIntersect.push_back(fromjson("{a: {$gt: 1}}"));
        toIntersect.push_back(fromjson("{a: {$ne: 2}}}"));
        toIntersect.push_back(fromjson("{a: {$lte: 6}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndIntersect(toIntersect, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 2U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(BSON("" << 1 << "" << 2), false, false)));
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[1].compare(
            Interval(BSON("" << 2 << "" << 6), false, true)));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, UnionizeWithNE) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        vector<BSONObj> toUnionize;
        toUnionize.push_back(fromjson("{a: {$ne: 3}}"));
        toUnionize.push_back(fromjson("{a: {$ne: 4}}}"));
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        testTranslateAndUnion(toUnionize, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            IndexBoundsBuilder::allValues()));
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
    }

    // Test $type bounds for Code BSON type.
    TEST(IndexBoundsBuilderTest, CodeTypeBounds) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$type: 13}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();

        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

        // Build the expected interval.
        BSONObjBuilder bob;
        bob.appendCode("", "");
        bob.appendCodeWScope("", "", BSONObj());
        BSONObj expectedInterval = bob.obj();

        // Check the output of translate().
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(expectedInterval, true, true)));
        ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    }

    // Test $type bounds for Code With Scoped BSON type.
    TEST(IndexBoundsBuilderTest, CodeWithScopeTypeBounds) {
        IndexEntry testIndex = IndexEntry(BSONObj());
        BSONObj obj = fromjson("{a: {$type: 15}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();

        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

        // Build the expected interval.
        BSONObjBuilder bob;
        bob.appendCodeWScope("", "", BSONObj());
        bob.appendMaxKey("");
        BSONObj expectedInterval = bob.obj();

        // Check the output of translate().
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(expectedInterval, true, true)));
        ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
    }

}  // namespace
