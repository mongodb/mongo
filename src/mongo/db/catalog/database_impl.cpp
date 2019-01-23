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
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
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

/**
 * Move entry in 'map' from key 'from' to key 'to'.
 */
void renameKey(Database::CollectionMap* map, std::string from, std::string to) {
    invariant(!map->count(to), "from == " + from + ", to == " + to);
    auto it = map->find(from);
    invariant(it != map->end(), "from == " + from + " , to == " + to);
    auto val = std::move(it->second);
    map->erase(it);
    map->emplace(to, std::move(val));
}

std::unique_ptr<Collection> _createCollectionInstance(OperationContext* opCtx,
                                                      DatabaseCatalogEntry* dbEntry,
                                                      const NamespaceString& nss) {

    std::unique_ptr<CollectionCatalogEntry> cce(dbEntry->getCollectionCatalogEntry(nss.ns()));
    std::unique_ptr<RecordStore> rs(dbEntry->getRecordStore(nss.ns()));
    auto uuid = cce->getCollectionOptions(opCtx).uuid;
    invariant(rs,
              str::stream() << "Record store did not exist. Collection: " << nss.ns() << " UUID: "
                            << uuid);
    invariant(uuid, str::stream() << "Record store has no UUID. Collection: " << nss.ns());

    auto coll = std::make_unique<CollectionImpl>(
        opCtx, nss.ns(), uuid, cce.release(), rs.release(), dbEntry);
    // We are not in a WUOW only when we are called from Database::init(). There is no need
    // to rollback UUIDCatalog changes because we are initializing existing collections.
    auto&& uuidCatalog = UUIDCatalog::get(opCtx);
    if (!opCtx->lockState()->inAWriteUnitOfWork()) {
        uuidCatalog.registerUUIDCatalogEntry(uuid.get(), coll.get());
    } else {
        uuidCatalog.onCreateCollection(opCtx, coll.get(), uuid.get());
    }

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

void DatabaseImpl::close(OperationContext* opCtx) {
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

DatabaseImpl::DatabaseImpl(const StringData name, DatabaseCatalogEntry* dbEntry, uint64_t epoch)
    : _name(name.toString()),
      _dbEntry(dbEntry),
      _epoch(epoch),
      _profileName(_name + ".system.profile"),
      _viewsName(_name + "." + DurableViewCatalog::viewsCollectionName().toString()) {
    auto durableViewCatalog = std::make_unique<DurableViewCatalogImpl>(this);
    auto viewCatalog = std::make_unique<ViewCatalog>(std::move(durableViewCatalog));

    ViewCatalog::set(this, std::move(viewCatalog));
}

void DatabaseImpl::init(OperationContext* const opCtx) {
    Status status = validateDBName(_name);

    if (!status.isOK()) {
        warning() << "tried to open invalid db: " << _name;
        uasserted(10028, status.toString());
    }

    _profile = serverGlobalParams.defaultProfile;

    std::list<std::string> collections;
    _dbEntry->getCollectionNamespaces(&collections);

    invariant(_collections.empty());
    for (auto ns : collections) {
        NamespaceString nss(ns);
        auto ownedCollection = _createCollectionInstance(opCtx, _dbEntry, nss);
        Collection* collection = ownedCollection.get();
        invariant(collection);
        _collections[ns] = std::make_pair(std::move(ownedCollection), collection);
    }

    // At construction time of the viewCatalog, the _collections map wasn't initialized yet, so no
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

void DatabaseImpl::clearTmpCollections(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    std::list<std::string> collections;
    _dbEntry->getCollectionNamespaces(&collections);

    for (auto ns : collections) {
        invariant(NamespaceString::normal(ns));

        CollectionCatalogEntry* coll = _dbEntry->getCollectionCatalogEntry(ns);

        CollectionOptions options = coll->getCollectionOptions(opCtx);

        if (!options.temp)
            continue;
        try {
            WriteUnitOfWork wunit(opCtx);
            Status status = dropCollection(opCtx, ns, {});

            if (!status.isOK()) {
                warning() << "could not drop temp collection '" << ns << "': " << redact(status);
                continue;
            }

            wunit.commit();
        } catch (const WriteConflictException&) {
            warning() << "could not drop temp collection '" << ns << "' due to "
                                                                     "WriteConflictException";
            opCtx->recoveryUnit()->abandonSnapshot();
        }
    }
}

Status DatabaseImpl::setProfilingLevel(OperationContext* opCtx, int newLevel) {
    if (_profile == newLevel) {
        return Status::OK();
    }

    if (newLevel == 0) {
        _profile = 0;
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

    _profile = newLevel;

    return Status::OK();
}

void DatabaseImpl::setDropPending(OperationContext* opCtx, bool dropPending) {
    if (dropPending) {
        invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
        uassert(ErrorCodes::DatabaseDropPending,
                str::stream() << "Unable to drop database " << name()
                              << " because it is already in the process of being dropped.",
                !_dropPending);
        _dropPending = true;
    } else {
        invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_IX));
        _dropPending = false;
    }
}

bool DatabaseImpl::isDropPending(OperationContext* opCtx) const {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    return _dropPending;
}

void DatabaseImpl::getStats(OperationContext* opCtx, BSONObjBuilder* output, double scale) {

    long long nCollections = 0;
    long long nViews = 0;
    long long objects = 0;
    long long size = 0;
    long long storageSize = 0;
    long long numExtents = 0;
    long long indexes = 0;
    long long indexSize = 0;

    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_IS));
    std::list<std::string> collections;
    _dbEntry->getCollectionNamespaces(&collections);


    for (auto ns : collections) {
        Lock::CollectionLock colLock(opCtx->lockState(), ns, MODE_IS);
        Collection* collection = getCollection(opCtx, ns);

        if (!collection)
            continue;

        nCollections += 1;
        objects += collection->numRecords(opCtx);
        size += collection->dataSize(opCtx);

        BSONObjBuilder temp;
        storageSize += collection->getRecordStore()->storageSize(opCtx, &temp);
        numExtents += temp.obj()["numExtents"].numberInt();  // XXX

        indexes += collection->getIndexCatalog()->numIndexesTotal(opCtx);
        indexSize += collection->getIndexSize(opCtx);
    }

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

    _dbEntry->appendExtraStats(opCtx, output, scale);

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

Status DatabaseImpl::dropView(OperationContext* opCtx, StringData fullns) {
    auto views = ViewCatalog::get(this);
    Status status = views->dropView(opCtx, NamespaceString(fullns));
    Top::get(opCtx->getServiceContext()).collectionDropped(fullns);
    return status;
}

Status DatabaseImpl::dropCollection(OperationContext* opCtx,
                                    StringData fullns,
                                    repl::OpTime dropOpTime) {
    if (!getCollection(opCtx, fullns)) {
        // Collection doesn't exist so don't bother validating if it can be dropped.
        return Status::OK();
    }

    NamespaceString nss(fullns);
    {
        verify(nss.db() == _name);

        if (nss.isSystem()) {
            if (nss.isSystemDotProfile()) {
                if (_profile != 0)
                    return Status(ErrorCodes::IllegalOperation,
                                  "turn off profiling before dropping system.profile collection");
            } else if (!(nss.isSystemDotViews() || nss.isHealthlog() ||
                         nss == NamespaceString::kLogicalSessionsNamespace ||
                         nss == NamespaceString::kSystemKeysNamespace)) {
                return Status(ErrorCodes::IllegalOperation,
                              str::stream() << "can't drop system collection " << fullns);
            }
        }
    }

    return dropCollectionEvenIfSystem(opCtx, nss, dropOpTime);
}

Status DatabaseImpl::dropCollectionEvenIfSystem(OperationContext* opCtx,
                                                const NamespaceString& fullns,
                                                repl::OpTime dropOpTime) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    LOG(1) << "dropCollection: " << fullns;

    // A valid 'dropOpTime' is not allowed when writes are replicated.
    if (!dropOpTime.isNull() && opCtx->writesAreReplicated()) {
        return Status(
            ErrorCodes::BadValue,
            "dropCollection() cannot accept a valid drop optime when writes are replicated.");
    }

    Collection* collection = getCollection(opCtx, fullns);

    if (!collection) {
        return Status::OK();  // Post condition already met.
    }

    auto numRecords = collection->numRecords(opCtx);

    auto uuid = collection->uuid();
    auto uuidString = uuid ? uuid.get().toString() : "no UUID";

    uassertNamespaceNotIndex(fullns.toString(), "dropCollection");

    BackgroundOperation::assertNoBgOpInProgForNs(fullns);

    // Make sure no indexes builds are in progress.
    // Use massert() to be consistent with IndexCatalog::dropAllIndexes().
    auto numIndexesInProgress = collection->getIndexCatalog()->numIndexesInProgress(opCtx);
    massert(40461,
            str::stream() << "cannot drop collection " << fullns << " (" << uuidString << ") when "
                          << numIndexesInProgress
                          << " index builds in progress.",
            numIndexesInProgress == 0);

    audit::logDropCollection(&cc(), fullns.toString());

    auto serviceContext = opCtx->getServiceContext();
    Top::get(serviceContext).collectionDropped(fullns.toString());

    // Drop unreplicated collections immediately.
    // If 'dropOpTime' is provided, we should proceed to rename the collection.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto opObserver = serviceContext->getOpObserver();
    auto isOplogDisabledForNamespace = replCoord->isOplogDisabledFor(opCtx, fullns);
    if (dropOpTime.isNull() && isOplogDisabledForNamespace) {
        auto status = _finishDropCollection(opCtx, fullns, collection);
        if (!status.isOK()) {
            return status;
        }
        opObserver->onDropCollection(
            opCtx, fullns, uuid, numRecords, OpObserver::CollectionDropType::kOnePhase);
        return Status::OK();
    }

    // Starting in 4.2, pending collection drops will be maintained in the storage engine and will
    // no longer be visible at the catalog layer with 3.6-style <db>.system.drop.* namespaces.
    auto supportsPendingDrops = serviceContext->getStorageEngine()->supportsPendingDrops();
    auto collectionDropType = supportsPendingDrops ? OpObserver::CollectionDropType::kOnePhase
                                                   : OpObserver::CollectionDropType::kTwoPhase;

    // Replicated collections will be renamed with a special drop-pending namespace and dropped when
    // the replica set optime reaches the drop optime.
    if (dropOpTime.isNull()) {
        // MMAPv1 requires that index namespaces are subject to the same length constraints as
        // indexes in collections that are not in a drop-pending state. Therefore, we check if the
        // drop-pending namespace is too long for any index names in the collection.
        // These indexes are dropped regardless of the storage engine on the current node because we
        // may still have nodes running MMAPv1 in the replica set.

        // Compile a list of any indexes that would become too long following the drop-pending
        // rename. In the case that this collection drop gets rolled back, this will incur a
        // performance hit, since those indexes will have to be rebuilt from scratch, but data
        // integrity is maintained.
        std::vector<const IndexDescriptor*> indexesToDrop;
        auto indexIter = collection->getIndexCatalog()->getIndexIterator(opCtx, true);

        // Determine which index names are too long. Since we don't have the collection drop optime
        // at this time, use the maximum optime to check the index names.
        auto longDpns = fullns.makeDropPendingNamespace(repl::OpTime::max());
        while (indexIter->more()) {
            auto index = indexIter->next()->descriptor();
            auto status = longDpns.checkLengthForRename(index->indexName().size());
            if (!status.isOK()) {
                indexesToDrop.push_back(index);
            }
        }

        // Drop the offending indexes.
        for (auto&& index : indexesToDrop) {
            log() << "dropCollection: " << fullns << " (" << uuidString << ") - index namespace '"
                  << index->indexNamespace()
                  << "' would be too long after drop-pending rename. Dropping index immediately.";
            // Log the operation before the drop so that each drop is timestamped at the same time
            // as the oplog entry.
            opObserver->onDropIndex(
                opCtx, fullns, collection->uuid(), index->indexName(), index->infoObj());
            fassert(40463, collection->getIndexCatalog()->dropIndex(opCtx, index));
        }

        // Log oplog entry for collection drop and proceed to complete rest of two phase drop
        // process.
        dropOpTime =
            opObserver->onDropCollection(opCtx, fullns, uuid, numRecords, collectionDropType);

        // The OpObserver should have written an entry to the oplog with a particular op time.
        // After writing the oplog entry, all errors are fatal. See getNextOpTime() comments in
        // oplog.cpp.
        if (dropOpTime.isNull()) {
            log() << "dropCollection: " << fullns << " (" << uuidString
                  << ") - expected oplog entry to be written";
            fassertFailed(40462);
        }
    } else {
        // If we are provided with a valid 'dropOpTime', it means we are dropping this collection
        // in the context of applying an oplog entry on a secondary.
        // OpObserver::onDropCollection() should be returning a null OpTime because we should not be
        // writing to the oplog.
        auto opTime =
            opObserver->onDropCollection(opCtx, fullns, uuid, numRecords, collectionDropType);
        if (!opTime.isNull()) {
            severe() << "dropCollection: " << fullns << " (" << uuidString
                     << ") - unexpected oplog entry written to the oplog with optime " << opTime;
            fassertFailed(40468);
        }
    }

    if (supportsPendingDrops) {
        auto commitTimestamp = opCtx->recoveryUnit()->getCommitTimestamp();
        log() << "dropCollection: " << fullns << " (" << uuidString
              << ") - storage engine will take ownership of drop-pending collection with optime "
              << dropOpTime << " and commit timestamp " << commitTimestamp;
        return _finishDropCollection(opCtx, fullns, collection);
    }

    auto dpns = fullns.makeDropPendingNamespace(dropOpTime);

    // Rename collection using drop-pending namespace generated from drop optime.
    const bool stayTemp = true;
    log() << "dropCollection: " << fullns << " (" << uuidString
          << ") - renaming to drop-pending collection: " << dpns << " with drop optime "
          << dropOpTime;
    fassert(40464, renameCollection(opCtx, fullns.ns(), dpns.ns(), stayTemp));

    // Register this drop-pending namespace with DropPendingCollectionReaper to remove when the
    // committed optime reaches the drop optime.
    repl::DropPendingCollectionReaper::get(opCtx)->addDropPendingNamespace(dropOpTime, dpns);

    return Status::OK();
}

