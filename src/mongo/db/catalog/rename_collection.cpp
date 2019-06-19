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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/rename_collection.h"

#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/drop_collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(writeConflictInRenameCollCopyToTmp);

boost::optional<NamespaceString> getNamespaceFromUUID(OperationContext* opCtx, const UUID& uuid) {
    return CollectionCatalog::get(opCtx).lookupNSSByUUID(uuid);
}

bool isCollectionSharded(OperationContext* opCtx, const NamespaceString& nss) {
    auto* const css = CollectionShardingState::get(opCtx, nss);
    const auto metadata = css->getCurrentMetadata();
    return metadata->isSharded();
}

// From a replicated to an unreplicated collection or vice versa.
bool isReplicatedChanged(OperationContext* opCtx,
                         const NamespaceString& source,
                         const NamespaceString& target) {
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto sourceIsUnreplicated = replCoord->isOplogDisabledFor(opCtx, source);
    auto targetIsUnreplicated = replCoord->isOplogDisabledFor(opCtx, target);
    return (sourceIsUnreplicated != targetIsUnreplicated);
}

Status checkSourceAndTargetNamespaces(OperationContext* opCtx,
                                      const NamespaceString& source,
                                      const NamespaceString& target,
                                      RenameCollectionOptions options) {

    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (opCtx->writesAreReplicated() && !replCoord->canAcceptWritesFor(opCtx, source))
        return Status(ErrorCodes::NotMaster,
                      str::stream() << "Not primary while renaming collection " << source << " to "
                                    << target);

    if (isCollectionSharded(opCtx, source))
        return {ErrorCodes::IllegalOperation, "source namespace cannot be sharded"};

    if (isReplicatedChanged(opCtx, source, target))
        return {ErrorCodes::IllegalOperation,
                "Cannot rename collections between a replicated and an unreplicated database"};

    auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, source.db());
    if (!db)
        return Status(ErrorCodes::NamespaceNotFound, "source namespace does not exist");

    {
        auto& dss = DatabaseShardingState::get(db);
        auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, &dss);
        dss.checkDbVersion(opCtx, dssLock);
    }

    Collection* const sourceColl = db->getCollection(opCtx, source);
    if (!sourceColl) {
        if (ViewCatalog::get(db)->lookup(opCtx, source.ns()))
            return Status(ErrorCodes::CommandNotSupportedOnView,
                          str::stream() << "cannot rename view: " << source);
        return Status(ErrorCodes::NamespaceNotFound, "source namespace does not exist");
    }

    BackgroundOperation::assertNoBgOpInProgForNs(source.ns());
    IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(
        sourceColl->uuid().get());

    Collection* targetColl = db->getCollection(opCtx, target);

    if (!targetColl) {
        if (ViewCatalog::get(db)->lookup(opCtx, target.ns()))
            return Status(ErrorCodes::NamespaceExists,
                          str::stream() << "a view already exists with that name: " << target);
    } else {
        if (isCollectionSharded(opCtx, target))
            return {ErrorCodes::IllegalOperation, "cannot rename to a sharded collection"};

        if (!options.dropTarget)
            return Status(ErrorCodes::NamespaceExists, "target namespace exists");
    }

    return Status::OK();
}

Status renameTargetCollectionToTmp(OperationContext* opCtx,
                                   const NamespaceString& sourceNs,
                                   const UUID& sourceUUID,
                                   Database* const targetDB,
                                   const NamespaceString& targetNs,
                                   const UUID& targetUUID) {
    repl::UnreplicatedWritesBlock uwb(opCtx);

    auto tmpNameResult = targetDB->makeUniqueCollectionNamespace(opCtx, "tmp%%%%%.rename");
    if (!tmpNameResult.isOK()) {
        return tmpNameResult.getStatus().withContext(
            str::stream() << "Cannot generate a temporary collection name for the target "
                          << targetNs
                          << " ("
                          << targetUUID
                          << ") so that the source"
                          << sourceNs
                          << " ("
                          << sourceUUID
                          << ") could be renamed to "
                          << targetNs);
    }
    const auto& tmpName = tmpNameResult.getValue();
    const bool stayTemp = true;
    return writeConflictRetry(opCtx, "renameCollection", targetNs.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);
        auto status = targetDB->renameCollection(opCtx, targetNs, tmpName, stayTemp);
        if (!status.isOK())
            return status;

        wunit.commit();

        log() << "Successfully renamed the target " << targetNs << " (" << targetUUID << ") to "
              << tmpName << " so that the source " << sourceNs << " (" << sourceUUID
              << ") could be renamed to " << targetNs;

        return Status::OK();
    });
}

