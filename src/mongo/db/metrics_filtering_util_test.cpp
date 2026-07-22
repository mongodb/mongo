// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/metrics_filtering_util.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/metrics_filtering_util_test_helpers.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using namespace metrics_filtering_util;

class MetricsFilteringUtilBuildPathMatcherTest : public unittest::Test {};

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, EmptyPathStringRejected) {
    std::vector<std::string> paths = {""};
    ASSERT_THROWS_CODE(buildPathMatcher(paths), DBException, ErrorCodes::BadValue);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, StartswithDot) {
    std::vector<std::string> paths = {".a"};
    ASSERT_THROWS_CODE(buildPathMatcher(paths), DBException, ErrorCodes::BadValue);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, ConsecutiveDotsInPath) {
    std::vector<std::string> paths = {"a..b"};
    ASSERT_THROWS_CODE(buildPathMatcher(paths), DBException, ErrorCodes::BadValue);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, TrailingDotInPath) {
    std::vector<std::string> paths = {"a.b."};
    ASSERT_THROWS_CODE(buildPathMatcher(paths), DBException, ErrorCodes::BadValue);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, WildcardNotSupported) {
    std::vector<std::string> paths = {"*"};
    ASSERT_THROWS_CODE(buildPathMatcher(paths), DBException, ErrorCodes::BadValue);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, WildcardInSegmentNotSupported) {
    std::vector<std::string> paths = {"a.*"};
    ASSERT_THROWS_CODE(buildPathMatcher(paths), DBException, ErrorCodes::BadValue);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, WildcardInMiddleNotSupported) {
    std::vector<std::string> paths = {"a.*.b"};
    ASSERT_THROWS_CODE(buildPathMatcher(paths), DBException, ErrorCodes::BadValue);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, SingleSegmentPath) {
    std::vector<std::string> paths = {"a"};
    auto matcher = buildPathMatcher(paths);

    ASSERT_TRUE(pathIsExactMatch(matcher, "a"));
    ASSERT_EQ(countExactMatches(matcher), 1);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, MultiSegmentPaths) {
    std::vector<std::string> paths = {"a.b.c"};
    auto matcher = buildPathMatcher(paths);

    ASSERT_TRUE(pathIsExactMatch(matcher, "a.b.c"));
    ASSERT_FALSE(pathIsExactMatch(matcher, "a"));
    ASSERT_FALSE(pathIsExactMatch(matcher, "a.b"));
    ASSERT_EQ(countExactMatches(matcher), 1);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, SharedPrefix) {
    std::vector<std::string> paths = {"a.b", "a.c"};
    auto matcher = buildPathMatcher(paths);

    ASSERT_TRUE(pathIsExactMatch(matcher, "a.b"));
    ASSERT_TRUE(pathIsExactMatch(matcher, "a.c"));
    ASSERT_FALSE(pathIsExactMatch(matcher, "a"));
    ASSERT_EQ(countExactMatches(matcher), 2);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, NoSharedPrefix) {
    std::vector<std::string> paths = {"a", "b"};
    auto matcher = buildPathMatcher(paths);

    ASSERT_TRUE(pathIsExactMatch(matcher, "a"));
    ASSERT_TRUE(pathIsExactMatch(matcher, "b"));
    ASSERT_EQ(countExactMatches(matcher), 2);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, HierarchicalPaths) {
    std::vector<std::string> paths = {"a", "a.b", "a.b.c"};
    auto matcher = buildPathMatcher(paths);

    ASSERT_TRUE(pathIsExactMatch(matcher, "a"));
    ASSERT_TRUE(pathIsExactMatch(matcher, "a.b"));
    ASSERT_TRUE(pathIsExactMatch(matcher, "a.b.c"));
    ASSERT_EQ(countExactMatches(matcher), 3);
}

TEST_F(MetricsFilteringUtilBuildPathMatcherTest, Duplicates) {
    std::vector<std::string> paths = {"a", "a", "b"};
    auto matcher = buildPathMatcher(paths);

    ASSERT_TRUE(pathIsExactMatch(matcher, "a"));
    ASSERT_TRUE(pathIsExactMatch(matcher, "b"));
    ASSERT_EQ(countExactMatches(matcher), 2);
}

class MetricsFilteringUtilAppendPathsTest : public unittest::Test {};

