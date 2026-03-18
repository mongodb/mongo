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

#include "mongo/db/shard_role/shard_catalog/drop_collection.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/crypto/encryption_fields_gen.h"
#include "mongo/db/audit.h"
#include "mongo/db/index_builds/index_builds_coordinator.h"
#include "mongo/db/namespace_string_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/profile_settings.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/shard_role/lock_manager/d_concurrency.h"
#include "mongo/db/shard_role/lock_manager/exception_util.h"
#include "mongo/db/shard_role/lock_manager/lock_manager_defs.h"
#include "mongo/db/shard_role/shard_catalog/catalog_helper_ddl.h"
#include "mongo/db/shard_role/shard_catalog/catalog_raii.h"
#include "mongo/db/shard_role/shard_catalog/clustered_collection_util.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog.h"
#include "mongo/db/shard_role/shard_catalog/collection_catalog_helper.h"
#include "mongo/db/shard_role/shard_catalog/collection_options.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_state.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch.h"
#include "mongo/db/shard_role/shard_catalog/collection_uuid_mismatch_info.h"
#include "mongo/db/shard_role/shard_catalog/database.h"
#include "mongo/db/shard_role/shard_catalog/database_holder.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/db/shard_role/transaction_resources.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/write_unit_of_work.h"
#include "mongo/db/timeseries/catalog_helper.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <boost/optional/optional.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kCommand

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(hangDropCollectionBeforeLockAcquisition);
MONGO_FAIL_POINT_DEFINE(hangDuringDropCollection);
MONGO_FAIL_POINT_DEFINE(allowSystemViewsDrop);

/**
 * Checks that writes are allowed to 'coll' -- e.g. whether this server is PRIMARY.
 */
Status _checkReplState(OperationContext* opCtx,
                       const CollectionPtr& coll,
                       const NamespaceString& nss) {
    if (opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, nss)) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream()
                          << "Not primary while dropping collection " << nss.toStringForErrorMsg());
    }

    return Status::OK();
}

