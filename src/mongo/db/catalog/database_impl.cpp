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

#include "mongo/db/catalog/database_impl.h"

#include <algorithm>
#include <boost/filesystem/operations.hpp>
#include <memory>
#include <vector>

#include "mongo/db/audit.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_catalog_helper.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/uncommitted_catalog_updates.h"
#include "mongo/db/catalog/virtual_collection_impl.h"
#include "mongo/db/catalog/virtual_collection_options.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/exception_util.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index_builds_coordinator.h"
#include "mongo/db/introspect.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_consistency_markers_impl.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/tenant_migration_decoration.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/durable_catalog.h"
#include "mongo/db/storage/historical_ident_tracker.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/storage/storage_util.h"
#include "mongo/db/system_index.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/random.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kStorage

namespace mongo {
namespace {

MONGO_FAIL_POINT_DEFINE(throwWCEDuringTxnCollCreate);
MONGO_FAIL_POINT_DEFINE(hangBeforeLoggingCreateCollection);
MONGO_FAIL_POINT_DEFINE(hangAndFailAfterCreateCollectionReservesOpTime);
MONGO_FAIL_POINT_DEFINE(openCreateCollectionWindowFp);
MONGO_FAIL_POINT_DEFINE(allowSystemViewsDrop);

// When active, a column index will be created for all new collections. This is used for the column
// index JS test passthrough suite. Other passthroughs work by overriding javascript methods on the
// client side, but this approach often requires the drop() function to create the collection. This
// behavior is confusing, and requires a large number of tests to be re-written to accommodate this
// passthrough behavior. In case you're wondering, this failpoint approach would not work as well
// for the sharded collections task, since mongos and the config servers are generally unaware of
// when a collection is created. There isn't a great server-side hook we can use to auto-shard a
// collection, and it is more complex technically to drive this process from one shard in the
// cluster. For column store indexes, we just need to change local state on each mongod.
MONGO_FAIL_POINT_DEFINE(createColumnIndexOnAllCollections);

Status validateDBNameForWindows(StringData dbname) {
    const std::vector<std::string> windowsReservedNames = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};

    std::string lower{dbname};
    std::transform(
        lower.begin(), lower.end(), lower.begin(), [](char c) { return ctype::toLower(c); });

    if (std::count(windowsReservedNames.begin(), windowsReservedNames.end(), lower))
        return Status(ErrorCodes::BadValue,
                      str::stream() << "db name \"" << dbname << "\" is a reserved name");
    return Status::OK();
}

void assertNoMovePrimaryInProgress(OperationContext* opCtx, NamespaceString const& nss) {
    const auto scopedDss =
        DatabaseShardingState::assertDbLockedAndAcquireShared(opCtx, nss.dbName());
    if (scopedDss->isMovePrimaryInProgress()) {
        LOGV2(4909100, "assertNoMovePrimaryInProgress", logAttrs(nss));

        uasserted(ErrorCodes::MovePrimaryInProgress,
                  "movePrimary is in progress for namespace " + nss.toStringForErrorMsg());
    }
}

static const BSONObj kColumnStoreSpec = BSON("name"
                                             << "$**_columnstore"
                                             << "key"
                                             << BSON("$**"
                                                     << "columnstore")
                                             << "v" << 2);
}  // namespace

Status DatabaseImpl::validateDBName(StringData dbname) {
    if (dbname.size() <= 0)
        return Status(ErrorCodes::BadValue, "db name is empty");

    if (dbname.size() >= 64)
        return Status(ErrorCodes::BadValue, "db name is too long");

    if (dbname.find('.') != std::string::npos)
        return Status(ErrorCodes::BadValue, "db name cannot contain a .");

    if (dbname.find(' ') != std::string::npos)
        return Status(ErrorCodes::BadValue, "db name cannot contain a space");

#ifdef _WIN32
    return validateDBNameForWindows(dbname);
#endif

    return Status::OK();
}

DatabaseImpl::DatabaseImpl(const DatabaseName& dbName)
    : _name(dbName), _viewsName(NamespaceString::makeSystemDotViewsNamespace(_name)) {}

