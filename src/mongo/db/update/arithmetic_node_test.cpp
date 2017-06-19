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

#include "mongo/db/update/arithmetic_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using mongo::mutablebson::Document;
using mongo::mutablebson::Element;
using mongo::mutablebson::countChildren;

DEATH_TEST(ArithmeticNodeTest, InitFailsForEmptyElement, "Invariant failure modExpr.ok()") {
    auto update = fromjson("{$inc: {}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    node.init(update["$inc"].embeddedObject().firstElement(), collator).transitional_ignore();
}

TEST(ArithmeticNodeTest, InitSucceedsForNumberIntElement) {
    auto update = fromjson("{$inc: {a: NumberInt(5)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));
}

TEST(ArithmeticNodeTest, InitSucceedsForNumberLongElement) {
    auto update = fromjson("{$inc: {a: NumberLong(5)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));
}

TEST(ArithmeticNodeTest, InitSucceedsForDoubleElement) {
    auto update = fromjson("{$inc: {a: 5.1}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));
}

TEST(ArithmeticNodeTest, InitSucceedsForDecimalElement) {
    auto update = fromjson("{$inc: {a: NumberDecimal(\"5.1\")}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));
}

TEST(ArithmeticNodeTest, InitFailsForNonNumericElement) {
    auto update = fromjson("{$inc: {a: 'foo'}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    Status result = node.init(update["$inc"]["a"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.code(), ErrorCodes::TypeMismatch);
    ASSERT_EQ(result.reason(), "Cannot increment with non-numeric argument: {a: \"foo\"}");
}

TEST(ArithmeticNodeTest, InitFailsForObjectElement) {
    auto update = fromjson("{$mul: {a: {b: 6}}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    Status result = node.init(update["$mul"]["a"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.code(), ErrorCodes::TypeMismatch);
    ASSERT_EQ(result.reason(), "Cannot multiply with non-numeric argument: {a: { b: 6 }}");
}

TEST(ArithmeticNodeTest, InitFailsForArrayElement) {
    auto update = fromjson("{$mul: {a: []}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    Status result = node.init(update["$mul"]["a"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.code(), ErrorCodes::TypeMismatch);
    ASSERT_EQ(result.reason(), "Cannot multiply with non-numeric argument: {a: []}");
}

TEST(ArithmeticNodeTest, ApplyIncNoOp) {
    auto update = fromjson("{$inc: {a: 0}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: 5}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyMulNoOp) {
    auto update = fromjson("{$mul: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a"], collator));

    Document doc(fromjson("{a: 5}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyRoundingNoOp) {
    auto update = fromjson("{$inc: {a: 1.0}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: 6.022e23}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 6.022e23}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyEmptyPathToCreate) {
    auto update = fromjson("{$inc: {a: 6}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: 5}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 11}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 11}}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyCreatePath) {
    auto update = fromjson("{$inc: {'a.b.c': 6}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b.c"], collator));

    Document doc(fromjson("{a: {d: 5}}"));
    FieldRef pathToCreate("b.c");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {d: 5, b: {c: 6}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {'a.b.c': 6}}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyExtendPath) {
    auto update = fromjson("{$inc: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{a: {c: 1}}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {c: 1, b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyCreatePathFromRoot) {
    auto update = fromjson("{$inc: {'a.b': 6}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{c: 5}"));
    FieldRef pathToCreate("a.b");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root(),
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{c: 5, a: {b: 6}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {'a.b': 6}}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyPositional) {
    auto update = fromjson("{$inc: {'a.$': 6}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.$"], collator));

    Document doc(fromjson("{a: [0, 1, 2]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.1");
    StringData matchedField("1");
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["1"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 7, 2]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {'a.1': 7}}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyNonViablePathToInc) {
    auto update = fromjson("{$inc: {'a.b': 5}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{a: 5}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root()["a"],
                                           &pathToCreate,
                                           &pathTaken,
                                           matchedField,
                                           fromReplication,
                                           &indexData,
                                           &logBuilder,
                                           &indexesAffected,
                                           &noop),
                                UserException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field 'b' in element {a: 5}");
}

TEST(ArithmeticNodeTest, ApplyNonViablePathToCreateFromReplicationIsNoOp) {
    auto update = fromjson("{$inc: {'a.b': 5}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{a: 5}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyNoIndexDataNoLogBuilder) {
    auto update = fromjson("{$inc: {a: 6}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: 5}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 11}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyDoesNotAffectIndexes) {
    auto update = fromjson("{$inc: {a: 6}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: 5}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 11}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, IncTypePromotionIsNotANoOp) {
    auto update = fromjson("{$inc: {a: NumberLong(0)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: NumberInt(2)}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberLong(2)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, MulTypePromotionIsNotANoOp) {
    auto update = fromjson("{$mul: {a: NumberLong(1)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a"], collator));

    Document doc(fromjson("{a: NumberInt(2)}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberLong(2)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, TypePromotionFromIntToDecimalIsNotANoOp) {
    auto update = fromjson("{$inc: {a: NumberDecimal(\"0.0\")}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: NumberInt(5)}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberDecimal(\"5.0\")}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: NumberDecimal(\"5.0\")}}"), logDoc);
}

TEST(ArithmeticNodeTest, TypePromotionFromLongToDecimalIsNotANoOp) {
    auto update = fromjson("{$inc: {a: NumberDecimal(\"0.0\")}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: NumberLong(5)}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberDecimal(\"5.0\")}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: NumberDecimal(\"5.0\")}}"), logDoc);
}

TEST(ArithmeticNodeTest, TypePromotionFromDoubleToDecimalIsNotANoOp) {
    auto update = fromjson("{$inc: {a: NumberDecimal(\"0.0\")}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: 5.25}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberDecimal(\"5.25\")}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: NumberDecimal(\"5.25\")}}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyPromoteToFloatingPoint) {
    auto update = fromjson("{$inc: {a: 0.2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: NumberLong(1)}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1.2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, IncrementedDecimalStaysDecimal) {
    auto update = fromjson("{$inc: {a: 5.25}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: NumberDecimal(\"6.25\")}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberDecimal(\"11.5\")}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: NumberDecimal(\"11.5\")}}"), logDoc);
}

TEST(ArithmeticNodeTest, OverflowIntToLong) {
    auto update = fromjson("{$inc: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    const int initialValue = std::numeric_limits<int>::max();
    Document doc(BSON("a" << initialValue));
    ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    ASSERT_EQUALS(BSON("a" << static_cast<long long>(initialValue) + 1), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, UnderflowIntToLong) {
    auto update = fromjson("{$inc: {a: -1}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    const int initialValue = std::numeric_limits<int>::min();
    Document doc(BSON("a" << initialValue));
    ASSERT_EQUALS(mongo::NumberInt, doc.root()["a"].getType());
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(mongo::NumberLong, doc.root()["a"].getType());
    ASSERT_EQUALS(BSON("a" << static_cast<long long>(initialValue) - 1), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, IncModeCanBeReused) {
    auto update = fromjson("{$inc: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc1(fromjson("{a: 1}"));
    Document doc2(fromjson("{a: 2}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc1.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc1);
    ASSERT_TRUE(doc1.isInPlaceModeEnabled());

    indexesAffected = false;
    noop = false;
    node.apply(doc2.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 3}"), doc2);
    ASSERT_TRUE(doc1.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, CreatedNumberHasSameTypeAsInc) {
    auto update = fromjson("{$inc: {a: NumberLong(5)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{b: 6}"));
    FieldRef pathToCreate("a");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root(),
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 6, a: NumberLong(5)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, CreatedNumberHasSameTypeAsMul) {
    auto update = fromjson("{$mul: {a: NumberLong(5)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a"], collator));

    Document doc(fromjson("{b: 6}"));
    FieldRef pathToCreate("a");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root(),
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 6, a: NumberLong(0)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyEmptyDocument) {
    auto update = fromjson("{$inc: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{}"));
    FieldRef pathToCreate("a");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root(),
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyIncToObjectFails) {
    auto update = fromjson("{$inc: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{_id: 'test_object', a: {b: 1}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root()["a"],
                                           &pathToCreate,
                                           &pathTaken,
                                           matchedField,
                                           fromReplication,
                                           &indexData,
                                           logBuilder,
                                           &indexesAffected,
                                           &noop),
                                UserException,
                                ErrorCodes::TypeMismatch,
                                "Cannot apply $inc to a value of non-numeric type. {_id: "
                                "\"test_object\"} has the field 'a' of non-numeric type object");
}

TEST(ArithmeticNodeTest, ApplyIncToArrayFails) {
    auto update = fromjson("{$inc: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{_id: 'test_object', a: []}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root()["a"],
                                           &pathToCreate,
                                           &pathTaken,
                                           matchedField,
                                           fromReplication,
                                           &indexData,
                                           logBuilder,
                                           &indexesAffected,
                                           &noop),
                                UserException,
                                ErrorCodes::TypeMismatch,
                                "Cannot apply $inc to a value of non-numeric type. {_id: "
                                "\"test_object\"} has the field 'a' of non-numeric type array");
}

TEST(ArithmeticNodeTest, ApplyIncToStringFails) {
    auto update = fromjson("{$inc: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{_id: 'test_object', a: \"foo\"}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root()["a"],
                                           &pathToCreate,
                                           &pathTaken,
                                           matchedField,
                                           fromReplication,
                                           &indexData,
                                           logBuilder,
                                           &indexesAffected,
                                           &noop),
                                UserException,
                                ErrorCodes::TypeMismatch,
                                "Cannot apply $inc to a value of non-numeric type. {_id: "
                                "\"test_object\"} has the field 'a' of non-numeric type string");
}

TEST(ArithmeticNodeTest, ApplyNewPath) {
    auto update = fromjson("{$inc: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{b: 1}"));
    FieldRef pathToCreate("a");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root(),
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 1, a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyEmptyIndexData) {
    auto update = fromjson("{$inc: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a"], collator));

    Document doc(fromjson("{a: 1}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_EQUALS(fromjson("{a: 3}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: 3}}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyNoOpDottedPath) {
    auto update = fromjson("{$inc: {'a.b': 0}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{a: {b: 2}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["b"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b : 2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, TypePromotionOnDottedPathIsNotANoOp) {
    auto update = fromjson("{$inc: {'a.b': NumberLong(0)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{a: {b: NumberInt(2)}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["b"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b : NumberLong(2)}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyPathNotViableArray) {
    auto update = fromjson("{$inc: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{a:[{b:1}]}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root()["a"],
                                           &pathToCreate,
                                           &pathTaken,
                                           matchedField,
                                           fromReplication,
                                           indexData,
                                           logBuilder,
                                           &indexesAffected,
                                           &noop),
                                UserException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field 'b' in element {a: [ { b: 1 } ]}");
}

TEST(ArithmeticNodeTest, ApplyInPlaceDottedPath) {
    auto update = fromjson("{$inc: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{a: {b: 1}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["b"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 3}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyPromotionDottedPath) {
    auto update = fromjson("{$inc: {'a.b': NumberLong(2)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{a: {b: NumberInt(3)}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["b"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: NumberLong(5)}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyDottedPathEmptyDoc) {
    auto update = fromjson("{$inc: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{}"));
    FieldRef pathToCreate("a.b");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root(),
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyFieldWithDot) {
    auto update = fromjson("{$inc: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.b"], collator));

    Document doc(fromjson("{'a.b':4}"));
    FieldRef pathToCreate("a.b");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root(),
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{'a.b':4, a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyNoOpArrayIndex) {
    auto update = fromjson("{$inc: {'a.2.b': 0}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.2.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["2"]["b"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, TypePromotionInArrayIsNotANoOp) {
    auto update = fromjson("{$set: {'a.2.b': NumberLong(0)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b: NumberInt(0)},{b: NumberInt(1)},{b: NumberInt(2)}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.2.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["2"]["b"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: NumberLong(2)}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyNonViablePathThroughArray) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: 0}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root()["a"],
                                           &pathToCreate,
                                           &pathTaken,
                                           matchedField,
                                           fromReplication,
                                           indexData,
                                           logBuilder,
                                           &indexesAffected,
                                           &noop),
                                UserException,
                                ErrorCodes::PathNotViable,
                                "Cannot create field '2' in element {a: 0}");
}

TEST(ArithmeticNodeTest, ApplyInPlaceArrayIndex) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 1}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.2.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["2"]["b"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 3}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyAppendArray) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b: 0},{b: 1}]}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyPaddingArray) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b: 0}]}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [{b: 0},null,{b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyNumericObject) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: {b: 0}}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 0, '2': {b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyNumericField) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: {'2': {b: 1}}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.2.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["2"]["b"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {'2': {b: 3}}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyExtendNumericField) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: {'2': {c: 1}}}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("a.2");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["2"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {'2': {c: 1, b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyNumericFieldToEmptyObject) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: {}}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyEmptyArray) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: []}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               &indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [null, null, {b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(ArithmeticNodeTest, ApplyLogDottedPath) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b:0}, {b:1}]}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_EQUALS(fromjson("{a: [{b:0}, {b:1}, {b:2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.2.b': 2}}"), logDoc);
}

