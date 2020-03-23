/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/index_builds_manager.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_timestamp_helper.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/str.h"

namespace mongo {

namespace {

/**
 * Returns basic info on index builders.
 */
std::string toSummary(const std::map<UUID, std::shared_ptr<MultiIndexBlock>>& builders) {
    str::stream ss;
    ss << "Number of builders: " << builders.size() << ": [";
    bool first = true;
    for (const auto& pair : builders) {
        if (!first) {
            ss << ", ";
        }
        ss << pair.first;
        first = false;
    }
    ss << "]";
    return ss;
}

}  // namespace

IndexBuildsManager::SetupOptions::SetupOptions() = default;

IndexBuildsManager::~IndexBuildsManager() {
    invariant(_builders.empty(),
              str::stream() << "Index builds still active: " << toSummary(_builders));
}

Status IndexBuildsManager::setUpIndexBuild(OperationContext* opCtx,
                                           Collection* collection,
                                           const std::vector<BSONObj>& specs,
                                           const UUID& buildUUID,
                                           OnInitFn onInit,
                                           SetupOptions options) {
    _registerIndexBuild(buildUUID);

    const auto& nss = collection->ns();
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X),
              str::stream() << "Unable to set up index build " << buildUUID << ": collection "
                            << nss.ns() << " is not locked in exclusive mode.");

    auto builder = invariant(_getBuilder(buildUUID));
    if (options.protocol == IndexBuildProtocol::kTwoPhase) {
        builder->setTwoPhaseBuildUUID(buildUUID);
    }

    // Ignore uniqueness constraint violations when relaxed, for single-phase builds on
    // secondaries. Secondaries can complete index builds in the middle of batches, which creates
    // the potential for finding duplicate key violations where there otherwise would be none at
    // consistent states.
    // Two-phase builds will defer any unique key violations until commit-time.
    if (options.indexConstraints == IndexConstraints::kRelax &&
        options.protocol == IndexBuildProtocol::kSinglePhase) {
        builder->ignoreUniqueConstraint();
    }

    std::vector<BSONObj> indexes;
    try {
        indexes = writeConflictRetry(opCtx, "IndexBuildsManager::setUpIndexBuild", nss.ns(), [&]() {
            return uassertStatusOK(builder->init(opCtx, collection, specs, onInit));
        });
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    LOGV2(
        20346,
        "Index build initialized: {buildUUID}: {nss} ({collection_uuid} ): indexes: {indexes_size}",
        "buildUUID"_attr = buildUUID,
        "nss"_attr = nss,
        "collection_uuid"_attr = collection->uuid(),
        "indexes_size"_attr = indexes.size());

    return Status::OK();
}

Status IndexBuildsManager::startBuildingIndex(OperationContext* opCtx,
                                              Collection* collection,
                                              const UUID& buildUUID) {
    auto builder = invariant(_getBuilder(buildUUID));

    return builder->insertAllDocumentsInCollection(opCtx, collection);
}

StatusWith<std::pair<long long, long long>> IndexBuildsManager::startBuildingIndexForRecovery(
    OperationContext* opCtx, NamespaceString ns, const UUID& buildUUID, RepairData repair) {
    auto builder = invariant(_getBuilder(buildUUID));

    auto coll = CollectionCatalog::get(opCtx).lookupCollectionByNamespace(opCtx, ns);
    auto rs = coll ? coll->getRecordStore() : nullptr;

    // Iterate all records in the collection. Validate the records and index them
    // if they are valid.  Delete them (if in repair mode), or crash, if they are not valid.
    long long numRecords = 0;
    long long dataSize = 0;

    auto cursor = rs->getCursor(opCtx);
    auto record = cursor->next();
    while (record) {
        opCtx->checkForInterrupt();
        // Cursor is left one past the end of the batch inside writeConflictRetry
        auto beginBatchId = record->id;
        Status status = writeConflictRetry(opCtx, "repairDatabase", ns.ns(), [&] {
            // In the case of WCE in a partial batch, we need to go back to the beginning
            if (!record || (beginBatchId != record->id)) {
                record = cursor->seekExact(beginBatchId);
            }
            WriteUnitOfWork wunit(opCtx);
            for (int i = 0; record && i < internalInsertMaxBatchSize.load(); i++) {
                RecordId id = record->id;
                RecordData& data = record->data;
                // Use the latest BSON validation version. We retain decimal data when repairing
                // database even if decimal is disabled.
                auto validStatus = validateBSON(data.data(), data.size(), BSONVersion::kLatest);
                if (!validStatus.isOK()) {
                    if (repair == RepairData::kNo) {
                        LOGV2_FATAL(31396,
                                    "Invalid BSON detected at {id}: {validStatus}",
                                    "id"_attr = id,
                                    "validStatus"_attr = redact(validStatus));
                    }
                    LOGV2_WARNING(20348,
                                  "Invalid BSON detected at {id}: {validStatus}. Deleting.",
                                  "id"_attr = id,
                                  "validStatus"_attr = redact(validStatus));
                    rs->deleteRecord(opCtx, id);
                } else {
                    numRecords++;
                    dataSize += data.size();
                    auto insertStatus = builder->insert(opCtx, data.releaseToBson(), id);
                    if (!insertStatus.isOK()) {
                        return insertStatus;
                    }
                }
                record = cursor->next();
            }

            // Time to yield; make a safe copy of the current record before releasing our cursor.
            if (record)
                record->data.makeOwned();

            cursor->save();  // Can't fail per API definition
            // When this exits via success or WCE, we need to restore the cursor
            ON_BLOCK_EXIT([opCtx, ns, &cursor]() {
                // restore CAN throw WCE per API
                writeConflictRetry(
                    opCtx, "retryRestoreCursor", ns.ns(), [&cursor] { cursor->restore(); });
            });
            wunit.commit();
            return Status::OK();
        });
        if (!status.isOK()) {
            return status;
        }
    }

    Status status = builder->dumpInsertsFromBulk(opCtx);
    if (!status.isOK()) {
        return status;
    }
    return std::make_pair(numRecords, dataSize);
}