class DatabaseImpl::FinishDropChange : public RecoveryUnit::Change {
public:
    FinishDropChange(CollectionMap* map, std::string key, CollectionMap::mapped_type value)
        : _map(map), _key(key), _value(std::move(value)) {}
    void commit(boost::optional<Timestamp>) override {
        _value.first.reset();
    }
    void rollback() override {
        _map->emplace(_key, std::move(_value));
    }
    CollectionMap* _map;
    std::string _key;
    CollectionMap::mapped_type _value;
};

Status DatabaseImpl::_finishDropCollection(OperationContext* opCtx,
                                           const NamespaceString& fullns,
                                           Collection* collection) {
    invariant(_name == fullns.db());
    LOG(1) << "dropCollection: " << fullns << " - dropAllIndexes start";
    collection->getIndexCatalog()->dropAllIndexes(opCtx, true);

    invariant(collection->getCatalogEntry()->getTotalIndexCount(opCtx) == 0);
    LOG(1) << "dropCollection: " << fullns << " - dropAllIndexes done";

    // We want to destroy the Collection object before telling the StorageEngine to destroy the
    // RecordStore.
    auto it = _collections.find(fullns.toString());

    if (it != _collections.end()) {
        // Takes ownership of the collection
        auto value = std::move(it->second);
        _collections.erase(it);
        // Register a Change to reinstall the Collection* in the collections map on rollback.
        opCtx->recoveryUnit()->registerChange(
            new FinishDropChange(&_collections, fullns.toString(), std::move(value)));
    }

    auto uuid = collection->uuid();
    auto uuidString = uuid ? uuid.get().toString() : "no UUID";
    log() << "Finishing collection drop for " << fullns << " (" << uuidString << ").";

    return _dbEntry->dropCollection(opCtx, fullns.toString());
}

