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

#include "mongo/db/catalog/drop_collection.h"

#include "mongo/db/audit.h"
#include "mongo/db/catalog/clustered_collection_util.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangDropCollectionBeforeLockAcquisition);
MONGO_FAIL_POINT_DEFINE(hangDuringDropCollection);

/**
 * Checks that the collection has the 'expectedUUID' if given.
 * Checks that writes are allowed to 'coll' -- e.g. whether this server is PRIMARY.
 */
Status _checkUUIDAndReplState(OperationContext* opCtx,
                              const CollectionPtr& coll,
                              const NamespaceString& nss,
                              const boost::optional<UUID>& expectedUUID = boost::none) {
    try {
        checkCollectionUUIDMismatch(opCtx, nss, coll, expectedUUID);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    if (opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while dropping collection " << nss);
    }

    return Status::OK();
}

void checkForCollection(std::shared_ptr<const CollectionCatalog> collectionCatalog,
                        OperationContext* opCtx,
                        const NamespaceString& baseNss,
                        boost::optional<StringData> collName,
                        std::vector<std::string>* pLeaked) {

    if (collName.has_value()) {
        auto nss = NamespaceString(baseNss.db(), collName.value());

        if (collectionCatalog->lookupCollectionByNamespace(opCtx, nss)) {
            pLeaked->push_back(nss.toString());
        }
    }
}

void warnEncryptedCollectionsIfNeeded(OperationContext* opCtx, const CollectionPtr& coll) {
    if (!coll->getCollectionOptions().encryptedFieldConfig.has_value()) {
        return;
    }

    auto catalog = CollectionCatalog::get(opCtx);
    auto efc = coll->getCollectionOptions().encryptedFieldConfig.value();

    std::vector<std::string> leaked;

    checkForCollection(catalog, opCtx, coll->ns(), efc.getEscCollection(), &leaked);
    checkForCollection(catalog, opCtx, coll->ns(), efc.getEccCollection(), &leaked);
    checkForCollection(catalog, opCtx, coll->ns(), efc.getEcocCollection(), &leaked);

    if (!leaked.empty()) {
        LOGV2_WARNING(
            6491401,
            "An encrypted collection was dropped before one or more of its state collections",
            "name"_attr = coll->ns(),
            "stateCollections"_attr = leaked);
    }
}

Status _dropView(OperationContext* opCtx,
                 Database* db,
                 const NamespaceString& collectionName,
                 const boost::optional<UUID>& expectedUUID,
                 DropReply* reply) {
    invariant(db);

    // Views don't have UUIDs so if the expectedUUID is specified, we will always throw.
    try {
        checkCollectionUUIDMismatch(opCtx, collectionName, CollectionPtr(), expectedUUID);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    auto view =
        CollectionCatalog::get(opCtx)->lookupViewWithoutValidatingDurable(opCtx, collectionName);
    if (!view) {
        audit::logDropView(
            opCtx->getClient(), collectionName, "", {}, ErrorCodes::NamespaceNotFound);
        return Status::OK();
    }

    // Validates the view or throws an "invalid view" error.
    CollectionCatalog::get(opCtx)->lookupView(opCtx, collectionName);

    // Operations all lock system.views in the end to prevent deadlock.
    Lock::CollectionLock systemViewsLock(opCtx, db->getSystemViewsName(), MODE_X);

    if (MONGO_unlikely(hangDuringDropCollection.shouldFail())) {
        LOGV2(20330,
              "hangDuringDropCollection fail point enabled. Blocking until fail point is "
              "disabled.");
        hangDuringDropCollection.pauseWhileSet();
    }

    AutoStatsTracker statsTracker(
        opCtx,
        collectionName,
        Top::LockType::NotLocked,
        AutoStatsTracker::LogMode::kUpdateCurOp,
        CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(collectionName.dbName()));

    if (opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, collectionName)) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while dropping collection " << collectionName);
    }

    WriteUnitOfWork wunit(opCtx);

    audit::logDropView(
        opCtx->getClient(), collectionName, view->viewOn().ns(), view->pipeline(), ErrorCodes::OK);

    Status status = db->dropView(opCtx, collectionName);
    if (!status.isOK()) {
        return status;
    }
    wunit.commit();

    reply->setNs(collectionName);
    return Status::OK();
}

