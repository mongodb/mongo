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

#include "mongo/platform/basic.h"

#include "mongo/db/query/index_bounds_builder.h"

#include <limits>
#include <memory>

#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/expression_index.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

using std::unique_ptr;
using std::numeric_limits;
using std::string;
using std::vector;

double numberMin = -numeric_limits<double>::max();
double numberMax = numeric_limits<double>::max();
double negativeInfinity = -numeric_limits<double>::infinity();
double positiveInfinity = numeric_limits<double>::infinity();
double NaN = numeric_limits<double>::quiet_NaN();

/**
 * Utility function to create MatchExpression
 */
MatchExpression* parseMatchExpression(const BSONObj& obj) {
    const CollatorInterface* collator = nullptr;
    StatusWithMatchExpression status =
        MatchExpressionParser::parse(obj, ExtensionsCallbackDisallowExtensions(), collator);
    ASSERT_TRUE(status.isOK());
    MatchExpression* expr(status.getValue().release());
    return expr;
}

/**
 * Given a list of queries in 'toUnion', translate into index bounds and return
 * the union of these bounds in the out-parameter 'oilOut'.
 */
void testTranslateAndUnion(const vector<BSONObj>& toUnion,
                           OrderedIntervalList* oilOut,
                           IndexBoundsBuilder::BoundsTightness* tightnessOut) {
    IndexEntry testIndex = IndexEntry(BSONObj());

    for (vector<BSONObj>::const_iterator it = toUnion.begin(); it != toUnion.end(); ++it) {
        unique_ptr<MatchExpression> expr(parseMatchExpression(*it));
        BSONElement elt = it->firstElement();
        if (toUnion.begin() == it) {
            IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
        } else {
            IndexBoundsBuilder::translateAndUnion(expr.get(), elt, testIndex, oilOut, tightnessOut);
        }
    }
}

/**
 * Given a list of queries in 'toUnion', translate into index bounds and return
 * the intersection of these bounds in the out-parameter 'oilOut'.
 */