Collection* DatabaseImpl::getCollection(OperationContext* opCtx, StringData ns) const {
    NamespaceString nss(ns);
    invariant(_name == nss.db());
    return getCollection(opCtx, nss);
}

Collection* DatabaseImpl::getCollection(OperationContext* opCtx, const NamespaceString& nss) const {
    dassert(!cc().getOperationContext() || opCtx == cc().getOperationContext());
    auto coll = UUIDCatalog::get(opCtx).lookupCollectionByNamespace(nss);
    if (!coll) {
        return nullptr;
    }

    NamespaceUUIDCache& cache = NamespaceUUIDCache::get(opCtx);
    auto uuid = coll->uuid();
    invariant(uuid);
    cache.ensureNamespaceInCache(nss, uuid.get());
    return coll;
}

Status DatabaseImpl::renameCollection(OperationContext* opCtx,
                                      StringData fromNS,
                                      StringData toNS,
                                      bool stayTemp) {
    audit::logRenameCollection(&cc(), fromNS, toNS);
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    BackgroundOperation::assertNoBgOpInProgForNs(fromNS);
    BackgroundOperation::assertNoBgOpInProgForNs(toNS);

    const NamespaceString fromNSS(fromNS);
    const NamespaceString toNSS(toNS);

    invariant(fromNSS.db() == _name);
    invariant(toNSS.db() == _name);
    if (getCollection(opCtx, toNSS)) {
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "Cannot rename '" << fromNS << "' to '" << toNS
                                    << "' because the destination namespace already exists");
    }

    Collection* collToRename = getCollection(opCtx, fromNSS);
    if (!collToRename) {
        return Status(ErrorCodes::NamespaceNotFound, "collection not found to rename");
    }

    log() << "renameCollection: renaming collection " << collToRename->uuid()->toString()
          << " from " << fromNS << " to " << toNS;

    Top::get(opCtx->getServiceContext()).collectionDropped(fromNS.toString());

    Status status = _dbEntry->renameCollection(opCtx, fromNS, toNS, stayTemp);
    // Make 'toNS' map to the collection instead of 'fromNS'.
    renameKey(&_collections, fromNSS.ns(), toNSS.ns());

    // Set the namespace of 'collToRename' from within the UUIDCatalog. This is necessary because
    // the UUIDCatalog mutex synchronizes concurrent access to the collection's namespace for
    // callers that may not hold a collection lock.
    UUIDCatalog::get(opCtx).setCollectionNamespace(opCtx, collToRename, fromNSS, toNSS);

    opCtx->recoveryUnit()->onCommit([collToRename](auto commitTime) {
        // Ban reading from this collection on committed reads on snapshots before now.
        if (commitTime) {
            collToRename->setMinimumVisibleSnapshot(commitTime.get());
        }
    });

    // Register a rollback handler, will reinstall the Collection* in the collections map
    // so that it is associated with 'fromNS', not 'toNS'.
    opCtx->recoveryUnit()->onRollback(
        [ map = &_collections, fromNSS, toNSS ]() { renameKey(map, toNSS.ns(), fromNSS.ns()); });

    return status;
}

