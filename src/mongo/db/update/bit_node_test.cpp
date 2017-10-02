/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/update/bit_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/update/update_node_test_fixture.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using BitNodeTest = UpdateNodeTest;
using mongo::mutablebson::Element;
using mongo::mutablebson::countChildren;

TEST(BitNodeTest, InitWithDoubleFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: 0}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, InitWithStringFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: ''}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, InitWithArrayFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: []}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, InitWithEmptyDocumentFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {}}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, InitWithUnknownOperatorFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {foo: 4}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, InitWithArrayArgumentToOperatorFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {or: []}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, InitWithStringArgumentToOperatorFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {or: 'foo'}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, InitWithDoubleArgumentToOperatorFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {or: 1.0}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, InitWithDecimalArgumentToOperatorFails) {
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto update = fromjson("{$bit: {a: {or: NumberDecimal(\"1.0\")}}}");
    BitNode node;
    ASSERT_NOT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, ParsesAndInt) {
    auto update = fromjson("{$bit: {a: {and: NumberInt(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, ParsesOrInt) {
    auto update = fromjson("{$bit: {a: {or: NumberInt(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, ParsesXorInt) {
    auto update = fromjson("{$bit: {a: {xor: NumberInt(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, ParsesAndLong) {
    auto update = fromjson("{$bit: {a: {and: NumberLong(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, ParsesOrLong) {
    auto update = fromjson("{$bit: {a: {or: NumberLong(1)}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));
}

TEST(BitNodeTest, ParsesXorLong) {
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
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 0}}"), getLogDoc());
}

TEST_F(BitNodeTest, ApplyAndLogEmptyDocumentOr) {
    auto update = fromjson("{$bit: {a: {or: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 1}}"), getLogDoc());
}

TEST_F(BitNodeTest, ApplyAndLogEmptyDocumentXor) {
    auto update = fromjson("{$bit: {a: {xor: 1}}}");
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(fromjson("{}"));
    setPathToCreate("a");
    auto result = node.apply(getApplyParams(doc.root()));
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 1}}"), getLogDoc());
}

TEST_F(BitNodeTest, ApplyAndLogSimpleDocumentAnd) {
    auto update = BSON("$bit" << BSON("a" << BSON("and" << 0b0110)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 0b0101));
    setPathTaken("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(BSON("a" << 0b0100), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(BSON("$set" << BSON("a" << 0b0100)), getLogDoc());
}

TEST_F(BitNodeTest, ApplyAndLogSimpleDocumentOr) {
    auto update = BSON("$bit" << BSON("a" << BSON("or" << 0b0110)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 0b0101));
    setPathTaken("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(BSON("a" << 0b0111), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(BSON("$set" << BSON("a" << 0b0111)), getLogDoc());
}

TEST_F(BitNodeTest, ApplyAndLogSimpleDocumentXor) {
    auto update = BSON("$bit" << BSON("a" << BSON("xor" << 0b0110)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 0b0101));
    setPathTaken("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(BSON("a" << 0b0011), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(BSON("$set" << BSON("a" << 0b0011)), getLogDoc());
}

TEST_F(BitNodeTest, ApplyShouldReportNoOp) {
    auto update = BSON("$bit" << BSON("a" << BSON("and" << static_cast<int>(1))));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 1));
    setPathTaken("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_TRUE(result.noop);
    ASSERT_EQUALS(BSON("a" << static_cast<int>(1)), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), getLogDoc());
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
    setPathTaken("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(BSON("a" << 0b0101011001100110), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(BSON("$set" << BSON("a" << 0b0101011001100110)), getLogDoc());
}

TEST_F(BitNodeTest, ApplyRepeatedBitOps) {
    auto update = BSON("$bit" << BSON("a" << BSON("xor" << 0b11001100 << "xor" << 0b10101010)));
    boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    BitNode node;
    ASSERT_OK(node.init(update["$bit"]["a"], expCtx));

    mutablebson::Document doc(BSON("a" << 0b11110000));
    setPathTaken("a");
    auto result = node.apply(getApplyParams(doc.root()["a"]));
    ASSERT_FALSE(result.noop);
    ASSERT_EQUALS(BSON("a" << 0b10010110), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(BSON("$set" << BSON("a" << 0b10010110)), getLogDoc());
}

}  // namespace
}  // namepace mongo
