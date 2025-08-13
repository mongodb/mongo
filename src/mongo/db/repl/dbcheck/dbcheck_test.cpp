/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/bson/bson_validate_gen.h"
#include "mongo/db/local_catalog/health_log.h"
#include "mongo/db/local_catalog/health_log_gen.h"
#include "mongo/db/local_catalog/health_log_interface.h"
#include "mongo/db/repl/dbcheck/dbcheck_test_fixture.h"

#include <boost/optional/optional.hpp>

namespace mongo {

TEST_F(DbCheckTest, DbCheckHasherErrorsOnCollectionCheckTimeout) {
    auto opCtx = operationContext();
    std::vector<std::string> fieldNames = {"a"};

    createIndex(opCtx, BSON("a" << 1));
    insertDocs(opCtx, 0, 5000, fieldNames);

    auto params = createSecondaryIndexCheckParams(
        DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck,
        "" /* secondaryIndex */);
    ASSERT_EQ(ErrorCodes::DbCheckSecondaryBatchTimeout,
              runHashForCollectionCheck(opCtx,
                                        docMinKey,
                                        docMaxKey,
                                        params,
                                        std::numeric_limits<int64_t>::max() /*maxCount*/,
                                        std::numeric_limits<int64_t>::max() /*maxBytes*/,
                                        Date_t::now() - Milliseconds(1) /*deadlineOnSecondary*/)
                  .code());
}

TEST_F(DbCheckTest, DbCheckHasherErrorsOnExtraIndexKeysCheckTimeout) {
    auto opCtx = operationContext();
    std::vector<std::string> fieldNames = {"a"};

    createIndex(opCtx, BSON("a" << 1));
    insertDocs(opCtx, 0, 5000, fieldNames);

    auto params = createSecondaryIndexCheckParams(
        DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck,
        "a_1" /* secondaryIndex */);
    ASSERT_EQ(ErrorCodes::DbCheckSecondaryBatchTimeout,
              runHashForExtraIndexKeysCheck(opCtx,
                                            docMinKey,
                                            docMaxKey,
                                            docMaxKey,
                                            params,
                                            std::numeric_limits<int64_t>::max() /*maxCount*/,
                                            std::numeric_limits<int64_t>::max() /*maxBytes*/,
                                            Date_t::now() - Milliseconds(1) /*deadlineOnSecondary*/)
                  .code());
}

TEST_F(DbCheckTest, DbCheckHasherNoMissingKeys) {
    auto opCtx = operationContext();
    std::vector<std::string> fieldNames = {"a"};

    createIndex(opCtx, BSON("a" << 1));
    insertDocs(opCtx, 0, 10, fieldNames);

    auto params = createSecondaryIndexCheckParams(
        DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck,
        "" /* secondaryIndex */);
    ASSERT_OK(runHashForCollectionCheck(opCtx, docMinKey, docMaxKey, params));

    // Shut down the health log writer so that the writes get flushed to the health log collection.
    auto service = getServiceContext();
    HealthLogInterface::get(service)->shutdown();

    // Verify that no error health log entries were logged.
    ASSERT_EQ(0, getNumDocsFoundInHealthLog(opCtx, errQuery));
}

TEST_F(DbCheckTest, DbCheckHasherMissingKeys) {
    auto opCtx = operationContext();
    std::vector<std::string> fieldNames = {"a"};

    createIndex(opCtx, BSON("a" << 1));
    insertDocs(opCtx, 0, 10, fieldNames);

    // Turn on failpoint.
    const auto numErrDocs = 5;
    insertDocsWithMissingIndexKeys(opCtx, 10, numErrDocs, fieldNames);

    auto params = createSecondaryIndexCheckParams(
        DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck,
        "" /* secondaryIndex */);
    ASSERT_OK(runHashForCollectionCheck(opCtx, docMinKey, docMaxKey, params));

    // Shut down the health log writer so that the writes get flushed to the health log collection.
    auto service = getServiceContext();
    HealthLogInterface::get(service)->shutdown();

    // Verify that the correct number of health log error entries were logged.
    ASSERT_EQ(numErrDocs, getNumDocsFoundInHealthLog(opCtx, errQuery));
    ASSERT_EQ(numErrDocs, getNumDocsFoundInHealthLog(opCtx, missingKeyQuery));
}

TEST_F(DbCheckTest, DbCheckDocumentWithDuplicateFieldNames) {
    auto opCtx = operationContext();
    std::vector<std::string> fieldNames = {"a", "b", "c"};

    createIndex(opCtx, BSON("a" << 1));
    const auto numWarningDocs = 10;
    insertDocs(
        opCtx, 0 /* startIdNum */, numWarningDocs, fieldNames, true /* duplicateFieldNames */);

    auto params = createSecondaryIndexCheckParams(
        DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck,
        "" /* secondaryIndex */,
        false /* skipLookupForExtraKeys */,
        BSONValidateModeEnum::kFull);
    ASSERT_OK(runHashForCollectionCheck(opCtx, docMinKey, docMaxKey, params));

    // Shut down the health log writer so that the writes get flushed to the health log collection.
    auto service = getServiceContext();
    HealthLogInterface::get(service)->shutdown();

    // Verify that the correct number of health log warning entries were logged.
    ASSERT_EQ(numWarningDocs, getNumDocsFoundInHealthLog(opCtx, BSONWarningQuery));
    ASSERT_EQ(numWarningDocs, getNumDocsFoundInHealthLog(opCtx, warningQuery));
    ASSERT_EQ(0, getNumDocsFoundInHealthLog(opCtx, errQuery));
}

TEST_F(DbCheckTest, DbCheckDocumentWithInvalidUuid) {
    auto opCtx = operationContext();
    std::vector<std::string> fieldNames = {"a", "b", "c"};

    createIndex(opCtx, BSON("a" << 1));
    const auto numWarningDocs = 1;
    insertInvalidUuid(opCtx, 0 /* startIdNum */, fieldNames);

    auto params = createSecondaryIndexCheckParams(
        DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck,
        "" /* secondaryIndex */,
        false /* skipLookupForExtraKeys */,
        BSONValidateModeEnum::kExtended);
    ASSERT_OK(runHashForCollectionCheck(opCtx, docMinKey, docMaxKey, params));

    // Shut down the health log writer so that the writes get flushed to the health log collection.
    auto service = getServiceContext();
    HealthLogInterface::get(service)->shutdown();

    // Verify that the correct number of health log warning entries were logged.
    ASSERT_EQ(numWarningDocs, getNumDocsFoundInHealthLog(opCtx, BSONWarningQuery));
    ASSERT_EQ(numWarningDocs, getNumDocsFoundInHealthLog(opCtx, warningQuery));
    ASSERT_EQ(0, getNumDocsFoundInHealthLog(opCtx, errQuery));
}

TEST_F(DbCheckClusteredCollectionTest, DbCheckIdRecordIdMismatch) {
    auto opCtx = operationContext();
    const AutoGetCollection coll(opCtx, kNss, MODE_IX);
    // The test fixture setUp() will make the collection a clustered collection.
    const auto& collection = coll.getCollection();
    ASSERT_TRUE(collection->isClustered());
    auto doc = BSON("_id" << 1 << "a" << 1);
    auto docToGenerateWrongRecordId = BSON("_id" << 2);
    auto mismatchedRecordId =
        uassertStatusOK(record_id_helpers::keyForDoc(docToGenerateWrongRecordId,
                                                     collection->getClusteredInfo()->getIndexSpec(),
                                                     collection->getDefaultCollator()));
    auto ts = Timestamp();
    WriteUnitOfWork wuow(opCtx);
    auto recordIdStatus =
        collection->getRecordStore()->insertRecord(opCtx,
                                                   *shard_role_details::getRecoveryUnit(opCtx),
                                                   mismatchedRecordId,
                                                   doc.objdata(),
                                                   doc.objsize(),
                                                   ts);
    wuow.commit();
    ASSERT_OK(recordIdStatus);

    auto params = createSecondaryIndexCheckParams(
        DbCheckValidationModeEnum::dataConsistencyAndMissingIndexKeysCheck,
        "" /* secondaryIndex */,
        false /* skipLookupForExtraKeys */,
        BSONValidateModeEnum::kExtended);
    ASSERT_OK(runHashForCollectionCheck(opCtx, docMinKey, docMaxKey, params));
    // Shut down the health log writer so that the writes get flushed to the health log collection.
    auto service = getServiceContext();
    HealthLogInterface::get(service)->shutdown();
    ASSERT_EQ(1, getNumDocsFoundInHealthLog(opCtx, idRecordIdMismatchQuery));
    ASSERT_EQ(1, getNumDocsFoundInHealthLog(opCtx, errQuery));
}

TEST(DbCheckHelperTest, IsIndexOrderAndUniquenessPreservedCorrectOrder) {
    RecordId recordId1(1);
    RecordId recordId2(2);
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, BSON("a" << 1), Ordering::make(BSONObj()), recordId1);
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, BSON("a" << 2), Ordering::make(BSONObj()), recordId2);
    KeyStringEntry entry1(keyString1.release(), recordId1);
    KeyStringEntry entry2(keyString2.release(), recordId2);

    ASSERT_TRUE(isIndexOrderAndUniquenessPreserved(entry1, entry2, false /*isUnique*/));
    ASSERT_TRUE(isIndexOrderAndUniquenessPreserved(entry1, entry2, true /*isUnique*/));
}

