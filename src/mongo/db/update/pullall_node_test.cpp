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

#include "mongo/db/update/pullall_node.h"

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

using PullAllNodeTest = UpdateTestFixture;
using mongo::mutablebson::countChildren;
using mongo::mutablebson::Element;

TEST(PullAllNodeTest, InitWithIntFails) {
    auto update = fromjson("{$pullAll: {a: 1}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullAllNodeTest, InitWithStringFails) {
    auto update = fromjson("{$pullAll: {a: 'test'}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullAllNodeTest, InitWithObjectFails) {
    auto update = fromjson("{$pullAll: {a: {}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], expCtx);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullAllNodeTest, InitWithBoolFails) {
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
    ASSERT_FALSE(result.indexesAffected);
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
    ASSERT_FALSE(result.indexesAffected);
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
    ASSERT_TRUE(result.indexesAffected);
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
    ASSERT_FALSE(result.indexesAffected);
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
    ASSERT_FALSE(result.indexesAffected);
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
    ASSERT_TRUE(result.indexesAffected);
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
    ASSERT_TRUE(result.indexesAffected);
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
    ASSERT_TRUE(result.indexesAffected);
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
    ASSERT_TRUE(result.indexesAffected);
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
    ASSERT_TRUE(result.indexesAffected);
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
    ASSERT_FALSE(result.indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['baz']}"), doc2);
    ASSERT_FALSE(doc2.isInPlaceModeEnabled());
}

}  // namespace
}  // namespace mongo
