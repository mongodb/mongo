// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/update/bit_node.h"

#include "mongo/bson/json.h"
#include "mongo/db/exec/mutable_bson/document.h"
#include "mongo/db/exec/mutable_bson/mutable_bson_test_utils.h"  // IWYU pragma: keep
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_executor.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/unittest.h"

#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

using BitNodeTest = UpdateTestFixture;

TEST(SimpleBitNodeTest, InitWithDoubleFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: 0}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, InitWithStringFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: ''}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, InitWithArrayFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: []}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, InitWithEmptyDocumentFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, InitWithUnknownOperatorFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {foo: 4}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, InitWithArrayArgumentToOperatorFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {or: []}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, InitWithStringArgumentToOperatorFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {or: 'foo'}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, InitWithDoubleArgumentToOperatorFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {or: 1.0}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, InitWithDecimalArgumentToOperatorFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {or: NumberDecimal(\"1.0\")}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, ParsesAndInt) {
    auto update = fromjson("{$bit: {a: {and: NumberInt(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, ParsesOrInt) {
    auto update = fromjson("{$bit: {a: {or: NumberInt(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, ParsesXorInt) {
    auto update = fromjson("{$bit: {a: {xor: NumberInt(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, ParsesAndLong) {
    auto update = fromjson("{$bit: {a: {and: NumberLong(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, ParsesOrLong) {
    auto update = fromjson("{$bit: {a: {or: NumberLong(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(SimpleBitNodeTest, ParsesXorLong) {
    auto update = fromjson("{$bit: {a: {xor: NumberLong(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST_F(BitNodeTest, ApplyAndLogEmptyDocumentAnd) {
    auto update = fromjson("{$bit: {a: {and: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: 0}}}"));
}

TEST_F(BitNodeTest, ApplyAndLogEmptyDocumentOr) {
    auto update = fromjson("{$bit: {a: {or: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: 1}}}"));
}

TEST_F(BitNodeTest, ApplyAndLogEmptyDocumentXor) {
    auto update = fromjson("{$bit: {a: {xor: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    auto result = node.apply(getApplyParams(doc.root()), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());

    assertOplogEntry(fromjson("{$v: 2, diff: {i: {a: 1}}}"));
}

TEST_F(BitNodeTest, ApplyAndLogSimpleDocumentAnd) {
    auto update = BSON("$bit" << BSON("a" << BSON("and" << 0b0110)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 0b0101));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(BSON("a" << 0b0100), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(BSON("$v" << 2 << "diff" << BSON("u" << BSON("a" << 0b0100))));
}

TEST_F(BitNodeTest, ApplyAndLogSimpleDocumentOr) {
    auto update = BSON("$bit" << BSON("a" << BSON("or" << 0b0110)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 0b0101));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(BSON("a" << 0b0111), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(BSON("$v" << 2 << "diff" << BSON("u" << BSON("a" << 0b0111))));
}

TEST_F(BitNodeTest, ApplyAndLogSimpleDocumentXor) {
    auto update = BSON("$bit" << BSON("a" << BSON("xor" << 0b0110)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 0b0101));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(BSON("a" << 0b0011), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(BSON("$v" << 2 << "diff" << BSON("u" << BSON("a" << 0b0011))));
}

TEST_F(BitNodeTest, ApplyShouldReportNoOp) {
    auto update = BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(1))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 1));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_TRUE(result.noop);
    ASSERT_EQUALS(BSON("a" << static_cast<int>(1)), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntryIsNoop();
}

TEST_F(BitNodeTest, ApplyMultipleBitOps) {
    // End-of-line comments help clang-format break up this line more readably.
    auto update = BSON("$bit" << BSON("a" << BSON("and" << 0b1111000011110000  //
                                                        <<                     //
                                                  "or" << 0b1100110011001100   //
                                                        <<                     //
                                                  "xor" << 0b1010101010101010)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 0b1111111100000000));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(BSON("a" << 0b0101011001100110), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(BSON("$v" << 2 << "diff" << BSON("u" << BSON("a" << 0b0101011001100110))));
}

TEST_F(BitNodeTest, ApplyRepeatedBitOps) {
    auto update = BSON("$bit" << BSON("a" << BSON("xor" << 0b11001100 << "xor" << 0b10101010)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 0b11110000));
    setPathTaken(makeRuntimeUpdatePathForTest("a"));
    auto result = node.apply(getApplyParams(doc.root()["a"]), getUpdateNodeApplyParams());
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(BSON("a" << 0b10010110), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    assertOplogEntry(BSON("$v" << 2 << "diff" << BSON("u" << BSON("a" << 0b10010110))));
}

}  // namespace
}  // namespace mongo