TEST_F(MetricsFilteringUtilAppendPathsTest, UndottedSingle) {
    BSONObj obj = BSON("a" << 1 << "b" << 2 << "c" << 3);
    std::vector<std::string> paths = {"a"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << 1);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, UndottedMultiple) {
    BSONObj obj = BSON("a" << 1 << "b" << 2 << "c" << 3);
    std::vector<std::string> paths = {"a", "c"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << 1 << "c" << 3);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, UndottedObjectIncludesAllNested) {
    BSONObj obj = BSON("a" << BSON("x" << 100 << "y" << 200 << "z" << 300) << "b" << 1);
    std::vector<std::string> paths = {"a"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << BSON("x" << 100 << "y" << 200 << "z" << 300));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, UndottedArray) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << 2 << 3) << "b" << 5);
    std::vector<std::string> paths = {"a"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << BSON_ARRAY(1 << 2 << 3));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, ExactFieldNameMatchingUndotted) {
    BSONObj obj = BSON("a" << 1 << "ab" << 2 << "abc" << 3);
    std::vector<std::string> paths = {"a", "abcde"};  // "a" matches, "abcde" doesn't exist.
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << 1);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, ExactFieldNameMatchingDotted) {
    BSONObj obj = BSON("a" << BSON("b" << 1 << "bc" << 2 << "bcd" << 3));
    std::vector<std::string> paths = {"a.b", "a.bcde"};  // "a.b" matches, "a.bcde" doesn't exist.
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << BSON("b" << 1));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, DottedSingle) {
    BSONObj obj = BSON("a" << BSON("x" << 100 << "y" << 200));
    std::vector<std::string> paths = {"a.x"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << BSON("x" << 100));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, DottedMultiple) {
    BSONObj obj = BSON("a" << BSON("x" << 100 << "y" << 200) << "b" << BSON("z" << 5));
    std::vector<std::string> paths = {"a.x", "a.y"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << BSON("x" << 100 << "y" << 200));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, DottedArray) {
    BSONObj obj = BSON("a" << BSON("b" << BSON_ARRAY(1 << 2 << 3)));
    std::vector<std::string> paths = {"a.b"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << BSON("b" << BSON_ARRAY(1 << 2 << 3)));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, DottedDeep) {
    BSONObj obj = BSON("a" << BSON("x" << BSON("i" << BSON("A" << 42))));
    std::vector<std::string> paths = {"a.x.i.A"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << BSON("x" << BSON("i" << BSON("A" << 42))));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, DottedComplex) {
    BSONObj obj = BSON("a" << BSON("x" << 100 << "y" << 200 << "z" << 300) << "b"
                           << BSON("i" << BSON("ii" << BSON("x" << 10 << "y" << 5))) << "c"
                           << BSON("x" << 1000 << "y" << 500));

    std::vector<std::string> paths = {"a.x", "a.y", "b.i.ii.x", "c.x"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected =
        BSON("a" << BSON("x" << 100 << "y" << 200) << "b"
                 << BSON("i" << BSON("ii" << BSON("x" << 10))) << "c" << BSON("x" << 1000));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, OverlappingPathPrefixes) {
    BSONObj obj = BSON("a" << BSON("x" << 100 << "y" << 200) << "b" << BSON("z" << 5));
    std::vector<std::string> paths = {"a", "a.x"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    // When "a" is in the allowlist, the entire object "a" is included, so "a.x" is redundant.
    BSONObj expected = BSON("a" << BSON("x" << 100 << "y" << 200));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, NonexistentPath) {
    BSONObj obj = BSON("a" << 1 << "b" << 2);
    std::vector<std::string> paths = {"z", "w.x"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    ASSERT(actual.isEmpty());
}

TEST_F(MetricsFilteringUtilAppendPathsTest, PartialPathThroughNonObject) {
    BSONObj obj = BSON("a" << 1 << "b" << 2);
    std::vector<std::string> paths = {"a.x"};  // a is not an object.
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    ASSERT(actual.isEmpty());
}

TEST_F(MetricsFilteringUtilAppendPathsTest, ArrayInMiddleOfPath) {
    BSONObj obj = BSON("a" << BSON_ARRAY(1 << 2 << 3));
    std::vector<std::string> paths = {"a.x"};  // a is an array, not traversable as an object.
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    ASSERT(actual.isEmpty());
}

TEST_F(MetricsFilteringUtilAppendPathsTest, NullValueInPath) {
    BSONObj obj = BSON("a" << BSONNULL << "b" << 2);
    std::vector<std::string> paths = {"a.x"};  // a is null, not traversable.
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    ASSERT(actual.isEmpty());
}

TEST_F(MetricsFilteringUtilAppendPathsTest, MixedExistentAndNonexistent) {
    BSONObj obj = BSON("a" << BSON("x" << 1) << "b" << BSON("y" << 2));
    // Test mix of existing paths ("a.x", "b.y") and missing paths:
    // - "a.w": intermediate level exists but field doesn't.
    // - "c.w": intermediate level doesn't exist.
    std::vector<std::string> paths = {"a.x", "a.w", "c.w", "b.y"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << BSON("x" << 1) << "b" << BSON("y" << 2));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, IntermediateObjectWithNoMatchingChildren) {
    // When an allowlist path refers to a child that doesn't exist in an intermediate object,
    // the entire branch is skipped.
    BSONObj obj = BSON("a" << BSON("b" << BSON("y" << 5)));
    std::vector<std::string> paths = {"a.b.x"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    ASSERT(actual.isEmpty());
}

TEST_F(MetricsFilteringUtilAppendPathsTest, SpecialCharactersInFieldNames) {
    BSONObj obj = BSON("field-with-dashes" << BSON("sub_field" << 123));
    std::vector<std::string> paths = {"field-with-dashes.sub_field"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj field = actual.getObjectField("field-with-dashes");
    ASSERT_EQ(field.getIntField("sub_field"), 123);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, ScalarTypeExtraction) {
    BSONObj obj = BSON("str" << "hello"
                             << "double" << 3.14 << "bool" << true << "int" << 42);
    std::vector<std::string> paths = {"str", "double", "bool", "int"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("str" << "hello"
                                  << "double" << 3.14 << "bool" << true << "int" << 42);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, NumericFieldNames) {
    BSONObj obj = BSON("a1" << BSON("2" << 100));
    std::vector<std::string> paths = {"a1.2"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a1" << BSON("2" << 100));
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, DotInFieldNameDoesNotMatchAcrossSegments) {
    // A literal '.' inside a single field name is indistinguishable from a path separator.
    // Matching is defined per actual nesting level, so a field named "dot.in.name" is not
    // reachable via a dotted allowlist path that spells it out segment by segment.
    BSONObj obj = BSON("a" << BSON("dot.in.name" << 123));
    std::vector<std::string> paths = {"a.dot.in.name"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    ASSERT(actual.getObjectField("a").isEmpty());
}

TEST_F(MetricsFilteringUtilAppendPathsTest, EmptyPathsList) {
    BSONObj obj = BSON("a" << 1 << "b" << 2);
    std::vector<std::string> paths = {};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    ASSERT(actual.isEmpty());
}

TEST_F(MetricsFilteringUtilAppendPathsTest, EmptySourceObject) {
    BSONObj obj = BSONObj();
    std::vector<std::string> paths = {"a", "b.x"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    ASSERT(actual.isEmpty());
}

TEST_F(MetricsFilteringUtilAppendPathsTest, DuplicatePaths) {
    BSONObj obj = BSON("a" << 1 << "b" << 2);
    std::vector<std::string> paths = {"a", "a", "a"};
    BSONObjBuilder builder;
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    ASSERT_EQ(actual.getField("a").numberInt(), 1);
    ASSERT(actual.getField("b").eoo());
}

TEST_F(MetricsFilteringUtilAppendPathsTest, PreservesFieldsAlreadyInBuilder) {
    // Fields already appended to the builder before extraction must be retained alongside the
    // extracted fields.
    BSONObj obj = BSON("a" << 1 << "b" << 2);
    std::vector<std::string> paths = {"a"};
    BSONObjBuilder builder;
    builder.append("preexisting", 99);
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("preexisting" << 99 << "a" << 1);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

TEST_F(MetricsFilteringUtilAppendPathsTest, ExtractedFieldConflictsWithExistingBuilderField) {
    // The extractor blindly appends. It does not de-duplicate against fields already in the
    // builder. When an extracted field name collides with a pre-existing one, both are present in
    // the result (BSON permits duplicate field names) and the pre-existing field is ordered first.
    BSONObj obj = BSON("a" << 1 << "b" << 2);
    std::vector<std::string> paths = {"a"};
    BSONObjBuilder builder;
    builder.append("a", 99);
    auto matcher = buildPathMatcher(paths);
    appendPaths(builder, obj, matcher);
    BSONObj actual = builder.obj();

    BSONObj expected = BSON("a" << 99 << "a" << 1);
    ASSERT_BSONOBJ_EQ(actual, expected);
}

}  // namespace
}  // namespace mongo
