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

#include "mongo/db/update/addtoset_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using mongo::mutablebson::Document;
using mongo::mutablebson::Element;
using mongo::mutablebson::countChildren;

DEATH_TEST(AddToSetNodeTest, InitFailsForEmptyElement, "Invariant failure modExpr.ok()") {
    auto update = fromjson("{$addToSet: {}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    node.init(update["$addToSet"].embeddedObject().firstElement(), collator).transitional_ignore();
}

TEST(AddToSetNodeTest, InitFailsIfEachIsNotArray) {
    auto update = fromjson("{$addToSet: {a: {$each: {}}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    auto result = node.init(update["$addToSet"]["a"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.code(), ErrorCodes::TypeMismatch);
    ASSERT_EQ(result.reason(),
              "The argument to $each in $addToSet must be an array but it was of type object");
}

TEST(AddToSetNodeTest, InitFailsIfThereAreFieldsAfterEach) {
    auto update = fromjson("{$addToSet: {a: {$each: [], bad: 1}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    auto result = node.init(update["$addToSet"]["a"], collator);
    ASSERT_NOT_OK(result);
    ASSERT_EQ(result.code(), ErrorCodes::BadValue);
    ASSERT_EQ(result.reason(),
              "Found unexpected fields after $each in $addToSet: { $each: [], bad: 1 }");
}

TEST(AddToSetNodeTest, InitSucceedsWithFailsBeforeEach) {
    auto update = fromjson("{$addToSet: {a: {other: 1, $each: []}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));
}

TEST(AddToSetNodeTest, InitSucceedsWithObject) {
    auto update = fromjson("{$addToSet: {a: {}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));
}

TEST(AddToSetNodeTest, InitSucceedsWithArray) {
    auto update = fromjson("{$addToSet: {a: []}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));
}

TEST(AddToSetNodeTest, InitSucceedsWithScaler) {
    auto update = fromjson("{$addToSet: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));
}

TEST(AddToSetNodeTest, ApplyFailsOnNonArray) {
    auto update = fromjson("{$addToSet: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: 2}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData = nullptr;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(doc.root()["a"],
                   &pathToCreate,
                   &pathTaken,
                   matchedField,
                   fromReplication,
                   validateForStorage,
                   immutablePaths,
                   indexData,
                   logBuilder,
                   &indexesAffected,
                   &noop),
        UserException,
        ErrorCodes::BadValue,
        "Cannot apply $addToSet to non-array field. Field named 'a' has non-array type int");
}

TEST(AddToSetNodeTest, ApplyNonEach) {
    auto update = fromjson("{$addToSet: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: [0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: [0, 1]}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyNonEachArray) {
    auto update = fromjson("{$addToSet: {a: [1]}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: [0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, [1]]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: [0, [1]]}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyEach) {
    auto update = fromjson("{$addToSet: {a: {$each: [1, 2]}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: [0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1, 2]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: [0, 1, 2]}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyToEmptyArray) {
    auto update = fromjson("{$addToSet: {a: {$each: [1, 2]}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: []}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1, 2]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: [1, 2]}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyDeduplicateElementsToAdd) {
    auto update = fromjson("{$addToSet: {a: {$each: [1, 1]}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: [0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: [0, 1]}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyDoNotAddExistingElements) {
    auto update = fromjson("{$addToSet: {a: {$each: [0, 1]}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: [0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: [0, 1]}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyDoNotDeduplicateExistingElements) {
    auto update = fromjson("{$addToSet: {a: {$each: [1]}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: [0, 0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: [0, 0, 1]}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyNoElementsToAdd) {
    auto update = fromjson("{$addToSet: {a: {$each: []}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: [0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyNoNonDuplicateElementsToAdd) {
    auto update = fromjson("{$addToSet: {a: {$each: [0]}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: [0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyCreateArray) {
    auto update = fromjson("{$addToSet: {a: {$each: [0, 1]}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{}"));
    FieldRef pathToCreate("a");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: [0, 1]}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyCreateEmptyArrayIsNotNoop) {
    auto update = fromjson("{$addToSet: {a: {$each: []}}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{}"));
    FieldRef pathToCreate("a");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: []}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyDeduplicationOfElementsToAddRespectsCollation) {
    auto update = fromjson("{$addToSet: {a: {$each: ['abc', 'ABC', 'def', 'abc']}}}");
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], &collator));

    Document doc(fromjson("{a: []}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['abc', 'def']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: ['abc', 'def']}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyComparisonToExistingElementsRespectsCollation) {
    auto update = fromjson("{$addToSet: {a: {$each: ['abc', 'def']}}}");
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], &collator));

    Document doc(fromjson("{a: ['ABC']}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['ABC', 'def']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: ['ABC', 'def']}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyRespectsCollationFromSetCollator) {
    auto update = fromjson("{$addToSet: {a: {$each: ['abc', 'ABC', 'def', 'abc']}}}");
    const CollatorInterface* binaryComparisonCollator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], binaryComparisonCollator));

    const CollatorInterfaceMock caseInsensitiveCollator(
        CollatorInterfaceMock::MockType::kToLowerString);
    node.setCollator(&caseInsensitiveCollator);

    Document doc(fromjson("{a: []}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: ['abc', 'def']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: ['abc', 'def']}}"), logDoc);
}

DEATH_TEST(AddToSetNodeTest, CannotSetCollatorIfCollatorIsNonNull, "Invariant failure !_collator") {
    auto update = fromjson("{$addToSet: {a: 1}}");
    const CollatorInterfaceMock caseInsensitiveCollator(
        CollatorInterfaceMock::MockType::kToLowerString);
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], &caseInsensitiveCollator));
    node.setCollator(&caseInsensitiveCollator);
}

DEATH_TEST(AddToSetNodeTest, CannotSetCollatorTwice, "Invariant failure !_collator") {
    auto update = fromjson("{$addToSet: {a: 1}}");
    const CollatorInterface* binaryComparisonCollator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], binaryComparisonCollator));

    const CollatorInterfaceMock caseInsensitiveCollator(
        CollatorInterfaceMock::MockType::kToLowerString);
    node.setCollator(&caseInsensitiveCollator);
    node.setCollator(&caseInsensitiveCollator);
}

TEST(AddToSetNodeTest, ApplyNestedArray) {
    auto update = fromjson("{ $addToSet : { 'a.1' : 1 } }");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a.1"], collator));

    Document doc(fromjson("{ _id : 1, a : [ 1, [ ] ] }"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.1");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
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
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{ _id : 1, a : [ 1, [ 1 ] ] }"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{ $set : { 'a.1' : [ 1 ] } }"), logDoc);
}

TEST(AddToSetNodeTest, ApplyIndexesNotAffected) {
    auto update = fromjson("{$addToSet: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: [0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("b");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               validateForStorage,
               immutablePaths,
               &indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: [0, 1]}}"), logDoc);
}

TEST(AddToSetNodeTest, ApplyNoIndexDataOrLogBuilder) {
    auto update = fromjson("{$addToSet: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    AddToSetNode node;
    ASSERT_OK(node.init(update["$addToSet"]["a"], collator));

    Document doc(fromjson("{a: [0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData = nullptr;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"],
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               validateForStorage,
               immutablePaths,
               indexData,
               logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, 1]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

}  // namespace