Status renameCollectionDirectly(OperationContext* opCtx,
                                Database* db,
                                OptionalCollectionUUID uuid,
                                NamespaceString source,
                                NamespaceString target,
                                RenameCollectionOptions options) {
    return writeConflictRetry(opCtx, "renameCollection", target.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);

        {
            // No logOp necessary because the entire renameCollection command is one logOp.
            repl::UnreplicatedWritesBlock uwb(opCtx);
            auto status = db->renameCollection(opCtx, source, target, options.stayTemp);
            if (!status.isOK())
                return status;
        }

        // Rename is not resilient to interruption when the onRenameCollection OpObserver
        // takes an oplog collection lock.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());

        // We have to override the provided 'dropTarget' setting for idempotency reasons to
        // avoid unintentionally removing a collection on a secondary with the same name as
        // the target.
        auto opObserver = opCtx->getServiceContext()->getOpObserver();
        opObserver->onRenameCollection(opCtx, source, target, uuid, {}, 0U, options.stayTemp);

        wunit.commit();
        return Status::OK();
    });
}

Status renameCollectionAndDropTarget(OperationContext* opCtx,
                                     Database* db,
                                     OptionalCollectionUUID uuid,
                                     NamespaceString source,
                                     NamespaceString target,
                                     Collection* targetColl,
                                     RenameCollectionOptions options,
                                     repl::OpTime renameOpTimeFromApplyOps) {
    return writeConflictRetry(opCtx, "renameCollection", target.ns(), [&] {
        WriteUnitOfWork wunit(opCtx);

        // Target collection exists - drop it.
        invariant(options.dropTarget);

        // If this rename collection is replicated, check for long index names in the target
        // collection that may exceed the MMAPv1 namespace limit when the target collection
        // is renamed with a drop-pending namespace.
        auto replCoord = repl::ReplicationCoordinator::get(opCtx);
        auto isOplogDisabledForNamespace = replCoord->isOplogDisabledFor(opCtx, target);
        if (!isOplogDisabledForNamespace) {
            invariant(opCtx->writesAreReplicated());
            invariant(renameOpTimeFromApplyOps.isNull());
        }

        auto numRecords = targetColl->numRecords(opCtx);
        auto opObserver = opCtx->getServiceContext()->getOpObserver();
        auto renameOpTime = opObserver->preRenameCollection(
            opCtx, source, target, uuid, targetColl->uuid(), numRecords, options.stayTemp);

        if (!renameOpTimeFromApplyOps.isNull()) {
            // 'renameOpTime' must be null because a valid 'renameOpTimeFromApplyOps' implies
            // replicated writes are not enabled.
            if (!renameOpTime.isNull()) {
                severe() << "renameCollection: " << source << " to " << target
                         << " (with dropTarget=true) - unexpected renameCollection oplog entry"
                         << " written to the oplog with optime " << renameOpTime;
                fassertFailed(40616);
            }
            renameOpTime = renameOpTimeFromApplyOps;
        }

        // No logOp necessary because the entire renameCollection command is one logOp.
        repl::UnreplicatedWritesBlock uwb(opCtx);

        BackgroundOperation::assertNoBgOpInProgForNs(targetColl->ns().ns());
        IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(
            targetColl->uuid().get());

        auto status = db->dropCollection(opCtx, targetColl->ns(), renameOpTime);
        if (!status.isOK())
            return status;

        status = db->renameCollection(opCtx, source, target, options.stayTemp);
        if (!status.isOK())
            return status;

        opObserver->postRenameCollection(
            opCtx, source, target, uuid, targetColl->uuid(), options.stayTemp);
        wunit.commit();
        return Status::OK();
    });
}

