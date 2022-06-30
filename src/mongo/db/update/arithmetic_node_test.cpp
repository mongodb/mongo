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

#include "mongo/db/update/arithmetic_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using ArithmeticNodeTest = UpdateTestFixture;

DEATH_TEST_REGEX(ArithmeticNodeTest,
                 InitFailsForEmptyElement,
                 R"#(Invariant failure.*modExpr.ok\(\))#") {
    auto update = fromjson("{$inc: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    node.init(update["$inc"].embeddedObject().firstElement(), expCtx).transitional_ignore();
}

TEST(ArithmeticNodeTest, InitSucceedsForNumberIntElement) {
    auto update = fromjson("{$inc: {a: NumberInt(5)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));
}

TEST(ArithmeticNodeTest, InitSucceedsForNumberLongElement) {
    auto update = fromjson("{$inc: {a: NumberLong(5)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));
}

TEST(ArithmeticNodeTest, InitSucceedsForDoubleElement) {
    auto update = fromjson("{$inc: {a: 5.1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));
}

TEST(ArithmeticNodeTest, InitSucceedsForDecimalElement) {
    auto update = fromjson("{$inc: {a: NumberDecimal(\"5.1\")}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));
}

TEST(ArithmeticNodeTest, InitFailsForNonNumericElement) {
    auto update = fromjson("{$inc: {a: 'foo'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    Status result = node.init(update["$inc"]["a"], expCtx);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.code(), ErrorCodes::TypeMismatch);
    ASSERT_EQ(result.reason(), "Cannot increment with non-numeric argument: {a: \"foo\"}");
}

TEST(ArithmeticNodeTest, InitFailsForObjectElement) {
    auto update = fromjson("{$mul: {a: {b: 6}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    Status result = node.init(update["$mul"]["a"], expCtx);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.code(), ErrorCodes::TypeMismatch);
    ASSERT_EQ(result.reason(), "Cannot multiply with non-numeric argument: {a: { b: 6 }}");
}

TEST(ArithmeticNodeTest, InitFailsForArrayElement) {
    auto update = fromjson("{$mul: {a: []}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    Status result = node.init(update["$mul"]["a"], expCtx);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.code(), ErrorCodes::TypeMismatch);
    ASSERT_EQ(result.reason(), "Cannot multiply with non-numeric argument: {a: []}");
}

TEST_F(ArithmeticNodeTest, ApplyIncNoOp) {
    auto update = fromjson("{$inc: {a: 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyMulNoOp) {
    auto update = fromjson("{$mul: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyRoundingNoOp) {
    auto update = fromjson("{$inc: {a: 1.0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 6.022e23}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 6.022e23}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyEmptyPathToCreate) {
    auto update = fromjson("{$inc: {a: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 11}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: 11}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyCreatePath) {
    auto update = fromjson("{$inc: {'a.b.c': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b.c"], expCtx));

    mutablebson::Document doc(fromjson("{a: {d: 5}}"));
    setPathToCreate("b.c");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {d: 5, b: {c: 6}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {i: {b: {c: 6}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b.c}");
}

TEST_F(ArithmeticNodeTest, ApplyExtendPath) {
    auto update = fromjson("{$inc: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {c: 1}}"));
    setPathToCreate("b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {c: 1, b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(ArithmeticNodeTest, ApplyCreatePathFromRoot) {
    auto update = fromjson("{$inc: {'a.b': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{c: 5}"));
    setPathToCreate("a.b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{c: 5, a: {b: 6}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: {b: 6}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(ArithmeticNodeTest, ApplyPositional) {
    auto update = fromjson("{$inc: {'a.$': 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.$"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.1"));
    setMatchedField("1");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"][1]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 7, 2]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u1: 7}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.1}");
}

TEST_F(ArithmeticNodeTest, ApplyNonViablePathToInc) {
    auto update = fromjson("{$inc: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathToCreate("b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::PathNotViable,
        "Cannot create field 'b' in element {a: 5}");
}

TEST_F(ArithmeticNodeTest, ApplyNonViablePathToCreateFromReplicationIsNoOp) {
    auto update = fromjson("{$inc: {'a.b': 5}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathToCreate("b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    setFromOplogApplication(true);
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(ArithmeticNodeTest, ApplyNoIndexDataNoLogBuilder) {
    auto update = fromjson("{$inc: {a: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 11}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyDoesNotAffectIndexes) {
    auto update = fromjson("{$inc: {a: 6}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("b");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 11}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, IncTypePromotionIsNotANoOp) {
    auto update = fromjson("{$inc: {a: NumberLong(0)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: NumberInt(2)}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberLong(2)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, MulTypePromotionIsNotANoOp) {
    auto update = fromjson("{$mul: {a: NumberLong(1)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: NumberInt(2)}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberLong(2)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, TypePromotionFromIntToDecimalIsNotANoOp) {
    auto update = fromjson("{$inc: {a: NumberDecimal(\"0.0\")}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: NumberInt(5)}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberDecimal(\"5.0\")}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: NumberDecimal('5.0')}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, TypePromotionFromLongToDecimalIsNotANoOp) {
    auto update = fromjson("{$inc: {a: NumberDecimal(\"0.0\")}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: NumberLong(5)}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberDecimal(\"5.0\")}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: NumberDecimal('5.0')}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, TypePromotionFromDoubleToDecimalIsNotANoOp) {
    auto update = fromjson("{$inc: {a: NumberDecimal(\"0.0\")}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 5.25}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberDecimal(\"5.25\")}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(
        fromjson("{$v: 2, diff: {u: {a: NumberDecimal('5.25')}}}"),
        false  // Not checking binary equality because the NumberDecimal in the expected output may
               // not be bitwise identical to the result produced by the update system.
    );
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyPromoteToFloatingPoint) {
    auto update = fromjson("{$inc: {a: 0.2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: NumberLong(1)}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1.2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, IncrementedDecimalStaysDecimal) {
    auto update = fromjson("{$inc: {a: 5.25}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: NumberDecimal(\"6.25\")}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberDecimal(\"11.5\")}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(
        fromjson("{$v: 2, diff: {u: {a: NumberDecimal('11.5')}}}"),
        false  // Not checking binary equality because the NumberDecimal in the expected output may
               // not be bitwise identical to the result produced by the update system.
    );
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, OverflowIntToLong) {
    auto update = fromjson("{$inc: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    const int initialValue = std::numeric_limits<int>::max();
    mutablebson::Document doc(BSON("a" << initialValue));
    ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    ASSERT_EQUALS(BSON("a" << static_cast<long long>(initialValue) + 1), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, UnderflowIntToLong) {
    auto update = fromjson("{$inc: {a: -1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    const int initialValue = std::numeric_limits<int>::min();
    mutablebson::Document doc(BSON("a" << initialValue));
    ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    ASSERT_EQUALS(BSON("a" << static_cast<long long>(initialValue) - 1), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, IncModeCanBeReused) {
    auto update = fromjson("{$inc: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc1(fromjson("{a: 1}"));
    mutablebson::Document doc2(fromjson("{a: 2}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc1.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc1);
    ASSERT_TRUE(doc1.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");

    resetApplyParams();
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    result = node.apply(getApplyParams(doc2.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 3}"), doc2);
    ASSERT_TRUE(doc1.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, CreatedNumberHasSameTypeAsInc) {
    auto update = fromjson("{$inc: {a: NumberLong(5)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{b: 6}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 6, a: NumberLong(5)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, CreatedNumberHasSameTypeAsMul) {
    auto update = fromjson("{$mul: {a: NumberLong(5)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{b: 6}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 6, a: NumberLong(0)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyEmptyDocument) {
    auto update = fromjson("{$inc: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyIncToObjectFails) {
    auto update = fromjson("{$inc: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 'test_object', a: {b: 1}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::TypeMismatch,
        "Cannot apply $inc to a value of non-numeric type. {_id: "
        "\"test_object\"} has the field 'a' of non-numeric type object");
}

TEST_F(ArithmeticNodeTest, ApplyIncToArrayFails) {
    auto update = fromjson("{$inc: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 'test_object', a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::TypeMismatch,
        "Cannot apply $inc to a value of non-numeric type. {_id: "
        "\"test_object\"} has the field 'a' of non-numeric type array");
}

TEST_F(ArithmeticNodeTest, ApplyIncToStringFails) {
    auto update = fromjson("{$inc: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 'test_object', a: \"foo\"}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::TypeMismatch,
        "Cannot apply $inc to a value of non-numeric type. {_id: "
        "\"test_object\"} has the field 'a' of non-numeric type string");
}

TEST_F(ArithmeticNodeTest, OverflowingOperationFails) {
    auto update = fromjson("{$mul: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{_id: 'test_object', a: NumberLong(9223372036854775807)}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "Failed to apply $mul operations to current value "
        "((NumberLong)9223372036854775807) for document {_id: "
        "\"test_object\"}");
}

TEST_F(ArithmeticNodeTest, ApplyNewPath) {
    auto update = fromjson("{$inc: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{b: 1}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 1, a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyEmptyIndexData) {
    auto update = fromjson("{$inc: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_EQUALS(fromjson("{a: 3}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: 3}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyNoOpDottedPath) {
    auto update = fromjson("{$inc: {'a.b': 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b : 2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(ArithmeticNodeTest, TypePromotionOnDottedPathIsNotANoOp) {
    auto update = fromjson("{$inc: {'a.b': NumberLong(0)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: NumberInt(2)}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b : NumberLong(2)}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(ArithmeticNodeTest, ApplyPathNotViableArray) {
    auto update = fromjson("{$inc: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a:[{b:1}]}"));
    setPathToCreate("b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::PathNotViable,
        "Cannot create field 'b' in element {a: [ { b: 1 } ]}");
}

TEST_F(ArithmeticNodeTest, ApplyInPlaceDottedPath) {
    auto update = fromjson("{$inc: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 1}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 3}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(ArithmeticNodeTest, ApplyPromotionDottedPath) {
    auto update = fromjson("{$inc: {'a.b': NumberLong(2)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: NumberInt(3)}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: NumberLong(5)}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(ArithmeticNodeTest, ApplyDottedPathEmptyDoc) {
    auto update = fromjson("{$inc: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a.b");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(ArithmeticNodeTest, ApplyFieldWithDot) {
    auto update = fromjson("{$inc: {'a.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{'a.b':4}"));
    setPathToCreate("a.b");
    addIndexedPath("a.b");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{'a.b':4, a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(ArithmeticNodeTest, ApplyNoOpArrayIndex) {
    auto update = fromjson("{$inc: {'a.2.b': 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.2.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"][2]["b"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.2.b}");
}

TEST_F(ArithmeticNodeTest, TypePromotionInArrayIsNotANoOp) {
    auto update = fromjson("{$set: {'a.2.b': NumberLong(0)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(
        fromjson("{a: [{b: NumberInt(0)},{b: NumberInt(1)},{b: NumberInt(2)}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.2.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"][2]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: NumberLong(2)}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.2.b}");
}

TEST_F(ArithmeticNodeTest, ApplyNonViablePathThroughArray) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathToCreate("2.b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::PathNotViable,
        "Cannot create field '2' in element {a: 0}");
}

TEST_F(ArithmeticNodeTest, ApplyInPlaceArrayIndex) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 1}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.2.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"][2]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 3}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.2.b}");
}

TEST_F(ArithmeticNodeTest, ApplyAppendArray) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0},{b: 1}]}"));
    setPathToCreate("2.b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyPaddingArray) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b: 0}]}"));
    setPathToCreate("2.b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},null,{b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyNumericObject) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 0}}"));
    setPathToCreate("2.b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 0, '2': {b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.2.b}");
}

TEST_F(ArithmeticNodeTest, ApplyNumericField) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {'2': {b: 1}}}"));
    setPathTaken(RuntimeUpdatePath(FieldRef("a.2.b"),
                                   {RuntimeUpdatePath::ComponentType::kFieldName,
                                    RuntimeUpdatePath::ComponentType::kFieldName,
                                    RuntimeUpdatePath::ComponentType::kFieldName}));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["2"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {'2': {b: 3}}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.2.b}");
}

TEST_F(ArithmeticNodeTest, ApplyExtendNumericField) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {'2': {c: 1}}}"));
    setPathToCreate("b");
    setPathTaken(RuntimeUpdatePath(FieldRef("a.2"),
                                   {RuntimeUpdatePath::ComponentType::kFieldName,
                                    RuntimeUpdatePath::ComponentType::kFieldName}));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["2"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {'2': {c: 1, b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.2.b}");
}

TEST_F(ArithmeticNodeTest, ApplyNumericFieldToEmptyObject) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {}}"));
    setPathToCreate("2.b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a.2.b}");
}

TEST_F(ArithmeticNodeTest, ApplyEmptyArray) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$set"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathToCreate("2.b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [null, null, {b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyLogDottedPath) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{b:0}, {b:1}]}"));
    setPathToCreate("2.b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_EQUALS(fromjson("{a: [{b:0}, {b:1}, {b:2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u2: {b: 2}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, LogEmptyArray) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathToCreate("2.b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_EQUALS(fromjson("{a: [null, null, {b:2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u2: {b: 2}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, LogEmptyObject) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {}}"));
    setPathToCreate("2.b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {i: {'2': {b: 2}}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.2.b}");
}

TEST_F(ArithmeticNodeTest, ApplyDeserializedDocNotNoOp) {
    auto update = fromjson("{$mul: {b: NumberInt(1)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    // De-serialize the int.
    doc.root()["a"].setValueInt(1).transitional_ignore();

    setPathToCreate("b");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: NumberInt(0)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {b: 0}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{b}");
}

TEST_F(ArithmeticNodeTest, ApplyToDeserializedDocNoOp) {
    auto update = fromjson("{$mul: {a: NumberInt(1)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    // De-serialize the int.
    doc.root()["a"].setValueInt(2).transitional_ignore();

    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberInt(2)}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(ArithmeticNodeTest, ApplyToDeserializedDocNestedNoop) {
    auto update = fromjson("{$mul: {'a.b': NumberInt(1)}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a.b"], expCtx));

    mutablebson::Document doc{BSONObj()};
    // De-serialize the int.
    doc.root().appendObject("a", BSON("b" << static_cast<int>(1))).transitional_ignore();

    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: NumberInt(1)}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

TEST_F(ArithmeticNodeTest, ApplyToDeserializedDocNestedNotNoop) {
    auto update = fromjson("{$mul: {'a.b': 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a.b"], expCtx));

    mutablebson::Document doc{BSONObj()};
    // De-serialize the int.
    doc.root().appendObject("a", BSON("b" << static_cast<int>(1))).transitional_ignore();

    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 3}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: 3}}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a.b}");
}

}  // namespace
}  // namespace mongo
