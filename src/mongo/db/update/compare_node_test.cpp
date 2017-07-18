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

#include "mongo/db/update/compare_node.h"

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

DEATH_TEST(CompareNodeTest, InitFailsForEmptyElement, "Invariant failure modExpr.ok()") {
    auto update = fromjson("{$max: {}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    node.init(update["$max"].embeddedObject().firstElement(), collator).ignore();
}

TEST(CompareNodeTest, ApplyMaxSameNumber) {
    auto update = fromjson("{$max: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

    Document doc(fromjson("{a: 1}"));
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
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(CompareNodeTest, ApplyMinSameNumber) {
    auto update = fromjson("{$min: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], collator));

    Document doc(fromjson("{a: 1}"));
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
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(CompareNodeTest, ApplyMaxNumberIsLess) {
    auto update = fromjson("{$max: {a: 0}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

    Document doc(fromjson("{a: 1}"));
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
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(CompareNodeTest, ApplyMinNumberIsMore) {
    auto update = fromjson("{$min: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], collator));

    Document doc(fromjson("{a: 1}"));
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
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(CompareNodeTest, ApplyMaxSameValInt) {
    auto update = BSON("$max" << BSON("a" << 1LL));
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

    Document doc(fromjson("{a: 1.0}"));
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
    ASSERT_EQUALS(fromjson("{a: 1.0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(CompareNodeTest, ApplyMaxSameValIntZero) {
    auto update = BSON("$max" << BSON("a" << 0LL));
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

    Document doc(fromjson("{a: 0.0}"));
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
    ASSERT_EQUALS(fromjson("{a: 0.0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(CompareNodeTest, ApplyMinSameValIntZero) {
    auto update = BSON("$min" << BSON("a" << 0LL));
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], collator));

    Document doc(fromjson("{a: 0.0}"));
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
    ASSERT_EQUALS(fromjson("{a: 0.0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(CompareNodeTest, ApplyMissingFieldMinNumber) {
    auto update = fromjson("{$min: {a: 0}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], collator));

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
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 0}}"), logDoc);
}

TEST(CompareNodeTest, ApplyExistingNumberMinNumber) {
    auto update = fromjson("{$min: {a: 0}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], collator));

    Document doc(fromjson("{a: 1}"));
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
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 0}}"), logDoc);
}

TEST(CompareNodeTest, ApplyMissingFieldMaxNumber) {
    auto update = fromjson("{$max: {a: 0}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

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
    ASSERT_EQUALS(fromjson("{a: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 0}}"), logDoc);
}

TEST(CompareNodeTest, ApplyExistingNumberMaxNumber) {
    auto update = fromjson("{$max: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

    Document doc(fromjson("{a: 1}"));
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
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 2}}"), logDoc);
}

TEST(CompareNodeTest, ApplyExistingDateMaxDate) {
    auto update = fromjson("{$max: {a: {$date: 123123123}}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

    Document doc(fromjson("{a: {$date: 0}}"));
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
    ASSERT_EQUALS(fromjson("{a: {$date: 123123123}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: {$date: 123123123}}}"), logDoc);
}

TEST(CompareNodeTest, ApplyExistingEmbeddedDocMaxDoc) {
    auto update = fromjson("{$max: {a: {b: 3}}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

    Document doc(fromjson("{a: {b: 2}}"));
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
    ASSERT_EQUALS(fromjson("{a: {b: 3}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: {b: 3}}}"), logDoc);
}

TEST(CompareNodeTest, ApplyExistingEmbeddedDocMaxNumber) {
    auto update = fromjson("{$max: {a: 3}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

    Document doc(fromjson("{a: {b: 2}}"));
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
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(CompareNodeTest, ApplyMinRespectsCollation) {
    auto update = fromjson("{$min: {a: 'dba'}}");
    const CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], &collator));

    Document doc(fromjson("{a: 'cbc'}"));
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
    ASSERT_EQUALS(fromjson("{a: 'dba'}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 'dba'}}"), logDoc);
}

TEST(CompareNodeTest, ApplyMinRespectsCollationFromSetCollator) {
    auto update = fromjson("{$min: {a: 'dba'}}");
    const CollatorInterface* binaryComparisonCollator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMin);
    ASSERT_OK(node.init(update["$min"]["a"], binaryComparisonCollator));

    const CollatorInterfaceMock reverseStringCollator(
        CollatorInterfaceMock::MockType::kReverseString);
    node.setCollator(&reverseStringCollator);

    Document doc(fromjson("{a: 'cbc'}"));
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
    ASSERT_EQUALS(fromjson("{a: 'dba'}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 'dba'}}"), logDoc);
}

TEST(CompareNodeTest, ApplyMaxRespectsCollationFromSetCollator) {
    auto update = fromjson("{$max: {a: 'abd'}}");
    const CollatorInterface* binaryComparisonCollator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], binaryComparisonCollator));

    const CollatorInterfaceMock reverseStringCollator(
        CollatorInterfaceMock::MockType::kReverseString);
    node.setCollator(&reverseStringCollator);

    Document doc(fromjson("{a: 'cbc'}"));
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
    ASSERT_EQUALS(fromjson("{a: 'abd'}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 'abd'}}"), logDoc);
}

DEATH_TEST(CompareNodeTest, CannotSetCollatorIfCollatorIsNonNull, "Invariant failure !_collator") {
    auto update = fromjson("{$max: {a: 1}}");
    const CollatorInterfaceMock caseInsensitiveCollator(
        CollatorInterfaceMock::MockType::kToLowerString);
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], &caseInsensitiveCollator));
    node.setCollator(&caseInsensitiveCollator);
}

DEATH_TEST(CompareNodeTest, CannotSetCollatorTwice, "Invariant failure !_collator") {
    auto update = fromjson("{$max: {a: 1}}");
    const CollatorInterface* binaryComparisonCollator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], binaryComparisonCollator));

    const CollatorInterfaceMock caseInsensitiveCollator(
        CollatorInterfaceMock::MockType::kToLowerString);
    node.setCollator(&caseInsensitiveCollator);
    node.setCollator(&caseInsensitiveCollator);
}

TEST(CompareNodeTest, ApplyIndexesNotAffected) {
    auto update = fromjson("{$max: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

    Document doc(fromjson("{a: 0}"));
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
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 1}}"), logDoc);
}

TEST(CompareNodeTest, ApplyNoIndexDataOrLogBuilder) {
    auto update = fromjson("{$max: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    CompareNode node(CompareNode::CompareMode::kMax);
    ASSERT_OK(node.init(update["$max"]["a"], collator));

    Document doc(fromjson("{a: 0}"));
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
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

}  // namespace