Status renameCollectionWithinDB(OperationContext* opCtx,
                                const NamespaceString& source,
                                const NamespaceString& target,
                                RenameCollectionOptions options) {
    invariant(source.db() == target.db());
    DisableDocumentValidation validationDisabler(opCtx);

    Lock::DBLock dbWriteLock(opCtx, source.db(), MODE_IX);
    boost::optional<Lock::CollectionLock> sourceLock;
    boost::optional<Lock::CollectionLock> targetLock;
    // To prevent deadlock, always lock system.views collection in the end because concurrent
    // view-related operations always lock system.views in the end.
    if (!source.isSystemDotViews() && (target.isSystemDotViews() ||
                                       ResourceId(RESOURCE_COLLECTION, source.ns()) <
                                           ResourceId(RESOURCE_COLLECTION, target.ns()))) {
        // To prevent deadlock, always lock source and target in ascending resourceId order.
        sourceLock.emplace(opCtx, source, MODE_X);
        targetLock.emplace(opCtx, target, MODE_X);
    } else {
        targetLock.emplace(opCtx, target, MODE_X);
        sourceLock.emplace(opCtx, source, MODE_X);
    }

    auto status = checkSourceAndTargetNamespaces(opCtx, source, target, options);
    if (!status.isOK())
        return status;

    auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, source.db());
    Collection* const sourceColl = db->getCollection(opCtx, source);
    Collection* const targetColl = db->getCollection(opCtx, target);

    AutoStatsTracker statsTracker(opCtx,
                                  source,
                                  Top::LockType::NotLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                                  db->getProfilingLevel());

    if (!targetColl) {
        return renameCollectionDirectly(opCtx, db, sourceColl->uuid(), source, target, options);
    } else {
        return renameCollectionAndDropTarget(
            opCtx, db, sourceColl->uuid(), source, target, targetColl, options, {});
    }
}

Status renameCollectionWithinDBForApplyOps(OperationContext* opCtx,
                                           const NamespaceString& source,
                                           const NamespaceString& target,
                                           OptionalCollectionUUID uuidToDrop,
                                           repl::OpTime renameOpTimeFromApplyOps,
                                           const RenameCollectionOptions& options) {
    invariant(source.db() == target.db());
    DisableDocumentValidation validationDisabler(opCtx);

    Lock::DBLock dbWriteLock(opCtx, source.db(), MODE_X);

    auto status = checkSourceAndTargetNamespaces(opCtx, source, target, options);
    if (!status.isOK())
        return status;

    auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, source.db());
    Collection* const sourceColl = db->getCollection(opCtx, source);
    Collection* targetColl = db->getCollection(opCtx, target);

    AutoStatsTracker statsTracker(opCtx,
                                  source,
                                  Top::LockType::NotLocked,
                                  AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                                  db->getProfilingLevel());

    if (targetColl) {
        if (sourceColl->uuid() == targetColl->uuid()) {
            if (!uuidToDrop || uuidToDrop == targetColl->uuid()) {
                return Status::OK();
            }

            // During initial sync, it is possible that the collection already
            // got renamed to the target, so there is not much left to do other
            // than drop the dropTarget. See SERVER-40861 for more details.
            return writeConflictRetry(opCtx, "renameCollection", target.ns(), [&] {
                WriteUnitOfWork wunit(opCtx);
                auto collToDropBasedOnUUID = getNamespaceFromUUID(opCtx, *uuidToDrop);
                if (!collToDropBasedOnUUID)
                    return Status::OK();
                repl::UnreplicatedWritesBlock uwb(opCtx);
                Status status =
                    db->dropCollection(opCtx, *collToDropBasedOnUUID, renameOpTimeFromApplyOps);
                if (!status.isOK())
                    return status;
                wunit.commit();
                return Status::OK();
            });
        }
        if (uuidToDrop && uuidToDrop != targetColl->uuid()) {
            // We need to rename the targetColl to a temporary name.
            auto status = renameTargetCollectionToTmp(
                opCtx, source, sourceColl->uuid().get(), db, target, targetColl->uuid().get());
            if (!status.isOK())
                return status;
            targetColl = nullptr;
        }
    }

    // When reapplying oplog entries (such as in the case of initial sync) we need
    // to identify the collection to drop by UUID, as otherwise we might end up
    // dropping the wrong collection.
    if (!targetColl && uuidToDrop) {
        invariant(options.dropTarget);
        auto collToDropBasedOnUUID = getNamespaceFromUUID(opCtx, uuidToDrop.get());
        if (collToDropBasedOnUUID && !collToDropBasedOnUUID->isDropPendingNamespace()) {
            invariant(collToDropBasedOnUUID->db() == target.db());
            targetColl = db->getCollection(opCtx, *collToDropBasedOnUUID);
        }
    }

    if (!targetColl) {
        return renameCollectionDirectly(opCtx, db, sourceColl->uuid(), source, target, options);
    } else {
        if (sourceColl == targetColl)
            return Status::OK();

        return renameCollectionAndDropTarget(opCtx,
                                             db,
                                             sourceColl->uuid(),
                                             source,
                                             target,
                                             targetColl,
                                             options,
                                             renameOpTimeFromApplyOps);
    }
}

