/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include <boost/filesystem/operations.hpp>
#include <memory>

#include "mongo/base/init.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/drop_indexes.h"
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
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/system_index.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {
namespace {
MONGO_INITIALIZER(InitializeDatabaseFactory)(InitializerContext* const) {
    Database::registerFactory([](Database* const this_,
                                 OperationContext* const opCtx,
                                 const StringData name,
                                 DatabaseCatalogEntry* const dbEntry) {
        return stdx::make_unique<DatabaseImpl>(this_, opCtx, name, dbEntry);
    });
    return Status::OK();
}
}  // namespace

using std::unique_ptr;
using std::endl;
using std::list;
using std::set;
using std::string;
using std::stringstream;
using std::vector;

void massertNamespaceNotIndex(StringData ns, StringData caller) {
    massert(17320,
            str::stream() << "cannot do " << caller << " on namespace with a $ in it: " << ns,
            NamespaceString::normal(ns));
}

class DatabaseImpl::AddCollectionChange : public RecoveryUnit::Change {
public:
    AddCollectionChange(OperationContext* opCtx, DatabaseImpl* db, StringData ns)
        : _opCtx(opCtx), _db(db), _ns(ns.toString()) {}

    virtual void commit() {
        CollectionMap::const_iterator it = _db->_collections.find(_ns);

        if (it == _db->_collections.end())
            return;

        // Ban reading from this collection on committed reads on snapshots before now.
        auto replCoord = repl::ReplicationCoordinator::get(_opCtx);
        auto snapshotName = replCoord->reserveSnapshotName(_opCtx);
        replCoord->forceSnapshotCreation();  // Ensures a newer snapshot gets created even if idle.
        it->second->setMinimumVisibleSnapshot(snapshotName);
    }

    virtual void rollback() {
        CollectionMap::const_iterator it = _db->_collections.find(_ns);

        if (it == _db->_collections.end())
            return;

        delete it->second;
        _db->_collections.erase(it);
    }

    OperationContext* const _opCtx;
    DatabaseImpl* const _db;
    const std::string _ns;
};

class DatabaseImpl::RemoveCollectionChange : public RecoveryUnit::Change {
public:
    // Takes ownership of coll (but not db).
    RemoveCollectionChange(DatabaseImpl* db, Collection* coll) : _db(db), _coll(coll) {}

    virtual void commit() {
        delete _coll;
    }

    virtual void rollback() {
        Collection*& inMap = _db->_collections[_coll->ns().ns()];
        invariant(!inMap);
        inMap = _coll;
    }

    DatabaseImpl* const _db;
    Collection* const _coll;
};

DatabaseImpl::~DatabaseImpl() {
    for (CollectionMap::const_iterator i = _collections.begin(); i != _collections.end(); ++i)
        delete i->second;
}

void DatabaseImpl::close(OperationContext* opCtx, const std::string& reason) {
    // XXX? - Do we need to close database under global lock or just DB-lock is sufficient ?
    invariant(opCtx->lockState()->isW());

    // oplog caches some things, dirty its caches
    repl::oplogCheckCloseDatabase(opCtx, this->_this);

    if (BackgroundOperation::inProgForDb(_name)) {
        log() << "warning: bg op in prog during close db? " << _name;
    }

    for (auto&& pair : _collections) {
        auto* coll = pair.second;
        coll->getCursorManager()->invalidateAll(opCtx, true, reason);
    }
}

Status DatabaseImpl::validateDBName(StringData dbname) {
    if (dbname.size() <= 0)
        return Status(ErrorCodes::BadValue, "db name is empty");

    if (dbname.size() >= 64)
        return Status(ErrorCodes::BadValue, "db name is too long");

    if (dbname.find('.') != string::npos)
        return Status(ErrorCodes::BadValue, "db name cannot contain a .");

    if (dbname.find(' ') != string::npos)
        return Status(ErrorCodes::BadValue, "db name cannot contain a space");

#ifdef _WIN32
    static const char* windowsReservedNames[] = {
        "con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
        "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};

    string lower(dbname.toString());
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

    for (size_t i = 0; i < (sizeof(windowsReservedNames) / sizeof(char*)); ++i) {
        if (lower == windowsReservedNames[i]) {
            stringstream errorString;
            errorString << "db name \"" << dbname.toString() << "\" is a reserved name";
            return Status(ErrorCodes::BadValue, errorString.str());
        }
    }
#endif

    return Status::OK();
}

Collection* DatabaseImpl::_getOrCreateCollectionInstance(OperationContext* opCtx,
                                                         const NamespaceString& nss) {
    Collection* collection = getCollection(opCtx, nss);

    if (collection) {
        return collection;
    }

    unique_ptr<CollectionCatalogEntry> cce(_dbEntry->getCollectionCatalogEntry(nss.ns()));
    auto uuid = cce->getCollectionOptions(opCtx).uuid;

    unique_ptr<RecordStore> rs(_dbEntry->getRecordStore(nss.ns()));
    invariant(rs.get());  // if cce exists, so should this

    // Not registering AddCollectionChange since this is for collections that already exist.
    Collection* coll = new Collection(opCtx, nss.ns(), uuid, cce.release(), rs.release(), _dbEntry);
    if (uuid) {
        // We are not in a WUOW only when we are called from Database::init(). There is no need
        // to rollback UUIDCatalog changes because we are initializing existing collections.
        auto&& uuidCatalog = UUIDCatalog::get(opCtx);
        if (!opCtx->lockState()->inAWriteUnitOfWork()) {
            uuidCatalog.registerUUIDCatalogEntry(uuid.get(), coll);
        } else {
            uuidCatalog.onCreateCollection(opCtx, coll, uuid.get());
        }
    }

    return coll;
}

DatabaseImpl::DatabaseImpl(Database* const this_,
                           OperationContext* const opCtx,
                           const StringData name,
                           DatabaseCatalogEntry* const dbEntry)
    : _name(name.toString()),
      _dbEntry(dbEntry),
      _profileName(_name + ".system.profile"),
      _indexesName(_name + ".system.indexes"),
      _viewsName(_name + "." + DurableViewCatalog::viewsCollectionName().toString()),
      _durableViews(DurableViewCatalogImpl(this_)),
      _views(&_durableViews),
      _this(this_) {}

void DatabaseImpl::init(OperationContext* const opCtx) {
    Status status = validateDBName(_name);

    if (!status.isOK()) {
        warning() << "tried to open invalid db: " << _name;
        uasserted(10028, status.toString());
    }

    _profile = serverGlobalParams.defaultProfile;

    list<string> collections;
    _dbEntry->getCollectionNamespaces(&collections);

    for (list<string>::const_iterator it = collections.begin(); it != collections.end(); ++it) {
        const string ns = *it;
        NamespaceString nss(ns);
        _collections[ns] = _getOrCreateCollectionInstance(opCtx, nss);
    }

    // At construction time of the viewCatalog, the _collections map wasn't initialized yet, so no
    // system.views collection would be found. Now we're sufficiently initialized, signal a version
    // change. Also force a reload, so if there are problems with the catalog contents as might be
    // caused by incorrect mongod versions or similar, they are found right away.
    _views.invalidate();
    Status reloadStatus = _views.reloadIfNeeded(opCtx);

    if (!reloadStatus.isOK()) {
        warning() << "Unable to parse views: " << redact(reloadStatus)
                  << "; remove any invalid views from the " << _viewsName
                  << " collection to restore server functionality." << startupWarningsLog;
    }
}

void DatabaseImpl::clearTmpCollections(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    list<string> collections;
    _dbEntry->getCollectionNamespaces(&collections);

    for (list<string>::iterator i = collections.begin(); i != collections.end(); ++i) {
        string ns = *i;
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
        } catch (const WriteConflictException& exp) {
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

    Status status = createProfileCollection(opCtx, this->_this);

    if (!status.isOK()) {
        return status;
    }

    _profile = newLevel;

    return Status::OK();
}

void DatabaseImpl::setDropPending(OperationContext* opCtx, bool dropPending) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    if (dropPending) {
        uassert(ErrorCodes::DatabaseDropPending,
                str::stream() << "Unable to drop database " << name()
                              << " because it is already in the process of being dropped.",
                !_dropPending);
        _dropPending = true;
    } else {
        _dropPending = false;
    }
}

bool DatabaseImpl::isDropPending(OperationContext* opCtx) const {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    return _dropPending;
}

void DatabaseImpl::getStats(OperationContext* opCtx, BSONObjBuilder* output, double scale) {
    list<string> collections;
    _dbEntry->getCollectionNamespaces(&collections);

    long long nCollections = 0;
    long long nViews = 0;
    long long objects = 0;
    long long size = 0;
    long long storageSize = 0;
    long long numExtents = 0;
    long long indexes = 0;
    long long indexSize = 0;

    for (list<string>::const_iterator it = collections.begin(); it != collections.end(); ++it) {
        const string ns = *it;

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

    getViewCatalog()->iterate(opCtx, [&](const ViewDefinition& view) { nViews += 1; });

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
}

Status DatabaseImpl::dropView(OperationContext* opCtx, StringData fullns) {
    Status status = _views.dropView(opCtx, NamespaceString(fullns));
    Top::get(opCtx->getClient()->getServiceContext()).collectionDropped(fullns);
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
            } else if (!nss.isSystemDotViews()) {
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

    massertNamespaceNotIndex(fullns.toString(), "dropCollection");

    BackgroundOperation::assertNoBgOpInProgForNs(fullns);

    // Make sure no indexes builds are in progress.
    // Use massert() to be consistent with IndexCatalog::dropAllIndexes().
    auto numIndexesInProgress = collection->getIndexCatalog()->numIndexesInProgress(opCtx);
    massert(40461,
            str::stream() << "cannot drop collection " << fullns.ns() << " when "
                          << numIndexesInProgress
                          << " index builds in progress.",
            numIndexesInProgress == 0);

    audit::logDropCollection(&cc(), fullns.toString());

    Top::get(opCtx->getClient()->getServiceContext()).collectionDropped(fullns.toString());

    auto uuid = collection->uuid();

    // Drop unreplicated collections immediately.
    // If 'dropOpTime' is provided, we should proceed to rename the collection.
    // Under master/slave, collections are always dropped immediately. This is because drop-pending
    // collections support the rollback process which is not applicable to master/slave.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto opObserver = getGlobalServiceContext()->getOpObserver();
    auto isOplogDisabledForNamespace = replCoord->isOplogDisabledFor(opCtx, fullns);
    auto isMasterSlave =
        repl::ReplicationCoordinator::modeMasterSlave == replCoord->getReplicationMode();
    if ((dropOpTime.isNull() && isOplogDisabledForNamespace) || isMasterSlave) {
        auto status = _finishDropCollection(opCtx, fullns, collection);
        if (!status.isOK()) {
            return status;
        }
        opObserver->onDropCollection(opCtx, fullns, uuid);
        return Status::OK();
    }

    // Replicated collections will be renamed with a special drop-pending namespace and dropped when
    // the replica set optime reaches the drop optime.
    if (dropOpTime.isNull()) {
        dropOpTime = opObserver->onDropCollection(opCtx, fullns, uuid);

        // Drop collection immediately if OpObserver did not write entry to oplog.
        // After writing the oplog entry, all errors are fatal. See getNextOpTime() comments in
        // oplog.cpp.
        if (dropOpTime.isNull()) {
            log() << "dropCollection: " << fullns
                  << " - no drop optime available for pending-drop. "
                  << "Dropping collection immediately.";
            fassertStatusOK(40462, _finishDropCollection(opCtx, fullns, collection));
            return Status::OK();
        }
    } else {
        // If we are provided with a valid 'dropOpTime', it means we are dropping this collection
        // in the context of applying an oplog entry on a secondary.
        // OpObserver::onDropCollection() should be returning a null OpTime because we should not be
        // writing to the oplog.
        auto opTime = opObserver->onDropCollection(opCtx, fullns, uuid);
        if (!opTime.isNull()) {
            severe() << "dropCollection: " << fullns
                     << " - unexpected oplog entry written to the oplog with optime " << opTime;
            fassertFailed(40468);
        }
    }

    auto dpns = fullns.makeDropPendingNamespace(dropOpTime);

    // MMAPv1 requires that index namespaces are subject to the same length constraints as indexes
    // in collections that are not in a drop-pending state. Therefore, we check if the drop-pending
    // namespace is too long for any index names in the collection.
    if (opCtx->getServiceContext()->getGlobalStorageEngine()->isMmapV1()) {

        // Compile a list of any indexes that would become too long following the drop-pending
        // rename. In the case that this collection drop gets rolled back, this will incur a
        // performance hit, since those indexes will have to be rebuilt from scratch, but data
        // integrity is maintained.
        std::vector<IndexDescriptor*> indexesToDrop;
        auto indexIter = collection->getIndexCatalog()->getIndexIterator(opCtx, true);

        // Determine which index names are too long.
        while (indexIter.more()) {
            auto index = indexIter.next();
            auto status = dpns.checkLengthForRename(index->indexName().size());
            if (!status.isOK()) {
                indexesToDrop.push_back(index);
            }
        }

        // Drop the offending indexes.
        for (auto&& index : indexesToDrop) {
            log() << "dropCollection: " << fullns << " - index namespace '"
                  << index->indexNamespace()
                  << "' would be too long after drop-pending rename. Dropping index immediately.";
            fassertStatusOK(40463, collection->getIndexCatalog()->dropIndex(opCtx, index));
            opObserver->onDropIndex(
                opCtx, fullns, collection->uuid(), index->indexName(), index->infoObj());
        }
    }


    // Rename collection using drop-pending namespace generated from drop optime.
    const bool stayTemp = true;
    log() << "dropCollection: " << fullns << " - renaming to drop-pending collection: " << dpns
          << " with drop optime " << dropOpTime;
    fassertStatusOK(40464, renameCollection(opCtx, fullns.ns(), dpns.ns(), stayTemp));

    // Register this drop-pending namespace with DropPendingCollectionReaper to remove when the
    // committed optime reaches the drop optime.
    repl::DropPendingCollectionReaper::get(opCtx)->addDropPendingNamespace(dropOpTime, dpns);

    return Status::OK();
}

Status DatabaseImpl::_finishDropCollection(OperationContext* opCtx,
                                           const NamespaceString& fullns,
                                           Collection* collection) {
    LOG(1) << "dropCollection: " << fullns << " - dropAllIndexes start";
    collection->getIndexCatalog()->dropAllIndexes(opCtx, true);

    invariant(collection->getCatalogEntry()->getTotalIndexCount(opCtx) == 0);
    LOG(1) << "dropCollection: " << fullns << " - dropAllIndexes done";

    // We want to destroy the Collection object before telling the StorageEngine to destroy the
    // RecordStore.
    _clearCollectionCache(
        opCtx, fullns.toString(), "collection dropped", /*collectionGoingAway*/ true);

    return _dbEntry->dropCollection(opCtx, fullns.toString());
}

void DatabaseImpl::_clearCollectionCache(OperationContext* opCtx,
                                         StringData fullns,
                                         const std::string& reason,
                                         bool collectionGoingAway) {
    verify(_name == nsToDatabaseSubstring(fullns));
    CollectionMap::const_iterator it = _collections.find(fullns.toString());

    if (it == _collections.end())
        return;

    // Takes ownership of the collection
    opCtx->recoveryUnit()->registerChange(new RemoveCollectionChange(this, it->second));

    it->second->getCursorManager()->invalidateAll(opCtx, collectionGoingAway, reason);
    _collections.erase(it);
}

Collection* DatabaseImpl::getCollection(OperationContext* opCtx, StringData ns) const {
    NamespaceString nss(ns);
    invariant(_name == nss.db());
    return getCollection(opCtx, nss);
}

Collection* DatabaseImpl::getCollection(OperationContext* opCtx, const NamespaceString& nss) const {
    dassert(!cc().getOperationContext() || opCtx == cc().getOperationContext());
    CollectionMap::const_iterator it = _collections.find(nss.ns());

    if (it != _collections.end() && it->second) {
        Collection* found = it->second;
        if (enableCollectionUUIDs) {
            NamespaceUUIDCache& cache = NamespaceUUIDCache::get(opCtx);
            if (auto uuid = found->uuid())
                cache.ensureNamespaceInCache(nss, uuid.get());
        }
        return found;
    }

    return NULL;
}

Status DatabaseImpl::renameCollection(OperationContext* opCtx,
                                      StringData fromNS,
                                      StringData toNS,
                                      bool stayTemp) {
    audit::logRenameCollection(&cc(), fromNS, toNS);
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    BackgroundOperation::assertNoBgOpInProgForNs(fromNS);
    BackgroundOperation::assertNoBgOpInProgForNs(toNS);

    NamespaceString fromNSS(fromNS);
    NamespaceString toNSS(toNS);
    {  // remove anything cached
        Collection* coll = getCollection(opCtx, fromNS);

        if (!coll)
            return Status(ErrorCodes::NamespaceNotFound, "collection not found to rename");

        string clearCacheReason = str::stream() << "renamed collection '" << fromNS << "' to '"
                                                << toNS << "'";
        IndexCatalog::IndexIterator ii = coll->getIndexCatalog()->getIndexIterator(opCtx, true);

        while (ii.more()) {
            IndexDescriptor* desc = ii.next();
            _clearCollectionCache(
                opCtx, desc->indexNamespace(), clearCacheReason, /*collectionGoingAway*/ true);
        }

        _clearCollectionCache(opCtx, fromNS, clearCacheReason, /*collectionGoingAway*/ true);
        _clearCollectionCache(opCtx, toNS, clearCacheReason, /*collectionGoingAway*/ false);

        Top::get(opCtx->getClient()->getServiceContext()).collectionDropped(fromNS.toString());
    }

    opCtx->recoveryUnit()->registerChange(new AddCollectionChange(opCtx, this, toNS));
    Status s = _dbEntry->renameCollection(opCtx, fromNS, toNS, stayTemp);
    _collections[toNS] = _getOrCreateCollectionInstance(opCtx, toNSS);

    return s;
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
            str::stream() << "Cannot create collection " << nss.ns()
                          << " - collection already exists.",
            getCollection(opCtx, nss) == nullptr);
    massertNamespaceNotIndex(nss.ns(), "createCollection");

    uassert(14037,
            "can't create user databases on a --configsvr instance",
            serverGlobalParams.clusterRole != ClusterRole::ConfigServer || nss.isOnInternalDb());

    // This check only applies for actual collections, not indexes or other types of ns.
    uassert(17381,
            str::stream() << "fully qualified namespace " << nss.ns() << " is too long "
                          << "(max is "
                          << NamespaceString::MaxNsCollectionLen
                          << " bytes)",
            !nss.isNormal() || nss.size() <= NamespaceString::MaxNsCollectionLen);

    uassert(17316, "cannot create a blank collection", nss.coll() > 0);
    uassert(28838, "cannot create a non-capped oplog collection", options.capped || !nss.isOplog());
    uassert(ErrorCodes::DatabaseDropPending,
            str::stream() << "Cannot create collection " << nss.ns()
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

    return _views.createView(opCtx, nss, viewOnNss, BSONArray(options.pipeline), options.collation);
}

Collection* DatabaseImpl::createCollection(OperationContext* opCtx,
                                           StringData ns,
                                           const CollectionOptions& options,
                                           bool createIdIndex,
                                           const BSONObj& idIndex) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    invariant(!options.isView());

    CollectionOptions optionsWithUUID = options;
    if (enableCollectionUUIDs && !optionsWithUUID.uuid &&
        serverGlobalParams.featureCompatibility.isSchemaVersion36.load() == true) {
        optionsWithUUID.uuid.emplace(CollectionUUID::gen());
    }

    NamespaceString nss(ns);
    _checkCanCreateCollection(opCtx, nss, optionsWithUUID);
    audit::logCreateCollection(&cc(), ns);

    Status status =
        _dbEntry->createCollection(opCtx, ns, optionsWithUUID, true /*allocateDefaultSpace*/);
    massertNoTraceStatusOK(status);

    opCtx->recoveryUnit()->registerChange(new AddCollectionChange(opCtx, this, ns));
    Collection* collection = _getOrCreateCollectionInstance(opCtx, nss);
    invariant(collection);
    _collections[ns] = collection;

    BSONObj fullIdIndexSpec;

    if (createIdIndex) {
        if (collection->requiresIdIndex()) {
            if (optionsWithUUID.autoIndexId == CollectionOptions::YES ||
                optionsWithUUID.autoIndexId == CollectionOptions::DEFAULT) {
                const auto featureCompatibilityVersion =
                    serverGlobalParams.featureCompatibility.version.load();
                IndexCatalog* ic = collection->getIndexCatalog();
                fullIdIndexSpec = uassertStatusOK(ic->createIndexOnEmptyCollection(
                    opCtx,
                    !idIndex.isEmpty() ? idIndex
                                       : ic->getDefaultIdIndexSpec(featureCompatibilityVersion)));
            }
        }

        if (nss.isSystem()) {
            createSystemIndexes(opCtx, collection);
        }
    }

    getGlobalServiceContext()->getOpObserver()->onCreateCollection(
        opCtx, collection, nss, optionsWithUUID, fullIdIndexSpec);

    return collection;
}

const DatabaseCatalogEntry* DatabaseImpl::getDatabaseCatalogEntry() const {
    return _dbEntry;
}

void DatabaseImpl::dropDatabase(OperationContext* opCtx, Database* db) {
    invariant(db);

    // Store the name so we have if for after the db object is deleted
    const string name = db->name();
    LOG(1) << "dropDatabase " << name;

    invariant(opCtx->lockState()->isDbLockedForMode(name, MODE_X));

    BackgroundOperation::assertNoBgOpInProgForDb(name);

    audit::logDropDatabase(opCtx->getClient(), name);

    for (auto&& coll : *db) {
        Top::get(opCtx->getClient()->getServiceContext()).collectionDropped(coll->ns().ns(), true);
    }

    dbHolder().close(opCtx, name, "database dropped");

    writeConflictRetry(opCtx, "dropDatabase", name, [&] {
        getGlobalServiceContext()
            ->getGlobalStorageEngine()
            ->dropDatabase(opCtx, name)
            .transitional_ignore();
    });
}

namespace {
MONGO_INITIALIZER(InitializeDropDatabaseImpl)(InitializerContext* const) {
    Database::registerDropDatabaseImpl(DatabaseImpl::dropDatabase);
    return Status::OK();
}
MONGO_INITIALIZER(InitializeUserCreateNSImpl)(InitializerContext* const) {
    registerUserCreateNSImpl(userCreateNSImpl);
    return Status::OK();
}

MONGO_INITIALIZER(InitializeDropAllDatabasesExceptLocalImpl)(InitializerContext* const) {
    registerDropAllDatabasesExceptLocalImpl(dropAllDatabasesExceptLocalImpl);
    return Status::OK();
}
}  // namespace
}  // namespace mongo

void mongo::dropAllDatabasesExceptLocalImpl(OperationContext* opCtx) {
    Lock::GlobalWrite lk(opCtx);

    vector<string> n;
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
    storageEngine->listDatabases(&n);

    if (n.size() == 0)
        return;
    log() << "dropAllDatabasesExceptLocal " << n.size();

    repl::getGlobalReplicationCoordinator()->dropAllSnapshots();

    for (const auto& dbName : n) {
        if (dbName != "local") {
            writeConflictRetry(opCtx, "dropAllDatabasesExceptLocal", dbName, [&opCtx, &dbName] {
                Database* db = dbHolder().get(opCtx, dbName);

                // This is needed since dropDatabase can't be rolled back.
                // This is safe be replaced by "invariant(db);dropDatabase(opCtx, db);" once fixed
                if (db == nullptr) {
                    log() << "database disappeared after listDatabases but before drop: " << dbName;
                } else {
                    DatabaseImpl::dropDatabase(opCtx, db);
                }
            });
        }
    }
}

auto mongo::userCreateNSImpl(OperationContext* opCtx,
                             Database* db,
                             StringData ns,
                             BSONObj options,
                             CollectionOptions::ParseKind parseKind,
                             bool createDefaultIndexes,
                             const BSONObj& idIndex) -> Status {
    invariant(db);

    LOG(1) << "create collection " << ns << ' ' << options;

    if (!NamespaceString::validCollectionComponent(ns))
        return Status(ErrorCodes::InvalidNamespace, str::stream() << "invalid ns: " << ns);

    Collection* collection = db->getCollection(opCtx, ns);

    if (collection)
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a collection '" << ns.toString() << "' already exists");

    if (db->getViewCatalog()->lookup(opCtx, ns))
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a view '" << ns.toString() << "' already exists");

    CollectionOptions collectionOptions;
    Status status = collectionOptions.parse(options, parseKind);

    if (!status.isOK())
        return status;

    // Validate the collation, if there is one.
    if (!collectionOptions.collation.isEmpty()) {
        auto collator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                            ->makeFromBSON(collectionOptions.collation);

        if (!collator.isOK()) {
            return collator.getStatus();
        }

        // If the collator factory returned a non-null collator, set the collation option to the
        // result of serializing the collator's spec back into BSON. We do this in order to fill in
        // all options that the user omitted.
        //
        // If the collator factory returned a null collator (representing the "simple" collation),
        // we simply unset the "collation" from the collection options. This ensures that
        // collections created on versions which do not support the collation feature have the same
        // format for representing the simple collation as collections created on this version.
        collectionOptions.collation =
            collator.getValue() ? collator.getValue()->getSpec().toBSON() : BSONObj();
    }

    status =
        validateStorageOptions(collectionOptions.storageEngine,
                               stdx::bind(&StorageEngine::Factory::validateCollectionStorageOptions,
                                          stdx::placeholders::_1,
                                          stdx::placeholders::_2));

    if (!status.isOK())
        return status;

    if (auto indexOptions = collectionOptions.indexOptionDefaults["storageEngine"]) {
        status =
            validateStorageOptions(indexOptions.Obj(),
                                   stdx::bind(&StorageEngine::Factory::validateIndexStorageOptions,
                                              stdx::placeholders::_1,
                                              stdx::placeholders::_2));

        if (!status.isOK()) {
            return status;
        }
    }

    if (collectionOptions.isView()) {
        invariant(parseKind == CollectionOptions::parseForCommand);
        uassertStatusOK(db->createView(opCtx, ns, collectionOptions));
    } else {
        invariant(
            db->createCollection(opCtx, ns, collectionOptions, createDefaultIndexes, idIndex));
    }

    return Status::OK();
}
