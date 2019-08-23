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

#include "mongo/platform/basic.h"

#include "mongo/db/exec/find_projection_executor.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace projection_executor {
namespace positional_projection_tests {
auto applyPositional(const BSONObj& match,
                     const std::string& path,
                     const Document& input,
                     const Document& output = {}) {
    MutableDocument doc(output);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(match, expCtx));
    projection_executor::applyPositionalProjection(input, *matchExpr, path, &doc);
    return doc.freeze();
}

TEST(PositionalProjection, CorrectlyProjectsSimplePath) {
    ASSERT_DOCUMENT_EQ(Document{fromjson("{foo: [6]}")},
                       applyPositional(fromjson("{bar: 1, foo: {$gte: 5}}"),
                                       "foo",
                                       Document{fromjson("{bar: 1, foo: [1,2,6,10]}")}));
}

TEST(PositionalProjection, CorrectlyProjectsDottedPath) {
    ASSERT_DOCUMENT_EQ(Document{fromjson("{x: {y: [6]}}")},
                       applyPositional(fromjson("{a: 1, 'x.y': {$gte: 5}}"),
                                       "x.y",
                                       Document{fromjson("{a: 1, x: {y: [1,2,6,10]}}")}));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{x: {y: {z: [6]}}}")},
                       applyPositional(fromjson("{a: 1, 'x.y.z': {$gte: 5}}"),
                                       "x.y.z",
                                       Document{fromjson("{a: 1, x: {y: {z: [1,2,6,10]}}}")}));
}

TEST(PositionalProjection, ProjectsAsEmptyValueIfFieldIsNotArray) {
    ASSERT_DOCUMENT_EQ(
        {}, applyPositional(fromjson("{foo: 3}"), "foo", Document{fromjson("{foo: 3}")}));
}

TEST(PositionalProjection, FailsToProjectPositionalPathComponentsForNestedArrays) {
    ASSERT_THROWS_CODE(applyPositional(fromjson("{'x.0.y': 42}"),
                                       "x.0.y",
                                       Document{fromjson("{x: [{y: [11, 42]}]}")}),
                       AssertionException,
                       51247);
}

TEST(PositionalProjection, CorrectlyProjectsNestedArrays) {
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: [1,2]}]}")},
                       applyPositional(fromjson("{'a.b': 1}"),
                                       "a",
                                       Document{fromjson("{a: [{b: [1,2]}, {b: [3,4]}]}")}));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: [3,4]}]}")},
                       applyPositional(fromjson("{'a.b': 3}"),
                                       "a",
                                       Document{fromjson("{a: [{b: [1,2]}, {b: [3,4]}]}")}));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: [3,4]}]}")},
                       applyPositional(fromjson("{'a.b': 3}"),
                                       "a.b",
                                       Document{fromjson("{a: [{b: [1,2]}, {b: [3,4]}]}")}));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [['d','e','f']]}")},
                       applyPositional(fromjson("{a: {$gt: ['a','b','c']}}"),
                                       "a",
                                       Document{fromjson("{a: [['a','b','c'],['d','e','f']]}")}));
}

TEST(PositionalProjection, FailsToProjectWithMultipleConditionsOnArray) {
    ASSERT_THROWS_CODE(applyPositional(fromjson("{$or: [{'x.y': 1}, {'x.y': 2}]}"),
                                       "x",
                                       Document{fromjson("{x: [{y: [1,2]}]}")}),
                       AssertionException,
                       51246);
}

TEST(PositionalProjection, CanMergeWithExistingFieldsInOutputDocument) {
    auto doc = Document{fromjson("{foo: {bar: [1,2,6,10]}}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{foo: {bar: [6]}}")},
                       applyPositional(fromjson("{'foo.bar': {$gte: 5}}"), "foo.bar", doc, doc));

    doc = Document{fromjson("{bar: 1, foo: {bar: [1,2,6,10]}}")};
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{bar: 1, foo: {bar: [6]}}")},
        applyPositional(fromjson("{bar: 1, 'foo.bar': {$gte: 5}}"), "foo.bar", doc, doc));

    doc = Document{fromjson("{bar: 1, foo: 3}")};
    ASSERT_DOCUMENT_EQ(doc, applyPositional(fromjson("{foo: 3}"), "foo", doc, doc));
}
}  // namespace positional_projection_tests

