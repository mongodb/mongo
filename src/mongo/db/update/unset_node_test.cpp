// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/unset_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <string>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using UnsetNodeTest = UpdateTestFixture;
using UnsetNodeDeathTest = UnsetNodeTest;

DEATH_TEST_REGEX(SimpleUnsetNodeDeathTest,
                 InitFailsForEmptyElement,
                 R"#(Invariant failure.*modExpr.ok\(\))#") {
    auto update = fromjson("{$unset: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    node.init(update["$unset"].embeddedObject().firstElement(), expCtx).transitional_ignore();
}

DEATH_TEST_REGEX_F(UnsetNodeDeathTest,
                   ApplyToRootFails,
                   R"#(Invariant failure.*!updateNodeApplyParams.pathTaken.*empty\(\))#") {
    auto update = fromjson("{$unset: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
}

TEST(SimpleUnsetNodeTest, InitSucceedsForNonemptyElement) {
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
    ASSERT_FALSE(getIndexAffectedFromLogEntry());
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
    ASSERT_FALSE(getIndexAffectedFromLogEntry());
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
    ASSERT_FALSE(getIndexAffectedFromLogEntry());
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
    ASSERT_FALSE(getIndexAffectedFromLogEntry());
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
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
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

    mutablebson::Document doc(fromjson("{a: {b: {c: 6}}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b.c"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]["c"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
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

    mutablebson::Document doc(fromjson("{a: {b: {c: 6}}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
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
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
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
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
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
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
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
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
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
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
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
    ASSERT_FALSE(getIndexAffectedFromLogEntry());
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
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
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
    ASSERT_FALSE(getIndexAffectedFromLogEntry());
    auto updated = BSON("a" << BSON("$ref" << "c"));
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
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
    auto updated = BSON("a" << BSON("$ref" << "c"));
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
    ASSERT_FALSE(getIndexAffectedFromLogEntry());
    ASSERT_EQUALS(fromjson("{a: {b: 1}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a.b.c}");
}

}  // namespace
}  // namespace mongo