Status _abortIndexBuildsAndDrop(OperationContext* opCtx,
                                AutoGetDb&& autoDb,
                                const NamespaceString& startingNss,
                                const boost::optional<UUID>& expectedUUID,
                                std::function<Status(Database*, const NamespaceString&)>&& dropFn,
                                DropReply* reply,
                                bool appendNs = true,
                                boost::optional<UUID> dropIfUUIDNotMatching = boost::none) {
    // We only need to hold an intent lock to send abort signals to the active index builder on this
    // collection.
    boost::optional<AutoGetDb> optionalAutoDb(std::move(autoDb));
    boost::optional<CollectionNamespaceOrUUIDLock> collLock;
    collLock.emplace(opCtx, startingNss, MODE_IX);

    // Abandon the snapshot as the index catalog will compare the in-memory state to the disk state,
    // which may have changed when we released the collection lock temporarily.
    opCtx->recoveryUnit()->abandonSnapshot();

    CollectionPtr coll(
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, startingNss));

    // Even if the collection doesn't exist, UUID mismatches must return an error.
    Status status = _checkUUIDAndReplState(opCtx, coll, startingNss, expectedUUID);
    if (!status.isOK()) {
        return status;
    } else if (!coll) {
        return Status::OK();
    }

    warnEncryptedCollectionsIfNeeded(opCtx, coll);

    if (MONGO_unlikely(hangDuringDropCollection.shouldFail())) {
        LOGV2(518090,
              "hangDuringDropCollection fail point enabled. Blocking until fail point is "
              "disabled.");
        hangDuringDropCollection.pauseWhileSet();
    }

    AutoStatsTracker statsTracker(
        opCtx,
        startingNss,
        Top::LockType::NotLocked,
        AutoStatsTracker::LogMode::kUpdateCurOp,
        CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(startingNss.dbName()));

    IndexBuildsCoordinator* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);
    const UUID collectionUUID = coll->uuid();
    if (dropIfUUIDNotMatching && collectionUUID == *dropIfUUIDNotMatching) {
        return Status::OK();
    }
    const NamespaceStringOrUUID dbAndUUID{coll->ns().dbName(), coll->uuid()};
    const int numIndexes = coll->getIndexCatalog()->numIndexesTotal();

    while (true) {
        // Save a copy of the namespace before yielding our locks.
        const NamespaceString collectionNs = coll->ns();

        // Release locks before aborting index builds. The helper will acquire locks on our behalf.
        collLock = boost::none;
        optionalAutoDb = boost::none;

        // Send the abort signal to any active index builds on the collection. This waits until all
        // aborted index builds complete.
        indexBuildsCoord->abortCollectionIndexBuilds(opCtx,
                                                     collectionNs,
                                                     collectionUUID,
                                                     str::stream()
                                                         << "Collection " << collectionNs << "("
                                                         << collectionUUID << ") is being dropped");

        // Take an exclusive lock to finish the collection drop.
        optionalAutoDb.emplace(opCtx, startingNss.dbName(), MODE_IX);
        collLock.emplace(opCtx, dbAndUUID, MODE_X);

        // Abandon the snapshot as the index catalog will compare the in-memory state to the
        // disk state, which may have changed when we released the collection lock temporarily.
        opCtx->recoveryUnit()->abandonSnapshot();

        coll = CollectionPtr(
            CollectionCatalog::get(opCtx)->lookupCollectionByUUID(opCtx, collectionUUID));

        // Even if the collection doesn't exist, UUID mismatches must return an error.
        status = _checkUUIDAndReplState(opCtx, coll, startingNss, expectedUUID);
        if (!status.isOK()) {
            return status;
        } else if (!coll) {
            return Status::OK();
        }

        // Check if any new index builds were started while releasing the collection lock
        // temporarily, if so, we need to abort the new index builders.
        const bool abortAgain = indexBuildsCoord->inProgForCollection(collectionUUID);
        if (!abortAgain) {
            break;
        }
    }

    // It's possible for the given collection to be drop pending after obtaining the locks again, if
    // that is the case, then the collection is already registered to be dropped. Return early.
    const NamespaceString resolvedNss = coll->ns();
    if (resolvedNss.isDropPendingNamespace()) {
        return Status::OK();
    }

    // Serialize the drop with refreshes to prevent dropping a collection and creating the same
    // nss as a view while refreshing.
    CollectionShardingState::assertCollectionLockedAndAcquire(opCtx, resolvedNss)
        ->checkShardVersionOrThrow(opCtx);

    invariant(coll->getIndexCatalog()->numIndexesInProgress() == 0);

    status = dropFn(optionalAutoDb->getDb(), resolvedNss);
    if (!status.isOK()) {
        return status;
    }

    reply->setNIndexesWas(numIndexes);
    if (appendNs) {
        reply->setNs(resolvedNss);
    }

    return Status::OK();
}

