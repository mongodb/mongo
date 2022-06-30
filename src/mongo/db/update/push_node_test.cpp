/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/update/push_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using PushNodeTest = UpdateTestFixture;
using mongo::mutablebson::countChildren;
using mongo::mutablebson::Element;

TEST(PushNodeTest, EachClauseWithNonArrayObjectFails) {
    auto update = fromjson("{$push: {x: {$each: {'0': 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, EachClauseWithPrimitiveFails) {
    auto update = fromjson("{$push: {x: {$each: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, PositionClauseWithObjectFails) {
    auto update = fromjson("{$push: {x: {$each: [1, 2], $position: {a: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, PositionClauseWithNonIntegerFails) {
    auto update = fromjson("{$push: {x: {$each: [1, 2], $position: -2.1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, PositionClauseWithIntegerDoubleSucceeds) {
    auto update = fromjson("{$push: {x: {$each: [1, 2], $position: -2.0}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_OK(status);
}

TEST(PushNodeTest, SliceClauseWithObjectFails) {
    auto update = fromjson("{$push: {x: {$each: [1, 2], $slice: {a: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SliceClauseWithNonIntegerFails) {
    auto update = fromjson("{$push: {x: {$each: [1, 2], $slice: -2.1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SliceClauseWithIntegerDoubleSucceeds) {
    auto update = fromjson("{$push: {x: {$each: [1, 2], $slice: 2.0}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["x"], expCtx));
}

TEST(PushNodeTest, SliceClauseWithArrayFails) {
    auto update = fromjson("{$push: {x: {$each: [1, 2], $slice: [1, 2]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SliceClauseWithStringFails) {
    auto update = fromjson("{$push: {x: {$each: [1, 2], $slice: '-1'}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SortClauseWithArrayFails) {
    auto update = fromjson("{$push: {x: {$each: [{a: 1},{a: 2}], $slice: -2.0, $sort: [{a: 1}]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SortClauseWithInvalidSortPatternFails) {
    auto update = fromjson("{$push: {x: {$each: [{a: 1},{a: 2}], $slice: -2.0, $sort: {a: 100}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SortClauseWithEmptyPathFails) {
    auto update = fromjson("{$push: {x: {$each: [{a: 1},{a: 2}], $slice: -2.0, $sort: {'': 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SortClauseWithEmptyFieldNamesFails) {
    auto update = fromjson("{$push: {x: {$each: [{a: 1},{a: 2}], $slice: -2.0, $sort: {'.': 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SortClauseWithEmptyFieldSuffixFails) {
    auto update =
        fromjson("{$push: {x: {$each: [{a: 1},{a: 2}], $slice: -2.0, $sort: {'a.': 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SortClauseWithEmptyFieldPrefixFails) {
    auto update =
        fromjson("{$push: {x: {$each: [{a: 1},{a: 2}], $slice: -2.0, $sort: {'.b': 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SortClauseWithEmptyFieldInfixFails) {
    auto update =
        fromjson("{$push: {x: {$each: [{a: 1},{a: 2}], $slice: -2.0, $sort: {'a..b': 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, SortClauseWithEmptyObjectFails) {
    auto update = fromjson("{$push: {x: {$each: [{a: 1},{a: 2}], $slice: -2.0, $sort: {}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, PushEachWithInvalidClauseFails) {
    auto update = fromjson("{$push: {x: {$each: [{a: 1}, {a: 2}], $xxx: -1, $sort: {a: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, PushEachWithDuplicateSortClauseFails) {
    auto update = fromjson(
        "{$push: {x: {$each: [{a: 1},{a: 2}], $slice: -2.0, $sort: {a: 1}, $sort: {a: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, PushEachWithDuplicateSliceClauseFails) {
    auto update =
        fromjson("{$push: {x: {$each: [{a: 1},{a: 2}], $slice: -2.0, $slice: -2, $sort: {a: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, PushEachWithDuplicateEachClauseFails) {
    auto update =
        fromjson("{$push: {x: {$each:[{a: 1}], $each:[{a: 2}], $slice: -3, $sort: {a: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PushNodeTest, PushEachWithDuplicatePositionClauseFails) {
    auto update = fromjson("{$push: {x: {$each: [{a: 1}], $position: 1, $position: 2}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    auto status = node.init(update["$push"]["x"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST_F(PushNodeTest, ApplyToNonArrayFails) {
    auto update = fromjson("{$push: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 'test_object', a: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "The field 'a' must be an array but is of type int in document {_id: \"test_object\"}");
}

TEST_F(PushNodeTest, ApplyToEmptyArray) {
    auto update = fromjson("{$push: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToEmptyDocument) {
    auto update = fromjson("{$push: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: [1]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToArrayWithOneElement) {
    auto update = fromjson("{$push: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u1: 1}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToDottedPathElement) {
    auto update = fromjson("{$push: {'choices.first.votes': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["choices.first.votes"], expCtx));

    mutablebson::Document doc(
        fromjson("{_id : 1 , "
                 " question : 'a', "
                 " choices: {first: { choice: 'b'}, "
                 "           second: { choice: 'c'}}"
                 "}"));
    setPathToCreate("votes");
    setPathTaken(makeRuntimeUpdatePathForTest("choices.first"));
    addIndexedPath("a");
    auto result =
        node.apply(getApplyParams(doc.root()["choices"]["first"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 1, "
                           " question: 'a', "
                           " choices: {first: {choice: 'b', votes: [1]}, "
                           "           second: {choice: 'c'}}"
                           "}"),
                  doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {schoices: {sfirst: {i: {votes: [1]}}}}}"));
    ASSERT_EQUALS("{choices.first.votes}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplySimpleEachToEmptyArray) {
    auto update = fromjson("{$push: {a: {$each: [1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplySimpleEachToEmptyDocument) {
    auto update = fromjson("{$push: {a: {$each: [1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: [1]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyMultipleEachToEmptyDocument) {
    auto update = fromjson("{$push: {a: {$each: [1, 2]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1, 2]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: [1, 2]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplySimpleEachToArrayWithOneElement) {
    auto update = fromjson("{$push: {a: {$each: [1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u1: 1}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyMultipleEachToArrayWithOneElement) {
    auto update = fromjson("{$push: {a: {$each: [1, 2]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1, 2]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u1: 1, u2: 2}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyEmptyEachToEmptyArray) {
    auto update = fromjson("{$push: {a: {$each: []}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyEmptyEachToEmptyDocument) {
    auto update = fromjson("{$push: {a: {$each: []}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: []}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyEmptyEachToArrayWithOneElement) {
    auto update = fromjson("{$push: {a: {$each: []}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToArrayWithSlice) {
    auto update = fromjson("{$push: {a: {$each: [2, -1], $slice: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [3]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [3]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [3]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyWithNumericSort) {
    auto update = fromjson("{$push: {a: {$each: [2, -1], $sort: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [3]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [-1, 2, 3]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [-1, 2, 3]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyWithReverseNumericSort) {
    auto update = fromjson("{$push: {a: {$each: [4, -1], $sort: -1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [3]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [4, 3, -1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [4, 3, -1]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyWithMixedSort) {
    auto update = fromjson("{$push: {a: {$each: [4, -1], $sort: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [3, 't', {b: 1}, {a: 1}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [-1, 3, 4, 't', {a: 1}, {b: 1}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [-1, 3, 4, 't', {a: 1}, {b: 1}]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyWithReverseMixedSort) {
    auto update = fromjson("{$push: {a: {$each: [4, -1], $sort: -1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [3, 't', {b: 1}, {a: 1}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 1}, {a: 1}, 't', 4, 3, -1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [{b: 1}, {a: 1}, 't', 4, 3, -1]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyWithEmbeddedFieldSort) {
    auto update = fromjson("{$push: {a: {$each: [4, -1], $sort: {a: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [3, 't', {b: 1}, {a: 1}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [3, 't', {b: 1}, 4, -1, {a: 1}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [3, 't', {b: 1}, 4, -1, {a: 1}]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplySortWithCollator) {
    auto update = fromjson("{$push: {a: {$each: ['ha'], $sort: 1}}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: ['dd', 'fc', 'gb']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['ha', 'gb', 'fc', 'dd']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: ['ha', 'gb', 'fc', 'dd']}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplySortAfterSetCollator) {
    auto update = fromjson("{$push: {a: {$each: ['ha'], $sort: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: ['dd', 'fc', 'gb']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: ['dd', 'fc', 'gb', 'ha']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS("{a}", getModifiedPaths());

    // Now with a collator.
    resetApplyParams();
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    setLogBuilderToNull();
    CollatorInterfaceMock mockCollator(CollatorInterfaceMock::MockType::kReverseString);
    node.setCollator(&mockCollator);
    mutablebson::Document doc2(fromjson("{a: ['dd', 'fc', 'gb']}"));
    result = node.apply(getApplyParams(doc2.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: ['ha', 'gb', 'fc', 'dd']}"), doc2);
    ASSERT_FALSE(doc2.isInPlaceModeEnabled());
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

// Some of the below tests apply multiple different update modifiers. This special check function
// prints out the modifier when it observes a failure, which will help with diagnosis.
void checkDocumentAndResult(BSONObj updateModifier,
                            BSONObj expectedDocument,
                            const mutablebson::Document& actualDocument,
                            UpdateExecutor::ApplyResult applyResult) {
    if (expectedDocument == actualDocument && !applyResult.noop && !applyResult.indexesAffected) {
        // Check succeeded.
    } else {
        FAIL(str::stream() << "apply() failure for " << updateModifier << ". Expected "
                           << expectedDocument
                           << " (noop = false, indexesAffected = false) but got "
                           << actualDocument.toString() << " (noop = "
                           << (applyResult.noop ? "true" : "false") << ", indexesAffected = "
                           << (applyResult.indexesAffected ? "true" : "false") << ").");
    }
}

TEST_F(PushNodeTest, ApplyToEmptyArrayWithSliceValues) {
    struct testData {
        long long sliceValue;
        BSONObj resultingDoc;
    };

    // We repeat the same test for several different values of $slice.
    std::vector<testData> testDataList{{LLONG_MIN, fromjson("{a: [1]}")},
                                       {-2, fromjson("{a: [1]}")},
                                       {-1, fromjson("{a: [1]}")},
                                       {0, fromjson("{a: []}")},
                                       {1, fromjson("{a: [1]}")},
                                       {2, fromjson("{a: [1]}")},
                                       {LLONG_MAX, fromjson("{a: [1]}")}};

    for (const auto& data : testDataList) {
        auto update = BSON(
            "$push" << BSON("a" << BSON("$each" << BSON_ARRAY(1) << "$slice" << data.sliceValue)));
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        PushNode node;
        ASSERT_OK(node.init(update["$push"]["a"], expCtx));

        mutablebson::Document doc(fromjson("{a: []}"));
        resetApplyParams();
        setPathTaken(makeRuntimeUpdatePathForTest("a"));
        setLogBuilderToNull();
        auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
        checkDocumentAndResult(update, data.resultingDoc, doc, result);
    }
}

TEST_F(PushNodeTest, ApplyToPopulatedArrayWithSliceValues) {
    struct testData {
        int sliceValue;
        BSONObj resultingDoc;
    };

    // We repeat the same test with for several different values of $slice.
    std::vector<testData> testDataList{{-4, fromjson("{a: [2, 3, 1]}")},
                                       {-3, fromjson("{a: [2, 3, 1]}")},
                                       {-2, fromjson("{a: [3, 1]}")},
                                       {-1, fromjson("{a: [1]}")},
                                       {0, fromjson("{a: []}")},
                                       {1, fromjson("{a: [2]}")},
                                       {2, fromjson("{a: [2, 3]}")},
                                       {3, fromjson("{a: [2, 3, 1]}")},
                                       {4, fromjson("{a: [2, 3, 1]}")}};

    for (const auto& data : testDataList) {
        auto update = BSON(
            "$push" << BSON("a" << BSON("$each" << BSON_ARRAY(1) << "$slice" << data.sliceValue)));
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        PushNode node;
        ASSERT_OK(node.init(update["$push"]["a"], expCtx));

        mutablebson::Document doc(fromjson("{a: [2, 3]}"));
        resetApplyParams();
        setPathTaken(makeRuntimeUpdatePathForTest("a"));
        setLogBuilderToNull();
        auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
        checkDocumentAndResult(update, data.resultingDoc, doc, result);
    }
}

// In this test, we apply
//   {$push: {a: {$each: [{a: 2, b: 1}, {a: 1, b: 1}], $slice: N, $sort: {a: I, b: J}}}} AND
//   {$push: {a: {$each: [{a: 2, b: 1}, {a: 1, b: 1}], $slice: N, $sort: {b: J, a: I}}}}
// for every N in [-5, 5], I in {-1, 1}, and J in {-1, 1}.
// Each application is to the test document {a: [{a: 2, b: 3}, {a: 3, b: 1}]}.
TEST_F(PushNodeTest, ApplyToPopulatedArrayWithSortAndSliceValues) {
    struct testItem {
        int aVal;
        int bVal;
    };

    std::vector<testItem> testItemList{{2, 3}, {3, 1}, {2, 1}, {1, 1}};

    struct testData {
        int sliceValue;
        BSONObj sortOrder;
        BSONObj resultingDoc;
    };

    // These nested loops compute all of the modifiers we want to test, as well as what the result
    // of their applications should be.
    std::vector<testData> testDataList;
    for (auto sortAFirst : std::set<bool>{false, true}) {
        for (auto sortOrderA : std::set<int>{-1, 1}) {
            for (auto sortOrderB : std::set<int>{-1, 1}) {
                for (int sliceValue = -5; sliceValue <= 5; ++sliceValue) {
                    std::vector<testItem> resultingItems(testItemList);

                    std::sort(
                        resultingItems.begin(),
                        resultingItems.end(),
                        [sortAFirst, sortOrderA, sortOrderB](const auto& left, const auto& right) {
                            std::pair<int, int> leftPair(left.aVal, left.bVal);
                            std::pair<int, int> rightPair(right.aVal, right.bVal);

                            if (sortOrderA == -1) {
                                std::swap(leftPair.first, rightPair.first);
                            }
                            if (sortOrderB == -1) {
                                std::swap(leftPair.second, rightPair.second);
                            }
                            if (!sortAFirst) {
                                std::swap(leftPair.first, leftPair.second);
                                std::swap(rightPair.first, rightPair.second);
                            }

                            return leftPair < rightPair;
                        });

                    if (sliceValue >= 0 &&
                        static_cast<size_t>(sliceValue) < resultingItems.size()) {
                        resultingItems = std::vector<testItem>(resultingItems.begin(),
                                                               resultingItems.begin() + sliceValue);
                    } else if (sliceValue < 0 &&
                               static_cast<size_t>(-sliceValue) < resultingItems.size()) {
                        resultingItems = std::vector<testItem>(resultingItems.end() - (-sliceValue),
                                                               resultingItems.end());
                    }

                    testData newData;
                    newData.sliceValue = sliceValue;
                    if (sortAFirst) {
                        newData.sortOrder = BSON("a" << sortOrderA << "b" << sortOrderB);
                    } else {
                        newData.sortOrder = BSON("b" << sortOrderB << "a" << sortOrderA);
                    }

                    BSONArrayBuilder arrBuilder;
                    for (auto item : resultingItems) {
                        arrBuilder.append(BSON("a" << item.aVal << "b" << item.bVal));
                    }

                    newData.resultingDoc = BSON("a" << arrBuilder.arr());

                    testDataList.push_back(newData);
                }
            }
        }
    }

    // Once we've generated testDataList, we actually apply and verify all the modifiers.
    for (const auto& data : testDataList) {
        auto update =
            BSON("$push" << BSON("a" << BSON("$each" << BSON_ARRAY(BSON("a" << 2 << "b" << 1)
                                                                   << BSON("a" << 1 << "b" << 1))
                                                     << "$slice" << data.sliceValue << "$sort"
                                                     << data.sortOrder)));
        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        PushNode node;
        ASSERT_OK(node.init(update["$push"]["a"], expCtx));

        mutablebson::Document doc(fromjson("{a: [{a: 2, b: 3}, {a: 3, b: 1}]}"));
        resetApplyParams();
        setPathTaken(makeRuntimeUpdatePathForTest("a"));
        setLogBuilderToNull();
        auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
        checkDocumentAndResult(update, data.resultingDoc, doc, result);
    }
}

TEST_F(PushNodeTest, ApplyToEmptyArrayWithPositionZero) {
    auto update = fromjson("{$push: {a: {$each: [1], $position: 0}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToEmptyArrayWithPositionOne) {
    auto update = fromjson("{$push: {a: {$each: [1], $position: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToEmptyArrayWithLargePosition) {
    auto update = fromjson("{$push: {a: {$each: [1], $position: 1000}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToSingletonArrayWithPositionZero) {
    auto update = fromjson("{$push: {a: {$each: [1], $position: 0}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1, 0]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1, 0]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToSingletonArrayWithLargePosition) {
    auto update = fromjson("{$push: {a: {$each: [1], $position: 1000}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson(" {$v: 2, diff: {sa: {a: true, u1: 1}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToEmptyArrayWithNegativePosition) {
    auto update = fromjson("{$push: {a: {$each: [1], $position: -1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToSingletonArrayWithNegativePosition) {
    auto update = fromjson("{$push: {a: {$each: [1], $position: -1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1, 0]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1, 0]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToPopulatedArrayWithNegativePosition) {
    auto update = fromjson("{$push: {a: {$each: [5], $position: -2}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2, 3, 4]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1, 2, 5, 3, 4]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [0, 1, 2, 5, 3, 4]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyToPopulatedArrayWithOutOfBoundsNegativePosition) {
    auto update = fromjson("{$push: {a: {$each: [5], $position: -1000}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2, 3, 4]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [5, 0, 1, 2, 3, 4]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [5, 0, 1, 2, 3, 4]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, ApplyMultipleElementsPushWithNegativePosition) {
    auto update = fromjson("{$push: {a: {$each: [5, 6, 7], $position: -2}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2, 3, 4]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1, 2, 5, 6, 7, 3, 4]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [0, 1, 2, 5, 6, 7, 3, 4]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

TEST_F(PushNodeTest, PushWithMinIntAsPosition) {
    auto update =
        BSON("$push" << BSON("a" << BSON("$each" << BSON_ARRAY(5) << "$position"
                                                 << std::numeric_limits<long long>::min())));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PushNode node;
    ASSERT_OK(node.init(update["$push"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2, 3, 4]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [5, 0, 1, 2, 3, 4]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [5, 0, 1, 2, 3, 4]}}}"));
    ASSERT_EQUALS("{a}", getModifiedPaths());
}

}  // namespace
}  // namespace mongo
