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

#include "mongo/db/update/set_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace {

using namespace mongo;
using mongo::mutablebson::Document;
using mongo::mutablebson::Element;
using mongo::mutablebson::countChildren;

DEATH_TEST(SetNodeTest, InitFailsForEmptyElement, "Invariant failure modExpr.ok()") {
    auto update = fromjson("{$set: {}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    node.init(update["$set"].embeddedObject().firstElement(), collator).transitional_ignore();
}

TEST(SetNodeTest, InitSucceedsForNonemptyElement) {
    auto update = fromjson("{$set: {a: 5}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));
}

TEST(SetNodeTest, ApplyNoOp) {
    auto update = fromjson("{$set: {a: 5}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

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

TEST(SetNodeTest, ApplyEmptyPathToCreate) {
    auto update = fromjson("{$set: {a: 6}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

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
    ASSERT_EQUALS(fromjson("{a: 6}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: 6}}"), logDoc);
}

TEST(SetNodeTest, ApplyCreatePath) {
    auto update = fromjson("{$set: {'a.b.c': 6}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b.c"], collator));

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

TEST(SetNodeTest, ApplyCreatePathFromRoot) {
    auto update = fromjson("{$set: {'a.b': 6}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

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

TEST(SetNodeTest, ApplyPositional) {
    auto update = fromjson("{$set: {'a.$': 6}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.$"], collator));

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
    ASSERT_EQUALS(fromjson("{a: [0, 6, 2]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {'a.1': 6}}"), logDoc);
}

TEST(SetNodeTest, ApplyNonViablePathToCreate) {
    auto update = fromjson("{$set: {'a.b': 5}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

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

TEST(SetNodeTest, ApplyNonViablePathToCreateFromReplicationIsNoOp) {
    auto update = fromjson("{$set: {'a.b': 5}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

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

TEST(SetNodeTest, ApplyNoIndexDataNoLogBuilder) {
    auto update = fromjson("{$set: {a: 6}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

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
    ASSERT_EQUALS(fromjson("{a: 6}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyDoesNotAffectIndexes) {
    auto update = fromjson("{$set: {a: 6}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

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
    ASSERT_EQUALS(fromjson("{a: 6}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, TypeChangeIsNotANoop) {
    auto update = fromjson("{$set: {a: NumberLong(2)}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

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

TEST(SetNodeTest, IdentityOpOnDeserializedIsNotANoOp) {
    // Apply an op that would be a no-op.
    auto update = fromjson("{$set: {a: {b : NumberInt(2)}}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

    Document doc(fromjson("{a: { b: NumberInt(0)}}"));
    // Apply a mutation to the document that will make it non-serialized.
    doc.root()["a"]["b"].setValueInt(2).transitional_ignore();

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
    ASSERT_EQUALS(fromjson("{a: {b : NumberInt(2)}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyEmptyDocument) {
    auto update = fromjson("{$set: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

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

TEST(SetNodeTest, ApplyInPlace) {
    auto update = fromjson("{$set: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

    Document doc(fromjson("{a: 1}"));
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
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyOverridePath) {
    auto update = fromjson("{$set: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

    Document doc(fromjson("{a: {b: 1}}"));
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
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyChangeType) {
    auto update = fromjson("{$set: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

    Document doc(fromjson("{a: 'str'}"));
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
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyNewPath) {
    auto update = fromjson("{$set: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

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

TEST(SetNodeTest, ApplyLog) {
    auto update = fromjson("{$set: {a: 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a"], collator));

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
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {a: 2}}"), logDoc);
}

TEST(SetNodeTest, ApplyNoOpDottedPath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

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

TEST(SetNodeTest, TypeChangeOnDottedPathIsNotANoOp) {
    auto update = fromjson("{$set: {'a.b': NumberInt(2)}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

    Document doc(fromjson("{a: {b: NumberLong(2)}}"));
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

TEST(SetNodeTest, ApplyPathNotViable) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

    Document doc(fromjson("{a:1}"));
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
                                "Cannot create field 'b' in element {a: 1}");
}

TEST(SetNodeTest, ApplyPathNotViableArrray) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

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

TEST(SetNodeTest, ApplyInPlaceDottedPath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

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
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyChangeTypeDottedPath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

    Document doc(fromjson("{a: {b: 'str'}}"));
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
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyChangePath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

    Document doc(fromjson("{a: {b: {c: 1}}}"));
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
    ASSERT_EQUALS(fromjson("{a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyExtendPath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

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

TEST(SetNodeTest, ApplyNewDottedPath) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

    Document doc(fromjson("{c: 1}"));
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
    ASSERT_EQUALS(fromjson("{c: 1, a: {b: 2}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyEmptyDoc) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

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

TEST(SetNodeTest, ApplyFieldWithDot) {
    auto update = fromjson("{$set: {'a.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b"], collator));

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

TEST(SetNodeTest, ApplyNoOpArrayIndex) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.2.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.2.b");
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

TEST(SetNodeTest, TypeChangeInArrayIsNotANoOp) {
    auto update = fromjson("{$set: {'a.2.b': NumberInt(2)}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 2.0}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.2.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.2.b");
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
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: NumberInt(2)}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyNonViablePath) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

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

TEST(SetNodeTest, ApplyInPlaceArrayIndex) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b: 0},{b: 1},{b: 1}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.2.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.2.b");
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
    ASSERT_EQUALS(fromjson("{a: [{b: 0},{b: 1},{b: 2}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyNormalArray) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b: 0},{b: 1}]}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.2.b");
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

TEST(SetNodeTest, ApplyPaddingArray) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: [{b: 0}]}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.2.b");
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

TEST(SetNodeTest, ApplyNumericObject) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: {b: 0}}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.2.b");
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

TEST(SetNodeTest, ApplyNumericField) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: {'2': {b: 1}}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.2.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.2.b");
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
    ASSERT_EQUALS(fromjson("{a: {'2': {b: 2}}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplyExtendNumericField) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: {'2': {c: 1}}}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("a.2");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.2.b");
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

TEST(SetNodeTest, ApplyEmptyObject) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: {}}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.2.b");
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

TEST(SetNodeTest, ApplyEmptyArray) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

    Document doc(fromjson("{a: []}"));
    FieldRef pathToCreate("2.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.2.b");
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

TEST(SetNodeTest, ApplyLogDottedPath) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

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

TEST(SetNodeTest, LogEmptyArray) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

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

TEST(SetNodeTest, LogEmptyObject) {
    auto update = fromjson("{$set: {'a.2.b': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.2.b"], collator));

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

TEST(SetNodeTest, ApplyNoOpComplex) {
    auto update = fromjson("{$set: {'a.1.b': {c: 1, d: 1}}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.b"], collator));

    Document doc(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, d: 1}}]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.1.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.1.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["1"]["b"],
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
    ASSERT_EQUALS(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, d: 1}}]}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ApplySameStructure) {
    auto update = fromjson("{$set: {'a.1.b': {c: 1, d: 1}}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.b"], collator));

    Document doc(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, xxx: 1}}]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.1.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a.1.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["1"]["b"],
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
    ASSERT_EQUALS(fromjson("{a: [{b: {c: 0, d: 0}}, {b: {c: 1, d: 1}}]}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, NonViablePathWithoutRepl) {
    auto update = fromjson("{$set: {'a.1.b': 1}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.b"], collator));

    Document doc(fromjson("{a: 1}"));
    FieldRef pathToCreate("1.b");
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
                                "Cannot create field '1' in element {a: 1}");
}

TEST(SetNodeTest, SingleFieldFromReplication) {
    auto update = fromjson("{$set: {'a.1.b': 1}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.b"], collator));

    Document doc(fromjson("{_id:1, a: 1}"));
    FieldRef pathToCreate("1.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    UpdateIndexData indexData;
    indexData.addPath("a.1.b");
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
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{_id:1, a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, SingleFieldNoIdFromReplication) {
    auto update = fromjson("{$set: {'a.1.b': 1}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.1.b"], collator));

    Document doc(fromjson("{a: 1}"));
    FieldRef pathToCreate("1.b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = true;
    UpdateIndexData indexData;
    indexData.addPath("a.1.b");
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
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, NestedFieldFromReplication) {
    auto update = fromjson("{$set: {'a.a.1.b': 1}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.a.1.b"], collator));

    Document doc(fromjson("{_id:1, a: {a: 1}}"));
    FieldRef pathToCreate("1.b");
    FieldRef pathTaken("a.a");
    StringData matchedField;
    auto fromReplication = true;
    UpdateIndexData indexData;
    indexData.addPath("a.a.1.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["a"],
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
    ASSERT_EQUALS(fromjson("{_id:1, a: {a: 1}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, DoubleNestedFieldFromReplication) {
    auto update = fromjson("{$set: {'a.b.c.d': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.b.c.d"], collator));

    Document doc(fromjson("{_id:1, a: {b: {c: 1}}}"));
    FieldRef pathToCreate("d");
    FieldRef pathTaken("a.b.c");
    StringData matchedField;
    auto fromReplication = true;
    UpdateIndexData indexData;
    indexData.addPath("a.b.c.d");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["b"]["c"],
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
    ASSERT_EQUALS(fromjson("{_id:1, a: {b: {c: 1}}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, NestedFieldNoIdFromReplication) {
    auto update = fromjson("{$set: {'a.a.1.b': 1}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.a.1.b"], collator));

    Document doc(fromjson("{a: {a: 1}}"));
    FieldRef pathToCreate("1.b");
    FieldRef pathTaken("a.a");
    StringData matchedField;
    auto fromReplication = true;
    UpdateIndexData indexData;
    indexData.addPath("a.a.1.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["a"],
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
    ASSERT_EQUALS(fromjson("{a: {a: 1}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, ReplayArrayFieldNotAppendedIntermediateFromReplication) {
    auto update = fromjson("{$set: {'a.0.b': [0,2]}}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["a.0.b"], collator));

    Document doc(fromjson("{_id: 0, a: [1, {b: [1]}]}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("a.0");
    StringData matchedField;
    auto fromReplication = true;
    UpdateIndexData indexData;
    indexData.addPath("a.0.b");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["a"]["0"],
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
    ASSERT_EQUALS(fromjson("{_id: 0, a: [1, {b: [1]}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
}

TEST(SetNodeTest, Set6) {
    auto update = fromjson("{$set: {'r.a': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["r.a"], collator));

    Document doc(fromjson("{_id: 1, r: {a:1, b:2}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("r.a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("r.a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["r"]["a"],
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
    ASSERT_EQUALS(fromjson("{_id: 1, r: {a:2, b:2}}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'r.a': 2}}"), logDoc);
}

TEST(SetNodeTest, Set6FromRepl) {
    auto update = fromjson("{$set: { 'r.a': 2}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["r.a"], collator));

    Document doc(fromjson("{_id: 1, r: {a:1, b:2}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("r.a");
    StringData matchedField;
    auto fromReplication = true;
    UpdateIndexData indexData;
    indexData.addPath("r.a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["r"]["a"],
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
    ASSERT_EQUALS(fromjson("{_id: 1, r: {a:2, b:2} }"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(countChildren(logDoc.root()), 1u);
    ASSERT_EQUALS(fromjson("{$set: {'r.a': 2}}"), logDoc);
}

TEST(SetNodeTest, ApplySetModToEphemeralDocument) {
    // The following mod when applied to a document constructed node by node exposed a
    // latent debug only defect in mutable BSON, so this is more a test of mutable than
    // $set.
    auto update = fromjson("{ $set: { x: { a: 100, b: 2 }}}");
    const CollatorInterface* collator = nullptr;
    SetNode node;
    ASSERT_OK(node.init(update["$set"]["x"], collator));

    Document doc;
    Element x = doc.makeElementObject("x");
    doc.root().pushBack(x).transitional_ignore();
    Element a = doc.makeElementInt("a", 100);
    x.pushBack(a).transitional_ignore();

    FieldRef pathToCreate("");
    FieldRef pathTaken("x");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("x");
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["x"],
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
    ASSERT_EQUALS(fromjson("{ x : { a : 100, b : 2 } }"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

}  // namespace
