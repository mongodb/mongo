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

#include "mongo/db/update/pull_node.h"

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

using PullNodeTest = UpdateTestFixture;

TEST(PullNodeTest, InitWithBadMatchExpressionFails) {
    auto update = fromjson("{$pull: {a: {b: {$foo: 1}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    auto status = node.init(update["$pull"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullNodeTest, InitWithBadTopLevelOperatorFails) {
    auto update = fromjson("{$pull: {a: {$foo: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    auto status = node.init(update["$pull"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullNodeTest, InitWithTextFails) {
    auto update = fromjson("{$pull: {a: {$text: {$search: 'str'}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    auto status = node.init(update["$pull"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullNodeTest, InitWithWhereFails) {
    auto update = fromjson("{$pull: {a: {$where: 'this.a == this.b'}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    auto status = node.init(update["$pull"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullNodeTest, InitWithGeoNearElemFails) {
    auto update =
        fromjson("{$pull: {a: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    auto status = node.init(update["$pull"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(5626500, status.code());
}

TEST(PullNodeTest, InitWithGeoNearObjectFails) {
    auto update = fromjson(
        "{$pull: {a: {b: {$nearSphere: {$geometry: {type: 'Point', coordinates: [0, 0]}}}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    auto status = node.init(update["$pull"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(5626500, status.code());
}

TEST(PullNodeTest, InitWithExprElemFails) {
    auto update = fromjson("{$pull: {a: {$expr: {$eq: [5, 5]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    auto status = node.init(update["$pull"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::QueryFeatureNotAllowed, status);
}

TEST(PullNodeTest, InitWithExprObjectFails) {
    auto update = fromjson("{$pull: {a: {$expr: {$eq: ['$a', {$literal: {b: 5}}]}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    auto status = node.init(update["$pull"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::QueryFeatureNotAllowed, status);
}

TEST(PullNodeTest, InitWithJSONSchemaFails) {
    auto update = fromjson("{$pull: {a: {$jsonSchema: {}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    auto status = node.init(update["$pull"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::QueryFeatureNotAllowed, status);
}

TEST_F(PullNodeTest, TargetNotFound) {
    auto update = fromjson("{$pull : {a: {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(PullNodeTest, ApplyToStringFails) {
    auto update = fromjson("{$pull : {a: {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 'foo'}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "Cannot apply $pull to a non-array value");
}

TEST_F(PullNodeTest, ApplyToObjectFails) {
    auto update = fromjson("{$pull : {a: {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {foo: 'bar'}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "Cannot apply $pull to a non-array value");
}

TEST_F(PullNodeTest, ApplyToNonViablePathFails) {
    auto update = fromjson("{$pull : {'a.b': {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathToCreate("b");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::PathNotViable,
        "Cannot use the part (b) of (a.b) to traverse the element ({a: 1})");
}

TEST_F(PullNodeTest, ApplyToMissingElement) {
    auto update = fromjson("{$pull: {'a.b.c.d': {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a.b.c.d"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: {c: {}}}}"));
    setPathToCreate("d");
    setPathTaken(makeRuntimeUpdatePathForTest("a.b.c"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]["c"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: {c: {}}}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(PullNodeTest, ApplyToEmptyArray) {
    auto update = fromjson("{$pull : {a: {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(PullNodeTest, ApplyToArrayMatchingNone) {
    auto update = fromjson("{$pull : {a: {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [2, 3, 4, 5]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [2, 3, 4, 5]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(PullNodeTest, ApplyToArrayMatchingOne) {
    auto update = fromjson("{$pull : {a: {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2, 3]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1, 2, 3]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1, 2, 3]}}}"));
}

TEST_F(PullNodeTest, ApplyToArrayMatchingSeveral) {
    auto update = fromjson("{$pull : {a: {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 0, 2, 0, 3, 0, 4, 0, 5]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1, 2, 3, 4, 5]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1, 2, 3, 4, 5]}}}"));
}

TEST_F(PullNodeTest, ApplyToArrayMatchingAll) {
    auto update = fromjson("{$pull : {a: {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, -1, -2, -3, -4, -5]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: []}}}"));
}

TEST_F(PullNodeTest, ApplyToArrayWithEq) {
    auto update = fromjson("{$pull : {a: {$eq: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2, 3]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 2, 3]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [0, 2, 3]}}}"));
}

TEST_F(PullNodeTest, ApplyNoIndexDataNoLogBuilder) {
    auto update = fromjson("{$pull : {a: {$lt: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 1, 2, 3]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1, 2, 3]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(PullNodeTest, ApplyWithCollation) {
    // With the collation, this update will pull any string whose reverse is greater than the
    // reverse of the "abc" string.
    auto update = fromjson("{$pull : {a: {$gt: 'abc'}}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: ['zaa', 'zcc', 'zbb', 'zee']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['zaa', 'zbb']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: ['zaa', 'zbb']}}}"));
}

TEST_F(PullNodeTest, ApplyWithCollationDoesNotAffectNonStringMatches) {
    auto update = fromjson("{$pull : {a: {$lt: 1}}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [2, 1, 0, -1, -2, -3]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [2, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [2, 1]}}}"));
}

TEST_F(PullNodeTest, ApplyWithCollationDoesNotAffectRegexMatches) {
    auto update = fromjson("{$pull : {a: /a/}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: ['b', 'a', 'aab', 'cb', 'bba']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['b', 'cb']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: ['b', 'cb']}}}"));
}

TEST_F(PullNodeTest, ApplyStringLiteralMatchWithCollation) {
    auto update = fromjson("{$pull : {a: 'c'}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: ['b', 'a', 'aab', 'cb', 'bba']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: []}}}"));
}

TEST_F(PullNodeTest, ApplyCollationDoesNotAffectNumberLiteralMatches) {
    auto update = fromjson("{$pull : {a: 99}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: ['a', 99, 'b', 2, 'c', 99, 'd']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['a', 'b', 2, 'c', 'd']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: ['a', 'b', 2, 'c', 'd']}}}"));
}

TEST_F(PullNodeTest, ApplyStringMatchAfterSetCollator) {
    auto update = fromjson("{$pull : {a: 'c'}}");
    PullNode node;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    // First without a collator.
    mutablebson::Document doc(fromjson("{ a : ['a', 'b', 'c', 'd'] }"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['a', 'b', 'd']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    // Now with a collator.
    CollatorInterfaceMock mockCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    node.setCollator(&mockCollator);
    mutablebson::Document doc2(fromjson("{ a : ['a', 'b', 'c', 'd'] }"));
    resetApplyParams();
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    result = node.apply(getApplyParams(doc2.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc2);
    ASSERT_FALSE(doc2.isInPlaceModeEnabled());
}

TEST_F(PullNodeTest, ApplyElementMatchAfterSetCollator) {
    auto update = fromjson("{$pull : {a: {$gte: 'c'}}}");
    PullNode node;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    // First without a collator.
    mutablebson::Document doc(fromjson("{ a : ['a', 'b', 'c', 'd'] }"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['a', 'b']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    // Now with a collator.
    CollatorInterfaceMock mockCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    node.setCollator(&mockCollator);
    mutablebson::Document doc2(fromjson("{ a : ['a', 'b', 'c', 'd'] }"));
    resetApplyParams();
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    result = node.apply(getApplyParams(doc2.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc2);
    ASSERT_FALSE(doc2.isInPlaceModeEnabled());
}

TEST_F(PullNodeTest, ApplyObjectMatchAfterSetCollator) {
    auto update = fromjson("{$pull : {a: {b: 'y'}}}");
    PullNode node;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    // First without a collator.
    mutablebson::Document doc(fromjson("{a : [{b: 'w'}, {b: 'x'}, {b: 'y'}, {b: 'z'}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a : [{b: 'w'}, {b: 'x'}, {b: 'z'}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    // Now with a collator.
    CollatorInterfaceMock mockCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    node.setCollator(&mockCollator);
    mutablebson::Document doc2(fromjson("{a : [{b: 'w'}, {b: 'x'}, {b: 'y'}, {b: 'z'}]}"));
    resetApplyParams();
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    result = node.apply(getApplyParams(doc2.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc2);
    ASSERT_FALSE(doc2.isInPlaceModeEnabled());
}

TEST_F(PullNodeTest, SetCollatorDoesNotAffectClone) {
    auto update = fromjson("{$pull : {a: 'c'}}");
    PullNode node;
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    auto cloneNode = node.clone();

    CollatorInterfaceMock mockCollator(CollatorInterfaceMock::MockType::kAlwaysEqual);
    node.setCollator(&mockCollator);

    // The original node should now have collation.
    mutablebson::Document doc(fromjson("{ a : ['a', 'b', 'c', 'd'] }"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    // The clone should have exact string matches (no collation).
    mutablebson::Document doc2(fromjson("{ a : ['a', 'b', 'c', 'd'] }"));
    resetApplyParams();
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    result = cloneNode->apply(getApplyParams(doc2.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['a', 'b', 'd']}"), doc2);
    ASSERT_FALSE(doc2.isInPlaceModeEnabled());
}

TEST_F(PullNodeTest, ApplyComplexDocAndMatching1) {
    auto update = fromjson(
        "{$pull: {'a.b': {$or: ["
        "  {'y': {$exists: true }},"
        "  {'z' : {$exists : true}} "
        "]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: [{x: 1}, {y: 'y'}, {x: 2}, {z: 'z'}]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: [{x: 1}, {x: 2}]}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: [{x: 1}, {x: 2}]}}}}"));
}

TEST_F(PullNodeTest, ApplyComplexDocAndMatching2) {
    auto update = fromjson("{$pull: {'a.b': {'y': {$exists: true}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: [{x: 1}, {y: 'y'}, {x: 2}, {z: 'z'}]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: [{x: 1}, {x: 2}, {z: 'z'}]}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: [{x: 1}, {x: 2}, {z: 'z'}]}}}}"));
}

TEST_F(PullNodeTest, ApplyComplexDocAndMatching3) {
    auto update = fromjson("{$pull: {'a.b': {$in: [{x: 1}, {y: 'y'}]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a.b"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: [{x: 1}, {y: 'y'}, {x: 2}, {z: 'z'}]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: [{x: 2}, {z: 'z'}]}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: [{x: 2}, {z: 'z'}]}}}}"));
}

TEST_F(PullNodeTest, ApplyFullPredicateWithCollation) {
    auto update = fromjson("{$pull: {'a.b': {x: 'blah'}}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kAlwaysEqual);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a.b"], expCtx));

    mutablebson::Document doc(
        fromjson("{a: {b: [{x: 'foo', y: 1}, {x: 'bar', y: 2}, {x: 'baz', y: 3}]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.b"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]["b"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: []}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {u: {b: []}}}}"));
}

TEST_F(PullNodeTest, ApplyScalarValueMod) {
    auto update = fromjson("{$pull: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 2, 1, 2, 1, 2]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [2, 2, 2]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [2, 2, 2]}}}"));
}

TEST_F(PullNodeTest, ApplyObjectValueMod) {
    auto update = fromjson("{$pull: {a: {y: 2}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{x: 1}, {y: 2}, {x: 1}, {y: 2}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{x: 1}, {x: 1}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [{x: 1}, {x: 1}]}}}"));
}

TEST_F(PullNodeTest, DocumentationExample1) {
    auto update = fromjson("{$pull: {flags: 'msr'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["flags"], expCtx));

    mutablebson::Document doc(
        fromjson("{flags: ['vme', 'de', 'pse', 'tsc', 'msr', 'pae', 'mce']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("flags"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["flags"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{flags: ['vme', 'de', 'pse', 'tsc', 'pae', 'mce']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(
        fromjson("{$v: 2, diff: {u: {flags: ['vme', 'de', 'pse', 'tsc', 'pae', 'mce']}}}"));
}

TEST_F(PullNodeTest, DocumentationExample2a) {
    auto update = fromjson("{$pull: {votes: 7}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["votes"], expCtx));

    mutablebson::Document doc(fromjson("{votes: [3, 5, 6, 7, 7, 8]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("votes"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["votes"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{votes: [3, 5, 6, 8]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {votes: [3, 5, 6, 8]}}}"));
}

TEST_F(PullNodeTest, DocumentationExample2b) {
    auto update = fromjson("{$pull: {votes: {$gt: 6}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["votes"], expCtx));

    mutablebson::Document doc(fromjson("{votes: [3, 5, 6, 7, 7, 8]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("votes"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["votes"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{votes: [3, 5, 6]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {votes: [3, 5, 6]}}}"));
}

TEST_F(PullNodeTest, ApplyPullWithObjectValueToArrayWithNonObjectValue) {
    auto update = fromjson("{$pull: {a: {x: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [{x: 1}, 2]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [2]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [2]}}}"));
}

TEST_F(PullNodeTest, CannotModifyImmutableField) {
    auto update = fromjson("{$pull: {'_id.a': 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["_id.a"], expCtx));

    mutablebson::Document doc(fromjson("{_id: {a: [0, 1, 2]}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("_id.a"));
    addImmutablePath("_id");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["_id"]["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::ImmutableField,
        "Performing an update on the path '_id.a' would modify the immutable field '_id'");
}

TEST_F(PullNodeTest, SERVER_3988) {
    auto update = fromjson("{$pull: {y: /yz/}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullNode node;
    ASSERT_OK(node.init(update["$pull"]["y"], expCtx));

    mutablebson::Document doc(fromjson("{x: 1, y: [2, 3, 4, 'abc', 'xyz']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("y"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["y"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{x: 1, y: [2, 3, 4, 'abc']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {y: [2, 3, 4, 'abc']}}}"));
}

}  // namespace
}  // namespace mongo
