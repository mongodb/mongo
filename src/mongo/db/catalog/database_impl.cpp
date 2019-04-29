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

#include "mongo/db/catalog/database_impl.h"

#include <algorithm>
#include <memory>
#include <vector>

#include <boost/filesystem/operations.hpp>

#include "mongo/base/init.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_impl.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/catalog/uuid_catalog_helper.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/introspect.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/drop_pending_collection_reaper.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/system_index.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/platform/random.h"
#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeLoggingCreateCollection);

std::unique_ptr<Collection> _createCollectionInstance(OperationContext* opCtx,
                                                      const NamespaceString& nss) {

    auto cce = UUIDCatalog::get(opCtx).lookupCollectionCatalogEntryByNamespace(nss);
    auto rs = cce->getRecordStore();
    auto uuid = cce->getCollectionOptions(opCtx).uuid;
    invariant(rs,
              str::stream() << "Record store did not exist. Collection: " << nss.ns() << " UUID: "
                            << uuid);
    invariant(uuid);

    auto coll = std::make_unique<CollectionImpl>(opCtx, nss.ns(), uuid, cce, rs);

    return coll;
}

Status validateDBNameForWindows(StringData dbname) {
    const std::vector<std::string> windowsReservedNames = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};

    std::string lower(dbname.toString());
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    if (std::count(windowsReservedNames.begin(), windowsReservedNames.end(), lower))
        return Status(ErrorCodes::BadValue,
                      str::stream() << "db name \"" << dbname << "\" is a reserved name");
    return Status::OK();
}
}  // namespace

void uassertNamespaceNotIndex(StringData ns, StringData caller) {
    uassert(17320,
            str::stream() << "cannot do " << caller << " on namespace with a $ in it: " << ns,
            NamespaceString::normal(ns));
}

void DatabaseImpl::close(OperationContext* opCtx) const {
    invariant(opCtx->lockState()->isW());

    // Clear cache of oplog Collection pointer.
    repl::oplogCheckCloseDatabase(opCtx, this);
}

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

DatabaseImpl::DatabaseImpl(const StringData name, uint64_t epoch)
    : _name(name.toString()),
      _epoch(epoch),
      _profileName(_name + ".system.profile"),
      _viewsName(_name + "." + DurableViewCatalog::viewsCollectionName().toString()) {
    auto durableViewCatalog = std::make_unique<DurableViewCatalogImpl>(this);
    auto viewCatalog = std::make_unique<ViewCatalog>(std::move(durableViewCatalog));

    ViewCatalog::set(this, std::move(viewCatalog));
    _profile.store(serverGlobalParams.defaultProfile);
}

void DatabaseImpl::init(OperationContext* const opCtx) const {
    Status status = validateDBName(_name);

    if (!status.isOK()) {
        warning() << "tried to open invalid db: " << _name;
        uasserted(10028, status.toString());
    }

    auto& uuidCatalog = UUIDCatalog::get(opCtx);
    for (const auto& nss : uuidCatalog.getAllCollectionNamesFromDb(opCtx, _name)) {
        auto ownedCollection = _createCollectionInstance(opCtx, nss);
        invariant(ownedCollection);

        // Call registerCollectionObject directly because we're not in a WUOW.
        auto uuid = *(ownedCollection->uuid());
        uuidCatalog.registerCollectionObject(uuid, std::move(ownedCollection));
    }

    // At construction time of the viewCatalog, the UUIDCatalog map wasn't initialized yet, so no
    // system.views collection would be found. Now we're sufficiently initialized, signal a version
    // change. Also force a reload, so if there are problems with the catalog contents as might be
    // caused by incorrect mongod versions or similar, they are found right away.
    auto views = ViewCatalog::get(this);
    views->invalidate();
    Status reloadStatus = views->reloadIfNeeded(opCtx);

    if (!reloadStatus.isOK()) {
        warning() << "Unable to parse views: " << redact(reloadStatus)
                  << "; remove any invalid views from the " << _viewsName
                  << " collection to restore server functionality." << startupWarningsLog;
    }
}