TEST(ArithmeticNodeTest, LogEmptyArray) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: []}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_EQUALS(fromjson("{a: [null, null, {b:2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.2.b': 2}}"), logDoc);
}

TEST(ArithmeticNodeTest, LogEmptyObject) {
    auto update = fromjson("{$inc: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kAdd);
    ASSERT_OK(node.init(update["$inc"]["a.2.b"], collator));

    Document doc(fromjson("{a: {}}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'a.2.b': 2}}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyDeserializedDocNotNoOp) {
    auto update = fromjson("{$mul: {b: NumberInt(1)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["b"], collator));

    Document doc(fromjson("{a: 1}"));
    // De-serialize the int.
    doc.root()["a"].setValueInt(1).transitional_ignore();

    FieldRef pathToCreate("b");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root(),
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: NumberInt(0)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {b: NumberInt(0)}}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyToDeserializedDocNoOp) {
    auto update = fromjson("{$mul: {a: NumberInt(1)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a"], collator));

    Document doc(fromjson("{a: 1}"));
    // De-serialize the int.
    doc.root()["a"].setValueInt(2).transitional_ignore();

    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: NumberInt(2)}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyToDeserializedDocNestedNoop) {
    auto update = fromjson("{$mul: {'a.b': NumberInt(1)}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a.b"], collator));

    Document doc{BSONObj()};
    // De-serialize the int.
    doc.root().appendObject("a", BSON("b" << static_cast<int>(1))).transitional_ignore();

    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["b"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: NumberInt(1)}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(ArithmeticNodeTest, ApplyToDeserializedDocNestedNotNoop) {
    auto update = fromjson("{$mul: {'a.b': 3}}");
    const CollatorInterface* collator = nullptr;
    ArithmeticNode node(ArithmeticNode::ArithmeticOp::kMultiply);
    ASSERT_OK(node.init(update["$mul"]["a.b"], collator));

    Document doc{BSONObj()};
    // De-serialize the int.
    doc.root().appendObject("a", BSON("b" << static_cast<int>(1))).transitional_ignore();

    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["b"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 3}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {'a.b': 3}}"), logDoc);
}

}  // namespace
}  // namespace mongo
