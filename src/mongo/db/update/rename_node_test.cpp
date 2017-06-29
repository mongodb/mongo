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

#include "mongo/db/update/rename_node.h"

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

TEST(RenameNodeTest, PositionalNotAllowedInFromField) {
    auto update = fromjson("{$rename: {'a.$': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    Status status = node.init(update["$rename"]["a.$"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, PositionalNotAllowedInToField) {
    auto update = fromjson("{$rename: {'a': 'b.$'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    Status status = node.init(update["$rename"]["a"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, ArrayFilterNotAllowedInFromField) {
    auto update = fromjson("{$rename: {'a.$[i]': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    Status status = node.init(update["$rename"]["a.$[i]"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, ArrayFilterNotAllowedInToField) {
    auto update = fromjson("{$rename: {'a': 'b.$[i]'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    Status status = node.init(update["$rename"]["a"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}


TEST(RenameNodeTest, MoveUpNotAllowed) {
    auto update = fromjson("{$rename: {'b.a': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    Status status = node.init(update["$rename"]["b.a"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, MoveDownNotAllowed) {
    auto update = fromjson("{$rename: {'b': 'b.a'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    Status status = node.init(update["$rename"]["b"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, MoveToSelfNotAllowed) {
    auto update = fromjson("{$rename: {'b.a': 'b.a'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    Status status = node.init(update["$rename"]["b.a"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(RenameNodeTest, SimpleNumberAtRoot) {
    auto update = fromjson("{$rename: {'a': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], collator));

    Document doc(fromjson("{a: 2}"));
    FieldRef pathToCreate("b");
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
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {b: 2}, $unset: {a: true}}"), logDoc);
}

TEST(RenameNodeTest, ToExistsAtSameLevel) {
    auto update = fromjson("{$rename: {'a': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], collator));

    Document doc(fromjson("{a: 2, b: 1}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["b"],
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
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {b: 2}, $unset: {a: true}}"), logDoc);
}

TEST(RenameNodeTest, ToAndFromHaveSameValue) {
    auto update = fromjson("{$rename: {'a': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], collator));

    Document doc(fromjson("{a: 2, b: 2}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["b"],
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
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);
    ASSERT_EQUALS(fromjson("{$unset: {a: true}}"), logDoc);
}

TEST(RenameNodeTest, FromDottedElement) {
    auto update = fromjson("{$rename: {'a.c': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.c"], collator));

    Document doc(fromjson("{a: {c: {d: 6}}, b: 1}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["b"],
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
    ASSERT_EQUALS(fromjson("{a: {}, b: {d: 6}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {b: {d: 6}}, $unset: {'a.c': true}}"), logDoc);
}

TEST(RenameNodeTest, RenameToExistingNestedFieldDoesNotReorderFields) {
    auto update = fromjson("{$rename: {'c.d': 'a.b.c'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["c.d"], collator));

    Document doc(fromjson("{a: {b: {c: 1, d: 2}}, b: 3, c: {d: 4}}"));
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
    node.apply(doc.root()["a"]["b"]["c"],
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
    ASSERT_EQUALS(fromjson("{a: {b: {c: 4, d: 2}}, b: 3, c: {}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {'a.b.c': 4}, $unset: {'c.d': true}}"), logDoc);
}

TEST(RenameNodeTest, MissingCompleteTo) {
    auto update = fromjson("{$rename: {a: 'c.r.d'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], collator));

    Document doc(fromjson("{a: 2, b: 1, c: {}}"));
    FieldRef pathToCreate("r.d");
    FieldRef pathTaken("c");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["c"],
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
    ASSERT_EQUALS(fromjson("{b: 1, c: {r: {d: 2}}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {'c.r.d': 2}, $unset: {'a': true}}"), logDoc);
}

TEST(RenameNodeTest, ToIsCompletelyMissing) {
    auto update = fromjson("{$rename: {a: 'b.c.d'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], collator));

    Document doc(fromjson("{a: 2}"));
    FieldRef pathToCreate("b.c.d");
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
    ASSERT_EQUALS(fromjson("{b: {c: {d: 2}}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {'b.c.d': 2}, $unset: {'a': true}}"), logDoc);
}

TEST(RenameNodeTest, ToMissingDottedField) {
    auto update = fromjson("{$rename: {a: 'b.c.d'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], collator));

    Document doc(fromjson("{a: [{a:2, b:1}]}"));
    FieldRef pathToCreate("b.c.d");
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
    ASSERT_EQUALS(fromjson("{b: {c: {d: [{a:2, b:1}]}}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {'b.c.d': [{a:2, b:1}]}, $unset: {'a': true}}"), logDoc);
}

TEST(RenameNodeTest, MoveIntoArray) {
    auto update = fromjson("{$rename: {b: 'a.2'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["b"], collator));

    Document doc(fromjson("{_id: 'test_object', a: [1, 2], b: 2}"));
    FieldRef pathToCreate("2");
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
                                ErrorCodes::BadValue,
                                "The destination field cannot be an array element, 'a.2' in doc "
                                "with _id: \"test_object\" has an array field called 'a'");
}

TEST(RenameNodeTest, MoveIntoArrayNoId) {
    auto update = fromjson("{$rename: {b: 'a.2'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["b"], collator));

    Document doc(fromjson("{a: [1, 2], b: 2}"));
    FieldRef pathToCreate("2");
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
                                ErrorCodes::BadValue,
                                "The destination field cannot be an array element, 'a.2' in doc "
                                "with no id has an array field called 'a'");
}

TEST(RenameNodeTest, MoveToArrayElement) {
    auto update = fromjson("{$rename: {b: 'a.1'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["b"], collator));

    Document doc(fromjson("{_id: 'test_object', a: [1, 2], b: 2}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.1");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root()["a"]["1"],
                                           &pathToCreate,
                                           &pathTaken,
                                           matchedField,
                                           fromReplication,
                                           &indexData,
                                           &logBuilder,
                                           &indexesAffected,
                                           &noop),
                                UserException,
                                ErrorCodes::BadValue,
                                "The destination field cannot be an array element, 'a.1' in doc "
                                "with _id: \"test_object\" has an array field called 'a'");
}

TEST(RenameNodeTest, MoveOutOfArray) {
    auto update = fromjson("{$rename: {'a.0': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.0"], collator));

    Document doc(fromjson("{_id: 'test_object', a: [1, 2]}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root(),
                                           &pathToCreate,
                                           &pathTaken,
                                           matchedField,
                                           fromReplication,
                                           &indexData,
                                           &logBuilder,
                                           &indexesAffected,
                                           &noop),
                                UserException,
                                ErrorCodes::BadValue,
                                "The source field cannot be an array element, 'a.0' in doc with "
                                "_id: \"test_object\" has an array field called 'a'");
}

TEST(RenameNodeTest, MoveNonexistentEmbeddedFieldOut) {
    auto update = fromjson("{$rename: {'a.a': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.a"], collator));

    Document doc(fromjson("{a: [{a: 1}, {b: 2}]}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(doc.root(),
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
        "cannot use the part (a of a.a) to traverse the element ({a: [ { a: 1 }, { b: 2 } ]})");
}

TEST(RenameNodeTest, MoveEmbeddedFieldOutWithElementNumber) {
    auto update = fromjson("{$rename: {'a.0.a': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a.0.a"], collator));

    Document doc(fromjson("{_id: 'test_object', a: [{a: 1}, {b: 2}]}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root(),
                                           &pathToCreate,
                                           &pathTaken,
                                           matchedField,
                                           fromReplication,
                                           &indexData,
                                           &logBuilder,
                                           &indexesAffected,
                                           &noop),
                                UserException,
                                ErrorCodes::BadValue,
                                "The source field cannot be an array element, 'a.0.a' in doc with "
                                "_id: \"test_object\" has an array field called 'a'");
}

TEST(RenameNodeTest, ReplaceArrayField) {
    auto update = fromjson("{$rename: {a: 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], collator));

    Document doc(fromjson("{a: 2, b: []}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["b"],
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
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {b: 2}, $unset: {a: true}}"), logDoc);
}

TEST(RenameNodeTest, ReplaceWithArrayField) {
    auto update = fromjson("{$rename: {a: 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], collator));

    Document doc(fromjson("{a: [], b: 2}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["b"],
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
    ASSERT_EQUALS(fromjson("{b: []}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {b: []}, $unset: {a: true}}"), logDoc);
}

TEST(RenameNodeTest, CanRenameFromInvalidFieldName) {
    auto update = fromjson("{$rename: {'$a': 'a'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["$a"], collator));

    Document doc(fromjson("{$a: 2}"));
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
    ASSERT_EQUALS(fromjson("{a: 2}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {a: 2}, $unset: {'$a': true}}"), logDoc);
}

TEST(RenameNodeTest, RenameWithoutLogBuilderOrIndexData) {
    auto update = fromjson("{$rename: {'a': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], collator));

    Document doc(fromjson("{a: 2}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData* indexData = nullptr;
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
    ASSERT_FALSE(noop);
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);
}

TEST(RenameNodeTest, RenameFromNonExistentPathIsNoOp) {
    auto update = fromjson("{$rename: {'a': 'b'}}");
    const CollatorInterface* collator = nullptr;
    RenameNode node;
    ASSERT_OK(node.init(update["$rename"]["a"], collator));

    Document doc(fromjson("{b: 2}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("b");
    StringData matchedField;
    auto fromReplication = false;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root()["b"],
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
    ASSERT_EQUALS(fromjson("{b: 2}"), doc);
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

}  // namespace
}  // namespace mongo
