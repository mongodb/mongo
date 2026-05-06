/**
 *    Copyright (C) 2026-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/db/matcher/doc_validation/constraint_validation_level_upgrade.h"

#include "mongo/bson/json.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/shard_role/shard_catalog/catalog_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_options_gen.h"
#include "mongo/db/shard_role/shard_catalog/create_collection.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {

CollectionAcquisition acquireForWrite(OperationContext* opCtx, const NamespaceString& nss) {
    return acquireCollection(
        opCtx,
        CollectionAcquisitionRequest(nss,
                                     PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                     repl::ReadConcernArgs::get(opCtx),
                                     AcquisitionPrerequisites::kWrite),
        MODE_IX);
}

class ConstraintValidationLevelUpgradeTest : public CatalogTestFixture {};

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsOKForNonExistentCollection) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    ASSERT_OK(noDocumentsViolatingValidator(
        operationContext(), nss, PlacementConcern(boost::none, ShardVersion::UNTRACKED())));
}

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsOKForCollectionWithNoValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.uuid = UUID::gen();
    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));

    ASSERT_OK(noDocumentsViolatingValidator(
        opCtx, nss, PlacementConcern(boost::none, ShardVersion::UNTRACKED())));
}

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsOKForEmptyCollectionWithValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.validator = fromjson("{a: {$exists: true}}");
    options.uuid = UUID::gen();

    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));

    ASSERT_OK(noDocumentsViolatingValidator(
        opCtx, nss, PlacementConcern(boost::none, ShardVersion::UNTRACKED())));
}

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsErrorWhenDocumentViolatesValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.validator = fromjson("{a: {$exists: true}}");
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = ValidationLevelEnum::moderate;
    options.uuid = UUID::gen();

    {
        ASSERT_OK(createCollection(opCtx, nss, options, boost::none));
        auto coll = acquireForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), BSON("_id" << 1 << "b" << 1)));
        wuow.commit();
    }

    ASSERT_EQ(noDocumentsViolatingValidator(
                  opCtx, nss, PlacementConcern(boost::none, ShardVersion::UNTRACKED()))
                  .code(),
              12370902);
}

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsOKWhenAllDocumentsConformToValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.validator = fromjson("{a: {$exists: true}}");
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = ValidationLevelEnum::moderate;
    options.uuid = UUID::gen();

    {
        ASSERT_OK(createCollection(opCtx, nss, options, boost::none));
        auto coll = acquireForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), BSON("_id" << 1 << "a" << 1)));
        wuow.commit();
    }

    ASSERT_OK(noDocumentsViolatingValidator(
        opCtx, nss, PlacementConcern(boost::none, ShardVersion::UNTRACKED())));
}

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsErrorWhenDocumentViolatesJsonSchemaValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.validator = fromjson("{$jsonSchema: {required: [\"a\"]}}");
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = ValidationLevelEnum::moderate;
    options.uuid = UUID::gen();

    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));
    {
        auto coll = acquireForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), BSON("_id" << 1 << "b" << 1)));
        wuow.commit();
    }

    ASSERT_EQ(noDocumentsViolatingValidator(
                  opCtx, nss, PlacementConcern(boost::none, ShardVersion::UNTRACKED()))
                  .code(),
              12370902);
}

TEST_F(ConstraintValidationLevelUpgradeTest, ErrorMessageTruncatesLargeValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    // Build a validator whose JSON serialization exceeds 10KB via a long schema title.
    std::string longTitle(15000, 'x');
    CollectionOptions options;
    options.validator =
        BSON("$jsonSchema" << BSON("title" << longTitle << "required" << BSON_ARRAY("a")));
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = ValidationLevelEnum::moderate;
    options.uuid = UUID::gen();

    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));
    {
        auto coll = acquireForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), BSON("_id" << 1 << "b" << 1)));
        wuow.commit();
    }

    auto status = noDocumentsViolatingValidator(
        opCtx, nss, PlacementConcern(boost::none, ShardVersion::UNTRACKED()));
    ASSERT_EQ(status.code(), 12370902);
    ASSERT_STRING_CONTAINS(status.reason(), "<your collection's validator>");
    ASSERT_STRING_OMITS(status.reason(), longTitle);
}

TEST_F(ConstraintValidationLevelUpgradeTest, ErrorMessageContainsValidatorAndCollectionHint) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.validator = fromjson("{a: {$exists: true}}");
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = ValidationLevelEnum::moderate;
    options.uuid = UUID::gen();

    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));
    {
        auto coll = acquireForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), BSON("_id" << 1 << "b" << 1)));
        wuow.commit();
    }

    auto status = noDocumentsViolatingValidator(
        opCtx, nss, PlacementConcern(boost::none, ShardVersion::UNTRACKED()));
    ASSERT_EQ(status.code(), 12370902);
    ASSERT_STRING_CONTAINS(status.reason(), "Run db.testcoll.find({\"$nor\": [");
    StringBuilder validatorStr;
    options.validator.toString(validatorStr, /*isArray=*/false, /*full=*/true);
    ASSERT_STRING_CONTAINS(status.reason(), validatorStr.str());
    ASSERT_STRING_CONTAINS(status.reason(), "]}) to find non-compliant documents.");
}

}  // namespace
}  // namespace mongo
