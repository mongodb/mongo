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

#include "mongo/db/update/pullall_node.h"

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

TEST(PullAllNodeTest, InitWithIntFails) {
    auto update = fromjson("{$pullAll: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullAllNodeTest, InitWithStringFails) {
    auto update = fromjson("{$pullAll: {a: 'test'}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullAllNodeTest, InitWithObjectFails) {
    auto update = fromjson("{$pullAll: {a: {}}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullAllNodeTest, InitWithBoolFails) {
    auto update = fromjson("{$pullAll: {a: true}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    auto status = node.init(update["$pullAll"]["a"], collator);
    ASSERT_NOT_OK(status);
    ASSERT_EQUALS(ErrorCodes::BadValue, status);
}

TEST(PullAllNodeTest, TargetNotFound) {
    auto update = fromjson("{$pullAll : {b: [1]}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["b"], collator));

    Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
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
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(PullAllNodeTest, TargetArrayElementNotFound) {
    auto update = fromjson("{$pullAll : {'a.2': [1]}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a.2"], collator));

    Document doc(fromjson("{a: [1, 2]}"));
    FieldRef pathToCreate("2");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
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
    ASSERT_EQUALS(fromjson("{a: [1, 2]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(PullAllNodeTest, ApplyToNonArrayFails) {
    auto update = fromjson("{$pullAll : {'a.0': [1, 2]}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a.0"], collator));

    Document doc(fromjson("{a: [1, 2]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.0");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a");
    Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root()["a"][0],
                                           &pathToCreate,
                                           &pathTaken,
                                           matchedField,
                                           fromReplication,
                                           validateForStorage,
                                           immutablePaths,
                                           &indexData,
                                           &logBuilder,
                                           &indexesAffected,
                                           &noop),
                                UserException,
                                ErrorCodes::BadValue,
                                "Cannot apply $pull to a non-array value");
}

TEST(PullAllNodeTest, ApplyWithSingleNumber) {
    auto update = fromjson("{$pullAll : {a: [1]}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], collator));

    Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
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
    ASSERT_EQUALS(fromjson("{a: ['a', {r: 1, b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: ['a', {r: 1, b: 2}]}}"), logDoc);
}

TEST(PullAllNodeTest, ApplyNoIndexDataNoLogBuilder) {
    auto update = fromjson("{$pullAll : {a: [1]}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], collator));

    Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData* indexData = nullptr;
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
    ASSERT_EQUALS(fromjson("{a: ['a', {r: 1, b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

TEST(PullAllNodeTest, ApplyWithElementNotPresentInArray) {
    auto update = fromjson("{$pullAll : {a: ['r']}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], collator));

    Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
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
    ASSERT_EQUALS(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(PullAllNodeTest, ApplyWithWithTwoElements) {
    auto update = fromjson("{$pullAll : {a: [1, 'a']}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], collator));

    Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
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
    ASSERT_EQUALS(fromjson("{a: [{r: 1, b: 2}]}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: [{r: 1, b: 2}]}}"), logDoc);
}

TEST(PullAllNodeTest, ApplyWithAllArrayElements) {
    auto update = fromjson("{$pullAll : {a: [1, 'a', {r: 1, b: 2}]}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], collator));

    Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
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
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: []}}"), logDoc);
}

TEST(PullAllNodeTest, ApplyWithAllArrayElementsButOutOfOrder) {
    auto update = fromjson("{$pullAll : {a: [{r: 1, b: 2}, 1, 'a']}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], collator));

    Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
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
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: []}}"), logDoc);
}

TEST(PullAllNodeTest, ApplyWithAllArrayElementsAndThenSome) {
    auto update = fromjson("{$pullAll : {a: [2, 3, 1, 'r', {r: 1, b: 2}, 'a']}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], collator));

    Document doc(fromjson("{a: [1, 'a', {r: 1, b: 2}]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
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
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: []}}"), logDoc);
}

TEST(PullAllNodeTest, ApplyWithCollator) {
    auto update = fromjson("{$pullAll : {a: ['FOO', 'BAR']}}");
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kToLowerString);
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], &collator));

    Document doc(fromjson("{a: ['foo', 'bar', 'baz']}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
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
    ASSERT_EQUALS(fromjson("{a: ['baz']}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{$set: {a: ['baz']}}"), logDoc);
}

TEST(PullAllNodeTest, ApplyAfterSetCollator) {
    auto update = fromjson("{$pullAll : {a: ['FOO', 'BAR']}}");
    const CollatorInterface* collator = nullptr;
    PullAllNode node;
    ASSERT_OK(node.init(update["$pullAll"]["a"], collator));

    // First without a collator.
    Document doc(fromjson("{a: ['foo', 'bar', 'baz']}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData* indexData = nullptr;
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
    ASSERT_TRUE(noop);
    ASSERT_EQUALS(fromjson("{a: ['foo', 'bar', 'baz']}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());

    // Now with a collator.
    CollatorInterfaceMock mockCollator(CollatorInterfaceMock::MockType::kToLowerString);
    node.setCollator(&mockCollator);
    indexesAffected = false;
    noop = false;
    Document doc2(fromjson("{a: ['foo', 'bar', 'baz']}"));
    node.apply(doc2.root()["a"],
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
    ASSERT_EQUALS(fromjson("{a: ['baz']}"), doc2);
    ASSERT_FALSE(doc2.isInPlaceModeEnabled());
}

}  // namespace mongo