void DatabaseImpl::init(OperationContext* const opCtx) {
    Status status = validateDBName(_name.db());

    if (!status.isOK()) {
        LOGV2_WARNING(
            20325, "tried to open invalid db: {name}", "Tried to open invalid db", logAttrs(_name));
        uasserted(10028, status.toString());
    }

    auto catalog = CollectionCatalog::get(opCtx);
    for (const auto& uuid : catalog->getAllCollectionUUIDsFromDb(_name)) {
        const Collection* collection = catalog->lookupCollectionByUUID(opCtx, uuid);
        invariant(collection);
        // If this is called from the repair path, the collection is already initialized.
        if (!collection->isInitialized()) {
            WriteUnitOfWork wuow(opCtx);
            catalog->lookupCollectionByUUIDForMetadataWrite(opCtx, uuid)->init(opCtx);
            wuow.commit();
        }
    }

    // When in restore mode, views created on collections that weren't restored will be removed. We
    // only do this during startup when the global lock is held.
    if (storageGlobalParams.restore && opCtx->lockState()->isW()) {
        // Refresh our copy of the catalog, since we may have modified it above.
        catalog = CollectionCatalog::get(opCtx);
        try {
            struct ViewToDrop {
                NamespaceString name;
                NamespaceString viewOn;
                NamespaceString resolvedNs;
            };
            std::vector<ViewToDrop> viewsToDrop;
            catalog->iterateViews(opCtx, _name, [&](const ViewDefinition& view) {
                auto swResolvedView =
                    view_catalog_helpers::resolveView(opCtx, catalog, view.name(), boost::none);
                if (!swResolvedView.isOK()) {
                    LOGV2_WARNING(6260802,
                                  "Could not resolve view during restore",
                                  "view"_attr = view.name(),
                                  "viewOn"_attr = view.viewOn(),
                                  "reason"_attr = swResolvedView.getStatus().reason());
                    return true;
                }

                // The name of the most resolved namespace, which is a collection.
                auto resolvedNs = swResolvedView.getValue().getNamespace();

                if (catalog->lookupCollectionByNamespace(opCtx, resolvedNs)) {
                    // The collection exists for this view.
                    return true;
                }

                // Defer the view to drop so we don't do it while iterating. In case we're updating
                // the catalog inplace, we cannot modify the same data structure as we're iterating
                // on.
                viewsToDrop.push_back({view.name(), view.viewOn(), resolvedNs});

                return true;
            });

            // Drop all collected views from above.
            for (const auto& view : viewsToDrop) {
                LOGV2(6260803,
                      "Removing view on collection not restored",
                      "view"_attr = view.name,
                      "viewOn"_attr = view.viewOn,
                      "resolvedNs"_attr = view.resolvedNs);

                WriteUnitOfWork wuow(opCtx);
                Status status = catalog->dropView(opCtx, view.name);
                if (!status.isOK()) {
                    LOGV2_WARNING(6260804,
                                  "Failed to remove view on unrestored collection",
                                  "view"_attr = view.name,
                                  "viewOn"_attr = view.viewOn,
                                  "resolvedNs"_attr = view.resolvedNs,
                                  "reason"_attr = status.reason());
                    continue;
                }
                wuow.commit();
            }
        } catch (const ExceptionFor<ErrorCodes::InvalidViewDefinition>& e) {
            LOGV2_WARNING(6260805,
                          "Failed to access the view catalog during restore",
                          logAttrs(_name),
                          "reason"_attr = e.reason());
        }
    }
}

void DatabaseImpl::setDropPending(OperationContext* opCtx, bool dropPending) {
    auto mode = dropPending ? MODE_X : MODE_IX;
    invariant(opCtx->lockState()->isDbLockedForMode(name(), mode));
    _dropPending.store(dropPending);
}

bool DatabaseImpl::isDropPending(OperationContext* opCtx) const {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_IS));
    return _dropPending.load();
}

void DatabaseImpl::getStats(OperationContext* opCtx,
                            DBStats* output,
                            bool includeFreeStorage,
                            double scale) const {

    long long nCollections = 0;
    long long nViews = 0;
    long long objects = 0;
    long long size = 0;
    long long storageSize = 0;
    long long freeStorageSize = 0;
    long long indexes = 0;
    long long indexSize = 0;
    long long indexFreeStorageSize = 0;

    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_IS));

    catalog::forEachCollectionFromDb(
        opCtx, name(), MODE_IS, [&](const Collection* collection) -> bool {
            nCollections += 1;
            objects += collection->numRecords(opCtx);
            size += collection->dataSize(opCtx);

            BSONObjBuilder temp;
            storageSize += collection->getRecordStore()->storageSize(opCtx, &temp);

            indexes += collection->getIndexCatalog()->numIndexesTotal();
            indexSize += collection->getIndexSize(opCtx);

            if (includeFreeStorage) {
                freeStorageSize += collection->getRecordStore()->freeStorageSize(opCtx);
                indexFreeStorageSize += collection->getIndexFreeStorageBytes(opCtx);
            }

            return true;
        });


    CollectionCatalog::get(opCtx)->iterateViews(opCtx, name(), [&](const ViewDefinition& view) {
        nViews += 1;
        return true;
    });

    // Make sure that the same fields are returned for non-existing dbs
    // in `DBStats::errmsgRun`
    output->setCollections(nCollections);
    output->setViews(nViews);
    output->setObjects(objects);
    output->setAvgObjSize(objects ? (double(size) / double(objects)) : 0);
    output->setDataSize(size / scale);
    output->setStorageSize(storageSize / scale);

    output->setIndexes(indexes);
    output->setIndexSize(indexSize / scale);
    output->setTotalSize((storageSize + indexSize) / scale);
    if (includeFreeStorage) {
        output->setFreeStorageSize(freeStorageSize / scale);
        output->setIndexFreeStorageSize(indexFreeStorageSize / scale);
        output->setTotalFreeStorageSize((freeStorageSize + indexFreeStorageSize) / scale);
    }
    output->setScaleFactor(scale);

    if (!opCtx->getServiceContext()->getStorageEngine()->isEphemeral()) {
        boost::filesystem::path dbpath(
            opCtx->getServiceContext()->getStorageEngine()->getFilesystemPathForDb(_name));
        boost::system::error_code ec;
        boost::filesystem::space_info spaceInfo = boost::filesystem::space(dbpath, ec);
        if (!ec) {
            output->setFsUsedSize((spaceInfo.capacity - spaceInfo.available) / scale);
            output->setFsTotalSize(spaceInfo.capacity / scale);
        } else {
            output->setFsUsedSize(-1);
            output->setFsTotalSize(-1);
            LOGV2(20312,
                  "Failed to query filesystem disk stats (code: {ec_value}): {ec_message}",
                  "Failed to query filesystem disk stats",
                  "error"_attr = ec.message(),
                  "errorCode"_attr = ec.value());
        }
    }
}

