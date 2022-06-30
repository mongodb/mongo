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

#include "mongo/db/update/current_date_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

void assertOplogEntryIsUpdateOfExpectedType(const BSONObj& obj,
                                            StringData fieldName,
                                            BSONType expectedType = BSONType::Date) {
    ASSERT_EQUALS(obj.nFields(), 2);
    ASSERT_EQUALS(obj["$v"].numberInt(), 2);
    ASSERT_EQUALS(obj["diff"]["u"][fieldName].type(), expectedType);
}

using CurrentDateNodeTest = UpdateTestFixture;

DEATH_TEST_REGEX(CurrentDateNodeTest,
                 InitFailsForEmptyElement,
                 R"#(Invariant failure.*modExpr.ok\(\))#") {
    auto update = fromjson("{$currentDate: {}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CurrentDateNode node;
    node.init(update["$currentDate"].embeddedObject().firstElement(), expCtx).ignore();
}

TEST(CurrentDateNodeTest, InitWithNonBoolNonObjectFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$currentDate: {a: 0}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], expCtx));
}

TEST(CurrentDateNodeTest, InitWithTrueSucceeds) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$currentDate: {a: true}}");
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));
}

TEST(CurrentDateNodeTest, InitWithFalseSucceeds) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$currentDate: {a: false}}");
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));
}

TEST(CurrentDateNodeTest, InitWithoutTypeFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$currentDate: {a: {}}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], expCtx));
}

TEST(CurrentDateNodeTest, InitWithNonStringTypeFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$currentDate: {a: {$type: 1}}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], expCtx));
}

TEST(CurrentDateNodeTest, InitWithBadValueTypeFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$currentDate: {a: {$type: 'bad'}}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], expCtx));
}

TEST(CurrentDateNodeTest, InitWithTypeDateSucceeds) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$currentDate: {a: {$type: 'date'}}}");
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));
}

TEST(CurrentDateNodeTest, InitWithTypeTimestampSucceeds) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$currentDate: {a: {$type: 'timestamp'}}}");
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));
}

TEST(CurrentDateNodeTest, InitWithExtraFieldBeforeFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$currentDate: {a: {$bad: 1, $type: 'date'}}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], expCtx));
}

TEST(CurrentDateNodeTest, InitWithExtraFieldAfterFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$currentDate: {a: {$type: 'date', $bad: 1}}}");
    CurrentDateNode node;
    ASSERT_NOT_OK(node.init(update["$currentDate"]["a"], expCtx));
}

TEST_F(CurrentDateNodeTest, ApplyTrue) {
    auto update = fromjson("{$currentDate: {a: true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);

    assertOplogEntryIsUpdateOfExpectedType(getOplogEntry(), "a");
}

TEST_F(CurrentDateNodeTest, ApplyFalse) {
    auto update = fromjson("{$currentDate: {a: false}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);

    assertOplogEntryIsUpdateOfExpectedType(getOplogEntry(), "a");
}

TEST_F(CurrentDateNodeTest, ApplyDate) {
    auto update = fromjson("{$currentDate: {a: {$type: 'date'}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);

    assertOplogEntryIsUpdateOfExpectedType(getOplogEntry(), "a");
}

TEST_F(CurrentDateNodeTest, ApplyTimestamp) {
    auto update = fromjson("{$currentDate: {a: {$type: 'timestamp'}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::bsonTimestamp);

    assertOplogEntryIsUpdateOfExpectedType(getOplogEntry(), "a", BSONType::bsonTimestamp);
}

TEST_F(CurrentDateNodeTest, ApplyFieldDoesNotExist) {
    auto update = fromjson("{$currentDate: {a: true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    addIndexedPath("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_TRUE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);

    ASSERT_EQUALS(getOplogEntry().nFields(), 2);
    ASSERT_EQUALS(getOplogEntry()["$v"].numberInt(), 2);
    ASSERT_EQUALS(getOplogEntry()["diff"]["i"]["a"].type(), BSONType::Date);
}

TEST_F(CurrentDateNodeTest, ApplyIndexesNotAffected) {
    auto update = fromjson("{$currentDate: {a: true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    addIndexedPath("b");
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);

    assertOplogEntryIsUpdateOfExpectedType(getOplogEntry(), "a");
}

TEST_F(CurrentDateNodeTest, ApplyNoIndexDataOrLogBuilder) {
    auto update = fromjson("{$currentDate: {a: true}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    CurrentDateNode node;
    ASSERT_OK(node.init(update["$currentDate"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{a: 0}"));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    setLogBuilderToNull();
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_FALSE(result.indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 1U);
    ASSERT_TRUE(doc.root()["a"].ok());
    ASSERT_EQUALS(doc.root()["a"].getType(), BSONType::Date);
}

}  // namespace
}  // namespace mongo
