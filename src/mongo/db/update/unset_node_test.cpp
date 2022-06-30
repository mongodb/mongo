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

#include "mongo/db/update/unset_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using UnsetNodeTest = UpdateTestFixture;

DEATH_TEST_REGEX(UnsetNodeTest,
                 InitFailsForEmptyElement,
                 R"#(Invariant failure.*modExpr.ok\(\))#") {
    auto update = fromjson("{$unset: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    node.init(update["$unset"].embeddedObject().firstElement(), expCtx).transitional_ignore();
}

DEATH_TEST_REGEX_F(UnsetNodeTest,
                   ApplyToRootFails,
                   R"#(Invariant failure.*!updateNodeApplyParams.pathTaken.*empty\(\))#") {
    auto update = fromjson("{$unset: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
}

TEST(UnsetNodeTest, InitSucceedsForNonemptyElement) {
    auto update = fromjson("{$unset: {a: 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], expCtx));
}

/* This is a no-op because we are unsetting a field that does not exit. */
TEST_F(UnsetNodeTest, UnsetNoOp) {
    auto update = fromjson("{$unset: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{b: 5}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(UnsetNodeTest, UnsetNoOpDottedPath) {
    auto update = fromjson("{$unset: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathToCreate("b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(UnsetNodeTest, UnsetNoOpThroughArray) {
    auto update = fromjson("{$unset: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a:[{b:1}]}"));
    setPathToCreate("b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a:[{b:1}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(UnsetNodeTest, UnsetNoOpEmptyDoc) {
    auto update = fromjson("{$unset: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(UnsetNodeTest, UnsetTopLevelPath) {
    auto update = fromjson("{$unset: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(UnsetNodeTest, UnsetNestedPath) {
    auto update = fromjson("{$unset: {'a.b.c': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {c: 6}}}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b.c"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]["c"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: {}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {sb: {d: {c: false}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b.c}");
}

TEST_F(UnsetNodeTest, UnsetObject) {
    auto update = fromjson("{$unset: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {c: 6}}}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {d: {b: false}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(UnsetNodeTest, UnsetArrayElement) {
    auto update = fromjson("{$unset: {'a.0': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.0"], expCtx));

    mutablebson::Document doc(fromjson("{a:[1], b:1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.0"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"][0]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a:[null], b:1}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u0: null}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0}");
}

TEST_F(UnsetNodeTest, UnsetPositional) {
    auto update = fromjson("{$unset: {'a.$': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.$"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.1"));
    setMatchedField("1");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"][1]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, null, 2]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u1: null}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.1}");
}

TEST_F(UnsetNodeTest, UnsetEntireArray) {
    auto update = fromjson("{$unset: {'a': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(UnsetNodeTest, UnsetFromObjectInArray) {
    auto update = fromjson("{$unset: {'a.0.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.0.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 1}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.0.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"][0]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a:[{}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, s0: {d: {b: false}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0.b}");
}

TEST_F(UnsetNodeTest, CanUnsetInvalidField) {
    auto update = fromjson("{$unset: {'a.$.$b': true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.$.$b"], expCtx));

    mutablebson::Document doc(fromjson("{b: 1, a: [{$b: 1}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.0.$b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"][0]["$b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 1, a: [{}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, s0: {d: {$b: false}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.0.$b}");
}

TEST_F(UnsetNodeTest, ApplyNoIndexDataNoLogBuilder) {
    auto update = fromjson("{$unset: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(UnsetNodeTest, ApplyDoesNotAffectIndexes) {
    auto update = fromjson("{$unset: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("b");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(UnsetNodeTest, ApplyFieldWithDot) {
    auto update = fromjson("{$unset: {'a.b': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{'a.b':4, a: {b: 2}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{'a.b':4, a: {}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {d: {b: false}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(UnsetNodeTest, ApplyCannotRemoveRequiredPartOfDBRef) {
    auto update = fromjson("{$unset: {'a.$id': true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.$id"], expCtx));

    mutablebson::Document doc(fromjson("{a: {$ref: 'c', $id: 0}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.$id"));
    auto result = node.apply(getApplyParams(doc.root()["a"]["$id"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    auto updated = BSON("a" << BSON("$ref"
                                    << "c"));
    ASSERT_EQUALS(updated, doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {d: {$id: false}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.$id}");
}

TEST_F(UnsetNodeTest, ApplyCanRemoveRequiredPartOfDBRefIfValidateForStorageIsFalse) {
    auto update = fromjson("{$unset: {'a.$id': true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.$id"], expCtx));

    mutablebson::Document doc(fromjson("{a: {$ref: 'c', $id: 0}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.$id"));
    addIndexedPath("a");
    setValidateForStorage(false);
    auto result = node.apply(getApplyParams(doc.root()["a"]["$id"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    auto updated = BSON("a" << BSON("$ref"
                                    << "c"));
    ASSERT_EQUALS(updated, doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {d: {$id: false}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.$id}");
}

TEST_F(UnsetNodeTest, ApplyCannotRemoveImmutablePath) {
    auto update = fromjson("{$unset: {'a.b': true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 1}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a.b' would modify the immutable field 'a.b'");
}

TEST_F(UnsetNodeTest, ApplyCannotRemovePrefixOfImmutablePath) {
    auto update = fromjson("{$unset: {a: true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 1}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a' would modify the immutable field 'a.b'");
}

TEST_F(UnsetNodeTest, ApplyCannotRemoveSuffixOfImmutablePath) {
    auto update = fromjson("{$unset: {'a.b.c': true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {c: 1}}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b.c"));
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]["b"]["c"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a.b.c' would modify the immutable field 'a.b'");
}

TEST_F(UnsetNodeTest, ApplyCanRemoveImmutablePathIfNoop) {
    auto update = fromjson("{$unset: {'a.b.c': true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 1}}"));
    setPathToCreate("c");
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addImmutablePath("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 1}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a.b.c}");
}

}  // namespace
}  // namespace mongo