Status DatabaseImpl::dropView(OperationContext* opCtx, NamespaceString viewName) const {
    dassert(opCtx->lockState()->isDbLockedForMode(name(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(viewName, MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(NamespaceString(_viewsName), MODE_X));

    Status status = CollectionCatalog::get(opCtx)->dropView(opCtx, viewName);
    Top::get(opCtx->getServiceContext()).collectionDropped(viewName);
    return status;
}

Status DatabaseImpl::dropCollection(OperationContext* opCtx,
                                    NamespaceString nss,
                                    repl::OpTime dropOpTime,
                                    bool markFromMigrate) const {
    // Cannot drop uncommitted collections.
    invariant(!UncommittedCatalogUpdates::isCreatedCollection(opCtx, nss));

    auto catalog = CollectionCatalog::get(opCtx);

    if (!catalog->lookupCollectionByNamespace(opCtx, nss)) {
        // Collection doesn't exist so don't bother validating if it can be dropped.
        return Status::OK();
    }

    invariant(nss.dbName() == _name);

    // Returns true if the supplied namespace 'nss' is a system collection that can be dropped,
    // false otherwise.
    auto isDroppableSystemCollection = [](const auto& nss) {
        return nss.isHealthlog() || nss == NamespaceString::kLogicalSessionsNamespace ||
            nss == NamespaceString::kKeysCollectionNamespace ||
            nss.isTemporaryReshardingCollection() || nss.isTimeseriesBucketsCollection() ||
            nss.isChangeStreamPreImagesCollection() ||
            nss == NamespaceString::kConfigsvrRestoreNamespace || nss.isChangeCollection() ||
            nss.isSystemDotJavascript() || nss.isSystemStatsCollection();
    };

    if (nss.isSystem()) {
        if (nss.isSystemDotProfile()) {
            if (catalog->getDatabaseProfileLevel(_name) != 0)
                return Status(ErrorCodes::IllegalOperation,
                              "turn off profiling before dropping system.profile collection");
        } else if (nss.isSystemDotViews()) {
            if (!MONGO_unlikely(allowSystemViewsDrop.shouldFail())) {
                const auto viewStats = catalog->getViewStatsForDatabase(opCtx, _name);
                uassert(ErrorCodes::CommandFailed,
                        str::stream() << "cannot drop collection " << nss.toStringForErrorMsg()
                                      << " when time-series collections are present.",
                        viewStats && viewStats->userTimeseries == 0);
            }
        } else if (!isDroppableSystemCollection(nss)) {
            return Status(ErrorCodes::IllegalOperation,
                          str::stream()
                              << "can't drop system collection " << nss.toStringForErrorMsg());
        }
    }

    return dropCollectionEvenIfSystem(opCtx, nss, dropOpTime, markFromMigrate);
}

Status DatabaseImpl::dropCollectionEvenIfSystem(OperationContext* opCtx,
                                                NamespaceString nss,
                                                repl::OpTime dropOpTime,
                                                bool markFromMigrate) const {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));

    LOGV2_DEBUG(20313, 1, "dropCollection: {namespace}", "dropCollection", logAttrs(nss));

    // A valid 'dropOpTime' is not allowed when writes are replicated.
    if (!dropOpTime.isNull() && opCtx->writesAreReplicated()) {
        return Status(
            ErrorCodes::BadValue,
            "dropCollection() cannot accept a valid drop optime when writes are replicated.");
    }

    CollectionWriter collection(opCtx, nss);

    if (!collection) {
        return Status::OK();  // Post condition already met.
    }

    auto numRecords = collection->numRecords(opCtx);

    auto uuid = collection->uuid();

    // Make sure no indexes builds are in progress.
    // Use massert() to be consistent with IndexCatalog::dropAllIndexes().
    auto numIndexesInProgress = collection->getIndexCatalog()->numIndexesInProgress();
    massert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            str::stream() << "cannot drop collection " << nss.toStringForErrorMsg() << " (" << uuid
                          << ") when " << numIndexesInProgress << " index builds in progress.",
            numIndexesInProgress == 0);

    audit::logDropCollection(opCtx->getClient(), nss);

    auto serviceContext = opCtx->getServiceContext();
    Top::get(serviceContext).collectionDropped(nss);

    // Drop unreplicated collections immediately.
    // If 'dropOpTime' is provided, we should proceed to rename the collection.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto opObserver = serviceContext->getOpObserver();
    auto isOplogDisabledForNamespace = replCoord->isOplogDisabledFor(opCtx, nss);
    if (dropOpTime.isNull() && isOplogDisabledForNamespace) {
        _dropCollectionIndexes(opCtx, nss, collection.getWritableCollection(opCtx));
        opObserver->onDropCollection(opCtx,
                                     nss,
                                     uuid,
                                     numRecords,
                                     OpObserver::CollectionDropType::kOnePhase,
                                     markFromMigrate);
        return _finishDropCollection(opCtx, nss, collection.getWritableCollection(opCtx));
    }

    // Replicated collections should be dropped in two phases.

    // New two-phase drop: Starting in 4.2, pending collection drops will be maintained in the
    // storage engine and will no longer be visible at the catalog layer with 3.6-style
    // <db>.system.drop.* namespaces.
    if (serviceContext->getStorageEngine()->supportsPendingDrops()) {
        _dropCollectionIndexes(opCtx, nss, collection.getWritableCollection(opCtx));

        auto commitTimestamp = opCtx->recoveryUnit()->getCommitTimestamp();
        LOGV2(20314,
              "dropCollection: {namespace} ({uuid}) - storage engine will take ownership of "
              "drop-pending "
              "collection with optime {dropOpTime} and commit timestamp {commitTimestamp}",
              "dropCollection: storage engine will take ownership of drop-pending "
              "collection",
              logAttrs(nss),
              "uuid"_attr = uuid,
              "dropOpTime"_attr = dropOpTime,
              "commitTimestamp"_attr = commitTimestamp);
        if (dropOpTime.isNull()) {
            // Log oplog entry for collection drop and remove the UUID.
            dropOpTime = opObserver->onDropCollection(opCtx,
                                                      nss,
                                                      uuid,
                                                      numRecords,
                                                      OpObserver::CollectionDropType::kOnePhase,
                                                      markFromMigrate);
            invariant(!dropOpTime.isNull());
        } else {
            // If we are provided with a valid 'dropOpTime', it means we are dropping this
            // collection in the context of applying an oplog entry on a secondary.
            auto opTime = opObserver->onDropCollection(opCtx,
                                                       nss,
                                                       uuid,
                                                       numRecords,
                                                       OpObserver::CollectionDropType::kOnePhase,
                                                       markFromMigrate);
            // OpObserver::onDropCollection should not be writing to the oplog on the secondary.
            // The exception is shard merge where, we perform unreplicated timestamped drops of
            // imported collection on observing the state document update to aborted state via op
            // observer, both on primary and secondaries. In such cases, on primary, we expect
            // `opTime` equal to dropOpTime (i.e, state document update opTime).
            invariant(opTime.isNull() || repl::tenantMigrationInfo(opCtx),
                      str::stream()
                          << "OpTime is not null or equal to dropOptime. OpTime: "
                          << opTime.toString() << " dropOpTime: " << dropOpTime.toString());
        }

        return _finishDropCollection(opCtx, nss, collection.getWritableCollection(opCtx));
    }

    // Old two-phase drop: Replicated collections will be renamed with a special drop-pending
    // namespace and dropped when the replica set optime reaches the drop optime.

    if (dropOpTime.isNull()) {
        // Log oplog entry for collection drop.
        dropOpTime = opObserver->onDropCollection(opCtx,
                                                  nss,
                                                  uuid,
                                                  numRecords,
                                                  OpObserver::CollectionDropType::kTwoPhase,
                                                  markFromMigrate);
        invariant(!dropOpTime.isNull());
    } else {
        // If we are provided with a valid 'dropOpTime', it means we are dropping this
        // collection in the context of applying an oplog entry on a secondary.
        auto opTime = opObserver->onDropCollection(opCtx,
                                                   nss,
                                                   uuid,
                                                   numRecords,
                                                   OpObserver::CollectionDropType::kTwoPhase,
                                                   markFromMigrate);
        // OpObserver::onDropCollection should not be writing to the oplog on the secondary.
        invariant(opTime.isNull());
    }

    // Rename collection using drop-pending namespace generated from drop optime.
    auto dpns = nss.makeDropPendingNamespace(dropOpTime);
    const bool stayTemp = true;
    LOGV2(20315,
          "dropCollection: {namespace} ({uuid}) - renaming to drop-pending collection: "
          "{dropPendingName} with drop "
          "optime {dropOpTime}",
          "dropCollection: renaming to drop-pending collection",
          logAttrs(nss),
          "uuid"_attr = uuid,
          "dropPendingName"_attr = dpns,
          "dropOpTime"_attr = dropOpTime);
    {
        // This is a uniquely generated drop-pending namespace that no other operations are using.
        AllowLockAcquisitionOnTimestampedUnitOfWork allowLockAcquisition(opCtx->lockState());
        Lock::CollectionLock collLk(opCtx, dpns, MODE_X);
        fassert(40464, renameCollection(opCtx, nss, dpns, stayTemp));
    }

    // Register this drop-pending namespace with DropPendingCollectionReaper to remove when the
    // committed optime reaches the drop optime.
    repl::DropPendingCollectionReaper::get(opCtx)->addDropPendingNamespace(opCtx, dropOpTime, dpns);

    return Status::OK();
}

