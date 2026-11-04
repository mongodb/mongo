// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

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
#include "mongo/util/uuid.h"

#include <boost/none.hpp>

// All tests use localOnly=true (local aggregate) since CatalogTestFixture does not spin up a
// sharding environment.

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

Status scanForUpgrade(OperationContext* opCtx,
                      const NamespaceString& nss,
                      boost::optional<ValidationActionEnum> newAction = boost::none,
                      bool hasNewValidator = false) {
    return noDocumentsViolatingValidator(opCtx,
                                         nss,
                                         PlacementConcern(boost::none, ShardVersion::UNTRACKED()),
                                         /*localOnly=*/true,
                                         newAction,
                                         hasNewValidator);
}

void insertDocument(OperationContext* opCtx, const NamespaceString& nss, const BSONObj& doc) {
    auto coll = acquireForWrite(opCtx, nss);
    WriteUnitOfWork wuow(opCtx);
    ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), doc));
    wuow.commit();
}

CollectionOptions makeUpgradeOptions(ValidationLevelEnum level, bool prepare) {
    CollectionOptions options;
    options.validator = fromjson("{a: {$exists: true}}");
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = level;
    if (prepare) {
        options.prepareConstraintValidationLevel = true;
    }
    options.uuid = UUID::gen();
    return options;
}

class ConstraintValidationLevelUpgradeTest : public CatalogTestFixture {};

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsOKForNonExistentCollection) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();
    ASSERT_OK(scanForUpgrade(opCtx, nss, ValidationActionEnum::error));
}

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsOKForCollectionWithNoValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    auto options = makeUpgradeOptions(ValidationLevelEnum::strict, true);
    options.validator = BSONObj();
    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));

    ASSERT_OK(scanForUpgrade(opCtx, nss, ValidationActionEnum::error));
}

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsOKForEmptyCollectionWithValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    ASSERT_OK(createCollection(
        opCtx, nss, makeUpgradeOptions(ValidationLevelEnum::strict, true), boost::none));

    ASSERT_OK(scanForUpgrade(opCtx, nss, ValidationActionEnum::error));
}

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsErrorWhenDocumentViolatesValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.validator = fromjson("{a: {$exists: true}}");
    // 'warn' lets the test insert violating documents; the upgrade request passes 'error'.
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = ValidationLevelEnum::strict;
    options.prepareConstraintValidationLevel = true;
    options.uuid = UUID::gen();

    {
        ASSERT_OK(createCollection(opCtx, nss, options, boost::none));
        auto coll = acquireForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), BSON("_id" << 1 << "b" << 1)));
        wuow.commit();
    }

    ASSERT_EQ(scanForUpgrade(opCtx, nss, ValidationActionEnum::error).code(), 12370902);
}

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsOKWhenAllDocumentsConformToValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.validator = fromjson("{a: {$exists: true}}");
    // 'warn' lets the test insert violating documents; the upgrade request passes 'error'.
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = ValidationLevelEnum::strict;
    options.prepareConstraintValidationLevel = true;
    options.uuid = UUID::gen();

    {
        ASSERT_OK(createCollection(opCtx, nss, options, boost::none));
        auto coll = acquireForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), BSON("_id" << 1 << "a" << 1)));
        wuow.commit();
    }

    ASSERT_OK(scanForUpgrade(opCtx, nss, ValidationActionEnum::error));
}

TEST_F(ConstraintValidationLevelUpgradeTest, ReturnsErrorWhenDocumentViolatesJsonSchemaValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.validator = fromjson("{$jsonSchema: {required: [\"a\"]}}");
    // 'warn' lets the test insert violating documents; the upgrade request passes 'error'.
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = ValidationLevelEnum::strict;
    options.prepareConstraintValidationLevel = true;
    options.uuid = UUID::gen();

    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));
    {
        auto coll = acquireForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), BSON("_id" << 1 << "b" << 1)));
        wuow.commit();
    }

    ASSERT_EQ(scanForUpgrade(opCtx, nss, ValidationActionEnum::error).code(), 12370902);
}

TEST_F(ConstraintValidationLevelUpgradeTest, ErrorMessageTruncatesLargeValidator) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    // Build a validator whose JSON serialization exceeds 10KB via a long schema title.
    std::string longTitle(15000, 'x');
    CollectionOptions options;
    options.validator =
        BSON("$jsonSchema" << BSON("title" << longTitle << "required" << BSON_ARRAY("a")));
    // 'warn' lets the test insert violating documents; the upgrade request passes 'error'.
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = ValidationLevelEnum::strict;
    options.prepareConstraintValidationLevel = true;
    options.uuid = UUID::gen();

    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));
    {
        auto coll = acquireForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), BSON("_id" << 1 << "b" << 1)));
        wuow.commit();
    }

    auto status = scanForUpgrade(opCtx, nss, ValidationActionEnum::error);
    ASSERT_EQ(status.code(), 12370902);
    ASSERT_STRING_CONTAINS(status.reason(), "First offending document _id: 1");
    ASSERT_STRING_CONTAINS(status.reason(), "<your collection's validator>");
    ASSERT_STRING_OMITS(status.reason(), longTitle);
}

TEST_F(ConstraintValidationLevelUpgradeTest, SkipsScanWhenCollectionAlreadyAtConstraintLevel) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.validator = fromjson("{a: {$exists: true}}");
    options.validationLevel = ValidationLevelEnum::constraint;
    options.uuid = UUID::gen();
    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));

    // The aggregate must not be dispatched when the collection is already at constraint level.
    ASSERT_OK(scanForUpgrade(opCtx, nss, ValidationActionEnum::error));
}