void DatabaseImpl::clearTmpCollections(OperationContext* opCtx) const {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    for (const auto& nss : UUIDCatalog::get(opCtx).getAllCollectionNamesFromDb(opCtx, _name)) {
        CollectionCatalogEntry* coll =
            UUIDCatalog::get(opCtx).lookupCollectionCatalogEntryByNamespace(nss);
        CollectionOptions options = coll->getCollectionOptions(opCtx);

        if (!options.temp)
            continue;
        try {
            WriteUnitOfWork wunit(opCtx);
            Status status = dropCollection(opCtx, nss, {});

            if (!status.isOK()) {
                warning() << "could not drop temp collection '" << nss << "': " << redact(status);
                continue;
            }

            wunit.commit();
        } catch (const WriteConflictException&) {
            warning() << "could not drop temp collection '" << nss << "' due to "
                                                                      "WriteConflictException";
            opCtx->recoveryUnit()->abandonSnapshot();
        }
    }
}

Status DatabaseImpl::setProfilingLevel(OperationContext* opCtx, int newLevel) {
    auto currLevel = _profile.load();

    if (currLevel == newLevel) {
        return Status::OK();
    }

    if (newLevel == 0) {
        _profile.store(0);
        return Status::OK();
    }

    if (newLevel < 0 || newLevel > 2) {
        return Status(ErrorCodes::BadValue, "profiling level has to be >=0 and <= 2");
    }

    // Can't support profiling without supporting capped collections.
    if (!opCtx->getServiceContext()->getStorageEngine()->supportsCappedCollections()) {
        return Status(ErrorCodes::CommandNotSupported,
                      "the storage engine doesn't support profiling.");
    }

    Status status = createProfileCollection(opCtx, this);

    if (!status.isOK()) {
        return status;
    }

    _profile.store(newLevel);

    return Status::OK();
}

void DatabaseImpl::setDropPending(OperationContext* opCtx, bool dropPending) {
    auto mode = dropPending ? MODE_X : MODE_IX;
    invariant(opCtx->lockState()->isDbLockedForMode(name(), mode));
    _dropPending.store(dropPending);
}

bool DatabaseImpl::isDropPending(OperationContext* opCtx) const {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    return _dropPending.load();
}

void DatabaseImpl::getStats(OperationContext* opCtx, BSONObjBuilder* output, double scale) const {

    long long nCollections = 0;
    long long nViews = 0;
    long long objects = 0;
    long long size = 0;
    long long storageSize = 0;
    long long numExtents = 0;
    long long indexes = 0;
    long long indexSize = 0;

    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_IS));

    catalog::forEachCollectionFromDb(
        opCtx,
        name(),
        MODE_IS,
        [&](Collection* collection, CollectionCatalogEntry* catalogEntry) -> bool {
            nCollections += 1;
            objects += collection->numRecords(opCtx);
            size += collection->dataSize(opCtx);

            BSONObjBuilder temp;
            storageSize += collection->getRecordStore()->storageSize(opCtx, &temp);
            numExtents += temp.obj()["numExtents"].numberInt();  // XXX

            indexes += collection->getIndexCatalog()->numIndexesTotal(opCtx);
            indexSize += collection->getIndexSize(opCtx);

            return true;
        });

    ViewCatalog::get(this)->iterate(opCtx, [&](const ViewDefinition& view) { nViews += 1; });

    output->appendNumber("collections", nCollections);
    output->appendNumber("views", nViews);
    output->appendNumber("objects", objects);
    output->append("avgObjSize", objects == 0 ? 0 : double(size) / double(objects));
    output->appendNumber("dataSize", size / scale);
    output->appendNumber("storageSize", storageSize / scale);
    output->appendNumber("numExtents", numExtents);
    output->appendNumber("indexes", indexes);
    output->appendNumber("indexSize", indexSize / scale);

    if (!opCtx->getServiceContext()->getStorageEngine()->isEphemeral()) {
        boost::filesystem::path dbpath(
            opCtx->getServiceContext()->getStorageEngine()->getFilesystemPathForDb(_name));
        boost::system::error_code ec;
        boost::filesystem::space_info spaceInfo = boost::filesystem::space(dbpath, ec);
        if (!ec) {
            output->appendNumber("fsUsedSize", (spaceInfo.capacity - spaceInfo.available) / scale);
            output->appendNumber("fsTotalSize", spaceInfo.capacity / scale);
        } else {
            output->appendNumber("fsUsedSize", -1);
            output->appendNumber("fsTotalSize", -1);
            log() << "Failed to query filesystem disk stats (code: " << ec.value()
                  << "): " << ec.message();
        }
    }
}