void DatabaseImpl::_dropCollectionIndexes(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          Collection* collection) const {
    invariant(_name == nss.dbName());
    LOGV2_DEBUG(20316, 1, "dropCollection: {namespace} - dropAllIndexes start", logAttrs(nss));
    collection->getIndexCatalog()->dropAllIndexes(opCtx, collection, true, {});

    invariant(collection->getTotalIndexCount() == 0);
    LOGV2_DEBUG(20317, 1, "dropCollection: {namespace} - dropAllIndexes done", logAttrs(nss));
}

Status DatabaseImpl::_finishDropCollection(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           Collection* collection) const {
    UUID uuid = collection->uuid();

    // Reduce log verbosity for virtual collections
    auto debugLevel = collection->getSharedIdent() ? 0 : 1;

    LOGV2_DEBUG(20318,
                debugLevel,
                "Finishing collection drop for {namespace} ({uuid}).",
                "Finishing collection drop",
                logAttrs(nss),
                "uuid"_attr = uuid);

    // A virtual collection does not have a durable catalog entry.
    if (auto sharedIdent = collection->getSharedIdent()) {
        auto status = catalog::dropCollection(
            opCtx, collection->ns(), collection->getCatalogId(), sharedIdent);
        if (!status.isOK())
            return status;

        opCtx->recoveryUnit()->onCommit(
            [nss, uuid, ident = sharedIdent->getIdent()](OperationContext* opCtx,
                                                         boost::optional<Timestamp> commitTime) {
                if (!commitTime) {
                    return;
                }

                HistoricalIdentTracker::get(opCtx).recordDrop(ident, nss, uuid, commitTime.value());
            });
    }

    CollectionCatalog::get(opCtx)->dropCollection(
        opCtx, collection, opCtx->getServiceContext()->getStorageEngine()->supportsPendingDrops());


    return Status::OK();
}

