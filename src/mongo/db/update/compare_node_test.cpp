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

#include "mongo/db/update/compare_node.h"

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

using CompareNodeTest = UpdateTestFixture;
using mongo::mutablebson::countChildren;
using mongo::mutablebson::Element;

DEATH_TEST_REGEX(CompareNodeTest,
                 InitFailsForEmptyElement,
                 R"#(Invariant failure.*modExpr.ok\(\))#") {
    auto update = fromjson("{$max: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    node.init(update["$max"].embeddedObject().firstElement(), expCtx).ignore();
}

TEST_F(CompareNodeTest, ApplyMaxSameNumber) {
    auto update = fromjson("{$max: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(CompareNodeTest, ApplyMinSameNumber) {
    auto update = fromjson("{$min: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(CompareNodeTest, ApplyMaxNumberIsLess) {
    auto update = fromjson("{$max: {a: 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(CompareNodeTest, ApplyMinNumberIsMore) {
    auto update = fromjson("{$min: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(CompareNodeTest, ApplyMaxSameValInt) {
    auto update = BSON("$max" << BSON("a" << 1LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1.0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1.0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(CompareNodeTest, ApplyMaxSameValIntZero) {
    auto update = BSON("$max" << BSON("a" << 0LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0.0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 0.0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(CompareNodeTest, ApplyMinSameValIntZero) {
    auto update = BSON("$min" << BSON("a" << 0LL));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0.0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 0.0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(CompareNodeTest, ApplyMissingFieldMinNumber) {
    auto update = fromjson("{$min: {a: 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v:2, diff: {i: {a: 0}}}"));
}

TEST_F(CompareNodeTest, ApplyExistingNumberMinNumber) {
    auto update = fromjson("{$min: {a: 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v:2, diff: {u: {a: 0}}}"));
}

TEST_F(CompareNodeTest, ApplyMissingFieldMaxNumber) {
    auto update = fromjson("{$max: {a: 0}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v:2, diff: {i: {a: 0}}}"));
}

TEST_F(CompareNodeTest, ApplyExistingNumberMaxNumber) {
    auto update = fromjson("{$max: {a: 2}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 1}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v:2, diff: {u: {a: 2}}}"));
}

TEST_F(CompareNodeTest, ApplyExistingDateMaxDate) {
    auto update = fromjson("{$max: {a: {$date: 123123123}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {$date: 0}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {$date: 123123123}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v:2, diff: {u: {a: {$date: 123123123}}}}"));
}

TEST_F(CompareNodeTest, ApplyExistingEmbeddedDocMaxDoc) {
    auto update = fromjson("{$max: {a: {b: 3}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 3}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v:2, diff: {u: {a: {b: 3}}}}"));
}

TEST_F(CompareNodeTest, ApplyExistingEmbeddedDocMaxNumber) {
    auto update = fromjson("{$max: {a: 3}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: {b: 2}}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(CompareNodeTest, ApplyMinRespectsCollation) {
    auto update = fromjson("{$min: {a: 'dba'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kReverseString);
    expCtx->setCollator(std::move(collator));
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 'cbc'}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 'dba'}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v:2, diff: {u: {a: 'dba'}}}"));
}

TEST_F(CompareNodeTest, ApplyMinRespectsCollationFromSetCollator) {
    auto update = fromjson("{$min: {a: 'dba'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], expCtx));

    const CollatorInterfaceMock reverseStringCollator(
        CollatorInterfaceMock::MockType::kReverseString);
    node.setCollator(&reverseStringCollator);

    mutablebson::Document doc(fromjson("{a: 'cbc'}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 'dba'}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v:2, diff: {u: {a: 'dba'}}}"));
}

TEST_F(CompareNodeTest, ApplyMaxRespectsCollationFromSetCollator) {
    auto update = fromjson("{$max: {a: 'abd'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    const CollatorInterfaceMock reverseStringCollator(
        CollatorInterfaceMock::MockType::kReverseString);
    node.setCollator(&reverseStringCollator);

    mutablebson::Document doc(fromjson("{a: 'cbc'}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 'abd'}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v:2, diff: {u: {a: 'abd'}}}"));
}

DEATH_TEST_REGEX(CompareNodeTest,
                 CannotSetCollatorIfCollatorIsNonNull,
                 "Invariant failure.*!_collator") {
    auto update = fromjson("{$max: {a: 1}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);

    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));
    node.setCollator(expCtx->getCollator());
}

DEATH_TEST_REGEX(CompareNodeTest, CannotSetCollatorTwice, "Invariant failure.*!_collator") {
    auto update = fromjson("{$max: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    const CollatorInterfaceMock caseInsensitiveCollator(
        CollatorInterfaceMock::MockType::kToLowerString);
    node.setCollator(&caseInsensitiveCollator);
    node.setCollator(&caseInsensitiveCollator);
}

TEST_F(CompareNodeTest, ApplyIndexesNotAffected) {
    auto update = fromjson("{$max: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("b");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v:2, diff: {u: {a: 1}}}"));
}

TEST_F(CompareNodeTest, ApplyNoIndexDataOrLogBuilder) {
    auto update = fromjson("{$max: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

}  // namespace
}  // namespace mongo
