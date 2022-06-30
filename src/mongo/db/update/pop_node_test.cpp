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

#include "mongo/db/update/pop_node.h"

#include "mongo/bson/json.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

namespace mmb = mongo::mutablebson;
using PopNodeTest = UpdateTestFixture;

TEST(PopNodeTest, InitSucceedsPositiveOne) {
    auto update = fromjson("{$pop: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a"], expCtx));
    ASSERT_FALSE(popNode.popFromFront());
}

TEST(PopNodeTest, InitSucceedsNegativeOne) {
    auto update = fromjson("{$pop: {a: -1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a"], expCtx));
    ASSERT_TRUE(popNode.popFromFront());
}

TEST(PopNodeTest, InitFailsOnePointOne) {
    auto update = fromjson("{$pop: {a: 1.1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], expCtx));
}

TEST(PopNodeTest, InitFailsZero) {
    auto update = fromjson("{$pop: {a: 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], expCtx));
}

TEST(PopNodeTest, InitFailsString) {
    auto update = fromjson("{$pop: {a: 'foo'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], expCtx));
}

TEST(PopNodeTest, InitFailsNestedObject) {
    auto update = fromjson("{$pop: {a: {b: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], expCtx));
}

TEST(PopNodeTest, InitFailsNestedArray) {
    auto update = fromjson("{$pop: {a: [{b: 1}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], expCtx));
}

TEST(PopNodeTest, InitFailsBool) {
    auto update = fromjson("{$pop: {a: true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], expCtx));
}

TEST_F(PopNodeTest, NoopWhenFirstPathComponentDoesNotExist) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));

    mmb::Document doc(fromjson("{b: [1, 2, 3]}"));
    setPathToCreate("a.b");
    addIndexedPath("a.b");
    auto result = popNode.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: [1, 2, 3]}"), doc);

    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a.b}", getModifiedPaths());
}

TEST_F(PopNodeTest, NoopWhenPathPartiallyExists) {
    auto update = fromjson("{$pop: {'a.b.c': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b.c"], expCtx));

    mmb::Document doc(fromjson("{a: {}}"));
    setPathToCreate("b.c");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a.b.c");
    auto result = popNode.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {}}"), doc);

    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a.b.c}", getModifiedPaths());
}

TEST_F(PopNodeTest, NoopWhenNumericalPathComponentExceedsArrayLength) {
    auto update = fromjson("{$pop: {'a.0': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.0"], expCtx));

    mmb::Document doc(fromjson("{a: []}"));
    setPathToCreate("0");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a.0");
    auto result = popNode.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc);

    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a.0}", getModifiedPaths());
}

TEST_F(PopNodeTest, ThrowsWhenPathIsBlockedByAScalar) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));

    mmb::Document doc(fromjson("{a: 'foo'}"));
    setPathToCreate("b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        popNode.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::PathNotViable,
        "Cannot use the part (b) of (a.b) to traverse the element ({a: \"foo\"})");
}

DEATH_TEST_REGEX_F(PopNodeTest,
                   NonOkElementWhenPathExistsIsFatal,
                   R"#(Invariant failure.*applyParams.element.ok\(\))#") {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b");
    popNode.apply(getApplyParams(doc.end()), getUpdateNodeApplyParams());
}

TEST_F(PopNodeTest, ThrowsWhenPathExistsButDoesNotContainAnArray) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));

    mmb::Document doc(fromjson("{a: {b: 'foo'}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::TypeMismatch,
        "Path 'a.b' contains an element of non-array type 'string'");
}

TEST_F(PopNodeTest, NoopWhenPathContainsAnEmptyArray) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));

    mmb::Document doc(fromjson("{a: {b: []}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b");
    auto result = popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: []}}"), doc);
    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a.b}", getModifiedPaths());
}

TEST_F(PopNodeTest, PopsSingleElementFromTheBack) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));
    ASSERT_FALSE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b");
    auto result = popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: []}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: []}}}}"));
    ASSERT_EQUALS("{a.b}", getModifiedPaths());
}

TEST_F(PopNodeTest, PopsSingleElementFromTheFront) {
    auto update = fromjson("{$pop: {'a.b': -1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));
    ASSERT_TRUE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [[1]]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a");
    auto result = popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: []}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: []}}}}"));
    ASSERT_EQUALS("{a.b}", getModifiedPaths());
}

TEST_F(PopNodeTest, PopsFromTheBackOfMultiElementArray) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));
    ASSERT_FALSE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b.c");
    auto result = popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: [1, 2]}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: [1, 2]}}}}"));
    ASSERT_EQUALS("{a.b}", getModifiedPaths());
}

TEST_F(PopNodeTest, PopsFromTheFrontOfMultiElementArray) {
    auto update = fromjson("{$pop: {'a.b': -1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));
    ASSERT_TRUE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b");
    auto result = popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: [2, 3]}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: [2, 3]}}}}"));
    ASSERT_EQUALS("{a.b}", getModifiedPaths());
}

TEST_F(PopNodeTest, PopsFromTheFrontOfMultiElementArrayWithoutAffectingIndexes) {
    auto update = fromjson("{$pop: {'a.b': -1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));
    ASSERT_TRUE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("unrelated.path");
    auto result = popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: [2, 3]}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: [2, 3]}}}}"));
    ASSERT_EQUALS("{a.b}", getModifiedPaths());
}

TEST_F(PopNodeTest, SucceedsWithNullUpdateIndexData) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));
    ASSERT_FALSE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    auto result = popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: [1, 2]}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: [1, 2]}}}}"));
    ASSERT_EQUALS("{a.b}", getModifiedPaths());
}

TEST_F(PopNodeTest, SucceedsWithNullLogBuilder) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));
    ASSERT_FALSE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b.c");
    setLogBuilderToNull();
    auto result = popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: [1, 2]}}"), doc);
    ASSERT_EQUALS("{a.b}", getModifiedPaths());
}

TEST_F(PopNodeTest, ThrowsWhenPathIsImmutable) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));

    mmb::Document doc(fromjson("{a: {b: [0]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addImmutablePath("a.b");
    addIndexedPath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a.b' would modify the immutable field 'a.b'");
}

TEST_F(PopNodeTest, ThrowsWhenPathIsPrefixOfImmutable) {

    // This is only possible for an upsert, since it is not legal to have an array in an immutable
    // path. If this update did not fail, we would fail later for storing an immutable path with an
    // array in it.

    auto update = fromjson("{$pop: {'a': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a"], expCtx));

    mmb::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addImmutablePath("a.0");
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        popNode.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a' would modify the immutable field 'a.0'");
}

TEST_F(PopNodeTest, ThrowsWhenPathIsSuffixOfImmutable) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));

    mmb::Document doc(fromjson("{a: {b: [0]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addImmutablePath("a");
    addIndexedPath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a.b' would modify the immutable field 'a'");
}

TEST_F(PopNodeTest, NoopOnImmutablePathSucceeds) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], expCtx));

    mmb::Document doc(fromjson("{a: {b: []}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addImmutablePath("a.b");
    addIndexedPath("a.b");
    auto result = popNode.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: []}}"), doc);

    assertOplogEntryIsNoop();
    ASSERT_EQUALS("{a.b}", getModifiedPaths());
}

}  // namespace
}  // namespace mongo