void checkForCollection(std::shared_ptr<const CollectionCatalog> collectionCatalog,
                        OperationContext* opCtx,
                        const NamespaceString& baseNss,
                        boost::optional<StringData> collName,
                        std::vector<std::string>* pLeaked) {

    if (collName.has_value()) {
        const auto nss = NamespaceStringUtil::deserialize(baseNss.dbName(), collName.value());

        if (collectionCatalog->lookupCollectionByNamespace(opCtx, nss)) {
            pLeaked->push_back(toStringForLogging(nss));
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
                 DropReply* reply) {
    invariant(db);

    auto view = CollectionCatalog::get(opCtx)->lookupView(opCtx, collectionName);

    if (MONGO_unlikely(hangDuringDropCollection.shouldFail())) {
        LOGV2(20330,
              "hangDuringDropCollection fail point enabled. Blocking until fail point is "
              "disabled.");
        hangDuringDropCollection.pauseWhileSet();
    }

    AutoStatsTracker statsTracker(opCtx,
                                  collectionName,
                                  Top::LockType::NotLocked,
                                  AutoStatsTracker::LogMode::kUpdateCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(collectionName.dbName()));

    if (opCtx->writesAreReplicated() &&
        !repl::ReplicationCoordinator::get(opCtx)->canAcceptWritesFor(opCtx, collectionName)) {
        return Status(ErrorCodes::NotWritablePrimary,
                      str::stream() << "Not primary while dropping collection "
                                    << collectionName.toStringForErrorMsg());
    }

    WriteUnitOfWork wunit(opCtx);

    audit::logDropView(
        opCtx->getClient(), collectionName, view->viewOn(), view->pipeline(), ErrorCodes::OK);

    Status status = db->dropView(opCtx, collectionName);
    if (!status.isOK()) {
        return status;
    }
    wunit.commit();

    reply->setNs(collectionName);
    return Status::OK();
}

StatusWith<timeseries::CollectionOrViewAcquisitionPlusTimeseriesView> _abortIndexBuilds(
    OperationContext* opCtx,
    timeseries::CollectionOrViewAcquisitionPlusTimeseriesView&& startingLocks,
    const NamespaceString& startingNss,
    const boost::optional<UUID>& expectedUUID) {
    // If this is a view or non-existing collection, there are no index builds to abort.
    if (!startingLocks.target.collectionExists()) {
        return StatusWith(std::move(startingLocks));
    }

    tassert(11609003,
            "Expected the collection given to _abortIndexBuilds to be locked",
            shard_role_details::getLocker(opCtx)->isCollectionLockedForMode(
                startingLocks.target.nss(), MODE_X));

    boost::optional<timeseries::CollectionOrViewAcquisitionPlusTimeseriesView> locks(
        std::move(startingLocks));

    if (MONGO_unlikely(hangDuringDropCollection.shouldFail())) {
        LOGV2(518090,
              "hangDuringDropCollection fail point enabled. Blocking until fail point is "
              "disabled.");
        hangDuringDropCollection.pauseWhileSet();
    }

    IndexBuildsCoordinator* indexBuildsCoord = IndexBuildsCoordinator::get(opCtx);

    while (true) {
        Status status = _checkReplState(opCtx, locks->target.getCollectionPtr(), startingNss);
        if (!status.isOK()) {
            return status;
        } else if (!locks->target.collectionExists()) {
            return StatusWith(std::move(*locks));
        }

        // Check if any index builds are in progress. If so, we need to abort the index builders.
        if (!indexBuildsCoord->inProgForCollection(locks->target.getCollection().uuid())) {
            break;
        }

        // Save a copy of the namespace before yielding our locks.
        const NamespaceString collectionNs = locks->target.nss();
        const UUID collectionUUID = locks->target.getCollection().uuid();

        // Release locks before aborting index builds. The helper will acquire locks on our behalf.
        locks = boost::none;

        // Send the abort signal to any active index builds on the collection. This waits until all
        // aborted index builds complete.
        indexBuildsCoord->abortCollectionIndexBuilds(
            opCtx,
            collectionNs,
            collectionUUID,
            str::stream() << "Collection " << toStringForLogging(collectionNs) << "("
                          << collectionUUID << ") is being dropped");

        // Abandon the snapshot as the index catalog will compare the in-memory state to the
        // disk state, which may have changed when we released the collection lock temporarily.
        shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();

        // Take an exclusive lock to finish the collection drop.
        locks.emplace(timeseries::acquireCollectionOrViewPlusTimeseriesView(
            opCtx,
            CollectionOrViewAcquisitionRequest::fromOpCtx(
                opCtx, startingNss, expectedUUID, AcquisitionPrerequisites::kWrite)));
    }

    invariant(locks->target.getCollectionPtr()->getIndexCatalog()->numIndexesInProgress() == 0);

    return StatusWith(std::move(*locks));
}
Status _dropCollectionForApplyOps(OperationContext* opCtx,
                                  Database* db,
                                  const NamespaceString& collectionName,
                                  const repl::OpTime& dropOpTime,
                                  DropCollectionSystemCollectionMode systemCollectionMode,
                                  DropReply* reply) {
    Lock::CollectionLock collLock(opCtx, collectionName, MODE_X);
    boost::optional<UUID> uuid;
    AutoStatsTracker statsTracker(opCtx,
                                  collectionName,
                                  Top::LockType::NotLocked,
                                  AutoStatsTracker::LogMode::kUpdateCurOp,
                                  DatabaseProfileSettings::get(opCtx->getServiceContext())
                                      .getDatabaseProfileLevel(collectionName.dbName()));
    int numIndexes;
    {
        CollectionPtr coll =
            CollectionPtr(CollectionCatalog::get(opCtx)->establishConsistentCollection(
                opCtx, collectionName, boost::none /*readTimestamp*/));

        Status status = _checkReplState(opCtx, coll, collectionName);
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
        uuid = coll->uuid();
        numIndexes = coll->getIndexCatalog()->numIndexesTotal();
    }

    WriteUnitOfWork wunit(opCtx);

    IndexBuildsCoordinator::get(opCtx)->assertNoIndexBuildInProgForCollection(*uuid);
    Status status =
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
                       const NamespaceString& nss,
                       const boost::optional<UUID>& expectedUUID,
                       DropReply* reply,
                       DropCollectionSystemCollectionMode systemCollectionMode,
                       bool fromMigrate,
                       boost::optional<UUID> dropIfUUIDNotMatching = boost::none) {

    try {
        return writeConflictRetry(opCtx, "drop", nss, [&] {
            auto locks = timeseries::acquireCollectionOrViewPlusTimeseriesView(
                opCtx,
                CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx, nss, expectedUUID, AcquisitionPrerequisites::kWrite));

            auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, nss.dbName());
            if (!db) {
                return Status::OK();
            }

            AutoStatsTracker statsTracker(opCtx,
                                          nss,
                                          Top::LockType::NotLocked,
                                          AutoStatsTracker::LogMode::kUpdateCurOp,
                                          DatabaseProfileSettings::get(opCtx->getServiceContext())
                                              .getDatabaseProfileLevel(nss.dbName()));

            // Abort in-progress index builds. This may need to release and re-acquire the locks,
            // so we should not take decisions on the collection metadata before this.
            auto s = _abortIndexBuilds(opCtx, std::move(locks), nss, expectedUUID);
            if (!s.isOK()) {
                return s.getStatus();
            }
            db = DatabaseHolder::get(opCtx)->getDb(opCtx, nss.dbName());
            locks = std::move(s.getValue());

            if (locks.target.isView()) {
                // We need a MODE_X lock to drop a view. This is to prevent a concurrent create
                // collection on the same namespace that will reserve an OpTime before this drop.
                return _dropView(opCtx, db, nss, reply);
            }

            if (!locks.target.collectionExists()) {
                audit::logDropView(opCtx->getClient(),
                                   nss,
                                   NamespaceString::kEmpty,
                                   {},
                                   ErrorCodes::NamespaceNotFound);
                return Status::OK();
            }

            if (dropIfUUIDNotMatching &&
                locks.target.getCollection().uuid() == *dropIfUUIDNotMatching) {
                return Status::OK();
            }

            warnEncryptedCollectionsIfNeeded(opCtx, locks.target.getCollectionPtr());

            const int numIndexes =
                locks.target.getCollectionPtr()->getIndexCatalog()->numIndexesTotal();

            const auto& targetNss = locks.target.nss();

            if (targetNss.isTimeseriesBucketsCollection()) {
                // We need a MODE_X lock to drop the timeseries view, which
                // `acquireCollectionOrViewPlusTimeseriesView` acquired. This is to prevent
                // a concurrent create collection on the same namespace that will
                // reserve an OpTime before this drop.
                if (locks.timeseriesView.has_value()) {
                    auto status = _dropView(opCtx, db, locks.timeseriesView->nss(), reply);
                    if (!status.isOK()) {
                        return status;
                    }
                }

                // Drop the buckets collection in its own writeConflictRetry so that if
                // it throws a WCE, only the buckets collection drop is retried.
                writeConflictRetry(opCtx, "drop", targetNss, [opCtx, db, &targetNss, fromMigrate] {
                    WriteUnitOfWork wuow(opCtx);
                    db->dropCollectionEvenIfSystem(opCtx, targetNss, {}, fromMigrate).ignore();
                    wuow.commit();
                });

                reply->setNIndexesWas(numIndexes);
                reply->setNs(nss);
                return Status::OK();
            }

            WriteUnitOfWork wuow(opCtx);

            auto status = systemCollectionMode ==
                    DropCollectionSystemCollectionMode::kDisallowSystemCollectionDrops
                ? db->dropCollection(opCtx, targetNss, {}, fromMigrate)
                : db->dropCollectionEvenIfSystem(opCtx, targetNss, {}, fromMigrate);
            if (!status.isOK()) {
                return status;
            }

            wuow.commit();

            reply->setNIndexesWas(numIndexes);
            reply->setNs(nss);
            return Status::OK();
        });
    } catch (const ExceptionFor<ErrorCodes::InvalidViewDefinition>&) {
        // Allow dropping a nonexistent view even if the view catalog is invalid.
        if (!CollectionCatalog::get(opCtx)->lookupViewWithoutValidatingDurable(opCtx, nss)) {
            audit::logDropView(opCtx->getClient(),
                               nss,
                               NamespaceString::kEmpty,
                               {},
                               ErrorCodes::NamespaceNotFound);
            return Status::OK();
        }

        throw;
    } catch (const ExceptionFor<ErrorCodes::CollectionUUIDMismatch>& e) {
        // Preserve behavior for legacy timeseries which returns InvalidOptions when passing
        // collectionUUID.
        // TODO SERVER-101784 Remove the code below once 9.0 becomes LTS and legacy time-series are
        // removed.
        if (!e->actualCollection()) {
            throw;
        }
        const auto expectedNs =
            NamespaceStringUtil::deserialize(nss.dbName(), e->expectedCollection());
        const auto actualNs =
            NamespaceStringUtil::deserialize(nss.dbName(), *e->actualCollection());
        uassert(ErrorCodes::InvalidOptions,
                "The collectionUUID parameter cannot be passed when dropping a "
                "time-series collection",
                actualNs != expectedNs.makeTimeseriesBucketsNamespace());
        throw;
    } catch (const ExceptionFor<ErrorCodes::NamespaceNotFound>&) {
        // If the command specified a UUID, the acquisition throws CollectionUUIDMismatch, so on
        // NamespaceNotFound we always return success.
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

    return _dropCollection(opCtx, nss, expectedUUID, reply, systemCollectionMode, fromMigrate);
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
                                       const UUID& expectedUUID,
                                       bool fromMigrate) {
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
                               fromMigrate,
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
    return writeConflictRetry(opCtx, "drop", collectionName, [&] {
        AutoGetDb autoDb(opCtx, collectionName.dbName(), MODE_IX);
        Database* db = autoDb.getDb();
        if (!db) {
            return Status::OK();
        }

        bool isView = [&]() {
            auto coll =
                acquireCollectionOrView(opCtx,
                                        CollectionOrViewAcquisitionRequest::fromOpCtx(
                                            opCtx, collectionName, AcquisitionPrerequisites::kRead),
                                        MODE_IS);
            return coll.isView();
        }();

        DropReply unusedReply;
        if (isView) {
            auto ddlAcq = catalog_helper_ddl::acquireCollectionOrViewForCatalogWrites(
                opCtx,
                {CollectionOrViewAcquisitionRequest::fromOpCtx(
                    opCtx, collectionName, AcquisitionPrerequisites::kWrite)});
            return _dropView(opCtx, db, collectionName, &unusedReply);
        } else {
            return _dropCollectionForApplyOps(
                opCtx, db, collectionName, dropOpTime, systemCollectionMode, &unusedReply);
        }
    });
}

void checkForIdIndexes(OperationContext* opCtx, const DatabaseName& dbName) {
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_X));

    if (dbName == DatabaseName::kLocal) {
        // Collections in the local database are not replicated, so we do not need an _id index on
        // any collection.
        return;
    }

    auto catalog = CollectionCatalog::get(opCtx);

    for (const auto& nss : catalog->getAllCollectionNamesFromDb(opCtx, dbName)) {
        if (nss.isSystem())
            continue;

        CollectionPtr coll = CollectionPtr(
            catalog->establishConsistentCollection(opCtx, nss, boost::none /* readTimestamp */));
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
    invariant(shard_role_details::getLocker(opCtx)->isDbLockedForMode(dbName, MODE_IX));

    auto db = DatabaseHolder::get(opCtx)->getDb(opCtx, dbName);
    invariant(db);

    CollectionCatalog::CollectionInfoFn callback = [&](const Collection* collection) {
        try {
            WriteUnitOfWork wuow(opCtx);
            Status status = db->dropCollection(opCtx, collection->ns());
            if (!status.isOK()) {
                LOGV2_WARNING(20327,
                              "could not drop temp collection",
                              logAttrs(collection->ns()),
                              "error"_attr = redact(status));
            }
            wuow.commit();
        } catch (const StorageUnavailableException&) {
            LOGV2_WARNING(20328,
                          "could not drop temp collection due to WriteConflictException",
                          logAttrs(collection->ns()));
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
        }
        return true;
    };

    catalog::forEachCollectionFromDb(
        opCtx, dbName, MODE_X, callback, [&](const Collection* collection) {
            return collection->getCollectionOptions().temp;
        });
}