Status _dropCollectionForApplyOps(OperationContext* opCtx,
                                  Database* db,
                                  const NamespaceString& collectionName,
                                  const repl::OpTime& dropOpTime,
                                  DropCollectionSystemCollectionMode systemCollectionMode,
                                  DropReply* reply) {
    Lock::CollectionLock collLock(opCtx, collectionName, MODE_X);
    CollectionPtr coll(
        CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, collectionName));

    // Even if the collection doesn't exist, UUID mismatches must return an error.
    Status status = _checkUUIDAndReplState(opCtx, coll, collectionName);
    if (!status.isOK()) {
        return status;
    } else if (!coll) {
        return Status::OK();
    }

    if (MONGO_unlikely(hangDuringDropCollection.shouldFail())) {
        LOGV2(20331,
              "hangDuringDropCollection fail point enabled. Blocking until fail point is "
              "disabled.");
        hangDuringDropCollection.pauseWhileSet();
    }

    AutoStatsTracker statsTracker(
        opCtx,
        collectionName,
        Top::LockType::NotLocked,
        AutoStatsTracker::LogMode::kUpdateCurOp,
        CollectionCatalog::get(opCtx)->getDatabaseProfileLevel(collectionName.dbName()));

    WriteUnitOfWork wunit(opCtx);

    int numIndexes = coll->getIndexCatalog()->numIndexesTotal();
    IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(coll->uuid());
    status =
        systemCollectionMode == DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops
        ? db->dropCollection(opCtx, collectionName, dropOpTime)
        : db->dropCollectionEvenIfSystem(opCtx, collectionName, dropOpTime);

    if (!status.isOK()) {
        return status;
    }
    wunit.commit();

    reply->setNIndexesWas(numIndexes);
    reply->setNs(collectionName);

    return Status::OK();
}

