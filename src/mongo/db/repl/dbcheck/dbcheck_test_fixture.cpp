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

#include "mongo/db/repl/dbcheck/dbcheck_test_fixture.h"

#include "mongo/bson/bson_validate_gen.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/collection_crud/collection_write_path.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/health_log.h"
#include "mongo/db/local_catalog/health_log_gen.h"
#include "mongo/db/local_catalog/health_log_interface.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/op_observer/op_observer_impl.h"
#include "mongo/db/op_observer/op_observer_registry.h"
#include "mongo/db/op_observer/operation_logger_mock.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/storage/snapshot_manager.h"
#include "mongo/util/fail_point.h"

#include <boost/optional/optional.hpp>

namespace mongo {

void DbCheckTest::setUp() {
    CatalogTestFixture::setUp();

    // Create collection kNss for unit tests to use. It will possess a default _id index.
    ASSERT_OK(storageInterface()->createCollection(operationContext(), kNss, _collectionOptions));

    auto service = getServiceContext();

    // Set up OpObserver so that we will append actual oplog entries to the oplog using
    // repl::logOp(). This supports index builds that have to look up the last oplog entry.
    auto opObserverRegistry = dynamic_cast<OpObserverRegistry*>(service->getOpObserver());
    opObserverRegistry->addObserver(
        std::make_unique<OpObserverImpl>(std::make_unique<OperationLoggerMock>()));

    // Index builds expect a non-empty oplog and a valid committed snapshot.
    auto opCtx = operationContext();
    Lock::GlobalLock lk(opCtx, MODE_IX);
    WriteUnitOfWork wuow(opCtx);
    service->getOpObserver()->onOpMessage(opCtx, BSONObj());
    wuow.commit();

    // Provide an initial committed snapshot so that index build can begin the collection scan.
    auto snapshotManager = service->getStorageEngine()->getSnapshotManager();
    auto lastAppliedOpTime = repl::ReplicationCoordinator::get(service)->getMyLastAppliedOpTime();
    snapshotManager->setCommittedSnapshot(lastAppliedOpTime.getTimestamp());

    // Set up the health log writer. To ensure writes are completed, each test should individually
    // shut down the health log.
    HealthLogInterface::set(service, std::make_unique<HealthLog>());
    HealthLogInterface::get(service)->startup();
};

void DbCheckTest::insertDocs(OperationContext* opCtx,
                             int startIDNum,
                             int numDocs,
                             const std::vector<std::string>& fieldNames,
                             bool duplicateFieldNames) {
    const AutoGetCollection coll(opCtx, kNss, MODE_IX);
    std::vector<InsertStatement> inserts;
    for (int i = 0; i < numDocs; ++i) {
        BSONObjBuilder bsonBuilder;
        bsonBuilder << "_id" << i + startIDNum;
        for (const auto& name : fieldNames) {
            bsonBuilder << name << i + startIDNum;
        }

        // If `duplicateFieldNames` is true, the inserted doc will have a duplicated field name so
        // that it fails the kExtended mode of BSON validate check.
        if (duplicateFieldNames && !fieldNames.empty()) {
            bsonBuilder << fieldNames[0] << i + startIDNum + 1;
        }

        const auto obj = bsonBuilder.obj();
        inserts.push_back(InsertStatement(obj));
    }

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(collection_internal::insertDocuments(
            opCtx, *coll, inserts.begin(), inserts.end(), nullptr, false));
        wuow.commit();
    }
}

void DbCheckTest::insertInvalidUuid(OperationContext* opCtx,
                                    int startIDNum,
                                    const std::vector<std::string>& fieldNames) {
    const AutoGetCollection coll(opCtx, kNss, MODE_IX);
    std::vector<InsertStatement> inserts;

    BSONObjBuilder bsonBuilder;
    bsonBuilder << "_id" << startIDNum;
    uint8_t uuidBytes[] = {0, 0, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 0};
    // The UUID is invalid because its length is 10 instead of 16.
    bsonBuilder << "invalid uuid" << BSONBinData(uuidBytes, 10, newUUID);
    const auto obj = bsonBuilder.obj();
    inserts.push_back(InsertStatement(obj));

    {
        WriteUnitOfWork wuow(opCtx);
        ASSERT_OK(collection_internal::insertDocuments(
            opCtx, *coll, inserts.begin(), inserts.end(), nullptr, false));
        wuow.commit();
    }
}

void DbCheckTest::deleteDocs(OperationContext* opCtx, int startIDNum, int numDocs) {
    auto cmdObj = [&] {
        write_ops::DeleteCommandRequest deleteOp(kNss);
        deleteOp.setDeletes({[&] {
            std::vector<write_ops::DeleteOpEntry> deleteStatements;
            for (int i = 0; i < numDocs; ++i) {
                write_ops::DeleteOpEntry entry;
                entry.setQ(BSON("_id" << i + startIDNum));
                entry.setMulti(false);
            }
            return deleteStatements;
        }()});
        return deleteOp.toBSON();
    }();

    DBDirectClient client(opCtx);
    BSONObj result;
    client.runCommand(kNss.dbName(), cmdObj, result);
    ASSERT_OK(getStatusFromWriteCommandReply(result));
}

void DbCheckTest::insertDocsWithMissingIndexKeys(OperationContext* opCtx,
                                                 int startIDNum,
                                                 int numDocs,
                                                 const std::vector<std::string>& fieldNames) {
    FailPointEnableBlock skipIndexFp("skipIndexNewRecords", BSON("skipIdIndex" << false));
    insertDocs(opCtx, startIDNum, numDocs, fieldNames);
}