Status DatabaseImpl::renameCollection(OperationContext* opCtx,
                                      NamespaceString fromNss,
                                      NamespaceString toNss,
                                      bool stayTemp) const {
    audit::logRenameCollection(opCtx->getClient(), fromNss, toNss);

    invariant(opCtx->lockState()->isCollectionLockedForMode(fromNss, MODE_X));
    invariant(opCtx->lockState()->isCollectionLockedForMode(toNss, MODE_X));

    invariant(fromNss.dbName() == _name);
    invariant(toNss.dbName() == _name);
    if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, toNss)) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "Cannot rename '" << fromNss.toStringForErrorMsg()
                                    << "' to '" << toNss.toStringForErrorMsg()
                                    << "' because the destination namespace already exists");
    }

    CollectionWriter collToRename(opCtx, fromNss);
    if (!collToRename) {
        return Status(ErrorCodes::NamespaceNotFound, "collection not found to rename");
    }

    assertNoMovePrimaryInProgress(opCtx, fromNss);

    LOGV2(20319,
          "renameCollection: renaming collection {collToRename_uuid} from {fromNss} to {toNss}",
          "renameCollection",
          "uuid"_attr = collToRename->uuid(),
          "fromName"_attr = fromNss,
          "toName"_attr = toNss);

    Top::get(opCtx->getServiceContext()).collectionDropped(fromNss);

    // Set the namespace of 'collToRename' from within the CollectionCatalog. This is necessary
    // because the CollectionCatalog manages the necessary isolation for this Collection until the
    // WUOW commits.
    auto writableCollection = collToRename.getWritableCollection(opCtx);
    Status status = writableCollection->rename(opCtx, toNss, stayTemp);
    if (!status.isOK())
        return status;

    CollectionCatalog::get(opCtx)->onCollectionRename(opCtx, writableCollection, fromNss);

    opCtx->recoveryUnit()->onCommit([fromNss,
                                     writableCollection](OperationContext* opCtx,
                                                         boost::optional<Timestamp> commitTime) {
        if (!commitTime) {
            return;
        }

        HistoricalIdentTracker::get(opCtx).recordRename(
            writableCollection->getSharedIdent()->getIdent(),
            fromNss,
            writableCollection->uuid(),
            commitTime.value());

        const auto readyIndexes = writableCollection->getIndexCatalog()->getAllReadyEntriesShared();
        for (const auto& readyIndex : readyIndexes) {
            HistoricalIdentTracker::get(opCtx).recordRename(
                readyIndex->getIdent(), fromNss, writableCollection->uuid(), commitTime.value());
        }
    });

    return status;
}

void DatabaseImpl::_checkCanCreateCollection(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionOptions& options) const {
    if (CollectionCatalog::get(opCtx)->lookupCollectionByNamespace(opCtx, nss)) {
        if (options.isView()) {
            uasserted(ErrorCodes::NamespaceExists,
                      str::stream() << "Cannot create collection " << nss.toStringForErrorMsg()
                                    << " - collection already exists.");
        } else {
            throwWriteConflictException(str::stream()
                                        << "Collection namespace '" << nss.toStringForErrorMsg()
                                        << "' is already in use.");
        }
    }

    if (MONGO_unlikely(throwWCEDuringTxnCollCreate.shouldFail()) &&
        opCtx->inMultiDocumentTransaction()) {
        LOGV2(4696600,
              "Throwing WriteConflictException due to failpoint 'throwWCEDuringTxnCollCreate'");
        throwWriteConflictException(str::stream() << "Hit failpoint '"
                                                  << throwWCEDuringTxnCollCreate.getName() << "'.");
    }

    uassert(28838, "cannot create a non-capped oplog collection", options.capped || !nss.isOplog());
    uassert(ErrorCodes::DatabaseDropPending,
            str::stream() << "Cannot create collection " << nss.toStringForErrorMsg()
                          << " - database is in the process of being dropped.",
            !_dropPending.load());
}

