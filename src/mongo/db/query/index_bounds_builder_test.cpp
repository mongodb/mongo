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

    TEST(IndexBoundsBuilderTest, TranslateLteNumber) {
        BSONObj obj = fromjson("{a: {$lte: 1}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
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
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(BSON("" << negativeInfinity << "" << numberMin), true, true)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateLteNegativeInfinity) {
        BSONObj obj = fromjson("{a: {$lte: -Infinity}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': -Infinity, '': -Infinity}"), true, true)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateLtNumber) {
        BSONObj obj = fromjson("{a: {$lt: 1}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': -Infinity, '': 1}"), true, false)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateLtNumberMin) {
        BSONObj obj = BSON("a" << BSON("$lt" << numberMin));
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(BSON("" << negativeInfinity << "" << numberMin), true, false)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateLtNegativeInfinity) {
        BSONObj obj = fromjson("{a: {$lt: -Infinity}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 0U);
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGtNumber) {
        BSONObj obj = fromjson("{a: {$gt: 1}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 1, '': Infinity}"), false, true)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGtNumberMax) {
        BSONObj obj = BSON("a" << BSON("$gt" << numberMax));
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(BSON("" << numberMax << "" << positiveInfinity), false, true)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGtPositiveInfinity) {
        BSONObj obj = fromjson("{a: {$gt: Infinity}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 0U);
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGteNumber) {
        BSONObj obj = fromjson("{a: {$gte: 1}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': 1, '': Infinity}"), true, true)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGteNumberMax) {
        BSONObj obj = BSON("a" << BSON("$gte" << numberMax));
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(BSON("" << numberMax << "" << positiveInfinity), true, true)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

    TEST(IndexBoundsBuilderTest, TranslateGtePositiveInfinity) {
        BSONObj obj = fromjson("{a: {$gte: Infinity}}");
        auto_ptr<MatchExpression> expr(parseMatchExpression(obj));
        BSONElement elt = obj.firstElement();
        OrderedIntervalList oil;
        IndexBoundsBuilder::BoundsTightness tightness;
        IndexBoundsBuilder::translate(expr.get(), elt, &oil, &tightness);
        ASSERT_EQUALS(oil.name, "a");
        ASSERT_EQUALS(oil.intervals.size(), 1U);
        ASSERT_EQUALS(Interval::INTERVAL_EQUALS, oil.intervals[0].compare(
            Interval(fromjson("{'': Infinity, '': Infinity}"), true, true)));
        ASSERT(tightness == IndexBoundsBuilder::EXACT);
    }

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
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    }

    TEST(SimpleRegexTest, RootedOptionalSecondChar) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex("^fz?oo", "", &tightness);
        ASSERT_EQUALS(prefix, "f");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    }

    TEST(SimpleRegexTest, RootedMultiline) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex("^foo", "m", &tightness);
        ASSERT_EQUALS(prefix, "");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
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
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
    }

    TEST(SimpleRegexTest, RootedComplex) {
        IndexBoundsBuilder::BoundsTightness tightness;
        string prefix = IndexBoundsBuilder::simpleRegex(
                "\\Af \t\vo\n\ro  \\ \\# #comment", "mx", &tightness);
        ASSERT_EQUALS(prefix, "foo #");
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
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
        ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
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

}  // namespace