Status DatabaseImpl::dropView(OperationContext* opCtx, NamespaceString viewName) const {
    dassert(opCtx->lockState()->isDbLockedForMode(name(), MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(viewName, MODE_IX));
    dassert(opCtx->lockState()->isCollectionLockedForMode(NamespaceString(_viewsName), MODE_X));

    auto views = ViewCatalog::get(this);
    Status status = views->dropView(opCtx, viewName);
    Top::get(opCtx->getServiceContext()).collectionDropped(viewName);
    return status;
}

Status DatabaseImpl::dropCollection(OperationContext* opCtx,
                                    NamespaceString nss,
                                    repl::OpTime dropOpTime) const {
    if (!getCollection(opCtx, nss)) {
        // Collection doesn't exist so don't bother validating if it can be dropped.
        return Status::OK();
    }

    invariant(nss.db() == _name);

    if (nss.isSystem()) {
        if (nss.isSystemDotProfile()) {
            if (_profile.load() != 0)
                return Status(ErrorCodes::IllegalOperation,
                              "turn off profiling before dropping system.profile collection");
        } else if (!(nss.isSystemDotViews() || nss.isHealthlog() ||
                     nss == NamespaceString::kLogicalSessionsNamespace ||
                     nss == NamespaceString::kSystemKeysNamespace)) {
            return Status(ErrorCodes::IllegalOperation,
                          str::stream() << "can't drop system collection " << nss);
        }
    }

    return dropCollectionEvenIfSystem(opCtx, nss, dropOpTime);
}

Status DatabaseImpl::dropCollectionEvenIfSystem(OperationContext* opCtx,
                                                NamespaceString nss,
                                                repl::OpTime dropOpTime) const {
    invariant(opCtx->lockState()->isCollectionLockedForMode(nss, MODE_X));

    LOG(1) << "dropCollection: " << nss;

    // A valid 'dropOpTime' is not allowed when writes are replicated.
    if (!dropOpTime.isNull() && opCtx->writesAreReplicated()) {
        return Status(
            ErrorCodes::BadValue,
            "dropCollection() cannot accept a valid drop optime when writes are replicated.");
    }

    Collection* collection = getCollection(opCtx, nss);

    if (!collection) {
        return Status::OK();  // Post condition already met.
    }

    auto numRecords = collection->numRecords(opCtx);

    auto uuid = collection->uuid();
    auto uuidString = uuid ? uuid.get().toString() : "no UUID";

    uassertNamespaceNotIndex(nss.toString(), "dropCollection");

    // Make sure no indexes builds are in progress.
    // Use massert() to be consistent with IndexCatalog::dropAllIndexes().
    auto numIndexesInProgress = collection->getIndexCatalog()->numIndexesInProgress(opCtx);
    massert(ErrorCodes::BackgroundOperationInProgressForNamespace,
            str::stream() << "cannot drop collection " << nss << " (" << uuidString << ") when "
                          << numIndexesInProgress
                          << " index builds in progress.",
            numIndexesInProgress == 0);

    audit::logDropCollection(&cc(), nss.toString());

    auto serviceContext = opCtx->getServiceContext();
    Top::get(serviceContext).collectionDropped(nss);

    // Drop unreplicated collections immediately.
    // If 'dropOpTime' is provided, we should proceed to rename the collection.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto opObserver = serviceContext->getOpObserver();
    auto isOplogDisabledForNamespace = replCoord->isOplogDisabledFor(opCtx, nss);
    if (dropOpTime.isNull() && isOplogDisabledForNamespace) {
        _dropCollectionIndexes(opCtx, nss, collection);
        opObserver->onDropCollection(
            opCtx, nss, uuid, numRecords, OpObserver::CollectionDropType::kOnePhase);
        return _finishDropCollection(opCtx, nss, collection);
    }

    // Replicated collections should be dropped in two phases.

    // New two-phase drop: Starting in 4.2, pending collection drops will be maintained in the
    // storage engine and will no longer be visible at the catalog layer with 3.6-style
    // <db>.system.drop.* namespaces.
    if (serviceContext->getStorageEngine()->supportsPendingDrops()) {
        _dropCollectionIndexes(opCtx, nss, collection);

        auto commitTimestamp = opCtx->recoveryUnit()->getCommitTimestamp();
        log() << "dropCollection: " << nss << " (" << uuidString
              << ") - storage engine will take ownership of drop-pending collection with optime "
              << dropOpTime << " and commit timestamp " << commitTimestamp;
        if (dropOpTime.isNull()) {
            // Log oplog entry for collection drop and remove the UUID.
            dropOpTime = opObserver->onDropCollection(
                opCtx, nss, uuid, numRecords, OpObserver::CollectionDropType::kOnePhase);
            invariant(!dropOpTime.isNull());
        } else {
            // If we are provided with a valid 'dropOpTime', it means we are dropping this
            // collection in the context of applying an oplog entry on a secondary.
            auto opTime = opObserver->onDropCollection(
                opCtx, nss, uuid, numRecords, OpObserver::CollectionDropType::kOnePhase);
            // OpObserver::onDropCollection should not be writing to the oplog on the secondary.
            invariant(opTime.isNull());
        }

        return _finishDropCollection(opCtx, nss, collection);
    }

    // Old two-phase drop: Replicated collections will be renamed with a special drop-pending
    // namespace and dropped when the replica set optime reaches the drop optime.

    if (dropOpTime.isNull()) {
        // Log oplog entry for collection drop.
        dropOpTime = opObserver->onDropCollection(
            opCtx, nss, uuid, numRecords, OpObserver::CollectionDropType::kTwoPhase);
        invariant(!dropOpTime.isNull());
    } else {
        // If we are provided with a valid 'dropOpTime', it means we are dropping this
        // collection in the context of applying an oplog entry on a secondary.
        auto opTime = opObserver->onDropCollection(
            opCtx, nss, uuid, numRecords, OpObserver::CollectionDropType::kTwoPhase);
        // OpObserver::onDropCollection should not be writing to the oplog on the secondary.
        invariant(opTime.isNull());
    }

    // Rename collection using drop-pending namespace generated from drop optime.
    auto dpns = nss.makeDropPendingNamespace(dropOpTime);
    const bool stayTemp = true;
    log() << "dropCollection: " << nss << " (" << uuidString
          << ") - renaming to drop-pending collection: " << dpns << " with drop optime "
          << dropOpTime;
    {
        Lock::CollectionLock(opCtx, dpns, MODE_X);
        fassert(40464, renameCollection(opCtx, nss, dpns, stayTemp));
    }

    // Register this drop-pending namespace with DropPendingCollectionReaper to remove when the
    // committed optime reaches the drop optime.
    repl::DropPendingCollectionReaper::get(opCtx)->addDropPendingNamespace(dropOpTime, dpns);

    return Status::OK();
}

void DatabaseImpl::_dropCollectionIndexes(OperationContext* opCtx,
                                          const NamespaceString& nss,
                                          Collection* collection) const {
    invariant(_name == nss.db());
    LOG(1) << "dropCollection: " << nss << " - dropAllIndexes start";
    collection->getIndexCatalog()->dropAllIndexes(opCtx, true);

    invariant(collection->getCatalogEntry()->getTotalIndexCount(opCtx) == 0);
    LOG(1) << "dropCollection: " << nss << " - dropAllIndexes done";
}

Status DatabaseImpl::_finishDropCollection(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           Collection* collection) const {
    UUID uuid = *collection->uuid();
    log() << "Finishing collection drop for " << nss << " (" << uuid << ").";

    UUIDCatalog& catalog = UUIDCatalog::get(opCtx);
    catalog.onDropCollection(opCtx, uuid);

    auto storageEngine =
        checked_cast<KVStorageEngine*>(opCtx->getServiceContext()->getStorageEngine());
    return storageEngine->getCatalog()->dropCollection(opCtx, nss);
}

Collection* DatabaseImpl::getCollection(OperationContext* opCtx, const NamespaceString& nss) const {
    return UUIDCatalog::get(opCtx).lookupCollectionByNamespace(nss);
}

Status DatabaseImpl::renameCollection(OperationContext* opCtx,
                                      NamespaceString fromNss,
                                      NamespaceString toNss,
                                      bool stayTemp) const {
    audit::logRenameCollection(&cc(), fromNss.ns(), toNss.ns());

    invariant(opCtx->lockState()->isCollectionLockedForMode(fromNss, MODE_X));
    invariant(opCtx->lockState()->isCollectionLockedForMode(toNss, MODE_X));

    invariant(fromNss.db() == _name);
    invariant(toNss.db() == _name);
    if (getCollection(opCtx, toNss)) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "Cannot rename '" << fromNss << "' to '" << toNss
                                    << "' because the destination namespace already exists");
    }

    Collection* collToRename = getCollection(opCtx, fromNss);
    if (!collToRename) {
        return Status(ErrorCodes::NamespaceNotFound, "collection not found to rename");
    }
    invariant(!collToRename->getIndexCatalog()->haveAnyIndexesInProgress(),
              str::stream() << "cannot perform operation: an index build is currently running for "
                               "collection "
                            << fromNss);

    Collection* toColl = getCollection(opCtx, toNss);
    if (toColl) {
        invariant(
            !toColl->getIndexCatalog()->haveAnyIndexesInProgress(),
            str::stream() << "cannot perform operation: an index build is currently running for "
                             "collection "
                          << toNss);
    }

    log() << "renameCollection: renaming collection " << collToRename->uuid()->toString()
          << " from " << fromNss << " to " << toNss;

    Top::get(opCtx->getServiceContext()).collectionDropped(fromNss);

    auto storageEngine =
        checked_cast<KVStorageEngine*>(opCtx->getServiceContext()->getStorageEngine());
    Status status = storageEngine->getCatalog()->renameCollection(opCtx, fromNss, toNss, stayTemp);

    // Set the namespace of 'collToRename' from within the UUIDCatalog. This is necessary because
    // the UUIDCatalog mutex synchronizes concurrent access to the collection's namespace for
    // callers that may not hold a collection lock.
    UUIDCatalog::get(opCtx).setCollectionNamespace(opCtx, collToRename, fromNss, toNss);

    opCtx->recoveryUnit()->onCommit([collToRename](auto commitTime) {
        // Ban reading from this collection on committed reads on snapshots before now.
        if (commitTime) {
            collToRename->setMinimumVisibleSnapshot(commitTime.get());
        }
    });

    return status;
}

