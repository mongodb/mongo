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

#include "mongo/db/update/object_replace_node.h"

#include "mongo/bson/mutable/algorithm.h"
#include "mongo/bson/mutable/mutable_bson_test_utils.h"
#include "mongo/db/json.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/service_context_noop.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using mongo::mutablebson::Document;
using mongo::mutablebson::Element;
using mongo::mutablebson::countChildren;

class ObjectReplaceNodeTest : public mongo::unittest::Test {
public:
    ~ObjectReplaceNodeTest() override = default;

protected:
    void setUp() override {
        auto service = mongo::getGlobalServiceContext();
        auto logicalClock = mongo::stdx::make_unique<mongo::LogicalClock>(service);
        mongo::LogicalClock::set(service, std::move(logicalClock));
    }
    void tearDown() override{};
};

TEST_F(ObjectReplaceNodeTest, Noop) {
    auto obj = fromjson("{a: 1, b: 2}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{a: 1, b: 2}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_TRUE(noop);
    ASSERT_FALSE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: 2}"), doc);
    ASSERT_TRUE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{}"), logDoc);
}

TEST_F(ObjectReplaceNodeTest, ShouldNotCreateIdIfNoIdExistsAndNoneIsSpecified) {
    auto obj = fromjson("{a: 1, b: 2}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{c: 1, d: 2}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: 1, b: 2}"), logDoc);
}

TEST_F(ObjectReplaceNodeTest, ShouldPreserveIdOfExistingDocumentIfIdNotSpecified) {
    auto obj = fromjson("{a: 1, b: 2}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{_id: 0, c: 1, d: 2}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 0, a: 1, b: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{_id: 0, a: 1, b: 2}"), logDoc);
}

TEST_F(ObjectReplaceNodeTest, ShouldSucceedWhenImmutableIdIsNotModified) {
    auto obj = fromjson("{_id: 0, a: 1, b: 2}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{_id: 0, c: 1, d: 2}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("_id");
    immutablePaths.insert(&path);
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 0, a: 1, b: 2}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{_id: 0, a: 1, b: 2}"), logDoc);
}

TEST_F(ObjectReplaceNodeTest, IdTimestampNotModified) {
    auto obj = fromjson("{_id: Timestamp(0,0)}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: Timestamp(0,0)}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{_id: Timestamp(0,0)}"), logDoc);
}

TEST_F(ObjectReplaceNodeTest, NonIdTimestampsModified) {
    auto obj = fromjson("{a: Timestamp(0,0), b: Timestamp(0,0)}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);

    ASSERT_EQUALS(doc.root().countChildren(), 2U);

    auto elemA = doc.root()["a"];
    ASSERT_TRUE(elemA.ok());
    ASSERT_EQUALS(elemA.getType(), BSONType::bsonTimestamp);
    ASSERT_NOT_EQUALS(0U, elemA.getValueTimestamp().getSecs());
    ASSERT_NOT_EQUALS(0U, elemA.getValueTimestamp().getInc());

    auto elemB = doc.root()["b"];
    ASSERT_TRUE(elemB.ok());
    ASSERT_EQUALS(elemB.getType(), BSONType::bsonTimestamp);
    ASSERT_NOT_EQUALS(0U, elemB.getValueTimestamp().getSecs());
    ASSERT_NOT_EQUALS(0U, elemB.getValueTimestamp().getInc());

    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(doc, logDoc);
}

TEST_F(ObjectReplaceNodeTest, ComplexDoc) {
    auto obj = fromjson("{a: 1, b: [0, 1, 2], c: {d: 1}}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{a: 1, b: [0, 2, 2], e: []}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1, b: [0, 1, 2], c: {d: 1}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: 1, b: [0, 1, 2], c: {d: 1}}"), logDoc);
}

TEST_F(ObjectReplaceNodeTest, CannotRemoveImmutablePath) {
    auto obj = fromjson("{_id: 0, c: 1}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{_id: 0, a: {b: 1}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("a.b");
    immutablePaths.insert(&path);
    const UpdateIndexData* indexData;
    Document logDoc;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root(),
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
                                ErrorCodes::ImmutableField,
                                "After applying the update, the 'a.b' (required and immutable) "
                                "field was found to have been removed --{ _id: 0, a: { b: 1 } }");
}

TEST_F(ObjectReplaceNodeTest, IdFieldIsNotRemoved) {
    auto obj = fromjson("{a: 1}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{_id: 0, b: 1}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("_id");
    immutablePaths.insert(&path);
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 0, a: 1}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{_id: 0, a: 1}"), logDoc);
}

