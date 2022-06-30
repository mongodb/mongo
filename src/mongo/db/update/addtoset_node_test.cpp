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

#include "mongo/db/update/addtoset_node.h"

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

using AddToSetNodeTest = UpdateTestFixture;
using mongo::mutablebson::countChildren;
using mongo::mutablebson::Element;

DEATH_TEST_REGEX(AddToSetNodeTest,
                 InitFailsForEmptyElement,
                 R"#(Invariant failure.*modExpr.ok())#") {
    auto update = fromjson("{$addToSet: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    node.init(update["$addToSet"].embeddedObject().firstElement(), expCtx).transitional_ignore();
}

TEST(AddToSetNodeTest, InitFailsIfEachIsNotArray) {
    auto update = fromjson("{$addToSet: {a: {$each: {}}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    auto result = node.init(update["$addToSet"]["a"], expCtx);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.code(), ErrorCodes::TypeMismatch);
    ASSERT_EQ(result.reason(),
              "The argument to $each in $addToSet must be an array but it was of type object");
}

TEST(AddToSetNodeTest, InitFailsIfThereAreFieldsAfterEach) {
    auto update = fromjson("{$addToSet: {a: {$each: [], bad: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    auto result = node.init(update["$addToSet"]["a"], expCtx);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.reason(),
              "Found unexpected fields after $each in $addToSet: { $each: [], bad: 1 }");
}

TEST(AddToSetNodeTest, InitSucceedsWithFailsBeforeEach) {
    auto update = fromjson("{$addToSet: {a: {other: 1, $each: []}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));
}

TEST(AddToSetNodeTest, InitSucceedsWithObject) {
    auto update = fromjson("{$addToSet: {a: {}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));
}

TEST(AddToSetNodeTest, InitSucceedsWithArray) {
    auto update = fromjson("{$addToSet: {a: []}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));
}

TEST(AddToSetNodeTest, InitSucceedsWithScaler) {
    auto update = fromjson("{$addToSet: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));
}

TEST_F(AddToSetNodeTest, ApplyFailsOnNonArray) {
    auto update = fromjson("{$addToSet: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 2}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "Cannot apply $addToSet to non-array field. Field named 'a' has non-array type int");
}

TEST_F(AddToSetNodeTest, ApplyNonEach) {
    auto update = fromjson("{$addToSet: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [0, 1]}}}"));

    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyNonEachArray) {
    auto update = fromjson("{$addToSet: {a: [1]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, [1]]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [0, [1]]}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyEach) {
    auto update = fromjson("{$addToSet: {a: {$each: [1, 2]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1, 2]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [0, 1, 2]}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyToEmptyArray) {
    auto update = fromjson("{$addToSet: {a: {$each: [1, 2]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1, 2]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [1, 2]}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyDeduplicateElementsToAdd) {
    auto update = fromjson("{$addToSet: {a: {$each: [1, 1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [0, 1]}}}"));

    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyDoNotAddExistingElements) {
    auto update = fromjson("{$addToSet: {a: {$each: [0, 1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [0, 1]}}}"));

    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyDoNotDeduplicateExistingElements) {
    auto update = fromjson("{$addToSet: {a: {$each: [1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0, 0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [0, 0, 1]}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyNoElementsToAdd) {
    auto update = fromjson("{$addToSet: {a: {$each: []}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyNoNonDuplicateElementsToAdd) {
    auto update = fromjson("{$addToSet: {a: {$each: [0]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyCreateArray) {
    auto update = fromjson("{$addToSet: {a: {$each: [0, 1]}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: [0, 1]}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyCreateEmptyArrayIsNotNoop) {
    auto update = fromjson("{$addToSet: {a: {$each: []}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: []}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyDeduplicationOfElementsToAddRespectsCollation) {
    auto update = fromjson("{$addToSet: {a: {$each: ['abc', 'ABC', 'def', 'abc']}}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['abc', 'def']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: ['abc', 'def']}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyComparisonToExistingElementsRespectsCollation) {
    auto update = fromjson("{$addToSet: {a: {$each: ['abc', 'def']}}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: ['ABC']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['ABC', 'def']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: ['ABC', 'def']}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyRespectsCollationFromSetCollator) {
    auto update = fromjson("{$addToSet: {a: {$each: ['abc', 'ABC', 'def', 'abc']}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    const CollatorInterfaceMock caseInsensitiveCollator(
        CollatorInterfaceMock::MockType::kToLowerString);
    node.setCollator(&caseInsensitiveCollator);

    mutablebson::Document doc(fromjson("{a: []}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['abc', 'def']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: ['abc', 'def']}}}"));

    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

DEATH_TEST_REGEX(AddToSetNodeTest,
                 CannotSetCollatorIfCollatorIsNonNull,
                 "Invariant failure.*!_collator") {
    auto update = fromjson("{$addToSet: {a: 1}}");
    auto caseInsensitiveCollator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(caseInsensitiveCollator));
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));
    node.setCollator(expCtx->getCollator());
}

DEATH_TEST_REGEX(AddToSetNodeTest, CannotSetCollatorTwice, "Invariant failure.*!_collator") {
    auto update = fromjson("{$addToSet: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    const CollatorInterfaceMock caseInsensitiveCollator(
        CollatorInterfaceMock::MockType::kToLowerString);
    node.setCollator(&caseInsensitiveCollator);
    node.setCollator(&caseInsensitiveCollator);
}

TEST_F(AddToSetNodeTest, ApplyNestedArray) {
    auto update = fromjson("{ $addToSet : { 'a.1' : 1 } }");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a.1"], expCtx));

    mutablebson::Document doc(fromjson("{ _id : 1, a : [1, []] }"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.1"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"][1]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{ _id : 1, a : [1, [1]] }"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {sa: {a: true, u1: [1]}}}"));

    ASSERT_EQUALS(getModifiedPaths(), "{a.1}");
}

TEST_F(AddToSetNodeTest, ApplyIndexesNotAffected) {
    auto update = fromjson("{$addToSet: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("b");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [0, 1]}}}"));
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

TEST_F(AddToSetNodeTest, ApplyNoIndexDataOrLogBuilder) {
    auto update = fromjson("{$addToSet: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [0]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(getModifiedPaths(), "{a}");
}

}  // namespace
}  // namespace mongo