Collection* DatabaseImpl::getOrCreateCollection(OperationContext* opCtx,
                                                const NamespaceString& nss) const {
    Collection* c = getCollection(opCtx, nss);

    if (!c) {
        c = createCollection(opCtx, nss);
    }
    return c;
}

void DatabaseImpl::_checkCanCreateCollection(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionOptions& options) const {
    massert(17399,
            str::stream() << "Cannot create collection " << nss << " - collection already exists.",
            getCollection(opCtx, nss) == nullptr);
    uassertNamespaceNotIndex(nss.ns(), "createCollection");

    uassert(14037,
            "can't create user databases on a --configsvr instance",
            serverGlobalParams.clusterRole != ClusterRole::ConfigServer || nss.isOnInternalDb());

    // This check only applies for actual collections, not indexes or other types of ns.
    uassert(17381,
            str::stream() << "fully qualified namespace " << nss << " is too long "
                          << "(max is "
                          << NamespaceString::MaxNsCollectionLen
                          << " bytes)",
            !nss.isNormal() || nss.size() <= NamespaceString::MaxNsCollectionLen);

    uassert(17316, "cannot create a blank collection", nss.coll() > 0);
    uassert(28838, "cannot create a non-capped oplog collection", options.capped || !nss.isOplog());
    uassert(ErrorCodes::DatabaseDropPending,
            str::stream() << "Cannot create collection " << nss
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
    audit::logCreateCollection(&cc(), viewName.toString());

    if (viewName.isOplog())
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid namespace name for a view: " + viewName.toString());

    return ViewCatalog::get(this)->createView(
        opCtx, viewName, viewOnNss, BSONArray(options.pipeline), options.collation);
}

Collection* DatabaseImpl::createCollection(OperationContext* opCtx,
                                           const NamespaceString& nss,
                                           const CollectionOptions& options,
                                           bool createIdIndex,
                                           const BSONObj& idIndex) const {
    invariant(!options.isView());

    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_IX));

    uassert(CannotImplicitlyCreateCollectionInfo(nss),
            "request doesn't allow collection to be created implicitly",
            OperationShardingState::get(opCtx).allowImplicitCollectionCreation());

    auto coordinator = repl::ReplicationCoordinator::get(opCtx);
    bool canAcceptWrites =
        (coordinator->getReplicationMode() != repl::ReplicationCoordinator::modeReplSet) ||
        coordinator->canAcceptWritesForDatabase(opCtx, nss.db()) || nss.isSystemDotProfile();


    CollectionOptions optionsWithUUID = options;
    bool generatedUUID = false;
    if (!optionsWithUUID.uuid) {
        if (!canAcceptWrites) {
            std::string msg = str::stream() << "Attempted to create a new collection " << nss
                                            << " without a UUID";
            severe() << msg;
            uasserted(ErrorCodes::InvalidOptions, msg);
        } else {
            optionsWithUUID.uuid.emplace(CollectionUUID::gen());
            generatedUUID = true;
        }
    }

    // Because writing the oplog entry depends on having the full spec for the _id index, which is
    // not available until the collection is actually created, we can't write the oplog entry until
    // after we have created the collection.  In order to make the storage timestamp for the
    // collection create always correct even when other operations are present in the same storage
    // transaction, we reserve an opTime before the collection creation, then pass it to the
    // opObserver.  Reserving the optime automatically sets the storage timestamp.
    OplogSlot createOplogSlot;
    if (canAcceptWrites && supportsDocLocking() && !coordinator->isOplogDisabledFor(opCtx, nss)) {
        createOplogSlot = repl::getNextOpTime(opCtx);
    }

    _checkCanCreateCollection(opCtx, nss, optionsWithUUID);
    audit::logCreateCollection(&cc(), nss.ns());

    log() << "createCollection: " << nss << " with " << (generatedUUID ? "generated" : "provided")
          << " UUID: " << optionsWithUUID.uuid.get() << " and options: " << options.toBSON();

    // Create CollectionCatalogEntry
    auto storageEngine =
        checked_cast<KVStorageEngine*>(opCtx->getServiceContext()->getStorageEngine());
    massertStatusOK(storageEngine->getCatalog()->createCollection(
        opCtx, nss, optionsWithUUID, true /*allocateDefaultSpace*/));

    // Create Collection object
    auto& uuidCatalog = UUIDCatalog::get(opCtx);
    auto ownedCollection = _createCollectionInstance(opCtx, nss);
    Collection* collection = ownedCollection.get();
    uuidCatalog.onCreateCollection(opCtx, std::move(ownedCollection), *(collection->uuid()));
    opCtx->recoveryUnit()->onCommit([collection](auto commitTime) {
        // Ban reading from this collection on committed reads on snapshots before now.
        if (commitTime)
            collection->setMinimumVisibleSnapshot(commitTime.get());
    });


    BSONObj fullIdIndexSpec;

    if (createIdIndex) {
        if (collection->requiresIdIndex()) {
            if (optionsWithUUID.autoIndexId == CollectionOptions::YES ||
                optionsWithUUID.autoIndexId == CollectionOptions::DEFAULT) {
                // createCollection() may be called before the in-memory fCV parameter is
                // initialized, so use the unsafe fCV getter here.
                IndexCatalog* ic = collection->getIndexCatalog();
                fullIdIndexSpec = uassertStatusOK(ic->createIndexOnEmptyCollection(
                    opCtx, !idIndex.isEmpty() ? idIndex : ic->getDefaultIdIndexSpec()));
            } else {
                // autoIndexId: false is only allowed on unreplicated collections.
                uassert(50001,
                        str::stream() << "autoIndexId:false is not allowed for collection " << nss
                                      << " because it can be replicated",
                        !nss.isReplicated());
            }
        }
    }

    MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeLoggingCreateCollection);

    opCtx->getServiceContext()->getOpObserver()->onCreateCollection(
        opCtx, collection, nss, optionsWithUUID, fullIdIndexSpec, createOplogSlot);

    // It is necessary to create the system index *after* running the onCreateCollection so that
    // the storage timestamp for the index creation is after the storage timestamp for the
    // collection creation, and the opTimes for the corresponding oplog entries are the same as the
    // storage timestamps.  This way both primary and any secondaries will see the index created
    // after the collection is created.
    if (canAcceptWrites && createIdIndex && nss.isSystem()) {
        createSystemIndexes(opCtx, collection);
    }

    return collection;
}

