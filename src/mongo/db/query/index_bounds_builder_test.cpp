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

    IndexEntry testIndex = IndexEntry(BSONObj());

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

    //
    // $elemMatch value
    // Example: {a: {$elemMatch: {$gt: 2}}}
    //

    TEST(IndexBoundsBuilderTest, TranslateElemMatchValue) {
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
    // Union tests
    //

    TEST(IndexBoundsBuilderTest, UnionTwoLt) {
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

}  // namespace
