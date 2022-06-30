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

#include "mongo/db/update/rename_node.h"

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

using RenameNodeTest = UpdateTestFixture;
using mongo::mutablebson::countChildren;
using mongo::mutablebson::Element;

TEST(RenameNodeTest, PositionalNotAllowedInFromField) {
    auto update = fromjson("{$rename: {'a.$': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    Status status = node.init(update["$rename"]["a.$"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, PositionalNotAllowedInToField) {
    auto update = fromjson("{$rename: {'a': 'b.$'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    Status status = node.init(update["$rename"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, ArrayFilterNotAllowedInFromField) {
    auto update = fromjson("{$rename: {'a.$[i]': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    Status status = node.init(update["$rename"]["a.$[i]"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, ArrayFilterNotAllowedInToField) {
    auto update = fromjson("{$rename: {'a': 'b.$[i]'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    Status status = node.init(update["$rename"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}


TEST(RenameNodeTest, MoveUpNotAllowed) {
    auto update = fromjson("{$rename: {'b.a': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    Status status = node.init(update["$rename"]["b.a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, MoveDownNotAllowed) {
    auto update = fromjson("{$rename: {'b': 'b.a'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    Status status = node.init(update["$rename"]["b"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, MoveToSelfNotAllowed) {
    auto update = fromjson("{$rename: {'b.a': 'b.a'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    Status status = node.init(update["$rename"]["b.a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST_F(RenameNodeTest, SimpleNumberAtRoot) {
    auto update = fromjson("{$rename: {'a': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 2}"));
    setPathToCreate("b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}, i: {b: 2}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b}");
}

TEST_F(RenameNodeTest, ToExistsAtSameLevel) {
    auto update = fromjson("{$rename: {'a': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 2, b: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}, u: {b: 2}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b}");
}

TEST_F(RenameNodeTest, ToAndFromHaveSameValue) {
    auto update = fromjson("{$rename: {'a': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 2, b: 2}"));
    setPathTaken(makeRuntimeUpdatePathForTest("b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}, u: {b: 2}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b}");
}

TEST_F(RenameNodeTest, RenameToFieldWithSameValueButDifferentType) {
    auto update = fromjson("{$rename: {a: 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1, b: NumberLong(1)}"));
    setPathTaken(makeRuntimeUpdatePathForTest("b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 1}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}, u: {b: 1}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b}");
}

TEST_F(RenameNodeTest, FromDottedElement) {
    auto update = fromjson("{$rename: {'a.c': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {c: {d: 6}}, b: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {}, b: {d: 6}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {b: {d: 6}}, sa: {d: {c: false}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.c, b}");
}

TEST_F(RenameNodeTest, RenameToExistingNestedFieldDoesNotReorderFields) {
    auto update = fromjson("{$rename: {'c.d': 'a.b.c'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["c.d"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {c: 1, d: 2}}, b: 3, c: {d: 4}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b.c"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]["c"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: {c: 4, d: 2}}, b: 3, c: {}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {sb: {u: {c: 4}}}, sc: {d: {d: false}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b.c, c.d}");
}

TEST_F(RenameNodeTest, MissingCompleteTo) {
    auto update = fromjson("{$rename: {a: 'c.r.d'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 2, b: 1, c: {}}"));
    setPathToCreate("r.d");
    setPathTaken(makeRuntimeUpdatePathForTest("c"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["c"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 1, c: {r: {d: 2}}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}, sc: {i: {r: {d: 2}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, c.r.d}");
}

TEST_F(RenameNodeTest, ToIsCompletelyMissing) {
    auto update = fromjson("{$rename: {a: 'b.c.d'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 2}"));
    setPathToCreate("b.c.d");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: {c: {d: 2}}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}, i: {b: {c: {d: 2}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b.c.d}");
}

TEST_F(RenameNodeTest, ToMissingDottedField) {
    auto update = fromjson("{$rename: {a: 'b.c.d'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{a:2, b:1}]}"));
    setPathToCreate("b.c.d");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: {c: {d: [{a:2, b:1}]}}}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}, i: {b: {c: {d: [{a: 2, b: 1}]}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b.c.d}");
}

TEST_F(RenameNodeTest, MoveIntoArray) {
    auto update = fromjson("{$rename: {b: 'a.2'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["b"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 'test_object', a: [1, 2], b: 2}"));
    setPathToCreate("2");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "The destination field cannot be an array element, 'a.2' in doc "
        "with _id: \"test_object\" has an array field called 'a'");
}

TEST_F(RenameNodeTest, MoveIntoArrayNoId) {
    auto update = fromjson("{$rename: {b: 'a.2'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 2], b: 2}"));
    setPathToCreate("2");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "The destination field cannot be an array element, 'a.2' in doc "
        "with no id has an array field called 'a'");
}

TEST_F(RenameNodeTest, MoveToArrayElement) {
    auto update = fromjson("{$rename: {b: 'a.1'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["b"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 'test_object', a: [1, 2], b: 2}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.1"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"][1]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "The destination field cannot be an array element, 'a.1' in doc "
        "with _id: \"test_object\" has an array field called 'a'");
}

TEST_F(RenameNodeTest, MoveOutOfArray) {
    auto update = fromjson("{$rename: {'a.0': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.0"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 'test_object', a: [1, 2]}"));
    setPathToCreate("b");
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "The source field cannot be an array element, 'a.0' in doc with "
                                "_id: \"test_object\" has an array field called 'a'");
}

TEST_F(RenameNodeTest, MoveNonexistentEmbeddedFieldOut) {
    auto update = fromjson("{$rename: {'a.a': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{a: 1}, {b: 2}]}"));
    setPathToCreate("b");
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::PathNotViable,
        "cannot use the part (a of a.a) to traverse the element ({a: [ { a: 1 }, { b: 2 } ]})");
}