namespace elem_match_projection_tests {
auto applyElemMatch(const BSONObj& match,
                    const std::string& path,
                    const Document& input,
                    const Document& output = {}) {
    MutableDocument doc(output);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto matchObj = BSON(path << BSON("$elemMatch" << match));
    auto matchExpr = uassertStatusOK(MatchExpressionParser::parse(matchObj, expCtx));
    projection_executor::applyElemMatchProjection(input, *matchExpr, path, &doc);
    return doc.freeze();
}

TEST(ElemMatchProjection, CorrectlyProjectsNonObjectElement) {
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{foo: [4]}")},
        applyElemMatch(fromjson("{$in: [4]}"), "foo", Document{fromjson("{foo: [1,2,3,4]}")}));
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{foo: [4]}")},
        applyElemMatch(fromjson("{$nin: [1,2,3]}"), "foo", Document{fromjson("{foo: [1,2,3,4]}")}));
}

TEST(ElemMatchProjection, CorrectlyProjectsObjectElement) {
    ASSERT_DOCUMENT_EQ(Document{fromjson("{foo: [{bar: 6, z: 6}]}")},
                       applyElemMatch(fromjson("{bar: {$gte: 5}}"),
                                      "foo",
                                      Document{fromjson("{foo: [{bar: 1, z: 1}, {bar: 2, z: 2}, "
                                                        "{bar: 6, z: 6}, {bar: 10, z: 10}]}")}));
}

TEST(ElemMatchProjection, CorrectlyProjectsArrayElement) {
    ASSERT_DOCUMENT_EQ(Document{fromjson("{foo: [[3,4]]}")},
                       applyElemMatch(fromjson("{$gt: [1,2]}"),
                                      "foo",
                                      Document{fromjson("{foo: [[1,2], [3,4]]}")}));
}

TEST(ElemMatchProjection, ProjectsAsEmptyDocumentIfInputIsEmpty) {
    ASSERT_DOCUMENT_EQ({}, applyElemMatch(fromjson("{bar: {$gte: 5}}"), "foo", {}));
}

TEST(ElemMatchProjection, RemovesFieldFromOutputDocumentIfUnableToMatchArrayElement) {
    ASSERT_DOCUMENT_EQ({},
                       applyElemMatch(fromjson("{bar: {$gte: 5}}"),
                                      "foo",
                                      Document{fromjson("{foo: [{bar: 1, z: 1}, "
                                                        "{bar: 2, z: 2}]}")}));
    auto doc =
        Document{fromjson("{bar: 1, foo: [{bar: 1, z: 1}, {bar: 2, z: 2}, "
                          "{bar: 6, z: 6}, {bar: 10, z: 10}]}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{bar:1}")},
                       applyElemMatch(fromjson("{bar: {$gte: 20}}"), "foo", doc, doc));
}

TEST(ElemMatchProjection, CorrectlyProjectsWithMultipleCriteriaInMatchExpression) {
    ASSERT_DOCUMENT_EQ(Document{fromjson("{foo: [{bar: 2, z: 2}]}")},
                       applyElemMatch(fromjson("{bar: {$gt: 1, $lt: 6}}"),
                                      "foo",
                                      Document{fromjson("{foo: [{bar: 1, z: 1}, {bar: 2, z: 2}, "
                                                        "{bar: 6, z: 6}, {bar: 10, z: 10}]}")}));
}

TEST(ElemMatchProjection, CanMergeWithExistingFieldsInOutputDocument) {
    auto doc =
        Document{fromjson("{foo: [{bar: 1, z: 1}, {bar: 2, z: 2}, "
                          "{bar: 6, z: 6}, {bar: 10, z: 10}]}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{foo: [{bar: 6, z: 6}]}")},
                       applyElemMatch(fromjson("{bar: {$gte: 5}}"), "foo", doc, doc));

    doc =
        Document{fromjson("{bar: 1, foo: [{bar: 1, z: 1}, {bar: 2, z: 2}, "
                          "{bar: 6, z: 6}, {bar: 10, z: 10}]}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{bar:1, foo: [{bar: 6, z: 6}]}")},
                       applyElemMatch(fromjson("{bar: {$gte: 5}}"), "foo", doc, doc));
}

TEST(ElemMatchProjection, RemovesFieldFromOutputDocumentIfItContainsNumericSubfield) {
    auto doc = Document{BSON("foo" << BSON(0 << 3))};
    ASSERT_DOCUMENT_EQ({}, applyElemMatch(fromjson("{$gt: 2}"), "foo", doc));

    doc = Document{BSON("bar" << 1 << "foo" << BSON(0 << 3))};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{bar: 1}")},
                       applyElemMatch(fromjson("{$gt: 2}"), "foo", doc, doc));
}
}  // namespace elem_match_projection_tests

