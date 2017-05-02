/**
 *    Copyright 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/repl/rollback_fix_up_info_descriptions.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/uuid.h"

namespace {

using namespace mongo;
using namespace mongo::repl;

TEST(RollbackFixUpInfoDescriptionsTest, SingleDocumentDescriptionToBson) {
    auto collectionUuid = UUID::gen();
    auto docId = BSON("_id"
                      << "mydocid");

    RollbackFixUpInfo::SingleDocumentOperationDescription description(
        collectionUuid,
        docId.firstElement(),
        RollbackFixUpInfo::SingleDocumentOpType::kInsert,
        "mydb");

    auto expectedDocument = BSON(
        "_id" << BSON("collectionUuid" << collectionUuid << "documentId" << docId.firstElement())
              << "operationType"
              << "insert"
              << "db"
              << "mydb"
              << "documentToRestore"
              << BSONNULL);

    ASSERT_BSONOBJ_EQ(expectedDocument, description.toBSON());
}

TEST(RollbackFixUpInfoDescriptionsTest, CollectionUuidDescriptionToBson) {
    auto collectionUuid = UUID::gen();
    NamespaceString nss("mydb.mylostcoll");

    RollbackFixUpInfo::CollectionUuidDescription description(collectionUuid, nss);

    auto expectedDocument = BSON("_id" << collectionUuid << "ns" << nss.ns());

    ASSERT_BSONOBJ_EQ(expectedDocument, description.toBSON());
}

TEST(RollbackFixUpInfoDescriptionsTest, CollectionUuidDescriptionWithEmptyNamespaceToBson) {
    auto collectionUuid = UUID::gen();
    NamespaceString emptyNss;

    RollbackFixUpInfo::CollectionUuidDescription description(collectionUuid, emptyNss);

    auto expectedDocument = BSON("_id" << collectionUuid << "ns"
                                       << "");

    ASSERT_BSONOBJ_EQ(expectedDocument, description.toBSON());
}

TEST(RollbackFixUpInfoDescriptionsTest, CollectionOptionsDescriptionToBson) {
    auto collectionUuid = UUID::gen();
    CollectionOptions options;
    options.collation = BSON("locale"
                             << "en_US"
                             << "strength"
                             << 1);
    ASSERT_OK(options.validate());

    RollbackFixUpInfo::CollectionOptionsDescription description(collectionUuid, options.toBSON());

    auto expectedDocument = BSON("_id" << collectionUuid << "options" << options.toBSON());

    ASSERT_BSONOBJ_EQ(expectedDocument, description.toBSON());
}

TEST(RollbackFixUpInfoDescriptionsTest, IndexDescriptionParseOpType) {
    ASSERT_EQUALS(
        RollbackFixUpInfo::IndexOpType::kCreate,
        unittest::assertGet(RollbackFixUpInfo::IndexDescription::parseOpType(BSON("operationType"
                                                                                  << "create"))));
    ASSERT_EQUALS(
        RollbackFixUpInfo::IndexOpType::kDrop,
        unittest::assertGet(RollbackFixUpInfo::IndexDescription::parseOpType(BSON("operationType"
                                                                                  << "drop"))));
    ASSERT_EQUALS(RollbackFixUpInfo::IndexOpType::kUpdateTTL,
                  unittest::assertGet(
                      RollbackFixUpInfo::IndexDescription::parseOpType(BSON("operationType"
                                                                            << "updateTTL"))));
    ASSERT_EQUALS(ErrorCodes::NoSuchKey,
                  RollbackFixUpInfo::IndexDescription::parseOpType(BSON("no_operation_type" << 1))
                      .getStatus());
    ASSERT_EQUALS(ErrorCodes::TypeMismatch,
                  RollbackFixUpInfo::IndexDescription::parseOpType(BSON("operationType" << 12345))
                      .getStatus());
    ASSERT_EQUALS(ErrorCodes::FailedToParse,
                  RollbackFixUpInfo::IndexDescription::parseOpType(BSON("operationType"
                                                                        << "unknown op type"))
                      .getStatus());
}

TEST(RollbackFixUpInfoDescriptionsTest, IndexDescriptionToBson) {
    auto collectionUuid = UUID::gen();
    const std::string indexName = "b_1";
    auto infoObj = BSON("v" << 2 << "key" << BSON("b" << 1) << "name" << indexName << "ns"
                            << "mydb.mycoll"
                            << "expireAfterSeconds"
                            << 60);

    RollbackFixUpInfo::IndexDescription description(
        collectionUuid, indexName, RollbackFixUpInfo::IndexOpType::kDrop, infoObj);

    auto expectedDocument =
        BSON("_id" << BSON("collectionUuid" << collectionUuid << "indexName" << indexName)
                   << "operationType"
                   << "drop"
                   << "infoObj"
                   << infoObj);

    ASSERT_BSONOBJ_EQ(expectedDocument, description.toBSON());
}

}  // namespace