StatusWith<NamespaceString> DatabaseImpl::makeUniqueCollectionNamespace(
    OperationContext* opCtx, StringData collectionNameModel) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    // There must be at least one percent sign within the first MaxNsCollectionLen characters of the
    // generated namespace after accounting for the database name prefix and dot separator:
    //     <db>.<truncated collection model name>
    auto maxModelLength = NamespaceString::MaxNsCollectionLen - (_name.length() + 1);
    auto model = collectionNameModel.substr(0, maxModelLength);
    auto numPercentSign = std::count(model.begin(), model.end(), '%');
    if (numPercentSign == 0) {
        return Status(ErrorCodes::FailedToParse,
                      str::stream() << "Cannot generate collection name for temporary collection: "
                                       "model for collection name "
                                    << collectionNameModel
                                    << " must contain at least one percent sign within first "
                                    << maxModelLength
                                    << " characters.");
    }

    if (!_uniqueCollectionNamespacePseudoRandom) {
        _uniqueCollectionNamespacePseudoRandom =
            std::make_unique<PseudoRandom>(Date_t::now().asInt64());
    }

    const auto charsToChooseFrom =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz"_sd;
    invariant((10U + 26U * 2) == charsToChooseFrom.size());

    auto replacePercentSign = [&, this](const auto& c) {
        if (c != '%') {
            return c;
        }
        auto i = _uniqueCollectionNamespacePseudoRandom->nextInt32(charsToChooseFrom.size());
        return charsToChooseFrom[i];
    };

    auto numGenerationAttempts = numPercentSign * charsToChooseFrom.size() * 100U;
    for (decltype(numGenerationAttempts) i = 0; i < numGenerationAttempts; ++i) {
        auto collectionName = model.toString();
        std::transform(collectionName.begin(),
                       collectionName.end(),
                       collectionName.begin(),
                       replacePercentSign);

        NamespaceString nss(_name, collectionName);
        if (!getCollection(opCtx, nss)) {
            return nss;
        }
    }

    return Status(
        ErrorCodes::NamespaceExists,
        str::stream() << "Cannot generate collection name for temporary collection with model "
                      << collectionNameModel
                      << " after "
                      << numGenerationAttempts
                      << " attempts due to namespace conflicts with existing collections.");
}