namespace slice_projection_tests {
DEATH_TEST(SliceProjection,
           ShouldFailIfNegativeLimitSpecifiedWithPositiveSkip,
           "Invariant failure limit >= 0") {
    auto doc = Document{fromjson("{a: [1,2,3,4]}")};
    applySliceProjection(doc, "a", 1, -1);
}

DEATH_TEST(SliceProjection,
           ShouldFailIfNegativeLimitSpecifiedWithNegativeSkip,
           "Invariant failure limit >= 0") {
    auto doc = Document{fromjson("{a: [1,2,3,4]}")};
    applySliceProjection(doc, "a", -1, -1);
}

TEST(SliceProjection, CorrectlyProjectsSimplePath) {
    auto doc = Document{fromjson("{a: [1,2,3,4]}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [1,2,3]}")},
                       applySliceProjection(doc, "a", boost::none, 3));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [2,3,4]}")},
                       applySliceProjection(doc, "a", boost::none, -3));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [2]}")}, applySliceProjection(doc, "a", -3, 1));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [2,3,4]}")}, applySliceProjection(doc, "a", -3, 4));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [4]}")}, applySliceProjection(doc, "a", 3, 1));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [1,2,3,4]}")}, applySliceProjection(doc, "a", -5, 5));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: []}")}, applySliceProjection(doc, "a", 5, 2));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [1,2]}")}, applySliceProjection(doc, "a", -5, 2));
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: [1,2,3,4]}")},
        applySliceProjection(doc, "a", boost::none, std::numeric_limits<int>::max()));
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: [1,2,3,4]}")},
        applySliceProjection(doc, "a", boost::none, std::numeric_limits<int>::min()));
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: [1,2,3,4]}")},
        applySliceProjection(
            doc, "a", std::numeric_limits<int>::min(), std::numeric_limits<int>::max()));
    ASSERT_DOCUMENT_EQ(
        Document{fromjson("{a: []}")},
        applySliceProjection(
            doc, "a", std::numeric_limits<int>::max(), std::numeric_limits<int>::max()));

    doc = Document{fromjson("{a: [{b: 1, c: 1}, {b: 2, c: 2}, {b: 3, c: 3}], d: 2}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: 1, c: 1}], d: 2}")},
                       applySliceProjection(doc, "a", boost::none, 1));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: 3, c: 3}], d: 2}")},
                       applySliceProjection(doc, "a", boost::none, -1));
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: 2, c: 2}], d: 2}")},
                       applySliceProjection(doc, "a", 1, 1));

    doc = Document{fromjson("{a: 1}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: 1}")},
                       applySliceProjection(doc, "a", boost::none, 2));
}

TEST(SliceProjection, CorrectlyProjectsDottedPath) {
    auto doc = Document{fromjson("{a: {b: [1,2,3], c: 1}}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: {b: [1,2], c: 1}}")},
                       applySliceProjection(doc, "a.b", boost::none, 2));

    doc = Document{fromjson("{a: {b: [1,2,3], c: 1}, d: 1}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: {b: [1,2], c: 1}, d: 1}")},
                       applySliceProjection(doc, "a.b", boost::none, 2));

    doc = Document{fromjson("{a: {b: [[1,2], [3,4], [5,6]]}}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: {b: [[1,2], [3,4]]}}")},
                       applySliceProjection(doc, "a.b", boost::none, 2));

    doc = Document{fromjson("{a: [{b: {c: [1,2,3,4]}}, {b: {c: [5,6,7,8]}}], d: 1}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: {c: [4]}}, {b: {c: [8]}}], d: 1}")},
                       applySliceProjection(doc, "a.b.c", -1, 2));

    doc = Document{fromjson("{a: {b: 1}}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: {b: 1}}")},
                       applySliceProjection(doc, "a.b", boost::none, 2));

    doc = Document{fromjson("{a: [{b: [1,2,3], c: 1}]}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: [3], c: 1}]}")},
                       applySliceProjection(doc, "a.b", boost::none, -1));

    doc = Document{fromjson("{a: [{b: [1,2,3], c: 4}, {b: [5,6,7], c: 8}]}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: [3], c: 4}, {b: [7], c: 8}]}")},
                       applySliceProjection(doc, "a.b", boost::none, -1));

    doc = Document{fromjson("{a: [{b: [{x:1, c: [1, 2]}, {y: 1, c: [3, 4]}]}], z: 1}")};
    ASSERT_DOCUMENT_EQ(Document{fromjson("{a: [{b: [{x:1, c: [1]}, {y: 1, c: [3]}]}], z: 1}")},
                       applySliceProjection(doc, "a.b.c", boost::none, 1));
}
}  // namespace slice_projection_tests
}  // namespace projection_executor
}  // namespace mongo