Status DatabaseImpl::createView(OperationContext* opCtx,
                                const NamespaceString& viewName,
                                const CollectionOptions& options) const {
    dassert(opCtx->lockState()->isDbLockedForMode(name(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(viewName, MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(NamespaceString(_viewsName), MODE_X));

    invariant(options.isView());

    NamespaceString viewOnNss(viewName.db(), options.viewOn);
    _checkCanCreateCollection(opCtx, viewName, options);

    BSONArray pipeline(options.pipeline);
    auto status = Status::OK();
    if (viewName.isOplog()) {
        status = {ErrorCodes::InvalidNamespace,
                  str::stream() << "invalid namespace name for a view: " + viewName.toString()};
    } else {
        status = CollectionCatalog::get(opCtx)->createView(opCtx,
                                                           viewName,
                                                           viewOnNss,
                                                           pipeline,
                                                           view_catalog_helpers::validatePipeline,
                                                           options.collation);
    }

    audit::logCreateView(
        opCtx->getClient(), viewName, viewOnNss.toString(), pipeline, status.code());
    return status;
}

Collection* DatabaseImpl::createCollection(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const CollectionOptions& options,
                                           bool createIdIndex,
                                           const BSONObj& idIndex,
                                           bool fromMigrate) const {
    return _createCollection(
        opCtx, nss, options, createIdIndex, idIndex, fromMigrate, /*vopts=*/boost::none);
}

Collection* DatabaseImpl::createVirtualCollection(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const CollectionOptions& opts,
                                                  const VirtualCollectionOptions& vopts) const {
    return _createCollection(opCtx,
                             nss,
                             opts,
                             /*createIdIndex=*/false,
                             /*idIndex=*/BSONObj(),
                             /*fromMigrate=*/false,
                             vopts);
}

/**
 * Some system collections (namely, 'config.transactions' and
 * 'local.replset.oplogTruncateAfterPoint') are special internal collections that are written to
 * without updating indexes, so there's no value in creating an index on them.
 *
 * @return true if any modification on the collection data leads to updating the indexes defined on
 * it.
 */
bool doesCollectionModificationsUpdateIndexes(const NamespaceString& nss) {
    const auto& collName = nss.ns();
    return collName != "config.transactions" &&
        collName !=
        repl::ReplicationConsistencyMarkersImpl::kDefaultOplogTruncateAfterPointNamespace;
}

Collection* DatabaseImpl::_createCollection(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const CollectionOptions& options,
    bool createIdIndex,
    const BSONObj& idIndex,
    bool fromMigrate,
    const boost::optional<VirtualCollectionOptions>& vopts) const {
    invariant(!options.isView());

    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_IX));

    auto coordinator = repl::ReplicationCoordinator::get(opCtx);
    bool canAcceptWrites =
        (coordinator->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) ||
        coordinator->canAcceptWritesFor(opCtx, nss);

    CollectionOptions optionsWithUUID = options;
    bool generatedUUID = false;
    if (!optionsWithUUID.uuid) {
        if (!canAcceptWrites) {
            LOGV2_ERROR_OPTIONS(20329,
                                {logv2::UserAssertAfterLog(ErrorCodes::InvalidOptions)},
                                "Attempted to create a new collection without a UUID",
                                logAttrs(nss));
        } else {
            optionsWithUUID.uuid.emplace(UUID::gen());
            generatedUUID = true;
        }
    }

    // Because writing the oplog entry depends on having the full spec for the _id index, which is
    // not available until the collection is actually created, we can't write the oplog entry until
    // after we have created the collection.  In order to make the storage timestamp for the
    // collection create always correct even when other operations are present in the same storage
    // transaction, we reserve an opTime before the collection creation, then pass it to the
    // opObserver.  Reserving the optime automatically sets the storage timestamp.
    // In order to ensure isolation of multi-document transactions, createCollection should only
    // reserve oplog slots here if it is run outside of a multi-document transaction. Multi-
    // document transactions reserve the appropriate oplog slots at commit time.
    OplogSlot createOplogSlot;
    if (canAcceptWrites && !coordinator->isOplogDisabledFor(opCtx, nss) &&
        !opCtx->inMultiDocumentTransaction()) {
        createOplogSlot = repl::getNextOpTime(opCtx);
    }

    hangAndFailAfterCreateCollectionReservesOpTime.executeIf(
        [&](const BSONObj&) {
            hangAndFailAfterCreateCollectionReservesOpTime.pauseWhileSet(opCtx);
            uasserted(51267, "hangAndFailAfterCreateCollectionReservesOpTime fail point enabled");
        },
        [&](const BSONObj& data) {
            auto fpNss = data["nss"].str();
            return fpNss.empty() || fpNss == nss.toString();
        });

    _checkCanCreateCollection(opCtx, nss, optionsWithUUID);
    assertNoMovePrimaryInProgress(opCtx, nss);
    audit::logCreateCollection(opCtx->getClient(), nss);

    // Reduce log verbosity for virtual collections
    auto debugLevel = vopts ? 1 : 0;

    LOGV2_DEBUG(20320,
                debugLevel,
                "createCollection: {namespace} with {generatedUUID_generated_provided} UUID: "
                "{optionsWithUUID_uuid_get} and options: {options}",
                "createCollection",
                logAttrs(nss),
                "uuidDisposition"_attr = (generatedUUID ? "generated" : "provided"),
                "uuid"_attr = optionsWithUUID.uuid.value(),
                "options"_attr = options);

    // Create Collection object
    auto ownedCollection = [&]() -> std::shared_ptr<Collection> {
        if (!vopts) {
            auto storageEngine = opCtx->getServiceContext()->getStorageEngine();
            std::pair<RecordId, std::unique_ptr<RecordStore>> catalogIdRecordStorePair =
                uassertStatusOK(storageEngine->getCatalog()->createCollection(
                    opCtx, nss, optionsWithUUID, true /*allocateDefaultSpace*/));
            auto& catalogId = catalogIdRecordStorePair.first;
            return Collection::Factory::get(opCtx)->make(
                opCtx, nss, catalogId, optionsWithUUID, std::move(catalogIdRecordStorePair.second));
        } else {
            // Virtual collection stays only in memory and its metadata need not persist on disk and
            // therefore we bypass DurableCatalog.
            return VirtualCollectionImpl::make(opCtx, nss, optionsWithUUID, *vopts);
        }
    }();
    auto collection = ownedCollection.get();
    ownedCollection->init(opCtx);
    ownedCollection->setCommitted(false);

    CollectionCatalog::get(opCtx)->onCreateCollection(opCtx, std::move(ownedCollection));
    openCreateCollectionWindowFp.executeIf([&](const BSONObj& data) { sleepsecs(3); },
                                           [&](const BSONObj& data) {
                                               const auto collElem = data["collectionNS"];
                                               return !collElem || nss.toString() == collElem.str();
                                           });

    BSONObj fullIdIndexSpec;

    bool createColumnIndex = false;
    if (createIdIndex && collection->requiresIdIndex()) {
        if (optionsWithUUID.autoIndexId == CollectionOptions::YES ||
            optionsWithUUID.autoIndexId == CollectionOptions::DEFAULT) {
            auto* ic = collection->getIndexCatalog();
            fullIdIndexSpec = uassertStatusOK(ic->createIndexOnEmptyCollection(
                opCtx,
                collection,
                !idIndex.isEmpty() ? idIndex
                                   : ic->getDefaultIdIndexSpec(CollectionPtr(collection))));
            createColumnIndex = createColumnIndexOnAllCollections.shouldFail() &&
                doesCollectionModificationsUpdateIndexes(nss);
        } else {
            // autoIndexId: false is only allowed on unreplicated collections.
            uassert(50001,
                    str::stream() << "autoIndexId:false is not allowed for collection "
                                  << nss.toStringForErrorMsg() << " because it can be replicated",
                    !nss.isReplicated());
        }
    }

    if (MONGO_unlikely(createColumnIndex)) {
        invariant(ServerParameterSet::getNodeParameterSet()
                          ->get<QueryFrameworkControl>("internalQueryFrameworkControl")
                          ->_data.get() != QueryFrameworkControlEnum::kForceClassicEngine,
                  "Column Store Indexes failpoint in use without enabling SBE engine");
        uassertStatusOK(collection->getIndexCatalog()->createIndexOnEmptyCollection(
            opCtx, collection, kColumnStoreSpec));
    }

    hangBeforeLoggingCreateCollection.pauseWhileSet();

    opCtx->getServiceContext()->getOpObserver()->onCreateCollection(opCtx,
                                                                    CollectionPtr(collection),
                                                                    nss,
                                                                    optionsWithUUID,
                                                                    fullIdIndexSpec,
                                                                    createOplogSlot,
                                                                    fromMigrate);

    // It is necessary to create the system index *after* running the onCreateCollection so that
    // the storage timestamp for the index creation is after the storage timestamp for the
    // collection creation, and the opTimes for the corresponding oplog entries are the same as the
    // storage timestamps.  This way both primary and any secondaries will see the index created
    // after the collection is created.
    if (canAcceptWrites && createIdIndex && nss.isSystem()) {
        CollectionWriter collWriter(collection);
        createSystemIndexes(opCtx, collWriter, fromMigrate);
    }

    return collection;
}

StatusWith<std::unique_ptr<CollatorInterface>> DatabaseImpl::_validateCollator(
    OperationContext* opCtx, CollectionOptions& opts) const {
    std::unique_ptr<CollatorInterface> collator;
    if (!opts.collation.isEmpty()) {
        auto collatorWithStatus =
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(opts.collation);

        if (!collatorWithStatus.isOK()) {
            return collatorWithStatus.getStatus();
        }

        collator = std::move(collatorWithStatus.getValue());

        // If the collator factory returned a non-null collator, set the collation option to the
        // result of serializing the collator's spec back into BSON. We do this in order to fill in
        // all options that the user omitted.
        //
        // If the collator factory returned a null collator (representing the "simple" collation),
        // we simply unset the "collation" from the collection options. This ensures that
        // collections created on versions which do not support the collation feature have the same
        // format for representing the simple collation as collections created on this version.
        opts.collation = collator ? collator->getSpec().toBSON() : BSONObj();
    }

    return {std::move(collator)};
}

Status DatabaseImpl::userCreateNS(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  CollectionOptions collectionOptions,
                                  bool createDefaultIndexes,
                                  const BSONObj& idIndex,
                                  bool fromMigrate) const {
    LOGV2_DEBUG(20324,
                1,
                "create collection {namespace} {collectionOptions}",
                logAttrs(nss),
                "collectionOptions"_attr = collectionOptions.toBSON());
    if (!NamespaceString::validCollectionComponent(nss.ns()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid ns: " << nss.toStringForErrorMsg());

    // Validate the collation, if there is one.
    auto swCollator = _validateCollator(opCtx, collectionOptions);
    if (!swCollator.isOK()) {
        return swCollator.getStatus();
    }

    if (!collectionOptions.validator.isEmpty()) {
        boost::intrusive_ptr<ExpressionContext> expCtx(
            new ExpressionContext(opCtx, std::move(swCollator.getValue()), nss));

        // If the feature compatibility version is not kLatest, and we are validating features as
        // primary, ban the use of new agg features introduced in kLatest to prevent them from being
        // persisted in the catalog.
        // (Generic FCV reference): This FCV check should exist across LTS binary versions.
        multiversion::FeatureCompatibilityVersion fcv;
        if (serverGlobalParams.validateFeaturesAsPrimary.load() &&
            serverGlobalParams.featureCompatibility.isLessThan(multiversion::GenericFCV::kLatest,
                                                               &fcv)) {
            expCtx->maxFeatureCompatibilityVersion = fcv;
        }

        // The match expression parser needs to know that we're parsing an expression for a
        // validator to apply some additional checks.
        expCtx->isParsingCollectionValidator = true;

        // If the validation action is "warn" or the level is "moderate", or if the user has
        // defined some encrypted fields in the collection options, then disallow any encryption
        // keywords. This is to prevent any plaintext data from showing up in the logs.
        auto allowedFeatures = MatchExpressionParser::kDefaultSpecialFeatures;

        if (collectionOptions.validationAction == ValidationActionEnum::warn ||
            collectionOptions.validationLevel == ValidationLevelEnum::moderate ||
            collectionOptions.encryptedFieldConfig.has_value())
            allowedFeatures &= ~MatchExpressionParser::AllowedFeatures::kEncryptKeywords;

        auto statusWithMatcher = MatchExpressionParser::parse(collectionOptions.validator,
                                                              std::move(expCtx),
                                                              ExtensionsCallbackNoop(),
                                                              allowedFeatures);

        // Increment counters to track the usage of schema validators.
        validatorCounters.incrementCounters(
            "create", collectionOptions.validator, statusWithMatcher.isOK());

        // We check the status of the parse to see if there are any banned features, but we don't
        // actually need the result for now.
        if (!statusWithMatcher.isOK()) {
            return statusWithMatcher.getStatus();
        }
    }

    Status status = validateStorageOptions(
        opCtx->getServiceContext(),
        collectionOptions.storageEngine,
        [](const auto& x, const auto& y) { return x->validateCollectionStorageOptions(y); });

    if (!status.isOK())
        return status;

    if (auto storageEngineOptions = collectionOptions.indexOptionDefaults.getStorageEngine()) {
        status = validateStorageOptions(
            opCtx->getServiceContext(), *storageEngineOptions, [](const auto& x, const auto& y) {
                return x->validateIndexStorageOptions(y);
            });

        if (!status.isOK()) {
            return status;
        }
    }

    if (collectionOptions.isView()) {
        if (nss.isSystem())
            return Status(
                ErrorCodes::InvalidNamespace,
                "View name cannot start with 'system.', which is reserved for system namespaces");
        return createView(opCtx, nss, collectionOptions);
    } else {
        invariant(_createCollection(
                      opCtx, nss, collectionOptions, createDefaultIndexes, idIndex, fromMigrate),
                  str::stream() << "Collection creation failed after validating options: "
                                << nss.toStringForErrorMsg()
                                << ". Options: " << collectionOptions.toBSON());
    }

    return Status::OK();
}

Status DatabaseImpl::userCreateVirtualNS(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         CollectionOptions opts,
                                         const VirtualCollectionOptions& vopts) const {
    LOGV2_DEBUG(6968505,
                1,
                "create collection {namespace} {collectionOptions}",
                logAttrs(nss),
                "collectionOptions"_attr = opts.toBSON());
    if (!NamespaceString::validCollectionComponent(nss.ns()))
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid ns: " << nss.toStringForErrorMsg());

    // Validate the collation, if there is one.
    if (auto swCollator = _validateCollator(opCtx, opts); !swCollator.isOK()) {
        return swCollator.getStatus();
    }

    invariant(_createCollection(opCtx,
                                nss,
                                opts,
                                /*createDefaultIndexes=*/false,
                                /*idIndex=*/BSONObj(),
                                /*fromMigrate=*/false,
                                vopts),
              str::stream() << "Collection creation failed after validating options: "
                            << nss.toStringForErrorMsg() << ". Options: " << opts.toBSON());

    return Status::OK();
}

}  // namespace mongo
