/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/db/local_catalog/collection_catalog_helper.h"

#include "mongo/base/error_codes.h"
#include "mongo/db/audit.h"
#include "mongo/db/global_settings.h"
#include "mongo/db/local_catalog/catalog_raii.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_catalog.h"
#include "mongo/db/local_catalog/durable_catalog.h"
#include "mongo/db/local_catalog/lock_manager/d_concurrency.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/repl/repl_set_member_in_standalone_mode.h"
#include "mongo/db/sharding_environment/sharding_feature_flags_gen.h"
#include "mongo/db/storage/control/storage_control.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/mdb_catalog.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/recovery_unit_noop.h"
#include "mongo/db/views/view.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/str.h"

#include <functional>
#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage


namespace mongo {

MONGO_FAIL_POINT_DEFINE(hangBeforeGettingNextCollection);

namespace catalog {

Status checkIfNamespaceExists(OperationContext* opCtx, const NamespaceString& nss) {
    auto catalog = CollectionCatalog::get(opCtx);
    if (catalog->lookupCollectionByNamespace(opCtx, nss)) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream()
                          << "Collection " << nss.toStringForErrorMsg() << " already exists.");
    }

    auto view = catalog->lookupView(opCtx, nss);
    if (!view)
        return Status::OK();

    if (view->timeseries()) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "A timeseries collection already exists. NS: "
                                    << nss.toStringForErrorMsg());
    }

    return Status(ErrorCodes::NamespaceExists,
                  str::stream() << "A view already exists. NS: " << nss.toStringForErrorMsg());
}


void forEachCollectionFromDb(OperationContext* opCtx,
                             const DatabaseName& dbName,
                             LockMode collLockMode,
                             CollectionCatalog::CollectionInfoFn callback,
                             CollectionCatalog::CollectionInfoFn predicate) {

    auto catalogForIteration = CollectionCatalog::get(opCtx);
    size_t collectionCount = 0;
    for (auto&& coll : catalogForIteration->range(dbName)) {
        auto uuid = coll->uuid();
        if (predicate && !catalogForIteration->checkIfCollectionSatisfiable(uuid, predicate)) {
            continue;
        }

        boost::optional<Lock::CollectionLock> clk;
        CollectionPtr collection;

        auto catalog = CollectionCatalog::get(opCtx);
        while (auto nss = catalog->lookupNSSByUUID(opCtx, uuid)) {
            // Get a fresh snapshot for each locked collection to see any catalog changes.
            clk.emplace(opCtx, *nss, collLockMode);
            shard_role_details::getRecoveryUnit(opCtx)->abandonSnapshot();
            catalog = CollectionCatalog::get(opCtx);

            if (catalog->lookupNSSByUUID(opCtx, uuid) == nss) {
                // Success: locked the namespace and the UUID still maps to it.
                collection = CollectionPtr(catalog->establishConsistentCollection(
                    opCtx, NamespaceStringOrUUID(dbName, uuid), boost::none));
                invariant(collection);
                break;
            }
            // Failed: collection got renamed before locking it, so unlock and try again.
            clk.reset();
        }

        // The NamespaceString couldn't be resolved from the uuid, so the collection was dropped.
        if (!collection)
            continue;

        if (!callback(collection.get()))
            break;

        // This was a rough heuristic that was found that 400 collections would take 100
        // milliseconds with calling checkForInterrupt() (with freeStorage: 1).
        // We made the checkForInterrupt() occur after 200 collections to be conservative.
        if (!(collectionCount % 200)) {
            opCtx->checkForInterrupt();
        }
        hangBeforeGettingNextCollection.pauseWhileSet();
        collectionCount += 1;
    }
}

boost::optional<bool> getConfigDebugDump(const VersionContext& vCtx, const NamespaceString& nss) {
    static const std::array kConfigDumpCollections = {
        "chunks"_sd,
        "collections"_sd,
        "databases"_sd,
        "settings"_sd,
        "shards"_sd,
        "tags"_sd,
        "version"_sd,
    };

    if (!nss.isConfigDB()) {
        return boost::none;
    }

    if (MONGO_unlikely(!mongo::feature_flags::gConfigDebugDumpSupported.isEnabled(
            vCtx, serverGlobalParams.featureCompatibility.acquireFCVSnapshot()))) {
        return boost::none;
    }

    return std::find(kConfigDumpCollections.begin(), kConfigDumpCollections.end(), nss.coll()) !=
        kConfigDumpCollections.end();
}