Status renameBetweenDBs(OperationContext* opCtx,
                        const NamespaceString& source,
                        const NamespaceString& target,
                        const RenameCollectionOptions& options) {
    invariant(source.db() != target.db());

    boost::optional<Lock::GlobalWrite> globalWriteLock;
    if (!opCtx->lockState()->isW())
        globalWriteLock.emplace(opCtx);

    DisableDocumentValidation validationDisabler(opCtx);

    auto sourceDB = DatabaseHolder::get(opCtx)->getDb(opCtx, source.db());
    if (!sourceDB)
        return Status(ErrorCodes::NamespaceNotFound, "source namespace does not exist");

    {
        auto& dss = DatabaseShardingState::get(sourceDB);
        auto dssLock = DatabaseShardingState::DSSLock::lockShared(opCtx, &dss);
        dss.checkDbVersion(opCtx, dssLock);
    }

    boost::optional<AutoStatsTracker> statsTracker(boost::in_place_init,
                                                   opCtx,
                                                   source,
                                                   Top::LockType::NotLocked,
                                                   AutoStatsTracker::LogMode::kUpdateTopAndCurop,
                                                   sourceDB->getProfilingLevel());

    Collection* const sourceColl = sourceDB->getCollection(opCtx, source);
    if (!sourceColl) {
        if (sourceDB && ViewCatalog::get(sourceDB)->lookup(opCtx, source.ns()))
            return Status(ErrorCodes::CommandNotSupportedOnView,
                          str::stream() << "cannot rename view: " << source);
        return Status(ErrorCodes::NamespaceNotFound, "source namespace does not exist");
    }

    if (isCollectionSharded(opCtx, source))
        return {ErrorCodes::IllegalOperation, "source namespace cannot be sharded"};

    if (isReplicatedChanged(opCtx, source, target))
        return {ErrorCodes::IllegalOperation,
                "Cannot rename collections between a replicated and an unreplicated database"};

    BackgroundOperation::assertNoBgOpInProgForNs(source.ns());
    IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(
        sourceColl->uuid().get());

    auto targetDB = DatabaseHolder::get(opCtx)->getDb(opCtx, target.db());

    // Check if the target namespace exists and if dropTarget is true.
    // Return a non-OK status if target exists and dropTarget is not true or if the collection
    // is sharded.
    Collection* targetColl = targetDB ? targetDB->getCollection(opCtx, target) : nullptr;
    if (targetColl) {
        if (sourceColl->uuid() == targetColl->uuid()) {
            invariant(source == target);
            return Status::OK();
        }

        if (isCollectionSharded(opCtx, target))
            return {ErrorCodes::IllegalOperation, "cannot rename to a sharded collection"};

        if (!options.dropTarget) {
            return Status(ErrorCodes::NamespaceExists, "target namespace exists");
        }

    } else if (targetDB && ViewCatalog::get(targetDB)->lookup(opCtx, target.ns())) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a view already exists with that name: " << target);
    }

    // Create a temporary collection in the target database. It will be removed if we fail to
    // copy the collection, or on restart, so there is no need to replicate these writes.
    if (!targetDB) {
        targetDB = DatabaseHolder::get(opCtx)->openDb(opCtx, target.db());
    }

    auto tmpNameResult =
        targetDB->makeUniqueCollectionNamespace(opCtx, "tmp%%%%%.renameCollection");
    if (!tmpNameResult.isOK()) {
        return tmpNameResult.getStatus().withContext(
            str::stream() << "Cannot generate temporary collection name to rename " << source
                          << " to "
                          << target);
    }
    const auto& tmpName = tmpNameResult.getValue();

    log() << "Attempting to create temporary collection: " << tmpName
          << " with the contents of collection: " << source;

    Collection* tmpColl = nullptr;
    {
        auto collectionOptions =
            DurableCatalog::get(opCtx)->getCollectionOptions(opCtx, sourceColl->ns());

        // Renaming across databases will result in a new UUID.
        collectionOptions.uuid = UUID::gen();

        writeConflictRetry(opCtx, "renameCollection", tmpName.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);
            tmpColl = targetDB->createCollection(opCtx, tmpName, collectionOptions);
            wunit.commit();
        });
    }

    // Dismissed on success
    auto tmpCollectionDropper = makeGuard([&] {
        BSONObjBuilder unusedResult;
        Status status = Status::OK();
        try {
            status =
                dropCollection(opCtx,
                               tmpName,
                               unusedResult,
                               {},
                               DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
        } catch (...) {
            status = exceptionToStatus();
        }
        if (!status.isOK()) {
            // Ignoring failure case when dropping the temporary collection during cleanup because
            // the rename operation has already failed for another reason.
            log() << "Unable to drop temporary collection " << tmpName << " while renaming from "
                  << source << " to " << target << ": " << status;
        }
    });

    // Copy the index descriptions from the source collection, adjusting the ns field.
    {
        std::vector<BSONObj> indexesToCopy;
        std::unique_ptr<IndexCatalog::IndexIterator> sourceIndIt =
            sourceColl->getIndexCatalog()->getIndexIterator(opCtx, true);
        while (sourceIndIt->more()) {
            auto descriptor = sourceIndIt->next()->descriptor();
            if (descriptor->isIdIndex()) {
                continue;
            }

            const BSONObj currIndex = descriptor->infoObj();

            // Process the source index, adding fields in the same order as they were originally.
            BSONObjBuilder newIndex;
            for (auto&& elem : currIndex) {
                if (elem.fieldNameStringData() == "ns") {
                    newIndex.append("ns", tmpName.ns());
                } else {
                    newIndex.append(elem);
                }
            }
            indexesToCopy.push_back(newIndex.obj());
        }

        // Create indexes using the namespace-adjusted index specs on the empty temporary collection
        // that was just created. Since each index build is possibly replicated to downstream nodes,
        // each createIndex oplog entry must have a distinct timestamp to support correct rollback
        // operation. This is achieved by writing the createIndexes oplog entry *before* creating
        // the index. Using IndexCatalog::createIndexOnEmptyCollection() for the index creation
        // allows us to add and commit the index within a single WriteUnitOfWork and avoids the
        // possibility of seeing the index in an unfinished state. For more information on assigning
        // timestamps to multiple index builds, please see SERVER-35780 and SERVER-35070.
        Status status = writeConflictRetry(opCtx, "renameCollection", tmpName.ns(), [&] {
            WriteUnitOfWork wunit(opCtx);
            auto tmpIndexCatalog = tmpColl->getIndexCatalog();
            auto opObserver = opCtx->getServiceContext()->getOpObserver();
            for (const auto& indexToCopy : indexesToCopy) {
                opObserver->onCreateIndex(opCtx,
                                          tmpName,
                                          *(tmpColl->uuid()),
                                          indexToCopy,
                                          false  // fromMigrate
                                          );
                auto indexResult =
                    tmpIndexCatalog->createIndexOnEmptyCollection(opCtx, indexToCopy);
                if (!indexResult.isOK()) {
                    return indexResult.getStatus();
                }
            };
            wunit.commit();
            return Status::OK();
        });
        if (!status.isOK()) {
            return status;
        }
    }

    {
        // Copy over all the data from source collection to temporary collection.
        // We do not need global write exclusive access after obtaining the collection locks on the
        // source and temporary collections. After copying the documents, each remaining stage of
        // the cross-database rename will be responsible for its own lock management.
        // Therefore, unless the caller has already acquired the global write lock prior to invoking
        // this function, we relinquish global write access to the database after acquiring the
        // collection locks.
        // Collection locks must be obtained while holding the global lock to avoid any possibility
        // of a deadlock.
        AutoGetCollectionForRead autoSourceColl(opCtx, source);
        AutoGetCollection autoTmpColl(opCtx, tmpName, MODE_IX);
        statsTracker.reset();

        if (opCtx->getServiceContext()->getStorageEngine()->supportsDBLocking()) {
            if (globalWriteLock) {
                opCtx->lockState()->downgrade(resourceIdGlobal, MODE_IX);
                invariant(!opCtx->lockState()->isW());
            } else {
                invariant(opCtx->lockState()->isW());
            }
        }

        auto cursor = sourceColl->getCursor(opCtx);
        auto record = cursor->next();
        while (record) {
            opCtx->checkForInterrupt();
            // Cursor is left one past the end of the batch inside writeConflictRetry.
            auto beginBatchId = record->id;
            Status status = writeConflictRetry(opCtx, "renameCollection", tmpName.ns(), [&] {
                WriteUnitOfWork wunit(opCtx);
                // Need to reset cursor if it gets a WCE midway through.
                if (!record || (beginBatchId != record->id)) {
                    record = cursor->seekExact(beginBatchId);
                }
                for (int i = 0; record && (i < internalInsertMaxBatchSize.load()); i++) {
                    const InsertStatement stmt(record->data.releaseToBson());
                    OpDebug* const opDebug = nullptr;
                    auto status = tmpColl->insertDocument(opCtx, stmt, opDebug, true);
                    if (!status.isOK()) {
                        return status;
                    }
                    record = cursor->next();
                }
                cursor->save();
                // When this exits via success or WCE, we need to restore the cursor.
                ON_BLOCK_EXIT([ opCtx, ns = tmpName.ns(), &cursor ]() {
                    writeConflictRetry(
                        opCtx, "retryRestoreCursor", ns, [&cursor] { cursor->restore(); });
                });
                // Used to make sure that a WCE can be handled by this logic without data loss.
                if (MONGO_FAIL_POINT(writeConflictInRenameCollCopyToTmp)) {
                    throw WriteConflictException();
                }
                wunit.commit();
                return Status::OK();
            });
            if (!status.isOK())
                return status;
        }
    }
    globalWriteLock.reset();

    // Getting here means we successfully built the target copy. We now do the final
    // in-place rename and remove the source collection.
    invariant(tmpName.db() == target.db());
    Status status = renameCollectionWithinDB(opCtx, tmpName, target, options);
    if (!status.isOK())
        return status;

    tmpCollectionDropper.dismiss();

    BSONObjBuilder unusedResult;
    return dropCollection(opCtx,
                          source,
                          unusedResult,
                          {},
                          DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
}

}  // namespace

