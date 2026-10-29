// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/pullall_node.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/element.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <utility>

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using PullAllNodeTest = UpdateTestFixture;

TEST(SimplePullAllNodeTest, InitWithIntFails) {
    auto update = fromjson("{$pullAll: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(SimplePullAllNodeTest, InitWithStringFails) {
    auto update = fromjson("{$pullAll: {a: 'test'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(SimplePullAllNodeTest, InitWithObjectFails) {
    auto update = fromjson("{$pullAll: {a: {}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(SimplePullAllNodeTest, InitWithBoolFails) {
    auto update = fromjson("{$pullAll: {a: true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST_F(PullAllNodeTest, TargetNotFound) {
    auto update = fromjson("{$pullAll : {b: [1]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["b"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    setPathToCreate("b");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(getIndexAffectedFromLogEntry());
    ASSERT_EQUALS(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(PullAllNodeTest, TargetArrayElementNotFound) {
    auto update = fromjson("{$pullAll : {'a.2': [1]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a.2"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 2]}"));
    setPathToCreate("2");
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(getIndexAffectedFromLogEntry());
    ASSERT_EQUALS(fromjson("{a: [1, 2]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(PullAllNodeTest, ApplyToNonArrayFails) {
    auto update = fromjson("{$pullAll : {'a.0': [1, 2]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a.0"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 2]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a.0"));
    addIndexedPath("a");
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(getApplyParams(doc.root()["a"][0]), getUpdateNodeApplyParams()),
        AssertionException,
        ErrorCodes::BadValue,
        "Cannot apply $pull to a non-array value");
}

TEST_F(PullAllNodeTest, ApplyWithSingleNumber) {
    auto update = fromjson("{$pullAll : {a: [1]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
    ASSERT_EQUALS(fromjson("{a: ['a', {r: 1, b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [\"a\", {r: 1, b: 2}]}}}"));
}

TEST_F(PullAllNodeTest, ApplyNoIndexDataNoLogBuilder) {
    auto update = fromjson("{$pullAll : {a: [1]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: ['a', {r: 1, b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST_F(PullAllNodeTest, ApplyWithElementNotPresentInArray) {
    auto update = fromjson("{$pullAll : {a: ['r']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_FALSE(getIndexAffectedFromLogEntry());
    ASSERT_EQUALS(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(PullAllNodeTest, ApplyWithWithTwoElements) {
    auto update = fromjson("{$pullAll : {a: [1, 'a']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
    ASSERT_EQUALS(fromjson("{a: [{r: 1, b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: [{r: 1, b: 2}]}}}"));
}

TEST_F(PullAllNodeTest, ApplyWithAllArrayElements) {
    auto update = fromjson("{$pullAll : {a: [1, 'a', {r: 1, b: 2}]}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: []}}}"));
}

TEST_F(PullAllNodeTest, ApplyWithAllArrayElementsButOutOfOrder) {
    auto update = fromjson("{$pullAll : {a: [{r: 1, b: 2}, 1, 'a']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: []}}}"));
}

TEST_F(PullAllNodeTest, ApplyWithAllArrayElementsAndThenSome) {
    auto update = fromjson("{$pullAll : {a: [2, 3, 1, 'r', {r: 1, b: 2}, 'a']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: []}}}"));
}

TEST_F(PullAllNodeTest, ApplyWithCollator) {
    auto update = fromjson("{$pullAll : {a: ['FOO', 'BAR']}}");
    auto collator =
        std::make_unique<CollatorInterfaceMock>(CollatorInterfaceMock::MockType::kToLowerString);
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    expCtx->setCollator(std::move(collator));
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: ['foo', 'bar', 'baz']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(getIndexAffectedFromLogEntry());
    ASSERT_EQUALS(fromjson("{a: ['baz']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {u: {a: ['baz']}}}"));
}

TEST_F(PullAllNodeTest, ApplyAfterSetCollator) {
    auto update = fromjson("{$pullAll : {a: ['FOO', 'BAR']}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], expCtx));

    // First without a collator.
    mutablebson::Document doc(fromjson("{a: ['foo', 'bar', 'baz']}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_EQUALS(fromjson("{a: ['foo', 'bar', 'baz']}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    // Now with a collator.
    CollatorInterfaceMock mockCollator(CollatorInterfaceMock::MockType::kToLowerString);
    node.setCollator(&mockCollator);
    mutablebson::Document doc2(fromjson("{a: ['foo', 'bar', 'baz']}"));
    resetApplyParams();
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    result = node.apply(getApplyParams(doc2.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: ['baz']}"), doc2);
    ASSERT_FALSE(doc2.isInPlaceModeEnabled());
}

}  // namespace
}  // namespace mongo