void DatabaseImpl::checkForIdIndexesAndDropPendingCollections(OperationContext* opCtx) const {
    if (name() == "local") {
        // Collections in the local database are not replicated, so we do not need an _id index on
        // any collection. For the same reason, it is not possible for the local database to contain
        // any drop-pending collections (drops are effective immediately).
        return;
    }

    for (const auto& nss : UUIDCatalog::get(opCtx).getAllCollectionNamesFromDb(opCtx, _name)) {
        if (nss.isDropPendingNamespace()) {
            auto dropOpTime = fassert(40459, nss.getDropPendingNamespaceOpTime());
            log() << "Found drop-pending namespace " << nss << " with drop optime " << dropOpTime;
            repl::DropPendingCollectionReaper::get(opCtx)->addDropPendingNamespace(dropOpTime, nss);
        }

        if (nss.isSystem())
            continue;

        Collection* coll = getCollection(opCtx, nss);
        if (!coll)
            continue;

        if (coll->getIndexCatalog()->findIdIndex(opCtx))
            continue;

        log() << "WARNING: the collection '" << nss << "' lacks a unique index on _id."
              << " This index is needed for replication to function properly" << startupWarningsLog;
        log() << "\t To fix this, you need to create a unique index on _id."
              << " See http://dochub.mongodb.org/core/build-replica-set-indexes"
              << startupWarningsLog;
    }
}