// Tests for 'noDocumentsViolatingValidator'. Where a test inserts a document that
// violates the validator, the scan would return 12370902, so a BadValue proves the precondition
// check ran first and the collection was not scanned.

TEST_F(ConstraintValidationLevelUpgradeTest, UpgradeCheckReturnsOKForNonExistentCollection) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    ASSERT_OK(scanForUpgrade(operationContext(), nss));
}

TEST_F(ConstraintValidationLevelUpgradeTest, UpgradeCheckRejectsStrictWithoutPrepare) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();
    ASSERT_OK(createCollection(
        opCtx, nss, makeUpgradeOptions(ValidationLevelEnum::strict, false), boost::none));
    insertDocument(opCtx, nss, BSON("_id" << 1 << "b" << 1));

    auto status = scanForUpgrade(opCtx, nss, ValidationActionEnum::error);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.reason(), "prepareConstraintValidationLevel");
}

TEST_F(ConstraintValidationLevelUpgradeTest, UpgradeCheckRejectsModerateBeforeScanning) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();
    ASSERT_OK(createCollection(
        opCtx, nss, makeUpgradeOptions(ValidationLevelEnum::moderate, true), boost::none));
    insertDocument(opCtx, nss, BSON("_id" << 1 << "b" << 1));

    auto status = scanForUpgrade(opCtx, nss, ValidationActionEnum::error);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.reason(), "only be changed to 'constraint' from 'strict'");
}

TEST_F(ConstraintValidationLevelUpgradeTest, UpgradeCheckRejectsValidatorChangeBeforeScanning) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();
    ASSERT_OK(createCollection(
        opCtx, nss, makeUpgradeOptions(ValidationLevelEnum::strict, true), boost::none));
    insertDocument(opCtx, nss, BSON("_id" << 1 << "b" << 1));

    auto status = scanForUpgrade(opCtx, nss, ValidationActionEnum::error, /*hasNewValidator=*/true);
    ASSERT_EQ(status.code(), ErrorCodes::BadValue);
    ASSERT_STRING_CONTAINS(status.reason(), "Validator cannot be changed");
}

TEST_F(ConstraintValidationLevelUpgradeTest, UpgradeCheckRejectsWarnActionBeforeScanning) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();
    ASSERT_OK(createCollection(
        opCtx, nss, makeUpgradeOptions(ValidationLevelEnum::strict, true), boost::none));
    insertDocument(opCtx, nss, BSON("_id" << 1 << "b" << 1));

    // The collection's own action is already 'warn', so this also covers the default.
    for (auto action : {boost::optional<ValidationActionEnum>(ValidationActionEnum::warn),
                        boost::optional<ValidationActionEnum>()}) {
        auto status = scanForUpgrade(opCtx, nss, action);
        ASSERT_EQ(status.code(), ErrorCodes::BadValue);
        ASSERT_STRING_CONTAINS(status.reason(), "'warn' is not allowed");
    }
}

TEST_F(ConstraintValidationLevelUpgradeTest, UpgradeCheckScansWhenPreconditionsHold) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();
    ASSERT_OK(createCollection(
        opCtx, nss, makeUpgradeOptions(ValidationLevelEnum::strict, true), boost::none));
    insertDocument(opCtx, nss, BSON("_id" << 1 << "a" << 1));

    ASSERT_OK(scanForUpgrade(opCtx, nss, ValidationActionEnum::error));

    insertDocument(opCtx, nss, BSON("_id" << 2 << "b" << 1));
    ASSERT_EQ(scanForUpgrade(opCtx, nss, ValidationActionEnum::error).code(), 12370902);
}

TEST_F(ConstraintValidationLevelUpgradeTest, UpgradeCheckIsNoOpWhenAlreadyAtConstraint) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();
    CollectionOptions options;
    options.validator = fromjson("{a: {$exists: true}}");
    options.validationLevel = ValidationLevelEnum::constraint;
    options.uuid = UUID::gen();
    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));

    ASSERT_OK(scanForUpgrade(opCtx, nss, ValidationActionEnum::error));
}

TEST_F(ConstraintValidationLevelUpgradeTest, ErrorMessageContainsValidatorAndCollectionHint) {
    const auto nss = NamespaceString::createNamespaceString_forTest("testdb", "testcoll");
    auto* opCtx = operationContext();

    CollectionOptions options;
    options.validator = fromjson("{a: {$exists: true}}");
    // 'warn' lets the test insert violating documents; the upgrade request passes 'error'.
    options.validationAction = ValidationActionEnum::warn;
    options.validationLevel = ValidationLevelEnum::strict;
    options.prepareConstraintValidationLevel = true;
    options.uuid = UUID::gen();

    ASSERT_OK(createCollection(opCtx, nss, options, boost::none));
    {
        auto coll = acquireForWrite(opCtx, nss);
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(Helpers::insert(opCtx, coll.getCollectionPtr(), BSON("_id" << 1 << "b" << 1)));
        wuow.commit();
    }

    auto status = scanForUpgrade(opCtx, nss, ValidationActionEnum::error);
    ASSERT_EQ(status.code(), 12370902);
    ASSERT_STRING_CONTAINS(status.reason(), "First offending document _id: 1");
    ASSERT_STRING_CONTAINS(status.reason(), "Run db.testcoll.find({\"$nor\": [");
    StringBuilder validatorStr;
    options.validator.toString(validatorStr, /*isArray=*/false, /*full=*/true);
    ASSERT_STRING_CONTAINS(status.reason(), validatorStr.str());
    ASSERT_STRING_CONTAINS(status.reason(), "]}) to find non-compliant documents.");
}

}  // namespace
}  // namespace mongo