TEST_F(ObjectReplaceNodeTest, CannotReplaceImmutablePathWithArrayField) {
    auto obj = fromjson("{_id: 0, a: [{b: 1}]}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{_id: 0, a: {b: 1}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("a.b");
    immutablePaths.insert(&path);
    const UpdateIndexData* indexData;
    Document logDoc;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root(),
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
                                ErrorCodes::NotSingleValueField,
                                "After applying the update to the document, the (immutable) field "
                                "'a.b' was found to be an array or array descendant.");
}

TEST_F(ObjectReplaceNodeTest, CannotMakeImmutablePathArrayDescendant) {
    auto obj = fromjson("{_id: 0, a: [1]}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{_id: 0, a: {'0': 1}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("a.0");
    immutablePaths.insert(&path);
    const UpdateIndexData* indexData;
    Document logDoc;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root(),
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
                                ErrorCodes::NotSingleValueField,
                                "After applying the update to the document, the (immutable) field "
                                "'a.0' was found to be an array or array descendant.");
}

TEST_F(ObjectReplaceNodeTest, CannotModifyImmutablePath) {
    auto obj = fromjson("{_id: 0, a: {b: 2}}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{_id: 0, a: {b: 1}}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("a.b");
    immutablePaths.insert(&path);
    const UpdateIndexData* indexData;
    Document logDoc;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root(),
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
                                ErrorCodes::ImmutableField,
                                "After applying the update, the (immutable) field 'a.b' was found "
                                "to have been altered to b: 2");
}

TEST_F(ObjectReplaceNodeTest, CannotModifyImmutableId) {
    auto obj = fromjson("{_id: 1}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{_id: 0}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("_id");
    immutablePaths.insert(&path);
    const UpdateIndexData* indexData;
    Document logDoc;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(node.apply(doc.root(),
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
                                ErrorCodes::ImmutableField,
                                "After applying the update, the (immutable) field '_id' was found "
                                "to have been altered to _id: 1");
}

TEST_F(ObjectReplaceNodeTest, CanAddImmutableField) {
    auto obj = fromjson("{a: {b: 1}}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{c: 1}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("a.b");
    immutablePaths.insert(&path);
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 1}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: {b: 1}}"), logDoc);
}

TEST_F(ObjectReplaceNodeTest, CanAddImmutableId) {
    auto obj = fromjson("{_id: 0}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{c: 1}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    FieldRef path("_id");
    immutablePaths.insert(&path);
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{_id: 0}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{_id: 0}"), logDoc);
}

TEST_F(ObjectReplaceNodeTest, CannotCreateDollarPrefixedNameWhenValidateForStorageIsTrue) {
    auto obj = fromjson("{a: {b: 1, $bad: 1}}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData;
    Document logDoc;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    ASSERT_THROWS_CODE_AND_WHAT(
        node.apply(doc.root(),
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
        ErrorCodes::DollarPrefixedFieldName,
        "The dollar ($) prefixed field '$bad' in 'a.$bad' is not valid for storage.");
}

TEST_F(ObjectReplaceNodeTest, CanCreateDollarPrefixedNameWhenValidateForStorageIsFalse) {
    auto obj = fromjson("{a: {b: 1, $bad: 1}}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = false;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData;
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
               indexData,
               &logBuilder,
               &indexesAffected,
               &noop);
    ASSERT_FALSE(noop);
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: {b: 1, $bad: 1}}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
    ASSERT_EQUALS(fromjson("{a: {b: 1, $bad: 1}}"), logDoc);
}

TEST_F(ObjectReplaceNodeTest, NoLogBuilder) {
    auto obj = fromjson("{a: 1}");
    ObjectReplaceNode node(obj);

    Document doc(fromjson("{b: 1}"));
    FieldRef pathToCreate("");
    FieldRef pathTaken("");
    StringData matchedField;
    auto fromReplication = false;
    auto validateForStorage = true;
    FieldRefSet immutablePaths;
    const UpdateIndexData* indexData;
    LogBuilder* logBuilder = nullptr;
    auto indexesAffected = false;
    auto noop = false;
    node.apply(doc.root(),
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
    ASSERT_TRUE(indexesAffected);
    ASSERT_EQUALS(fromjson("{a: 1}"), doc);
    ASSERT_FALSE(doc.isInPlaceModeEnabled());
}

}  // namespace
}  // namespace mongo