void testTranslateAndIntersect(const vector<BSONObj>& toIntersect,
                               OrderedIntervalList* oilOut,
                               IndexBoundsBuilder::BoundsTightness* tightnessOut) {
    IndexEntry testIndex = IndexEntry(BSONObj());

    for (vector<BSONObj>::const_iterator it = toIntersect.begin(); it != toIntersect.end(); ++it) {
        unique_ptr<MatchExpression> expr(parseMatchExpression(*it));
        BSONElement elt = it->firstElement();
        if (toIntersect.begin() == it) {
            IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
        } else {
            IndexBoundsBuilder::translateAndIntersect(
                expr.get(), elt, testIndex, oilOut, tightnessOut);
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
void testTranslate(const vector<std::pair<BSONObj, bool>>& constraints,
                   OrderedIntervalList* oilOut,
                   IndexBoundsBuilder::BoundsTightness* tightnessOut) {
    IndexEntry testIndex = IndexEntry(BSONObj());

    for (vector<std::pair<BSONObj, bool>>::const_iterator it = constraints.begin();
         it != constraints.end();
         ++it) {
        BSONObj obj = it->first;
        bool isIntersect = it->second;
        unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        if (constraints.begin() == it) {
            IndexBoundsBuilder::translate(expr.get(), elt, testIndex, oilOut, tightnessOut);
        } else if (isIntersect) {
            IndexBoundsBuilder::translateAndIntersect(
                expr.get(), elt, testIndex, oilOut, tightnessOut);
        } else {
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
    return IndexBoundsBuilder::isSingleInterval(bounds, &startKey, &startKeyIn, &endKey, &endKeyIn);
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
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 2, '': Infinity}"), false, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

//
// Comparison operators ($lte, $lt, $gt, $gte, $eq)
//

TEST(IndexBoundsBuilderTest, TranslateLteNumber) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$lte: 1}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteNumberMin) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = BSON("a" << BSON("$lte" << numberMin));
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << negativeInfinity << "" << numberMin), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteNegativeInfinity) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$lte: -Infinity}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': -Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtNumber) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$lt: 1}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtNumberMin) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = BSON("a" << BSON("$lt" << numberMin));
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << negativeInfinity << "" << numberMin), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtNegativeInfinity) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$lt: -Infinity}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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
    BSONObj obj = BSON("a" << LT << Date_t::fromMillisSinceEpoch(5000));
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(fromjson("{'': true, '': new Date(5000)}"), false, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtTimestamp) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = BSON("a" << GT << Timestamp(2, 3));
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  //  Constant below is ~0U, or 2**32 - 1, but cannot be written that way in JS
                  oil.intervals[0].compare(Interval(
                      fromjson("{'': Timestamp(2, 3), '': Timestamp(4294967295, 4294967295)}"),
                      false,
                      true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateGtNumber) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$gt: 1}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 1, '': Infinity}"), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtNumberMax) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = BSON("a" << BSON("$gt" << numberMax));
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << numberMax << "" << positiveInfinity), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtPositiveInfinity) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$gt: Infinity}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 1, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteNumberMax) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = BSON("a" << BSON("$gte" << numberMax));
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(
                      Interval(BSON("" << numberMax << "" << positiveInfinity), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtePositiveInfinity) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$gte: Infinity}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': Infinity, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtString) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$gt: 'abc'}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'abc', '': {}}"), false, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateEqualNan) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: NaN}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': NaN}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtNan) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$lt: NaN}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLteNan) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$lte: NaN}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': NaN}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtNan) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$gt: NaN}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 0U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteNan) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$gte: NaN}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': NaN, '': NaN}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateEqual) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = BSON("a" << 4);
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 4, '': 4}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateArrayEqualBasic) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: [1, 2, 3]}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': [1, 2, 3], '': [1, 2, 3]}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateIn) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$in: [8, 44, -1, -3]}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 4U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': -3, '': -3}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': -1, '': -1}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[2].compare(Interval(fromjson("{'': 8, '': 8}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[3].compare(Interval(fromjson("{'': 44, '': 44}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateInArray) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$in: [[1], 2]}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 3U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': 2, '': 2}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[2].compare(Interval(fromjson("{'': [1], '': [1]}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateLteBinData) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson(
        "{a: {$lte: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA',"
        "$type: '00'}}}");
    std::unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '', $type: '00'},"
                                    "'': {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}"),
                           true,
                           true)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLtBinData) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson(
        "{a: {$lt: {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA',"
        "$type: '00'}}}");
    std::unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '', $type: '00'},"
                                    "'': {$binary: 'AAAAAAAAAAAAAAAAAAAAAAAAAAAA', $type: '00'}}"),
                           true,
                           false)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGtBinData) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson(
        "{a: {$gt: {$binary: '////////////////////////////',"
        "$type: '00'}}}");
    std::unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '////////////////////////////', $type: '00'},"
                                    "'': ObjectId('000000000000000000000000')}"),
                           false,
                           false)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGteBinData) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson(
        "{a: {$gte: {$binary: '////////////////////////////',"
        "$type: '00'}}}");
    std::unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQ(oil.name, "a");
    ASSERT_EQ(oil.intervals.size(), 1U);
    ASSERT_EQ(Interval::INTERVAL_EQUALS,
              oil.intervals[0].compare(
                  Interval(fromjson("{'': {$binary: '////////////////////////////', $type: '00'},"
                                    "'': ObjectId('000000000000000000000000')}"),
                           true,
                           false)));
    ASSERT_EQ(tightness, IndexBoundsBuilder::EXACT);
}

//
// $type
//

