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

#include "mongo/db/update/pop_node.h"

#include "mongo/bson/json.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

namespace mmb = mongo::mutablebson;

TEST(PopNodeTest, InitSucceedsPositiveOne) {
    auto update = fromjson("{$pop: {a: 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a"], collator));
    ASSERT_FALSE(popNode.popFromFront());
}

TEST(PopNodeTest, InitSucceedsNegativeOne) {
    auto update = fromjson("{$pop: {a: -1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a"], collator));
    ASSERT_TRUE(popNode.popFromFront());
}

TEST(PopNodeTest, InitFailsOnePointOne) {
    auto update = fromjson("{$pop: {a: 1.1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], collator));
}

TEST(PopNodeTest, InitFailsZero) {
    auto update = fromjson("{$pop: {a: 0}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], collator));
}

TEST(PopNodeTest, InitFailsString) {
    auto update = fromjson("{$pop: {a: 'foo'}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], collator));
}

TEST(PopNodeTest, InitFailsNestedObject) {
    auto update = fromjson("{$pop: {a: {b: 1}}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], collator));
}

TEST(PopNodeTest, InitFailsNestedArray) {
    auto update = fromjson("{$pop: {a: [{b: 1}]}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], collator));
}

TEST(PopNodeTest, InitFailsBool) {
    auto update = fromjson("{$pop: {a: true}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_EQ(ErrorCodes::FailedToParse, popNode.init(update["$pop"]["a"], collator));
}

TEST(PopNodeTest, NoopWhenFirstPathComponentDoesNotExist) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));

    mmb::Document doc(fromjson("{b: [1, 2, 3]}"));
    FieldRef pathToCreate("a.b");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root(),
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
    ASSERT_EQUALS(fromjson("{b: [1, 2, 3]}"), doc);
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(PopNodeTest, NoopWhenPathPartiallyExists) {
    auto update = fromjson("{$pop: {'a.b.c': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b.c"], collator));

    mmb::Document doc(fromjson("{a: {}}"));
    FieldRef pathToCreate("b.c");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.b.c");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"],
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
    ASSERT_EQUALS(fromjson("{a: {}}"), doc);
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(PopNodeTest, NoopWhenNumericalPathComponentExceedsArrayLength) {
    auto update = fromjson("{$pop: {'a.0': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.0"], collator));

    mmb::Document doc(fromjson("{a: []}"));
    FieldRef pathToCreate("0");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.0");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"],
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
    ASSERT_EQUALS(fromjson("{a: []}"), doc);
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(PopNodeTest, ThrowsWhenPathIsBlockedByAScalar) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));

    mmb::Document doc(fromjson("{a: 'foo'}"));
    FieldRef pathToCreate("b");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    ASSERT_THROWS_CODE_AND_WHAT(
        popNode.apply(doc.root()["a"],
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
        ErrorCodes::PathNotViable,
        "Cannot use the part (b) of (a.b) to traverse the element ({a: \"foo\"})");
}

DEATH_TEST(PopNodeTest, NonOkElementWhenPathExistsIsFatal, "Invariant failure element.ok()") {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.end(),
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
}

TEST(PopNodeTest, ThrowsWhenPathExistsButDoesNotContainAnArray) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));

    mmb::Document doc(fromjson("{a: {b: 'foo'}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    ASSERT_THROWS_CODE_AND_WHAT(popNode.apply(doc.root()["a"]["b"],
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
                                ErrorCodes::TypeMismatch,
                                "Path 'a.b' contains an element of non-array type 'string'");
}

TEST(PopNodeTest, NoopWhenPathContainsAnEmptyArray) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));

    mmb::Document doc(fromjson("{a: {b: []}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"]["b"],
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
    ASSERT_EQUALS(fromjson("{a: {b: []}}"), doc);
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST(PopNodeTest, PopsSingleElementFromTheBack) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));
    ASSERT_FALSE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"]["b"],
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
    ASSERT_EQUALS(fromjson("{a: {b: []}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {'a.b': []}}"), logDoc);
}

TEST(PopNodeTest, PopsSingleElementFromTheFront) {
    auto update = fromjson("{$pop: {'a.b': -1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));
    ASSERT_TRUE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [[1]]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"]["b"],
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
    ASSERT_EQUALS(fromjson("{a: {b: []}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {'a.b': []}}"), logDoc);
}

TEST(PopNodeTest, PopsFromTheBackOfMultiElementArray) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));
    ASSERT_FALSE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.b.c");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"]["b"],
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
    ASSERT_EQUALS(fromjson("{a: {b: [1, 2]}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {'a.b': [1, 2]}}"), logDoc);
}