Status IndexBuildsManager::drainBackgroundWrites(
    OperationContext* opCtx,
    const UUID& buildUUID,
    RecoveryUnit::ReadSource readSource,
    IndexBuildInterceptor::DrainYieldPolicy drainYieldPolicy) {
    auto builder = invariant(_getBuilder(buildUUID));

    return builder->drainBackgroundWrites(opCtx, readSource, drainYieldPolicy);
}

Status IndexBuildsManager::retrySkippedRecords(OperationContext* opCtx,
                                               const UUID& buildUUID,
                                               Collection* collection) {
    auto builder = invariant(_getBuilder(buildUUID));
    return builder->retrySkippedRecords(opCtx, collection);
}

Status IndexBuildsManager::checkIndexConstraintViolations(OperationContext* opCtx,
                                                          const UUID& buildUUID) {
    auto builder = invariant(_getBuilder(buildUUID));

    return builder->checkConstraints(opCtx);
}

Status IndexBuildsManager::commitIndexBuild(OperationContext* opCtx,
                                            Collection* collection,
                                            const NamespaceString& nss,
                                            const UUID& buildUUID,
                                            MultiIndexBlock::OnCreateEachFn onCreateEachFn,
                                            MultiIndexBlock::OnCommitFn onCommitFn) {
    auto builder = invariant(_getBuilder(buildUUID));

    return writeConflictRetry(
        opCtx,
        "IndexBuildsManager::commitIndexBuild",
        nss.ns(),
        [this, builder, buildUUID, opCtx, collection, nss, &onCreateEachFn, &onCommitFn] {
            WriteUnitOfWork wunit(opCtx);
            auto status = builder->commit(opCtx, collection, onCreateEachFn, onCommitFn);
            if (!status.isOK()) {
                return status;
            }
            wunit.commit();

            // Required call to clean up even though commit cleaned everything up.
            builder->cleanUpAfterBuild(opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn);
            _unregisterIndexBuild(buildUUID);
            return Status::OK();
        });
}

bool IndexBuildsManager::abortIndexBuild(const UUID& buildUUID, const std::string& reason) {
    stdx::unique_lock<Latch> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return false;
    }

    std::shared_ptr<MultiIndexBlock> builder = builderIt->second;

    lk.unlock();
    builder->abort(reason);
    return true;
}

bool IndexBuildsManager::abortIndexBuildWithoutCleanup(OperationContext* opCtx,
                                                       Collection* collection,
                                                       const UUID& buildUUID,
                                                       const std::string& reason) {
    auto builder = _getBuilder(buildUUID);
    if (!builder.isOK()) {
        return false;
    }

    LOGV2(20347,
          "Index build aborted without cleanup: {buildUUID}: {reason}",
          "buildUUID"_attr = buildUUID,
          "reason"_attr = reason);

    builder.getValue()->abortWithoutCleanup(opCtx);
    builder.getValue()->cleanUpAfterBuild(opCtx, collection, MultiIndexBlock::kNoopOnCleanUpFn);
    _unregisterIndexBuild(buildUUID);

    return true;
}

void IndexBuildsManager::tearDownIndexBuild(OperationContext* opCtx,
                                            Collection* collection,
                                            const UUID& buildUUID,
                                            OnCleanUpFn onCleanUpFn) {
    auto builder = _getBuilder(buildUUID);
    if (!builder.isOK()) {
        return;
    }

    builder.getValue()->cleanUpAfterBuild(opCtx, collection, onCleanUpFn);
    _unregisterIndexBuild(buildUUID);
}

bool IndexBuildsManager::isBackgroundBuilding(const UUID& buildUUID) {
    auto builder = invariant(_getBuilder(buildUUID));
    return builder->isBackgroundBuilding();
}

void IndexBuildsManager::verifyNoIndexBuilds_forTestOnly() {
    invariant(_builders.empty());
}

void IndexBuildsManager::_registerIndexBuild(UUID buildUUID) {
    stdx::unique_lock<Latch> lk(_mutex);

    std::shared_ptr<MultiIndexBlock> mib = std::make_shared<MultiIndexBlock>();
    invariant(_builders.insert(std::make_pair(buildUUID, mib)).second);
}

void IndexBuildsManager::_unregisterIndexBuild(const UUID& buildUUID) {
    stdx::unique_lock<Latch> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return;
    }
    _builders.erase(builderIt);
}

StatusWith<std::shared_ptr<MultiIndexBlock>> IndexBuildsManager::_getBuilder(
    const UUID& buildUUID) {
    stdx::unique_lock<Latch> lk(_mutex);
    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return {ErrorCodes::NoSuchKey, str::stream() << "No index build with UUID: " << buildUUID};
    }
    return builderIt->second;
}

}  // namespace mongo