TEST(IndexBoundsBuilderTest, TypeNumber) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$type: 'number'}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);

    // Build the expected interval.
    BSONObjBuilder bob;
    BSONType type = BSONType::NumberInt;
    bob.appendMinForType("", type);
    bob.appendMaxForType("", type);
    BSONObj expectedInterval = bob.obj();

    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(expectedInterval, true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

//
// $exists tests
//

TEST(IndexBoundsBuilderTest, ExistsTrue) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$exists: true}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': null, '': null}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, ExistsTrueSparse) {
    IndexEntry testIndex = IndexEntry(BSONObj(),
                                      false,  // multikey
                                      true,   // sparse
                                      false,  // unique
                                      "exists_true_sparse",
                                      nullptr,  // filterExpr
                                      BSONObj());
    BSONObj obj = fromjson("{a: {$exists: true}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 5}"), true, false)));
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
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': 5, '': 5}"), true, true)));
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
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, UnionTwoEmptyRanges) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    vector<std::pair<BSONObj, bool>> constraints;
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
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 1}"), true, false)));
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
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
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
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 0, '': 10}"), false, true)));
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
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 5, '': 5}"), true, true)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(fromjson("{'': 6, '': 6}"), true, true)));
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
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 1, '': 1}"), true, true)));
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
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 6, '': 13}"), true, true)));
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
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(BSON("" << NaN << "" << positiveInfinity), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

//
// Test simpleRegex
//

TEST(SimpleRegexTest, RootedLine) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^foo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedString) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedOptionalFirstChar) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^f?oo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedOptionalSecondChar) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^fz?oo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "f");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedMultiline) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^foo", "m", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedStringMultiline) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "m", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedCaseInsensitiveMulti) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "mi", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedComplex) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex(
        "\\Af \t\vo\n\ro  \\ \\# #comment", "mx", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo #");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedLiteral) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^\\Qasdf\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedLiteralWithExtra) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^\\Qasdf\\E.*", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedLiteralNoEnd) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^\\Qasdf", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedLiteralBackslash) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^\\Qasdf\\\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf\\");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedLiteralDotStar) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^\\Qas.*df\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "as.*df");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedLiteralNestedEscape) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^\\Qas\\Q[df\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "as\\Q[df");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(SimpleRegexTest, RootedLiteralNestedEscapeEnd) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qas\\E\\\\E\\Q$df\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "as\\E$df");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

// A regular expression with the "|" character is not considered simple. See SERVER-15235.
TEST(SimpleRegexTest, PipeCharacterDisallowed) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^(a(a|$)|b", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, PipeCharacterDisallowed2) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^(a(a|$)|^b", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

// SERVER-9035
TEST(SimpleRegexTest, RootedSingleLineMode) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^foo", "s", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

// SERVER-9035
TEST(SimpleRegexTest, NonRootedSingleLineMode) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("foo", "s", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

// SERVER-9035
TEST(SimpleRegexTest, RootedComplexSingleLineMode) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex(
        "\\Af \t\vo\n\ro  \\ \\# #comment", "msx", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo #");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(SimpleRegexTest, RootedRegexCantBeIndexedTightlyIfIndexHasCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    IndexBoundsBuilder::BoundsTightness tightness;
    string prefix = IndexBoundsBuilder::simpleRegex("^foo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

//
// Regex bounds
//

TEST(IndexBoundsBuilderTest, SimpleNonPrefixRegex) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: /foo/}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /foo/, '': /foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(IndexBoundsBuilderTest, NonSimpleRegexWithPipe) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: /^foo.*|bar/}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(
                      Interval(fromjson("{'': /^foo.*|bar/, '': /^foo.*|bar/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
}

TEST(IndexBoundsBuilderTest, SimpleRegexSingleLineMode) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: /^foo/s}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'foo', '': 'fop'}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/s, '': /^foo/s}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, SimplePrefixRegex) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: /^foo/}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'foo', '': 'fop'}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/, '': /^foo/}"), true, true)));
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
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minKeyIntObj(3), true, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(maxKeyIntObj(3), false, true)));
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
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(BSON("" << 1 << "" << 2), false, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(BSON("" << 2 << "" << 6), false, true)));
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
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(IndexBoundsBuilder::allValues()));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

// Test $type bounds for Code BSON type.
TEST(IndexBoundsBuilderTest, CodeTypeBounds) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$type: 13}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(expectedInterval, true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

// Test $type bounds for Code With Scoped BSON type.
TEST(IndexBoundsBuilderTest, CodeWithScopeTypeBounds) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$type: 15}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(expectedInterval, true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

// Test $type bounds for double BSON type.
TEST(IndexBoundsBuilderTest, DoubleTypeBounds) {
    IndexEntry testIndex = IndexEntry(BSONObj());
    BSONObj obj = fromjson("{a: {$type: 1}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Build the expected interval.
    BSONObjBuilder bob;
    bob.appendNumber("", NaN);
    bob.appendNumber("", positiveInfinity);
    BSONObj expectedInterval = bob.obj();

    // Check the output of translate().
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(expectedInterval, true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

//
// Collation-related tests.
//

TEST(IndexBoundsBuilderTest, TranslateEqualityToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = BSON("a"
                       << "foo");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateEqualityToNonStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = BSON("a" << 3);
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 3, '': 3}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateNotEqualToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = BSON("a" << BSON("$ne"
                                   << "bar"));
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    // Bounds should be [MinKey, "rab"), ("rab", MaxKey].
    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);

    {
        BSONObjBuilder bob;
        bob.appendMinKey("");
        bob.append("", "rab");
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                      oil.intervals[0].compare(Interval(bob.obj(), true, false)));
    }

    {
        BSONObjBuilder bob;
        bob.append("", "rab");
        bob.appendMaxKey("");
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                      oil.intervals[1].compare(Interval(bob.obj(), false, true)));
    }
}

