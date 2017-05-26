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

#include "mongo/db/update/unset_node.h"

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

DEATH_TEST(UnsetNodeTest, InitFailsForEmptyElement, "Invariant failure modExpr.ok()") {
    auto update = fromjson("{$unset: {}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    node.init(update["$unset"].embeddedObject().firstElement(), collator);
}

DEATH_TEST(UnsetNodeTest, ApplyToRootFails, "Invariant failure parent.ok()") {
    auto update = fromjson("{$unset: {}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"], collator));

    Document doc(fromjson("{a: 5}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root(),
               &pathToCreate,
               &pathTaken,
               matchedField,
               fromReplication,
               indexData,
               logBuilder,
               &indexesAffected,
               &noop);
}

TEST(UnsetNodeTest, InitSucceedsForNonemptyElement) {
    auto update = fromjson("{$unset: {a: 5}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], collator));
}

/* This is a no-op because we are unsetting a field that does not exit. */
TEST(UnsetNodeTest, UnsetNoOp) {
    auto update = fromjson("{$unset: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], collator));

    Document doc(fromjson("{b: 5}"));
    FieldRef pathToCreate("a");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root(),
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(UnsetNodeTest, UnsetNoOpDottedPath) {
    auto update = fromjson("{$unset: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b"], collator));

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
    ASSERT_OK(node.apply(doc.root()["a"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 5}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(UnsetNodeTest, UnsetNoOpThroughArray) {
    auto update = fromjson("{$unset: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b"], collator));

    Document doc(fromjson("{a:[{b:1}]}"));
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
    ASSERT_OK(node.apply(doc.root()["a"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a:[{b:1}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(UnsetNodeTest, UnsetNoOpEmptyDoc) {
    auto update = fromjson("{$unset: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], collator));

    Document doc(fromjson("{}"));
    FieldRef pathToCreate("a");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root(),
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(UnsetNodeTest, UnsetTopLevelPath) {
    auto update = fromjson("{$unset: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], collator));

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
    ASSERT_OK(node.apply(doc.root()["a"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$unset: {a: true}}"), logDoc);
}

TEST(UnsetNodeTest, UnsetNestedPath) {
    auto update = fromjson("{$unset: {'a.b.c': 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b.c"], collator));

    Document doc(fromjson("{a: {b: {c: 6}}}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b.c");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root()["a"]["b"]["c"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: {}}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$unset: {'a.b.c': true}}"), logDoc);
}

TEST(UnsetNodeTest, UnsetObject) {
    auto update = fromjson("{$unset: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b"], collator));

    Document doc(fromjson("{a: {b: {c: 6}}}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root()["a"]["b"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$unset: {'a.b': true}}"), logDoc);
}

TEST(UnsetNodeTest, UnsetArrayElement) {
    auto update = fromjson("{$unset: {'a.0': 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.0"], collator));

    Document doc(fromjson("{a:[1], b:1}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.0");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root()["a"]["0"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a:[null], b:1}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$unset: {'a.0': true}}"), logDoc);
}

TEST(UnsetNodeTest, UnsetPositional) {
    auto update = fromjson("{$unset: {'a.$': 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.$"], collator));

    Document doc(fromjson("{a: [0, 1, 2]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.1");
    StringData matchedField = "1";
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root()["a"]["1"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [0, null, 2]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$unset: {'a.1': true}}"), logDoc);
}

TEST(UnsetNodeTest, UnsetEntireArray) {
    auto update = fromjson("{$unset: {'a': 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], collator));

    Document doc(fromjson("{a: [0, 1, 2]}"));
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
    ASSERT_OK(node.apply(doc.root()["a"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$unset: {a: true}}"), logDoc);
}

TEST(UnsetNodeTest, UnsetFromObjectInArray) {
    auto update = fromjson("{$unset: {'a.0.b': 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.0.b"], collator));

    Document doc(fromjson("{a: [{b: 1}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.0.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root()["a"]["0"]["b"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a:[{}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$unset: {'a.0.b': true}}"), logDoc);
}

TEST(UnsetNodeTest, CanUnsetInvalidField) {
    auto update = fromjson("{$unset: {'a.$.$b': true}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.$.$b"], collator));

    Document doc(fromjson("{b: 1, a: [{$b: 1}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.0.$b");
    StringData matchedField = "0";
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root()["a"]["0"]["$b"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{b: 1, a: [{}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$unset: {'a.0.$b': true}}"), logDoc);
}

TEST(UnsetNodeTest, ApplyNoIndexDataNoLogBuilder) {
    auto update = fromjson("{$unset: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], collator));

    Document doc(fromjson("{a: 5}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    const UpdateIndexData* indexData = nullptr;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root()["a"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         indexData,
                         logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(UnsetNodeTest, ApplyDoesNotAffectIndexes) {
    auto update = fromjson("{$unset: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a"], collator));

    Document doc(fromjson("{a: 5}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("b");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root()["a"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$unset: {a: true}}"), logDoc);
}

TEST(UnsetNodeTest, ApplyFieldWithDot) {
    auto update = fromjson("{$unset: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    UnsetNode node;
    ASSERT_OK(node.init(update["$unset"]["a.b"], collator));

    Document doc(fromjson("{'a.b':4, a: {b: 2}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_OK(node.apply(doc.root()["a"]["b"],
                         &pathToCreate,
                         &pathTaken,
                         matchedField,
                         fromReplication,
                         &indexData,
                         &logBuilder,
                         &indexesAffected,
                         &noop));
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{'a.b':4, a: {}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$unset: {'a.b': true}}"), logDoc);
}

}  // namespace