Collection* DatabaseImpl::getOrCreateCollection(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    Collection* c = getCollection(opCtx, nss);

    if (!c) {
        c = createCollection(opCtx, nss.ns());
    }
    return c;
}

void DatabaseImpl::_checkCanCreateCollection(OperationContext* opCtx,
                                             const NamespaceString& nss,
                                             const CollectionOptions& options) {
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
            !_dropPending);
}

Status DatabaseImpl::createView(OperationContext* opCtx,
                                StringData ns,
                                const CollectionOptions& options) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    invariant(options.isView());

    NamespaceString nss(ns);
    NamespaceString viewOnNss(nss.db(), options.viewOn);
    _checkCanCreateCollection(opCtx, nss, options);
    audit::logCreateCollection(&cc(), ns);

    if (nss.isOplog())
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid namespace name for a view: " + nss.toString());

    auto views = ViewCatalog::get(this);
    return views->createView(opCtx, nss, viewOnNss, BSONArray(options.pipeline), options.collation);
}

Collection* DatabaseImpl::createCollection(OperationContext* opCtx,
                                           StringData ns,
                                           const CollectionOptions& options,
                                           bool createIdIndex,
                                           const BSONObj& idIndex) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    invariant(!options.isView());
    NamespaceString nss(ns);

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
        }
        if (canAcceptWrites) {
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
    audit::logCreateCollection(&cc(), ns);

    if (optionsWithUUID.uuid) {
        log() << "createCollection: " << ns << " with "
              << (generatedUUID ? "generated" : "provided")
              << " UUID: " << optionsWithUUID.uuid.get();
    } else {
        log() << "createCollection: " << ns << " with no UUID.";
    }

    massertStatusOK(
        _dbEntry->createCollection(opCtx, nss, optionsWithUUID, true /*allocateDefaultSpace*/));

    opCtx->recoveryUnit()->onRollback([ db = this, nss ]() { db->_collections.erase(nss.ns()); });


    invariant(!_collections.count(ns));
    auto ownedCollection = _createCollectionInstance(opCtx, _dbEntry, nss);
    Collection* collection = ownedCollection.get();
    invariant(collection);
    _collections[ns] = std::make_pair(std::move(ownedCollection), collection);
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

const DatabaseCatalogEntry* DatabaseImpl::getDatabaseCatalogEntry() const {
    return _dbEntry;
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

void DatabaseImpl::checkForIdIndexesAndDropPendingCollections(OperationContext* opCtx) {
    if (name() == "local") {
        // Collections in the local database are not replicated, so we do not need an _id index on
        // any collection. For the same reason, it is not possible for the local database to contain
        // any drop-pending collections (drops are effective immediately).
        return;
    }

    std::list<std::string> collectionNames;
    getDatabaseCatalogEntry()->getCollectionNamespaces(&collectionNames);

    for (const auto& collectionName : collectionNames) {
        const NamespaceString ns(collectionName);

        if (ns.isDropPendingNamespace()) {
            auto dropOpTime = fassert(40459, ns.getDropPendingNamespaceOpTime());
            log() << "Found drop-pending namespace " << ns << " with drop optime " << dropOpTime;
            repl::DropPendingCollectionReaper::get(opCtx)->addDropPendingNamespace(dropOpTime, ns);
        }

        if (ns.isSystem())
            continue;

        Collection* coll = getCollection(opCtx, collectionName);
        if (!coll)
            continue;

        if (coll->getIndexCatalog()->findIdIndex(opCtx))
            continue;

        log() << "WARNING: the collection '" << collectionName << "' lacks a unique index on _id."
              << " This index is needed for replication to function properly" << startupWarningsLog;
        log() << "\t To fix this, you need to create a unique index on _id."
              << " See http://dochub.mongodb.org/core/build-replica-set-indexes"
              << startupWarningsLog;
    }
}

Status DatabaseImpl::userCreateNS(OperationContext* opCtx,
                                  const NamespaceString& fullns,
                                  CollectionOptions collectionOptions,
                                  bool createDefaultIndexes,
                                  const BSONObj& idIndex) {
    LOG(1) << "create collection " << fullns << ' ' << collectionOptions.toBSON();

    if (!NamespaceString::validCollectionComponent(fullns.ns()))
        return Status(ErrorCodes::InvalidNamespace, str::stream() << "invalid ns: " << fullns);

    Collection* collection = getCollection(opCtx, fullns);

    if (collection)
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a collection '" << fullns << "' already exists");

    if (ViewCatalog::get(this)->lookup(opCtx, fullns.ns()))
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a view '" << fullns << "' already exists");

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
        uassertStatusOK(createView(opCtx, fullns.ns(), collectionOptions));
    } else {
        invariant(
            createCollection(opCtx, fullns.ns(), collectionOptions, createDefaultIndexes, idIndex),
            str::stream() << "Collection creation failed after validating options: " << fullns
                          << ". Options: "
                          << collectionOptions.toBSON());
    }

    return Status::OK();
}

}  // namespace mongo