TEST(PopNodeTest, PopsFromTheFrontOfMultiElementArray) {
    auto update = fromjson("{$pop: {'a.b': -1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));
    ASSERT_TRUE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"]["b"],
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
    ASSERT_EQUALS(fromjson("{a: {b: [2, 3]}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {'a.b': [2, 3]}}"), logDoc);
}

TEST(PopNodeTest, PopsFromTheFrontOfMultiElementArrayWithoutAffectingIndexes) {
    auto update = fromjson("{$pop: {'a.b': -1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));
    ASSERT_TRUE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("unrelated.path");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"]["b"],
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
    ASSERT_EQUALS(fromjson("{a: {b: [2, 3]}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {'a.b': [2, 3]}}"), logDoc);
}

TEST(PopNodeTest, SucceedsWithNullUpdateIndexData) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));
    ASSERT_FALSE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"]["b"],
                  &pathToCreate,
                  &pathTaken,
                  matchedField,
                  fromReplication,
                  validateForStorage,
                  immutablePaths,
                  nullptr,
                  &logBuilder,
                  &indexesAffected,
                  &noop);
    ASSERT_FALSE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: [1, 2]}}"), doc);
    ASSERT_EQUALS(fromjson("{$set: {'a.b': [1, 2]}}"), logDoc);
}

TEST(PopNodeTest, SucceedsWithNullLogBuilder) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));
    ASSERT_FALSE(popNode.popFromFront());

    mmb::Document doc(fromjson("{a: {b: [1, 2, 3]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    UpdateIndexData indexData;
    indexData.addPath("a.b.c");
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"]["b"],
                  &pathToCreate,
                  &pathTaken,
                  matchedField,
                  fromReplication,
                  validateForStorage,
                  immutablePaths,
                  &indexData,
                  nullptr,
                  &indexesAffected,
                  &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: [1, 2]}}"), doc);
}

TEST(PopNodeTest, ThrowsWhenPathIsImmutable) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));

    mmb::Document doc(fromjson("{a: {b: [0]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("a.b");
    immutablePaths.insert(&path);
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    ASSERT_THROWS_CODE_AND_WHAT(
        popNode.apply(doc.root()["a"]["b"],
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
        ErrorCodes::ImmutableField,
        "Performing a $pop on the path 'a.b' would modify the immutable field 'a.b'");
}

TEST(PopNodeTest, ThrowsWhenPathIsPrefixOfImmutable) {

    // This is only possible for an upsert, since it is not legal to have an array in an immutable
    // path. If this update did not fail, we would fail later for storing an immutable path with an
    // array in it.

    auto update = fromjson("{$pop: {'a': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a"], collator));

    mmb::Document doc(fromjson("{a: [0]}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("a.0");
    immutablePaths.insert(&path);
    UpdateIndexData indexData;
    indexData.addPath("a");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    ASSERT_THROWS_CODE_AND_WHAT(
        popNode.apply(doc.root()["a"],
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
        ErrorCodes::ImmutableField,
        "Performing a $pop on the path 'a' would modify the immutable field 'a.0'");
}

TEST(PopNodeTest, ThrowsWhenPathIsSuffixOfImmutable) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));

    mmb::Document doc(fromjson("{a: {b: [0]}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("a");
    immutablePaths.insert(&path);
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    ASSERT_THROWS_CODE_AND_WHAT(
        popNode.apply(doc.root()["a"]["b"],
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
        ErrorCodes::ImmutableField,
        "Performing a $pop on the path 'a.b' would modify the immutable field 'a'");
}

TEST(PopNodeTest, NoopOnImmutablePathSucceeds) {
    auto update = fromjson("{$pop: {'a.b': 1}}");
    const CollatorInterface* collator = nullptr;
    PopNode popNode;
    ASSERT_OK(popNode.init(update["$pop"]["a.b"], collator));

    mmb::Document doc(fromjson("{a: {b: []}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("a.b");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("a.b");
    immutablePaths.insert(&path);
    UpdateIndexData indexData;
    indexData.addPath("a.b");
    mmb::Document logDoc;
    LogBuilder logBuilder(logDoc.root());
    bool indexesAffected;
    bool noop;
    popNode.apply(doc.root()["a"]["b"],
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
    ASSERT_EQUALS(fromjson("{a: {b: []}}"), doc);
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

}  // namespace
}  // namespace mongo