TEST(IndexBoundsBuilderTest, TranslateEqualToStringElemMatchValueWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$elemMatch: {$eq: 'baz'}}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'zab', '': 'zab'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateLTEToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lte: 'foo'}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateLTEToNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lte: 3}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 3}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateLTStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lt: 'foo'}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': 'oof'}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateLTNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lt: 3}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': -Infinity, '': 3}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGTStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gt: 'foo'}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': {}}"), false, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateGTNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gt: 3}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 3, '': Infinity}"), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, TranslateGTEToStringWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gte: 'foo'}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': {}}"), true, false)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TranslateGTEToNumberWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gte: 3}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 3, '': Infinity}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, SimplePrefixRegexWithMockCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: /^foo/}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/, '': /^foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, NotWithMockCollatorIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$ne:  3}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(minKeyIntObj(3), true, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(Interval(maxKeyIntObj(3), false, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, ExistsTrueWithMockCollatorAndSparseIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;
    testIndex.sparse = true;

    BSONObj obj = fromjson("{a: {$exists: true}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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

TEST(IndexBoundsBuilderTest, ExistsFalseWithMockCollatorIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$exists: false}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': null, '': null}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, TypeStringIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$type: 'string'}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, InWithStringAndCollatorIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: ['foo']}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, InWithNumberAndStringAndCollatorIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: [2, 'foo']}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 2, '': 2}"), true, true)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': 'oof', '': 'oof'}"), true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, InWithRegexAndCollatorIsInexactFetch) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: [/^foo/]}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/, '': /^foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, InWithNumberAndCollatorIsExact) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: [2]}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
    BSONElement elt = obj.firstElement();

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': 2, '': 2}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
}

TEST(IndexBoundsBuilderTest, LTEMaxKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lte: {$maxKey: 1}}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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

TEST(IndexBoundsBuilderTest, LTMaxKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$lt: {$maxKey: 1}}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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

TEST(IndexBoundsBuilderTest, GTEMinKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gte: {$minKey: 1}}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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

TEST(IndexBoundsBuilderTest, GTMinKeyWithCollator) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(BSONObj());
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$gt: {$minKey: 1}}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));
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

TEST(IndexBoundsBuilderTest, StringEqualityAgainstHashedIndexWithCollatorUsesHashOfCollationKey) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: 'foo'}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    BSONObj expectedCollationKey = BSON(""
                                        << "oof");
    BSONObj expectedHash = ExpressionMapping::hash(expectedCollationKey.firstElement());
    BSONObjBuilder intervalBuilder;
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    BSONObj intervalObj = intervalBuilder.obj();

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(intervalObj, true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, EqualityToNumberAgainstHashedIndexWithCollatorUsesHash) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: 3}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    BSONObj expectedHash = ExpressionMapping::hash(obj.firstElement());
    BSONObjBuilder intervalBuilder;
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    BSONObj intervalObj = intervalBuilder.obj();

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(intervalObj, true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST(IndexBoundsBuilderTest, InWithStringAgainstHashedIndexWithCollatorUsesHashOfCollationKey) {
    BSONObj keyPattern = fromjson("{a: 'hashed'}");
    BSONElement elt = keyPattern.firstElement();
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    IndexEntry testIndex = IndexEntry(keyPattern);
    testIndex.collator = &collator;

    BSONObj obj = fromjson("{a: {$in: ['foo']}}");
    unique_ptr<MatchExpression> expr(parseMatchExpression(obj));

    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness);

    BSONObj expectedCollationKey = BSON(""
                                        << "oof");
    BSONObj expectedHash = ExpressionMapping::hash(expectedCollationKey.firstElement());
    BSONObjBuilder intervalBuilder;
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    intervalBuilder.append("", expectedHash.firstElement().numberLong());
    BSONObj intervalObj = intervalBuilder.obj();

    ASSERT_EQUALS(oil.name, "a");
    ASSERT_EQUALS(oil.intervals.size(), 1U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(intervalObj, true, true)));
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

}  // namespace