namespace {
BSONObj toBSON(const std::variant<Timestamp, StorageEngine::CheckpointIteration>& x) {
    return visit(OverloadedVisitor{[](const Timestamp& ts) { return ts.toBSON(); },
                                   [](const StorageEngine::CheckpointIteration& iter) {
                                       auto underlyingValue = uint64_t{iter};
                                       return BSON("checkpointIteration"
                                                   << std::to_string(underlyingValue));
                                   }},
                 x);
}

/**
 * Returns the first `dropCollection` error that this method encounters. This method will attempt
 * to drop all collections, regardless of the error status. This method will attempt to drop all
 * collections matching the prefix 'collectionNamePrefix'. To drop all collections regardless of
 * prefix, use an empty string.
 */
Status dropCollections(OperationContext* opCtx,
                       const std::vector<UUID>& toDrop,
                       const std::string& collectionNamePrefix) {
    Status firstError = Status::OK();
    WriteUnitOfWork wuow(opCtx);
    for (auto& uuid : toDrop) {
        CollectionWriter writer{opCtx, uuid};
        auto coll = writer.getWritableCollection(opCtx);
        if (coll->ns().coll().starts_with(collectionNamePrefix)) {
            // Drop all indexes in the collection.
            coll->getIndexCatalog()->dropAllIndexes(
                opCtx, coll, /*includingIdIndex=*/true, /*onDropFn=*/{});

            audit::logDropCollection(opCtx->getClient(), coll->ns());

            if (auto sharedIdent = coll->getSharedIdent()) {
                Status result =
                    catalog::dropCollection(opCtx, coll->ns(), coll->getCatalogId(), sharedIdent);
                if (!result.isOK() && firstError.isOK()) {
                    firstError = result;
                }
            }

            CollectionCatalog::get(opCtx)->dropCollection(opCtx, coll);
        }
    }

    wuow.commit();
    return firstError;
}
}  // namespace

void removeIndex(OperationContext* opCtx,
                 StringData indexName,
                 Collection* collection,
                 std::shared_ptr<IndexCatalogEntry> entry,
                 DataRemoval dataRemoval) {
    std::shared_ptr<Ident> ident = [&]() -> std::shared_ptr<Ident> {
        if (!entry) {
            return nullptr;
        }
        return entry->getSharedIdent();
    }();

    // If 'ident' is a nullptr, then there is no in-memory state. In that case, create an otherwise
    // unreferenced Ident for the ident reaper to use: the reaper will not need to wait for existing
    // users to finish.
    if (!ident) {
        ident = std::make_shared<Ident>(
            MDBCatalog::get(opCtx)->getIndexIdent(opCtx, collection->getCatalogId(), indexName));
    }

    // Run the first phase of drop to remove the catalog entry.
    collection->removeIndex(opCtx, indexName);

    // The OperationContext may not be valid when the RecoveryUnit executes the onCommit handlers.
    // Therefore, anything that would normally be fetched from the opCtx must be passed in
    // separately to the onCommit handler below.
    //
    // Index creation (and deletion) are allowed in multi-document transactions that use the same
    // RecoveryUnit throughout but not the same OperationContext.
    auto recoveryUnit = shard_role_details::getRecoveryUnit(opCtx);
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();

    const bool isTwoPhaseDrop = dataRemoval == DataRemoval::kTwoPhase;

    // Schedule the second phase of drop to delete the data when it is no longer in use, if the
    // first phase is successfully committed.
    shard_role_details::getRecoveryUnit(opCtx)->onCommitForTwoPhaseDrop(
        [svcCtx = opCtx->getServiceContext(),
         recoveryUnit,
         storageEngine,
         uuid = collection->uuid(),
         nss = collection->ns(),
         indexNameStr = std::string{indexName},
         ident,
         isTwoPhaseDrop](OperationContext*, boost::optional<Timestamp> commitTimestamp) {
            if (isTwoPhaseDrop) {
                std::variant<Timestamp, StorageEngine::CheckpointIteration> dropTime;
                if (!commitTimestamp) {
                    // Standalone mode and unreplicated drops will not provide a timestamp. Use the
                    // checkpoint iteration instead.
                    dropTime = storageEngine->getEngine()->getCheckpointIteration();
                } else {
                    dropTime = *commitTimestamp;
                }
                LOGV2_PROD_ONLY(22206,
                                "Deferring table drop for index",
                                "index"_attr = indexNameStr,
                                logAttrs(nss),
                                "uuid"_attr = uuid,
                                "ident"_attr = ident->getIdent(),
                                "dropTime"_attr = toBSON(dropTime));
                storageEngine->addDropPendingIdent(dropTime, ident);
            } else {
                LOGV2(6361201,
                      "Completing drop for index table immediately",
                      "ident"_attr = ident->getIdent(),
                      "index"_attr = indexNameStr,
                      logAttrs(nss));
                // Intentionally ignoring failure here. Since we've removed the metadata pointing to
                // the collection, we should never see it again anyway.
                storageEngine->getEngine()
                    ->dropIdent(*recoveryUnit,
                                ident->getIdent(),
                                /*identHasSizeInfo=*/false)
                    .ignore();
            }
        });
}

