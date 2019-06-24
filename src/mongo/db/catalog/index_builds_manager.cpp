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
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_timestamp_helper.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
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
                            << nss.ns()
                            << " is not locked in exclusive mode.");

    auto builder = _getBuilder(buildUUID);

    // Ignore uniqueness constraint violations when relaxed (on secondaries). Secondaries can
    // complete index builds in the middle of batches, which creates the potential for finding
    // duplicate key violations where there otherwise would be none at consistent states.
    if (options.indexConstraints == IndexConstraints::kRelax) {
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

    if (options.forRecovery) {
        log() << "Index build initialized: " << buildUUID << ": " << nss
              << ": indexes: " << indexes.size();
    } else {
        log() << "Index build initialized: " << buildUUID << ": " << nss << " ("
              << *collection->uuid() << " ): indexes: " << indexes.size();
    }

    return Status::OK();
}

StatusWith<IndexBuildRecoveryState> IndexBuildsManager::recoverIndexBuild(
    const NamespaceString& nss, const UUID& buildUUID, std::vector<std::string> indexNames) {

    // TODO: Not yet implemented.

    return IndexBuildRecoveryState::Building;
}

Status IndexBuildsManager::startBuildingIndex(OperationContext* opCtx,
                                              Collection* collection,
                                              const UUID& buildUUID) {
    auto builder = _getBuilder(buildUUID);

    return builder->insertAllDocumentsInCollection(opCtx, collection);
}

StatusWith<std::pair<long long, long long>> IndexBuildsManager::startBuildingIndexForRecovery(
    OperationContext* opCtx, NamespaceString ns, const UUID& buildUUID) {
    auto builder = _getBuilder(buildUUID);

    auto cce = CollectionCatalog::get(opCtx).lookupCollectionCatalogEntryByNamespace(ns);
    auto rs = cce ? cce->getRecordStore() : nullptr;

    // Iterate all records in the collection. Delete them if they aren't valid BSON. Index them
    // if they are.
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
                    warning() << "Invalid BSON detected at " << id << ": " << redact(validStatus)
                              << ". Deleting.";
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

Status IndexBuildsManager::drainBackgroundWrites(OperationContext* opCtx,
                                                 const UUID& buildUUID,
                                                 RecoveryUnit::ReadSource readSource) {
    auto builder = _getBuilder(buildUUID);

    return builder->drainBackgroundWrites(opCtx, readSource);
}

Status IndexBuildsManager::finishBuildingPhase(const UUID& buildUUID) {
    auto multiIndexBlockPtr = _getBuilder(buildUUID);
    // TODO: verify that the index builder is in the expected state.

    // TODO: Not yet implemented.

    return Status::OK();
}

Status IndexBuildsManager::checkIndexConstraintViolations(OperationContext* opCtx,
                                                          const UUID& buildUUID) {
    auto builder = _getBuilder(buildUUID);

    return builder->checkConstraints(opCtx);
}

Status IndexBuildsManager::commitIndexBuild(OperationContext* opCtx,
                                            Collection* collection,
                                            const NamespaceString& nss,
                                            const UUID& buildUUID,
                                            MultiIndexBlock::OnCreateEachFn onCreateEachFn,
                                            MultiIndexBlock::OnCommitFn onCommitFn) {
    auto builder = _getBuilder(buildUUID);

    return writeConflictRetry(
        opCtx,
        "IndexBuildsManager::commitIndexBuild",
        nss.ns(),
        [builder, opCtx, collection, nss, &onCreateEachFn, &onCommitFn] {
            WriteUnitOfWork wunit(opCtx);
            auto status = builder->commit(opCtx, collection, onCreateEachFn, onCommitFn);
            if (!status.isOK()) {
                return status;
            }

            // Eventually, we will obtain the timestamp for completing the index build from the
            // commitIndexBuild oplog entry.
            // The current logic for timestamping index completion is consistent with the
            // IndexBuilder. See SERVER-38986 and SERVER-34896.
            IndexTimestampHelper::setGhostCommitTimestampForCatalogWrite(opCtx, nss);
            wunit.commit();
            return Status::OK();
        });
}

bool IndexBuildsManager::abortIndexBuild(const UUID& buildUUID, const std::string& reason) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return false;
    }

    std::shared_ptr<MultiIndexBlock> builder = builderIt->second;

    lk.unlock();
    builder->abort(reason);
    return true;
}

bool IndexBuildsManager::interruptIndexBuild(OperationContext* opCtx,
                                             const UUID& buildUUID,
                                             const std::string& reason) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    if (builderIt == _builders.end()) {
        return false;
    }

    log() << "Index build interrupted: " << buildUUID << ": " << reason;
    std::shared_ptr<MultiIndexBlock> builder = builderIt->second;

    lk.unlock();
    builder->abortWithoutCleanup(opCtx);

    return true;
}

void IndexBuildsManager::tearDownIndexBuild(OperationContext* opCtx,
                                            Collection* collection,
                                            const UUID& buildUUID) {
    // TODO verify that the index builder is in a finished state before allowing its destruction.
    auto builder = _getBuilder(buildUUID);
    builder->cleanUpAfterBuild(opCtx, collection);
    _unregisterIndexBuild(buildUUID);
}

bool IndexBuildsManager::isBackgroundBuilding(const UUID& buildUUID) {
    auto builder = _getBuilder(buildUUID);
    return builder->isBackgroundBuilding();
}

void IndexBuildsManager::verifyNoIndexBuilds_forTestOnly() {
    invariant(_builders.empty());
}

void IndexBuildsManager::_registerIndexBuild(UUID buildUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    std::shared_ptr<MultiIndexBlock> mib = std::make_shared<MultiIndexBlock>();
    invariant(_builders.insert(std::make_pair(buildUUID, mib)).second);
}

void IndexBuildsManager::_unregisterIndexBuild(const UUID& buildUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);

    auto builderIt = _builders.find(buildUUID);
    invariant(builderIt != _builders.end());
    _builders.erase(builderIt);
}

std::shared_ptr<MultiIndexBlock> IndexBuildsManager::_getBuilder(const UUID& buildUUID) {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    auto builderIt = _builders.find(buildUUID);
    invariant(builderIt != _builders.end());
    return builderIt->second;
}

}  // namespace mongo