Status _dropCollection(OperationContext* opCtx,
                       const NamespaceString& collectionName,
                       const boost::optional<UUID>& expectedUUID,
                       DropReply* reply,
                       DropCollectionSystemCollectionMode systemCollectionMode,
                       bool fromMigrate,
                       boost::optional<UUID> dropIfUUIDNotMatching = boost::none) {

    try {
        return writeConflictRetry(opCtx, "drop", collectionName.ns(), [&] {
            // If a change collection is to be dropped, that is, the change streams are being
            // disabled for a tenant, acquire exclusive tenant lock.
            AutoGetDb autoDb(opCtx,
                             collectionName.dbName(),
                             MODE_IX /* database lock mode*/,
                             boost::make_optional(collectionName.tenantId() &&
                                                      collectionName.isChangeCollection(),
                                                  MODE_X));
            auto db = autoDb.getDb();
            if (!db) {
                return expectedUUID
                    ? Status{CollectionUUIDMismatchInfo(collectionName.dbName(),
                                                        *expectedUUID,
                                                        collectionName.coll().toString(),
                                                        boost::none),
                             "Database does not exist"}
                    : Status::OK();
            }

            if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, collectionName)) {
                return _abortIndexBuildsAndDrop(
                    opCtx,
                    std::move(autoDb),
                    collectionName,
                    expectedUUID,
                    [opCtx, systemCollectionMode, fromMigrate](Database* db,
                                                               const NamespaceString& resolvedNs) {
                        WriteUnitOfWork wuow(opCtx);

                        auto status = systemCollectionMode ==
                                DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops
                            ? db->dropCollection(opCtx, resolvedNs, {}, fromMigrate)
                            : db->dropCollectionEvenIfSystem(opCtx, resolvedNs, {}, fromMigrate);
                        if (!status.isOK()) {
                            return status;
                        }

                        wuow.commit();
                        return Status::OK();
                    },
                    reply,
                    true /* appendNs */,
                    dropIfUUIDNotMatching);
            }

            auto dropTimeseries = [opCtx,
                                   &expectedUUID,
                                   &autoDb,
                                   &collectionName,
                                   &reply,
                                   fromMigrate](const NamespaceString& bucketNs, bool dropView) {
                return _abortIndexBuildsAndDrop(
                    opCtx,
                    std::move(autoDb),
                    bucketNs,
                    expectedUUID,
                    [opCtx, dropView, &expectedUUID, &collectionName, &reply, fromMigrate](
                        Database* db, const NamespaceString& bucketsNs) {
                        // Disallow checking the expectedUUID when dropping time-series collections.
                        uassert(ErrorCodes::InvalidOptions,
                                "The collectionUUID parameter cannot be passed when dropping a "
                                "time-series collection",
                                !expectedUUID);

                        if (dropView) {
                            // Take a MODE_X lock when dropping timeseries view. This is to prevent
                            // a concurrent create collection on the same namespace that will
                            // reserve an OpTime before this drop. We already hold a MODE_X lock on
                            // the bucket collection inside '_abortIndexBuildsAndDrop' above. When
                            // taking both these locks it needs to happen in this order to prevent a
                            // deadlock.
                            Lock::CollectionLock viewLock(opCtx, collectionName, MODE_X);
                            auto status = _dropView(opCtx, db, collectionName, boost::none, reply);
                            if (!status.isOK()) {
                                return status;
                            }
                        }

                        // Drop the buckets collection in its own writeConflictRetry so that if
                        // it throws a WCE, only the buckets collection drop is retried.
                        writeConflictRetry(
                            opCtx, "drop", bucketsNs.ns(), [opCtx, db, &bucketsNs, fromMigrate] {
                                WriteUnitOfWork wuow(opCtx);
                                db->dropCollectionEvenIfSystem(opCtx, bucketsNs, {}, fromMigrate)
                                    .ignore();
                                wuow.commit();
                            });

                        return Status::OK();
                    },
                    reply,
                    false /* appendNs */);
            };

            auto view = CollectionCatalog::get(opCtx)->lookupViewWithoutValidatingDurable(
                opCtx, collectionName);
            if (!view) {
                // Timeseries bucket collection may exist even without the view. If that is the case
                // delete it.
                auto bucketsNs = collectionName.makeTimeseriesBucketsNamespace();
                if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, bucketsNs)) {
                    return dropTimeseries(bucketsNs, false);
                }

                // There is no collection or view at the namespace. Check whether a UUID was given
                // and error if so because the caller expects the collection to exist. If no UUID
                // was given, then it is OK to return success.
                try {
                    checkCollectionUUIDMismatch(
                        opCtx, collectionName, CollectionPtr(), expectedUUID);
                } catch (const DBException& ex) {
                    return ex.toStatus();
                }

                audit::logDropView(
                    opCtx->getClient(), collectionName, "", {}, ErrorCodes::NamespaceNotFound);
                return Status::OK();
            }
            if (view->timeseries() &&
                CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, view->viewOn())) {
                return dropTimeseries(view->viewOn(), true);
            }

            // Take a MODE_X lock when dropping a view. This is to prevent a concurrent create
            // collection on the same namespace that will reserve an OpTime before this drop.
            Lock::CollectionLock viewLock(opCtx, collectionName, MODE_X);
            return _dropView(opCtx, db, collectionName, expectedUUID, reply);
        });
    } catch (ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // Any unhandled namespace not found errors should be converted into success. Unless the
        // caller specified a UUID and expects the collection to exist.
        try {
            checkCollectionUUIDMismatch(opCtx, collectionName, CollectionPtr(), expectedUUID);
        } catch (const DBException& ex) {
            return ex.toStatus();
        }

        return Status::OK();
    }
}
}  // namespace

Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      const boost::optional<UUID>& expectedUUID,
                      DropReply* reply,
                      DropCollectionSystemCollectionMode systemCollectionMode,
                      bool fromMigrate) {
    if (!serverGlobalParams.quiet.load()) {
        LOGV2(518070, "CMD: drop", logAttrs(nss));
    }

    if (MONGO_unlikely(hangDropCollectionBeforeLockAcquisition.shouldFail())) {
        LOGV2(518080, "Hanging drop collection before lock acquisition while fail point is set");
        hangDropCollectionBeforeLockAcquisition.pauseWhileSet();
    }

    // We rewrite drop of time-series buckets collection to drop of time-series view collection.
    // This ensures that such drop will delete both collections.
    const auto collectionName =
        nss.isTimeseriesBucketsCollection() ? nss.getTimeseriesViewNamespace() : nss;

    return _dropCollection(
        opCtx, collectionName, expectedUUID, reply, systemCollectionMode, fromMigrate);
}

Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      DropReply* reply,
                      DropCollectionSystemCollectionMode systemCollectionMode,
                      bool fromMigrate) {
    return dropCollection(opCtx, nss, boost::none, reply, systemCollectionMode, fromMigrate);
}

Status dropCollectionIfUUIDNotMatching(OperationContext* opCtx,
                                       const NamespaceString& ns,
                                       const UUID& expectedUUID) {
    AutoGetDb autoDb(opCtx, ns.dbName(), MODE_IX);
    if (autoDb.getDb()) {
        {
            Lock::CollectionLock collLock(opCtx, ns, MODE_IS);
            const auto coll = CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, ns);
            if (!coll || coll->uuid() == expectedUUID) {
                return Status::OK();
            }
        }

        DropReply repl;
        return _dropCollection(opCtx,
                               ns,
                               boost::none,
                               &repl,
                               DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops,
                               false /*fromMigrate*/,
                               expectedUUID);
    }

    return Status::OK();
}