Status DatabaseImpl::userCreateNS(OperationContext* opCtx,
                                  const NamespaceString& nss,
                                  CollectionOptions collectionOptions,
                                  bool createDefaultIndexes,
                                  const BSONObj& idIndex) const {
    LOG(1) << "create collection " << nss << ' ' << collectionOptions.toBSON();

    if (!NamespaceString::validCollectionComponent(nss.ns()))
        return Status(ErrorCodes::InvalidNamespace, str::stream() << "invalid ns: " << nss);

    Collection* collection = getCollection(opCtx, nss);

    if (collection)
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a collection '" << nss << "' already exists");

    if (ViewCatalog::get(this)->lookup(opCtx, nss.ns()))
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a view '" << nss << "' already exists");

    // Validate the collation, if there is one.
    std::unique_ptr<CollatorInterface> collator;
    if (!collectionOptions.collation.isEmpty()) {
        auto collatorWithStatus = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(collectionOptions.collation);

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
        collectionOptions.collation = collator ? collator->getSpec().toBSON() : BSONObj();
    }

    if (!collectionOptions.validator.isEmpty()) {
        boost::intrusive_ptr<ExpressionContext> expCtx(
            new ExpressionContext(opCtx, collator.get()));

        // Save this to a variable to avoid reading the atomic variable multiple times.
        const auto currentFCV = serverGlobalParams.featureCompatibility.getVersion();

        // If the feature compatibility version is not 4.2, and we are validating features as
        // master, ban the use of new agg features introduced in 4.2 to prevent them from being
        // persisted in the catalog.
        if (serverGlobalParams.validateFeaturesAsMaster.load() &&
            currentFCV != ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo42) {
            expCtx->maxFeatureCompatibilityVersion = currentFCV;
        }
        auto statusWithMatcher =
            MatchExpressionParser::parse(collectionOptions.validator, std::move(expCtx));

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

    if (auto indexOptions = collectionOptions.indexOptionDefaults["storageEngine"]) {
        status = validateStorageOptions(
            opCtx->getServiceContext(), indexOptions.Obj(), [](const auto& x, const auto& y) {
                return x->validateIndexStorageOptions(y);
            });

        if (!status.isOK()) {
            return status;
        }
    }

    if (collectionOptions.isView()) {
        uassertStatusOK(createView(opCtx, nss, collectionOptions));
    } else {
        invariant(createCollection(opCtx, nss, collectionOptions, createDefaultIndexes, idIndex),
                  str::stream() << "Collection creation failed after validating options: " << nss
                                << ". Options: "
                                << collectionOptions.toBSON());
    }

    return Status::OK();
}

}  // namespace mongo