Status renameCollection(OperationContext* opCtx,
                        const NamespaceString& source,
                        const NamespaceString& target,
                        const RenameCollectionOptions& options) {
    if (source.isDropPendingNamespace()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "renameCollection() cannot accept a source "
                                       "collection that is in a drop-pending state: "
                                    << source);
    }

    const std::string dropTargetMsg =
        options.dropTarget ? " and drop " + target.toString() + "." : ".";
    log() << "renameCollectionForCommand: rename " << source << " to " << target << dropTargetMsg;

    if (source.db() == target.db())
        return renameCollectionWithinDB(opCtx, source, target, options);
    else {
        return renameBetweenDBs(opCtx, source, target, options);
    }
}

Status renameCollectionForApplyOps(OperationContext* opCtx,
                                   const std::string& dbName,
                                   const BSONElement& ui,
                                   const BSONObj& cmd,
                                   const repl::OpTime& renameOpTime) {

    // A valid 'renameOpTime' is not allowed when writes are replicated.
    if (!renameOpTime.isNull() && opCtx->writesAreReplicated()) {
        return Status(
            ErrorCodes::BadValue,
            "renameCollection() cannot accept a rename optime when writes are replicated.");
    }

    const auto sourceNsElt = cmd.firstElement();
    const auto targetNsElt = cmd["to"];
    uassert(ErrorCodes::TypeMismatch,
            "'renameCollection' must be of type String",
            sourceNsElt.type() == BSONType::String);
    uassert(ErrorCodes::TypeMismatch,
            "'to' must be of type String",
            targetNsElt.type() == BSONType::String);

    NamespaceString sourceNss(sourceNsElt.valueStringData());
    NamespaceString targetNss(targetNsElt.valueStringData());
    OptionalCollectionUUID uuidToRename;
    if (!ui.eoo()) {
        uuidToRename = uassertStatusOK(UUID::parse(ui));
        auto nss = CollectionCatalog::get(opCtx).lookupNSSByUUID(uuidToRename.get());
        if (nss)
            sourceNss = *nss;
    }

    RenameCollectionOptions options;
    options.dropTarget = cmd["dropTarget"].trueValue();
    options.stayTemp = cmd["stayTemp"].trueValue();

    OptionalCollectionUUID uuidToDrop;
    if (cmd["dropTarget"].type() == BinData) {
        auto uuid = uassertStatusOK(UUID::parse(cmd["dropTarget"]));
        uuidToDrop = uuid;
    }

    // Check that the target namespace is in the correct form, "database.collection".
    auto targetStatus = userAllowedWriteNS(targetNss);
    if (!targetStatus.isOK()) {
        return Status(targetStatus.code(),
                      str::stream() << "error with target namespace: " << targetStatus.reason());
    }

    if ((repl::ReplicationCoordinator::get(opCtx)->getReplicationMode() ==
         repl::ReplicationCoordinator::modeNone) &&
        targetNss.isOplog()) {
        return Status(ErrorCodes::IllegalOperation,
                      str::stream() << "Cannot rename collection to the oplog");
    }

    const Collection* const sourceColl =
        AutoGetCollectionForRead(opCtx, sourceNss, AutoGetCollection::ViewMode::kViewsPermitted)
            .getCollection();

    if (sourceNss.isDropPendingNamespace() || sourceColl == nullptr) {
        boost::optional<NamespaceString> dropTargetNss;

        if (options.dropTarget)
            dropTargetNss = targetNss;

        if (uuidToDrop)
            dropTargetNss = getNamespaceFromUUID(opCtx, uuidToDrop.get());

        // Downgrade renameCollection to dropCollection.
        if (dropTargetNss) {
            BSONObjBuilder unusedResult;
            return dropCollection(opCtx,
                                  *dropTargetNss,
                                  unusedResult,
                                  renameOpTime,
                                  DropCollectionSystemCollectionMode::kAllowSystemCollectionDrops);
        }

        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream()
                          << "renameCollection() cannot accept a source "
                             "collection that does not exist or is in a drop-pending state: "
                          << sourceNss.toString());
    }

    const std::string dropTargetMsg =
        uuidToDrop ? " and drop " + uuidToDrop->toString() + "." : ".";
    const std::string uuidString = uuidToRename ? uuidToRename->toString() : "UUID unknown";
    log() << "renameCollectionForApplyOps: rename " << sourceNss << " (" << uuidString << ") to "
          << targetNss << dropTargetMsg;

    if (sourceNss.db() == targetNss.db()) {
        return renameCollectionWithinDBForApplyOps(
            opCtx, sourceNss, targetNss, uuidToDrop, renameOpTime, options);
    } else {
        return renameBetweenDBs(opCtx, sourceNss, targetNss, options);
    }
}

Status renameCollectionForRollback(OperationContext* opCtx,
                                   const NamespaceString& target,
                                   const UUID& uuid) {
    // If the UUID we're targeting already exists, rename from there no matter what.
    auto source = getNamespaceFromUUID(opCtx, uuid);
    invariant(source);
    invariant(source->db() == target.db(),
              str::stream() << "renameCollectionForRollback: source and target namespaces must "
                               "have the same database. source: "
                            << *source
                            << ". target: "
                            << target);

    log() << "renameCollectionForRollback: rename " << *source << " (" << uuid << ") to " << target
          << ".";

    return renameCollectionWithinDB(opCtx, *source, target, {});
}

}  // namespace mongo