Status dropCollectionForApplyOps(OperationContext* opCtx,
                                 const NamespaceString& collectionName,
                                 const repl::OpTime& dropOpTime,
                                 DropCollectionSystemCollectionMode systemCollectionMode) {
    if (!serverGlobalParams.quiet.load()) {
        LOGV2(20332, "CMD: drop", logAttrs(collectionName));
    }

    if (MONGO_unlikely(hangDropCollectionBeforeLockAcquisition.shouldFail())) {
        LOGV2(20333, "Hanging drop collection before lock acquisition while fail point is set");
        hangDropCollectionBeforeLockAcquisition.pauseWhileSet();
    }
    return writeConflictRetry(opCtx, "drop", collectionName.ns(), [&] {
        AutoGetDb autoDb(opCtx, collectionName.dbName(), MODE_IX);
        Database* db = autoDb.getDb();
        if (!db) {
            return Status::OK();
        }

        CollectionPtr coll(
            CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, collectionName));

        DropReply unusedReply;
        if (!coll) {
            Lock::CollectionLock viewLock(opCtx, collectionName, MODE_IX);
            return _dropView(opCtx, db, collectionName, boost::none, &unusedReply);
        } else {
            return _dropCollectionForApplyOps(
                opCtx, db, collectionName, dropOpTime, systemCollectionMode, &unusedReply);
        }
    });
}

void checkForIdIndexesAndDropPendingCollections(OperationContext* opCtx,
                                                const DatabaseName& dbName) {
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_IX));

    if (dbName == DatabaseName::kLocal) {
        // Collections in the local database are not replicated, so we do not need an _id index on
        // any collection. For the same reason, it is not possible for the local database to contain
        // any drop-pending collections (drops are effective immediately).
        return;
    }

    auto catalog = CollectionCatalog::get(opCtx);

    for (const auto& nss : catalog->getAllCollectionNamesFromDb(opCtx, dbName)) {
        if (nss.isDropPendingNamespace()) {
            auto dropOpTime = fassert(40459, nss.getDropPendingNamespaceOpTime());
            LOGV2(20321,
                  "Found drop-pending namespace {namespace} with drop optime {dropOpTime}",
                  "Found drop-pending namespace",
                  logAttrs(nss),
                  "dropOpTime"_attr = dropOpTime);
            repl::DropPendingCollectionReaper::get(opCtx)->addDropPendingNamespace(
                opCtx, dropOpTime, nss);
        }

        if (nss.isSystem())
            continue;

        CollectionPtr coll(catalog->lookupCollectionByNamespace(opCtx, nss));
        if (!coll)
            continue;

        if (coll->getIndexCatalog()->findIdIndex(opCtx))
            continue;

        if (clustered_util::isClusteredOnId(coll->getClusteredInfo())) {
            continue;
        }

        LOGV2_OPTIONS(
            20322,
            {logv2::LogTag::kStartupWarnings},
            "Collection lacks a unique index on _id. This index is "
            "needed for replication to function properly. To fix this, you need to create a unique "
            "index on _id. See http://dochub.mongodb.org/core/build-replica-set-indexes",
            logAttrs(nss));
    }
}

void clearTempCollections(OperationContext* opCtx, const DatabaseName& dbName) {
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_IX));

    auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, dbName);
    invariant(db);

    CollectionCatalog::CollectionInfoFn callback = [&](const Collection* collection) {
        try {
            WriteUnitOfWork wuow(opCtx);
            Status status = db->dropCollection(opCtx, collection->ns());
            if (!status.isOK()) {
                LOGV2_WARNING(20327,
                              "could not drop temp collection '{namespace}': {error}",
                              "could not drop temp collection",
                              logAttrs(collection->ns()),
                              "error"_attr = redact(status));
            }
            wuow.commit();
        } catch (const WriteConflictException&) {
            LOGV2_WARNING(
                20328,
                "could not drop temp collection '{namespace}' due to WriteConflictException",
                "could not drop temp collection due to WriteConflictException",
                logAttrs(collection->ns()));
            opCtx->recoveryUnit()->abandonSnapshot();
        }
        return true;
    };

    catalog::forEachCollectionFromDb(
        opCtx, dbName, MODE_X, callback, [&](const Collection* collection) {
            return collection->getCollectionOptions().temp;
        });
}

}  // namespace mongo