Status dropCollection(OperationContext* opCtx,
                      const NamespaceString& nss,
                      RecordId collectionCatalogId,
                      std::shared_ptr<Ident> ident) {
    invariant(ident);

    // Run the first phase of drop to remove the catalog entry.
    Status status =
        durable_catalog::dropCollection(opCtx, collectionCatalogId, MDBCatalog::get(opCtx));
    if (!status.isOK()) {
        return status;
    }

    // The OperationContext may not be valid when the RecoveryUnit executes the onCommit handlers.
    // Therefore, anything that would normally be fetched from the opCtx must be passed in
    // separately to the onCommit handler below.
    //
    // Create (and drop) collection are allowed in multi-document transactions that use the same
    // RecoveryUnit throughout but not the same OperationContext.
    auto recoveryUnit = shard_role_details::getRecoveryUnit(opCtx);
    auto storageEngine = opCtx->getServiceContext()->getStorageEngine();


    // Schedule the second phase of drop to delete the data when it is no longer in use, if the
    // first phase is successfully committed.
    shard_role_details::getRecoveryUnit(opCtx)->onCommitForTwoPhaseDrop(
        [svcCtx = opCtx->getServiceContext(), recoveryUnit, storageEngine, nss, ident](
            OperationContext*, boost::optional<Timestamp> commitTimestamp) {
            std::variant<Timestamp, StorageEngine::CheckpointIteration> dropTime;
            if (!commitTimestamp) {
                // Standalone mode and unreplicated drops will not provide a timestamp. Use the
                // checkpoint iteration instead.
                dropTime = storageEngine->getEngine()->getCheckpointIteration();
            } else {
                dropTime = *commitTimestamp;
            }
            LOGV2_PROD_ONLY(22214,
                            "Deferring table drop for collection",
                            logAttrs(nss),
                            "ident"_attr = ident->getIdent(),
                            "dropTime"_attr = toBSON(dropTime));
            storageEngine->addDropPendingIdent(dropTime, ident);
        });

    return Status::OK();
}

Status dropDatabase(OperationContext* opCtx, const DatabaseName& dbName) {
    return dropCollectionsWithPrefix(opCtx, dbName, "" /*collectionNamePrefix*/);
}

Status dropCollectionsWithPrefix(OperationContext* opCtx,
                                 const DatabaseName& dbName,
                                 const std::string& collectionNamePrefix) {
    auto catalog = CollectionCatalog::get(opCtx);
    {
        auto dbNames = catalog->getAllDbNames();
        if (std::count(dbNames.begin(), dbNames.end(), dbName) == 0) {
            return Status(ErrorCodes::NamespaceNotFound, "db not found to drop");
        }
    }

    std::vector<UUID> toDrop = catalog->getAllCollectionUUIDsFromDb(dbName);
    return dropCollections(opCtx, toDrop, collectionNamePrefix);
}

void shutDownCollectionCatalogAndGlobalStorageEngineCleanly(ServiceContext* service,
                                                            bool memLeakAllowed) {
    if (auto truncateMarkers = LocalOplogInfo::get(service)->getTruncateMarkers()) {
        truncateMarkers->kill();
    }

    // SERVER-103812 Shut down JournalFlusher before closing CollectionCatalog
    StorageControl::stopStorageControls(
        service,
        {ErrorCodes::ShutdownInProgress, "The storage catalog is being closed."},
        /*forRestart=*/false);
    CollectionCatalog::write(service, [service](CollectionCatalog& catalog) {
        catalog.onCloseCatalog();
        catalog.deregisterAllCollectionsAndViews(service);
    });
    shutdownGlobalStorageEngineCleanly(service, memLeakAllowed);
}

StorageEngine::LastShutdownState startUpStorageEngine(OperationContext* opCtx,
                                                      StorageEngineInitFlags initFlags,
                                                      BSONObjBuilder* startupTimeElapsedBuilder) {
    // As the storage engine is not yet initialized, a noop recovery unit is used until the
    // initialization is complete.
    shard_role_details::setRecoveryUnit(opCtx,
                                        std::make_unique<RecoveryUnitNoop>(),
                                        WriteUnitOfWork::RecoveryUnitState::kNotInUnitOfWork);

    return initializeStorageEngine(opCtx,
                                   initFlags,
                                   getGlobalReplSettings().isReplSet(),
                                   repl::ReplSettings::shouldRecoverFromOplogAsStandalone(),
                                   getReplSetMemberInStandaloneMode(getGlobalServiceContext()),
                                   startupTimeElapsedBuilder);
}

StorageEngine::LastShutdownState startUpStorageEngineAndCollectionCatalog(
    ServiceContext* service,
    Client* client,
    StorageEngineInitFlags initFlags,
    BSONObjBuilder* startupTimeElapsedBuilder) {
    // Creating the operation context before initializing the storage engine allows the storage
    // engine initialization to make use of the lock manager.
    auto initializeStorageEngineOpCtx = service->makeOperationContext(client);

    auto lastShutdownState = startUpStorageEngine(
        initializeStorageEngineOpCtx.get(), initFlags, startupTimeElapsedBuilder);

    Lock::GlobalWrite globalLk(initializeStorageEngineOpCtx.get());
    catalog::initializeCollectionCatalog(initializeStorageEngineOpCtx.get(),
                                         service->getStorageEngine());

    return lastShutdownState;
}
}  // namespace catalog
}  // namespace mongo