TEST(DbCheckHelperTest, IsIndexOrderAndUniquenessPreservedKeyOrderViolation) {
    RecordId recordId1(1);
    RecordId recordId2(2);
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, BSON("a" << 2), Ordering::make(BSONObj()), recordId1);
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, BSON("a" << 1), Ordering::make(BSONObj()), recordId2);
    KeyStringEntry entry1(keyString1.release(), recordId1);
    KeyStringEntry entry2(keyString2.release(), recordId2);

    // The check should fail when the key order is violated regardless of index type and recordId.
    ASSERT_FALSE(isIndexOrderAndUniquenessPreserved(entry1, entry2, false /*isUnique*/));
    ASSERT_FALSE(isIndexOrderAndUniquenessPreserved(entry1, entry2, true /*isUnique*/));
}

TEST(DbCheckHelperTest, IsIndexOrderAndUniquenessPreservedSameKeyDifferentRecordId) {
    RecordId recordId1(1);
    RecordId recordId2(2);
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, BSON("a" << 1), Ordering::make(BSONObj()), recordId1);
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, BSON("a" << 1), Ordering::make(BSONObj()), recordId2);
    KeyStringEntry entry1(keyString1.release(), recordId1);
    KeyStringEntry entry2(keyString2.release(), recordId2);

    // The check should only fail when it is on unique index because unique index should not have
    // the same key string.
    ASSERT_TRUE(isIndexOrderAndUniquenessPreserved(entry1, entry2, false /*isUnique*/));
    ASSERT_FALSE(isIndexOrderAndUniquenessPreserved(entry1, entry2, true /*isUnique*/));
}

TEST(DbCheckHelperTest, IsIndexOrderAndUniquenessPreservedSameKeySameRecordId) {
    RecordId recordId1(1);
    RecordId recordId2(1);
    key_string::HeapBuilder keyString1(
        key_string::Version::kLatestVersion, BSON("a" << 1), Ordering::make(BSONObj()), recordId1);
    key_string::HeapBuilder keyString2(
        key_string::Version::kLatestVersion, BSON("a" << 1), Ordering::make(BSONObj()), recordId2);
    KeyStringEntry entry1(keyString1.release(), recordId1);
    KeyStringEntry entry2(keyString2.release(), recordId2);

    // The check should fail for both unique and non-unique indexes because (key string + recordId)
    // should always be unique.
    ASSERT_FALSE(isIndexOrderAndUniquenessPreserved(entry1, entry2, false /*isUnique*/));
    ASSERT_FALSE(isIndexOrderAndUniquenessPreserved(entry1, entry2, true /*isUnique*/));
}

}  // namespace mongo