TEST_F(RenameNodeTest, MoveEmbeddedFieldOutWithElementNumber) {
    auto update = fromjson("{$rename: {'a.0.a': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.0.a"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 'test_object', a: [{a: 1}, {b: 2}]}"));
    setPathToCreate("b");
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
                                AssertionException,
                                ErrorCodes::BadValue,
                                "The source field cannot be an array element, 'a.0.a' in doc with "
                                "_id: \"test_object\" has an array field called 'a'");
}

TEST_F(RenameNodeTest, ReplaceArrayField) {
    auto update = fromjson("{$rename: {a: 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 2, b: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}, u: {b: 2}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b}");
}

TEST_F(RenameNodeTest, ReplaceWithArrayField) {
    auto update = fromjson("{$rename: {a: 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [], b: 2}"));
    setPathTaken(makeRuntimeUpdatePathForTest("b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: []}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {a: false}, u: {b: []}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a, b}");
}

TEST_F(RenameNodeTest, CanRenameFromInvalidFieldName) {
    auto update = fromjson("{$rename: {'$a': 'a'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["$a"], expCtx));

    mutablebson::Document doc(fromjson("{$a: 2}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);

    assertOplogEntry(fromjson("{$v: 2, diff: {d: {$a: false}, i: {a: 2}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{$a, a}");
}

TEST_F(RenameNodeTest, RenameWithoutLogBuilderOrIndexData) {
    auto update = fromjson("{$rename: {'a': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 2}"));
    setPathToCreate("b");
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);
}

TEST_F(RenameNodeTest, RenameFromNonExistentPathIsNoOp) {
    auto update = fromjson("{$rename: {'a': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{b: 2}"));
    setPathTaken(makeRuntimeUpdatePathForTest("b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["b"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a, b}");
}

TEST_F(RenameNodeTest, ApplyCanRemoveRequiredPartOfDBRefIfValidateForStorageIsFalse) {
    auto update = fromjson("{$rename: {'a.$id': 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.$id"], expCtx));

    mutablebson::Document doc(fromjson("{a: {$ref: 'c', $id: 0}}"));
    setPathToCreate("b");
    addIndexedPath("a");
    setValidateForStorage(false);
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    auto updated = BSON("a" << BSON("$ref"
                                    << "c")
                            << "b" << 0);
    ASSERT_EQUALS(updated, doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {b: 0}, sa: {d: {$id: false}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.$id, b}");
}

TEST_F(RenameNodeTest, ApplyCannotRemoveImmutablePath) {
    auto update = fromjson("{$rename: {'a.b': 'c'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 1}}"));
    setPathToCreate("c");
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a.b' would modify the immutable field 'a.b'");
}

TEST_F(RenameNodeTest, ApplyCannotRemovePrefixOfImmutablePath) {
    auto update = fromjson("{$rename: {a: 'c'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 1}}"));
    setPathToCreate("c");
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a' would modify the immutable field 'a.b'");
}

TEST_F(RenameNodeTest, ApplyCannotRemoveSuffixOfImmutablePath) {
    auto update = fromjson("{$rename: {'a.b.c': 'd'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {c: 1}}}"));
    setPathToCreate("d");
    addImmutablePath("a.b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'a.b.c' would modify the immutable field 'a.b'");
}

TEST_F(RenameNodeTest, ApplyCanRemoveImmutablePathIfNoop) {
    auto update = fromjson("{$rename: {'a.b.c': 'd'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {}}}"));
    setPathToCreate("d");
    addImmutablePath("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: {}}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a.b.c, d}");
}

TEST_F(RenameNodeTest, ApplyCannotOverwriteImmutablePath) {
    auto update = fromjson("{$rename: {a: 'b'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0, b: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("b"));
    addImmutablePath("b");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["b"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path 'b' would modify the immutable field 'b'");
}

}  // namespace
}  // namespace mongo
