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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/index_bounds_builder_test_fixture.h"
#include "mongo/db/query/compiler/optimizer/index_bounds_builder/interval_evaluation_tree.h"
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <vector>

namespace mongo {
namespace {

TEST_F(IndexBoundsBuilderTest, RootedLine) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^foo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, RootedString) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, RootedOptionalFirstChar) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^f?oo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, RootedOptionalSecondChar) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^fz?oo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "f");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, RootedMultiline) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^foo", "m", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, RootedStringMultiline) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "m", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, RootedCaseInsensitiveMulti) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("\\Afoo", "mi", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, RootedComplex) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(
        "\\Af \t\vo\n\ro  \\ \\# #comment", "mx", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo #");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, RootedLiteral) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^\\Qasdf\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, RootedLiteralNoEnd) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^\\Qasdf", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, RootedLiteralBackslash) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qasdf\\\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf\\");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, RootedLiteralDotStar) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qas.*df\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "as.*df");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, RootedLiteralNestedEscape) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qas\\Q[df\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "as\\Q[df");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, RootedLiteralNestedEscapeEnd) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qas\\E\\\\E\\Q$df\\E", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "as\\E$df");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

// An anchored regular expression that uses the "|" operator is not considered "simple" and has
// non-tight index bounds.
TEST_F(IndexBoundsBuilderTest, PipeCharacterUsesInexactBounds) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^(a(a|$)|b", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, PipeCharacterUsesInexactBoundsWithTwoPrefixes) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^(a(a|$)|^b", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, PipeCharacterPrecededByEscapedBackslashUsesInexactBounds) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(R"(^a\\|b)", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);

    prefix = IndexBoundsBuilder::simpleRegex(R"(^(foo\\|bar)\\|baz)", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

// However, a regular expression with an escaped pipe (that is, using no special meaning) can use
// exact index bounds.
TEST_F(IndexBoundsBuilderTest, PipeCharacterEscapedWithBackslashUsesExactBounds) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(R"(^a\|b)", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "a|b");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);

    prefix = IndexBoundsBuilder::simpleRegex(R"(^\|1\|2\|\|)", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "|1|2||");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, FalsePositiveOnPipeInQEEscapeSequenceUsesInexactBounds) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(R"(^\Q|\E)", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, FalsePositiveOnPipeInCharacterClassUsesInexactBounds) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(R"(^[|])", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

// SERVER-9035
TEST_F(IndexBoundsBuilderTest, RootedSingleLineMode) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^foo", "s", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

// SERVER-9035
TEST_F(IndexBoundsBuilderTest, NonRootedSingleLineMode) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("foo", "s", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

// SERVER-9035
TEST_F(IndexBoundsBuilderTest, RootedComplexSingleLineMode) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex(
        "\\Af \t\vo\n\ro  \\ \\# #comment", "msx", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "foo #");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

TEST_F(IndexBoundsBuilderTest, RootedRegexCantBeIndexedTightlyIfIndexHasCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    auto testIndex = buildSimpleIndexEntry();
    testIndex.collator = &collator;

    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^foo", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_FETCH);
}

TEST_F(IndexBoundsBuilderTest, SimpleNonPrefixRegex) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: /foo/}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /foo/, '': /foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, NonSimpleRegexWithPipe) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: /^foo.*|bar/}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[0].compare(Interval(fromjson("{'': '', '': {}}"), true, false)));
    ASSERT_EQUALS(Interval::INTERVAL_EQUALS,
                  oil.intervals[1].compare(
                      Interval(fromjson("{'': /^foo.*|bar/, '': /^foo.*|bar/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::INEXACT_COVERED);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, SimpleRegexSingleLineMode) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: /^foo/s}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'foo', '': 'fop'}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/s, '': /^foo/s}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

TEST_F(IndexBoundsBuilderTest, SimplePrefixRegex) {
    auto testIndex = buildSimpleIndexEntry();
    BSONObj obj = fromjson("{a: /^foo/}");
    auto [expr, inputParamIdMap] = parseMatchExpression(obj);
    BSONElement elt = obj.firstElement();
    OrderedIntervalList oil;
    IndexBoundsBuilder::BoundsTightness tightness;
    interval_evaluation_tree::Builder ietBuilder{};
    IndexBoundsBuilder::translate(expr.get(), elt, testIndex, &oil, &tightness, &ietBuilder);
    ASSERT_EQUALS(oil.intervals.size(), 2U);
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[0].compare(Interval(fromjson("{'': 'foo', '': 'fop'}"), true, false)));
    ASSERT_EQUALS(
        Interval::INTERVAL_EQUALS,
        oil.intervals[1].compare(Interval(fromjson("{'': /^foo/, '': /^foo/}"), true, true)));
    ASSERT(tightness == IndexBoundsBuilder::EXACT);
    assertIET(inputParamIdMap, ietBuilder, elt, testIndex, oil);
}

// Using exact index bounds for prefix regex in the form ^[].*
TEST_F(IndexBoundsBuilderTest, RootedLiteralWithExtra) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix =
        IndexBoundsBuilder::simpleRegex("^\\Qasdf\\E.*", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "asdf");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, PrefixRegex) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^abc.*", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "abc");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::EXACT);
}

TEST_F(IndexBoundsBuilderTest, RegexWithCharactersFollowingPrefix) {
    auto testIndex = buildSimpleIndexEntry();
    IndexBoundsBuilder::BoundsTightness tightness;
    std::string prefix = IndexBoundsBuilder::simpleRegex("^abc.*f", "", testIndex, &tightness);
    ASSERT_EQUALS(prefix, "abc");
    ASSERT_EQUALS(tightness, IndexBoundsBuilder::INEXACT_COVERED);
}

}  // namespace
}  // namespace mongo