DbCheckCollectionInfo DbCheckTest::createDbCheckCollectionInfo(
    OperationContext* opCtx,
    const BSONObj& start,
    const BSONObj& end,
    const SecondaryIndexCheckParameters& params) {
    auto swUUID = storageInterface()->getCollectionUUID(opCtx, kNss);
    ASSERT_OK(swUUID);

    DbCheckCollectionInfo info = {
        .nss = kNss,
        .uuid = swUUID.getValue(),
        .start = start,
        .end = end,
        .maxCount = kDefaultMaxCount,
        .maxSize = kDefaultMaxSize,
        .maxDocsPerBatch = kDefaultMaxDocsPerBatch,
        .maxBatchTimeMillis = kDefaultMaxBatchTimeMillis,
        .writeConcern = WriteConcernOptions(),
        .secondaryIndexCheckParameters = params,
        .dataThrottle = DataThrottle(opCtx, []() { return 0; }),
    };
    return info;
}

/**
 * Builds an index on kNss. 'indexKey' specifies the index key, e.g. {'a': 1};
 */
void DbCheckTest::createIndex(OperationContext* opCtx, const BSONObj& indexKey) {
    AutoGetCollection collection(opCtx, kNss, MODE_X);
    ASSERT(collection);

    ASSERT_EQ(1, indexKey.nFields()) << kNss.toStringForErrorMsg() << "/" << indexKey;
    auto spec = BSON("v" << int(IndexConfig::kLatestIndexVersion) << "key" << indexKey << "name"
                         << (indexKey.firstElementFieldNameStringData() + "_1"));

    auto indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    auto indexConstraints = IndexBuildsManager::IndexConstraints::kEnforce;
    auto fromMigrate = false;
    indexBuildsCoord->createIndex(opCtx, collection->uuid(), spec, indexConstraints, fromMigrate);
}

Status DbCheckTest::runHashForCollectionCheck(
    OperationContext* opCtx,
    const BSONObj& start,
    const BSONObj& end,
    boost::optional<SecondaryIndexCheckParameters> secondaryIndexCheckParams,
    int64_t maxCount,
    int64_t maxBytes,
    Date_t deadlineOnSecondary) {
    const DbCheckAcquisition acquisition(
        opCtx, kNss, {RecoveryUnit::ReadSource::kNoTimestamp}, PrepareConflictBehavior::kEnforce);
    const auto& collection = acquisition.collection().getCollectionPtr();
    // Disable throttling for testing.
    DataThrottle dataThrottle(opCtx, []() { return 0; });
    auto hasher = DbCheckHasher(opCtx,
                                acquisition,
                                start,
                                end,
                                secondaryIndexCheckParams,
                                &dataThrottle,
                                boost::none /* indexName */,
                                maxCount,
                                maxBytes,
                                deadlineOnSecondary);
    return hasher.hashForCollectionCheck(opCtx, collection);
}

Status DbCheckTest::runHashForExtraIndexKeysCheck(
    OperationContext* opCtx,
    const BSONObj& batchStart,
    const BSONObj& batchEnd,
    const BSONObj& lastKeyChecked,
    boost::optional<SecondaryIndexCheckParameters> secondaryIndexCheckParams,
    int64_t maxCount,
    int64_t maxBytes,
    Date_t deadlineOnSecondary) {
    const DbCheckAcquisition acquisition(
        opCtx, kNss, {RecoveryUnit::ReadSource::kNoTimestamp}, PrepareConflictBehavior::kEnforce);
    const auto& collection = acquisition.collection().getCollectionPtr();
    // Disable throttling for testing.
    DataThrottle dataThrottle(opCtx, []() { return 0; });
    auto hasher = DbCheckHasher(opCtx,
                                acquisition,
                                batchStart,
                                batchEnd,
                                secondaryIndexCheckParams,
                                &dataThrottle,
                                secondaryIndexCheckParams.get().getSecondaryIndex(),
                                maxCount,
                                maxBytes,
                                deadlineOnSecondary);
    return hasher.hashForExtraIndexKeysCheck(
        opCtx, collection.get(), batchStart, batchEnd, lastKeyChecked);
}

SecondaryIndexCheckParameters DbCheckTest::createSecondaryIndexCheckParams(
    DbCheckValidationModeEnum validateMode,
    StringData secondaryIndex,
    bool skipLookupForExtraKeys,
    BSONValidateModeEnum bsonValidateMode) {
    auto params = SecondaryIndexCheckParameters();
    params.setValidateMode(validateMode);
    params.setSecondaryIndex(secondaryIndex);
    params.setSkipLookupForExtraKeys(skipLookupForExtraKeys);
    params.setBsonValidateMode(bsonValidateMode);
    return params;
}

int DbCheckTest::getNumDocsFoundInHealthLog(OperationContext* opCtx, const BSONObj& query) {
    FindCommandRequest findCommand(NamespaceString::kLocalHealthLogNamespace);
    findCommand.setFilter(query);

    DBDirectClient client(opCtx);
    auto cursor = client.find(
        findCommand, ReadPreferenceSetting{ReadPreference::PrimaryPreferred}, ExhaustMode::kOff);

    int count = 0;
    while (cursor->more()) {
        count++;
        cursor->next();
    }
    return count;
}

}  // namespace mongo