Status isDroppableCollection(OperationContext* opCtx, const NamespaceString& nss) {
    if (!nss.isSystem()) {
        return Status::OK();
    }

    auto isDroppableSystemCollection = [](const auto& nss) {
        return nss.isHealthlog() || nss == NamespaceString::kLogicalSessionsNamespace ||
            nss == NamespaceString::kKeysCollectionNamespace ||
            nss.isTemporaryReshardingCollection() || nss.isTimeseriesBucketsCollection() ||
            nss.isChangeStreamPreImagesCollection() ||
            nss == NamespaceString::kConfigsvrRestoreNamespace || nss.isSystemDotJavascript() ||
            nss.isSystemStatsCollection() || nss == NamespaceString::kBlockFCVChangesNamespace;
    };

    if (nss.isSystemDotProfile()) {
        if (DatabaseProfileSettings::get(opCtx->getServiceContext())
                .getDatabaseProfileLevel(nss.dbName()) != 0)
            return Status(ErrorCodes::IllegalOperation,
                          "turn off profiling before dropping system.profile collection");
    } else if (nss.isSystemDotViews()) {
        if (!MONGO_unlikely(allowSystemViewsDrop.shouldFail())) {
            const auto viewStats =
                CollectionCatalog::get(opCtx)->getViewStatsForDatabase(opCtx, nss.dbName());
            if (!viewStats || viewStats->userTimeseries != 0) {
                return Status(
                    ErrorCodes::CommandFailed,
                    fmt::format(
                        "cannot drop collection {} when time-series collections are present",
                        nss.toStringForErrorMsg()));
            }
        }
    } else if (!isDroppableSystemCollection(nss)) {
        return Status(ErrorCodes::IllegalOperation,
                      fmt::format("cannot drop system collection {}", nss.toStringForErrorMsg()));
    }

    return Status::OK();
}

}  // namespace mongo
