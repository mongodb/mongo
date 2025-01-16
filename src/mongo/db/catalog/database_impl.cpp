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

#include <algorithm>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <boost/filesystem/operations.hpp>

#include "mongo/base/init.h"
#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/audit.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/database_impl.h"
#include "mongo/db/catalog/drop_indexes.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog/index_key_validate.h"
#include "mongo/db/catalog/namespace_uuid_cache.h"
#include "mongo/db/catalog/uuid_catalog.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index_legacy.h"
#include "mongo/db/introspect.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/server_options.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/storage_engine_init.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/system_index.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/platform/basic.h"
#include "mongo/platform/random.h"
#include "mongo/s/cannot_implicitly_create_collection_info.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"
#include "mongo/util/represent_as.h"

namespace mongo {
MONGO_REGISTER_SHIM(Database::makeImpl)
(Database* const this_,
 OperationContext* const opCtx,
 const StringData name,
 DatabaseCatalogEntry* const dbEntry,
 PrivateTo<Database>)
    ->std::unique_ptr<Database::Impl> {
    return stdx::make_unique<DatabaseImpl>(this_, opCtx, name, dbEntry);
}

namespace {
MONGO_FAIL_POINT_DEFINE(hangBeforeLoggingCreateCollection);
}  // namespace

using std::endl;
using std::list;
using std::set;
using std::string;
using std::stringstream;
using std::unique_ptr;
using std::vector;

void uassertNamespaceNotIndex(StringData ns, StringData caller) {
    uassert(17320,
            str::stream() << "cannot do " << caller << " on namespace with a $ in it: " << ns,
            NamespaceString::normal(ns));
}

// class DatabaseImpl::AddCollectionChange : public RecoveryUnit::Change {
// public:
//     AddCollectionChange(OperationContext* opCtx, DatabaseImpl* db, StringData ns)
//         : _opCtx(opCtx), _db(db), _ns(ns.toString()) {}

//     virtual void commit(boost::optional<Timestamp> commitTime) {
//         CollectionMap::const_iterator it = _db->_collections.find(_ns);

//         if (it == _db->_collections.end())
//             return;

//         // Ban reading from this collection on committed reads on snapshots before now.
//         if (commitTime) {
//             it->second->setMinimumVisibleSnapshot(commitTime.get());
//         }
//     }

//     virtual void rollback() {
//         CollectionMap::const_iterator it = _db->_collections.find(_ns);

//         if (it == _db->_collections.end())
//             return;

//         delete it->second;
//         _db->_collections.erase(it);
//     }

//     OperationContext* const _opCtx;
//     DatabaseImpl* const _db;
//     const std::string _ns;
// };

// class DatabaseImpl::RemoveCollectionChange : public RecoveryUnit::Change {
// public:
//     // Takes ownership of coll (but not db).
//     RemoveCollectionChange(DatabaseImpl* db, Collection* coll) : _db(db), _coll(coll) {}

//     virtual void commit(boost::optional<Timestamp>) {
//         delete _coll;
//     }

//     virtual void rollback() {
//         Collection*& inMap = _db->_collections[_coll->ns().ns()];
//         invariant(!inMap);
//         inMap = _coll;
//     }

//     DatabaseImpl* const _db;
//     Collection* const _coll;
// };

DatabaseImpl::~DatabaseImpl() {
    _collections.clear();
    // for (CollectionMap::const_iterator i = _collections.begin(); i != _collections.end(); ++i)
    //     delete i->second;
}

void DatabaseImpl::close(OperationContext* opCtx, const std::string& reason) {
    invariant(opCtx->lockState()->isW());

    // Clear cache of oplog Collection pointer.
    repl::oplogCheckCloseDatabase(opCtx, this->_this);

    for (const auto& [name, coll] : _collections) {
        // auto coll = pair.second;
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
    MONGO_UNREACHABLE;
    // this function is used to construct Collection handler for Mongo
    //
    Collection* collection = getCollection(opCtx, nss);

    if (collection) {
        return collection;
    }

    unique_ptr<CollectionCatalogEntry> cce(_dbEntry->getCollectionCatalogEntry(opCtx, nss.ns()));
    if (!cce) {
        return nullptr;
    }

    auto uuid = cce->getCollectionOptions(opCtx).uuid;

    unique_ptr<RecordStore> rs(_dbEntry->getRecordStore(nss.ns()));
    if (rs.get() == nullptr) {
        severe() << "Record store did not exist. Collection: " << nss.ns() << " UUID: "
                 << (uuid ? uuid->toString() : "none");  // if cce exists, so should this
        fassertFailedNoTrace(50936);
    }

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

Collection* DatabaseImpl::_createCollectionHandler(OperationContext* opCtx,
                                                   const NamespaceString& nss,
                                                   bool createIdIndex,
                                                   const BSONObj& idIndexSpec,
                                                   bool forView) {
    MONGO_LOG(1) << "DatabaseImpl::_createCollectionHandler";
    if (!forView) {
        if (auto iter = _collections.find(nss.toString()); iter != _collections.end()) {
            return iter->second.get();
        }
    }
    auto cce = _dbEntry->getCollectionCatalogEntry(opCtx, nss.toStringData());
    if (!cce) {
        // The collection not exists in the Monograph
        return nullptr;
    }
    CollectionCatalogEntry::MetaData metadata = cce->getMetaData(opCtx);
    auto uuid = metadata.options.uuid;
    auto rs = cce->getRecordStore();
    auto collection =
        std::make_unique<Collection>(opCtx, nss.toStringData(), uuid, cce, rs, _dbEntry);

    if (forView) {
        _collectionsView.try_emplace(nss.toString(), std::move(collection));
        return nullptr;
    }

    if (uuid) {
        // We are not in a WUOW only when we are called from Database::init(). There is no need
        // to rollback UUIDCatalog changes because we are initializing existing collections.
        auto&& uuidCatalog = UUIDCatalog::get(opCtx);
        if (!opCtx->lockState()->inAWriteUnitOfWork()) {
            uuidCatalog.registerUUIDCatalogEntry(uuid.get(), collection.get());
        } else {
            uuidCatalog.onCreateCollection(opCtx, collection.get(), uuid.get());
        }
    }

    BSONObj fullIdIndexSpec;
    if (createIdIndex) {
        if (collection->requiresIdIndex()) {
            if (metadata.options.autoIndexId == CollectionOptions::YES ||
                metadata.options.autoIndexId == CollectionOptions::DEFAULT) {
                // createCollection() may be called before the in-memory fCV parameter is
                // initialized, so use the unsafe fCV getter here.

                // There is no need for Monograph to call this function.
                // The check for index specification has been done through `prepareSpecForCreate`
                // during DatabaseImpl::createCollection.

                // IndexCatalog* ic = collection->getIndexCatalog();
                // fullIdIndexSpec = uassertStatusOK(ic->createIndexOnEmptyCollection(
                //     opCtx, !idIndexSpec.isEmpty() ? idIndexSpec : ic->getDefaultIdIndexSpec()));
            } else {
                // autoIndexId: false is only allowed on unreplicated collections.
                uassert(50001,
                        str::stream() << "autoIndexId:false is not allowed for collection "
                                      << nss.ns() << " because it can be replicated",
                        !nss.isReplicated());
            }
        }
    }
    // Because writing the oplog entry depends on having the full spec for the _id index, which is
    // not available until the collection is actually created, we can't write the oplog entry until
    // after we have created the collection.  In order to make the storage timestamp for the
    // collection create always correct even when other operations are present in the same storage
    // transaction, we reserve an opTime before the collection creation, then pass it to the
    // opObserver.  Reserving the optime automatically sets the storage timestamp.
    // OplogSlot createOplogSlot;
    // if (canAcceptWrites && supportsDocLocking() && !coordinator->isOplogDisabledFor(opCtx, nss))
    // {
    //     createOplogSlot = repl::getNextOpTime(opCtx);
    // }
    // MONGO_FAIL_POINT_PAUSE_WHILE_SET(hangBeforeLoggingCreateCollection);

    // opCtx->getServiceContext()->getOpObserver()->onCreateCollection(
    //     opCtx, collection, nss, metadata.options, fullIdIndexSpec, createOplogSlot);

    // It is necessary to create the system index *after* running the onCreateCollection so that
    // the storage timestamp for the index creation is after the storage timestamp for the
    // collection creation, and the opTimes for the corresponding oplog entries are the same as the
    // storage timestamps.  This way both primary and any secondaries will see the index created
    // after the collection is created.
    bool canAcceptWrites = true;
    if (canAcceptWrites && createIdIndex && nss.isSystem()) {
        // TODO: we should it in another place
        // createSystemIndexes(opCtx, collection.get());
    }

    auto [iter, _] = _collections.try_emplace(nss.toString(), std::move(collection));


    MONGO_LOG(1) << "[opID]=" << opCtx->getOpID() << "DatabaseImpl::createCollection"
                 << ". create done and handler to collection is available";
    return iter->second.get();
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

    {
        std::vector<std::string> collections;
        _dbEntry->getCollectionNamespaces(collections);

        for (auto& ns : collections) {
            NamespaceString nss{std::move(ns)};
            _createCollectionHandler(opCtx, nss, false);
        }
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
    // MONGO_UNREACHABLE;
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    std::vector<std::string> collections;
    _dbEntry->getCollectionNamespaces(collections);

    for (const auto& ns : collections) {
        invariant(NamespaceString::normal(ns));

        CollectionCatalogEntry* coll = _dbEntry->getCollectionCatalogEntry(opCtx, ns);

        CollectionOptions options = coll->getCollectionOptions(opCtx);

        if (!options.temp) {
            continue;
        }
        try {
            WriteUnitOfWork wunit(opCtx);
            Status status = dropCollection(opCtx, ns, {});

            if (!status.isOK()) {
                warning() << "could not drop temp collection '" << ns << "': " << redact(status);
                continue;
            }

            wunit.commit();
        } catch (const WriteConflictException&) {
            warning() << "could not drop temp collection '" << ns
                      << "' due to "
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
    std::vector<std::string> collections;
    _dbEntry->getCollectionNamespaces(collections);

    long long nCollections = 0;
    long long nViews = 0;
    long long objects = 0;
    long long size = 0;
    long long storageSize = 0;
    long long numExtents = 0;
    long long indexes = 0;
    long long indexSize = 0;

    for (auto it = collections.begin(); it != collections.end(); ++it) {
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

    if (!opCtx->getServiceContext()->getStorageEngine()->isEphemeral()) {
        boost::filesystem::path dbpath(storageGlobalParams.dbpath);
        if (storageGlobalParams.directoryperdb) {
            dbpath /= _name;
        }

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
    Status status = _views.dropView(opCtx, NamespaceString(fullns));
    Top::get(opCtx->getServiceContext()).collectionDropped(fullns);
    return status;
}

Status DatabaseImpl::dropCollection(OperationContext* opCtx,
                                    StringData fullns,
                                    repl::OpTime dropOpTime) {
    MONGO_LOG(1) << "DatabaseImpl::dropCollection"
                 << ". fullns: " << fullns;
    NamespaceString nss{fullns};
    // if (!getCollection(opCtx, nss)) {
    //     // Collection doesn't exist so don't bother validating if it can be dropped.
    //     return Status::OK();
    // }


    {
        invariant(nss.db() == _name);

        if (nss.isSystem()) {
            if (nss.isSystemDotProfile()) {
                if (_profile != 0)
                    return Status(ErrorCodes::IllegalOperation,
                                  "turn off profiling before dropping system.profile collection");
            } else if (!(nss.isSystemDotViews() || nss.isHealthlog() ||
                         nss == SessionsCollection::kSessionsNamespaceString ||
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
    // drop indexes here
    MONGO_LOG(1) << "DatabaseImpl::dropCollectionEvenIfSystem"
                 << ". fullns: " << fullns;
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    // LOG(1) << "dropCollection: " << fullns;

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

    auto uuid = collection->uuid();
    auto uuidString = uuid ? uuid.get().toString() : "no UUID";

    uassertNamespaceNotIndex(fullns.toStringData(), "dropCollection");

    BackgroundOperation::assertNoBgOpInProgForNs(fullns);

    // Make sure no indexes builds are in progress.
    // Use massert() to be consistent with IndexCatalog::dropAllIndexes().
    auto numIndexesInProgress = collection->getIndexCatalog()->numIndexesInProgress(opCtx);
    massert(40461,
            str::stream() << "cannot drop collection " << fullns.ns() << " (" << uuidString
                          << ") when " << numIndexesInProgress << " index builds in progress.",
            numIndexesInProgress == 0);

    audit::logDropCollection(&cc(), fullns.toString());

    Top::get(opCtx->getServiceContext()).collectionDropped(fullns.toStringData());

    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    opObserver->onDropCollection(opCtx, fullns, uuid);
    auto status = _finishDropCollection(opCtx, fullns, collection);
    if (!status.isOK()) {
        return status;
    }
    return Status::OK();
    MONGO_UNREACHABLE;
    /*
    // Drop unreplicated collections immediately.
    // If 'dropOpTime' is provided, we should proceed to rename the collection.
    auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    auto opObserver = opCtx->getServiceContext()->getOpObserver();
    auto isOplogDisabledForNamespace = replCoord->isOplogDisabledFor(opCtx, fullns);
    if (dropOpTime.isNull() && isOplogDisabledForNamespace) {
        opObserver->onDropCollection(opCtx, fullns, uuid);
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
        // MMAPv1 requires that index namespaces are subject to the same length constraints as
        // indexes in collections that are not in a drop-pending state. Therefore, we check if the
        // drop-pending namespace is too long for any index names in the collection.
        // These indexes are dropped regardless of the storage engine on the current node because we
        // may still have nodes running MMAPv1 in the replica set.

        // Compile a list of any indexes that would become too long following the drop-pending
        // rename. In the case that this collection drop gets rolled back, this will incur a
        // performance hit, since those indexes will have to be rebuilt from scratch, but data
        // integrity is maintained.
        // std::vector<IndexDescriptor*> indexesToDrop;
        // auto indexIter = collection->getIndexCatalog()->getIndexIterator(opCtx, true);

        // Determine which index names are too long. Since we don't have the collection drop optime
        // at this time, use the maximum optime to check the index names.
        // auto longDpns = fullns.makeDropPendingNamespace(repl::OpTime::max());
        // while (indexIter.more()) {
        //     auto index = indexIter.next();
        //     auto status = longDpns.checkLengthForRename(index->indexName().size());
        //     if (!status.isOK()) {
        //         indexesToDrop.push_back(index);
        //     }
        // }

        // Drop the offending indexes.
        // for (auto&& index : indexesToDrop) {
        //     log() << "dropCollection: " << fullns << " (" << uuidString << ") - index namespace
        //     '"
        //           << index->indexNamespace()
        //           << "' would be too long after drop-pending rename. Dropping index
        //           immediately.";
        //     // Log the operation before the drop so that each drop is timestamped at the same
        //     time
        //     // as the oplog entry.
        // opObserver->onDropIndex(
        //     opCtx, fullns, collection->uuid(), index->indexName(), index->infoObj());
        // fassert(40463, collection->getIndexCatalog()->dropIndex(opCtx, index));
        // }

        // Log oplog entry for collection drop and proceed to complete rest of two phase drop
        // process.
        dropOpTime = opObserver->onDropCollection(opCtx, fullns, uuid);

        // Drop collection immediately if OpObserver did not write entry to oplog.
        // After writing the oplog entry, all errors are fatal. See getNextOpTime() comments in
        // oplog.cpp.
        if (dropOpTime.isNull()) {
            log() << "dropCollection: " << fullns << " (" << uuidString
                  << ") - no drop optime available for pending-drop. "
                  << "Dropping collection immediately.";
            fassert(40462, _finishDropCollection(opCtx, fullns, collection));
            return Status::OK();
        }
    } else {
        // If we are provided with a valid 'dropOpTime', it means we are dropping this collection
        // in the context of applying an oplog entry on a secondary.
        // OpObserver::onDropCollection() should be returning a null OpTime because we should not be
        // writing to the oplog.
        auto opTime = opObserver->onDropCollection(opCtx, fullns, uuid);
        if (!opTime.isNull()) {
            severe() << "dropCollection: " << fullns << " (" << uuidString
                     << ") - unexpected oplog entry written to the oplog with optime " << opTime;
            fassertFailed(40468);
        }
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
    */
}

Status DatabaseImpl::_finishDropCollection(OperationContext* opCtx,
                                           const NamespaceString& fullns,
                                           Collection* collection) {
    MONGO_LOG(1) << "DatabaseImpl::_finishDropCollection";
    // LOG(1) << "dropCollection: " << fullns << " - dropAllIndexes start";
    // collection->getIndexCatalog()->dropAllIndexes(opCtx, true);

    // invariant(collection->getCatalogEntry()->getTotalIndexCount(opCtx) == 0);
    // LOG(1) << "dropCollection: " << fullns << " - dropAllIndexes done";

    auto uuid = collection->uuid();
    auto uuidString = uuid ? uuid.get().toString() : "no UUID";

    // We want to destroy the Collection object before telling the StorageEngine to destroy the
    // RecordStore.
    _clearCollectionCache(
        opCtx, fullns.toStringData(), "collection dropped", /*collectionGoingAway*/ true);


    log() << "Finishing collection drop for " << fullns << " (" << uuidString << ").";

    return _dbEntry->dropCollection(opCtx, fullns.toStringData());
}

void DatabaseImpl::_clearCollectionCache(OperationContext* opCtx,
                                         StringData fullns,
                                         const std::string& reason,
                                         bool collectionGoingAway) {
    invariant(_name == nsToDatabaseSubstring(fullns));
    auto it = _collections.find(fullns);

    if (it == _collections.end()) {
        return;
    }

    // Takes ownership of the collection
    // opCtx->recoveryUnit()->registerChange(new RemoveCollectionChange(this, it->second));

    it->second->getCursorManager()->invalidateAll(opCtx, collectionGoingAway, reason);
    _collections.erase(it);
}

Collection* DatabaseImpl::getCollection(OperationContext* opCtx, StringData ns) {
    NamespaceString nss{ns};
    return getCollection(opCtx, nss);
}

Collection* DatabaseImpl::getCollection(OperationContext* opCtx, const NamespaceString& nss) {
    MONGO_LOG(1) << "DatabaseImpl::getCollection"
                 << ", nss: " << nss.toStringData();
    invariant(_name == nss.db());
    dassert(!cc().getOperationContext() || opCtx == cc().getOperationContext());

    auto [exists, _] = opCtx->getServiceContext()->getStorageEngine()->lockCollection(
        opCtx, nss.toStringData(), false);
    if (!exists) {
        return nullptr;
    }

    MONGO_LOG(1) << "nss: " << nss.toStringData() << " exists. get handler";

    if (auto it = _collections.find(nss.ns()); it != _collections.end() && it->second) {
        auto found = it->second.get();
        NamespaceUUIDCache& cache = NamespaceUUIDCache::get(opCtx);
        if (auto uuid = found->uuid()) {
            cache.ensureNamespaceInCache(nss, uuid.get());
        }
        return found;
    } else {
        return _createCollectionHandler(opCtx, nss, false);
    }
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

        string clearCacheReason = str::stream()
            << "renamed collection '" << fromNS << "' to '" << toNS << "'";
        IndexCatalog::IndexIterator ii = coll->getIndexCatalog()->getIndexIterator(opCtx, true);

        while (ii.more()) {
            IndexDescriptor* desc = ii.next();
            _clearCollectionCache(
                opCtx, desc->indexNamespace(), clearCacheReason, /*collectionGoingAway*/ true);
        }

        _clearCollectionCache(opCtx, fromNS, clearCacheReason, /*collectionGoingAway*/ true);
        _clearCollectionCache(opCtx, toNS, clearCacheReason, /*collectionGoingAway*/ false);

        Top::get(opCtx->getServiceContext()).collectionDropped(fromNS.toString());

        log() << "renameCollection: renaming collection " << coll->uuid()->toString() << " from "
              << fromNS << " to " << toNS;
    }

    Status s = _dbEntry->renameCollection(opCtx, fromNS, toNS, stayTemp);
    // opCtx->recoveryUnit()->registerChange(new AddCollectionChange(opCtx, this, toNS));
    // _collections[toNS] = _getOrCreateCollectionInstance(opCtx, toNSS);

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
    uassertNamespaceNotIndex(nss.ns(), "createCollection");

    uassert(14037,
            "can't create user databases on a --configsvr instance",
            serverGlobalParams.clusterRole != ClusterRole::ConfigServer || nss.isOnInternalDb());

    // This check only applies for actual collections, not indexes or other types of ns.
    uassert(17381,
            str::stream() << "fully qualified namespace " << nss.ns() << " is too long "
                          << "(max is " << NamespaceString::MaxNsCollectionLen << " bytes)",
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

/*
 * Refer to src/mongo/db/catalog/database_impl.cpp parseCollation
 */
std::unique_ptr<CollatorInterface> parseCollation(OperationContext* opCtx,
                                                  const NamespaceString& nss,
                                                  const BSONObj& collationSpec) {
    if (collationSpec.isEmpty()) {
        return {nullptr};
    }

    auto collator =
        CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationSpec);

    // If the collection's default collator has a version not currently supported by our ICU
    // integration, shut down the server. Errors other than IncompatibleCollationVersion should not
    // be possible, so these are an invariant rather than fassert.
    if (collator == ErrorCodes::IncompatibleCollationVersion) {
        log() << "Collection " << nss
              << " has a default collation which is incompatible with this version: "
              << collationSpec;
    }
    invariant(collator.getStatus());

    return std::move(collator.getValue());
}

/*
 * Refer to src/mongo/db/catalog/index_catalog_impl.cpp IndexCatalogImpl::getDefaultIdIndexSpec
 */
BSONObj buildDefaultIdIndexSpec(OperationContext* opCtx,
                                const NamespaceString& nss,
                                const CollectionOptions& options) {
    const auto indexVersion = IndexDescriptor::getDefaultIndexVersion();

    BSONObjBuilder b;
    b.append("v", static_cast<int>(indexVersion));
    b.append("name", "_id_");
    b.append("ns", nss.ns());
    b.append("key", BSON("_id" << 1));

    if (auto collator = parseCollation(opCtx, nss, options.collation);
        collator && indexVersion >= IndexDescriptor::IndexVersion::kV2) {
        // Creating an index with the "collation" option requires a v=2 index.
        b.append("collation", collator->getSpec().toBSON());
    }
    return b.obj();
}

namespace index_check {
using IndexVersion = IndexDescriptor::IndexVersion;

Status checkValidFilterExpressions(MatchExpression* expression, int level = 0) {
    if (!expression)
        return Status::OK();

    switch (expression->matchType()) {
        case MatchExpression::AND:
            if (level > 0)
                return Status(ErrorCodes::CannotCreateIndex,
                              "$and only supported in partialFilterExpression at top level");
            for (size_t i = 0; i < expression->numChildren(); i++) {
                Status status = checkValidFilterExpressions(expression->getChild(i), level + 1);
                if (!status.isOK())
                    return status;
            }
            return Status::OK();
        case MatchExpression::EQ:
        case MatchExpression::LT:
        case MatchExpression::LTE:
        case MatchExpression::GT:
        case MatchExpression::GTE:
        case MatchExpression::EXISTS:
        case MatchExpression::TYPE_OPERATOR:
            return Status::OK();
        default:
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "unsupported expression in partial index: "
                                        << expression->toString());
    }
}

Status isSpecOk(OperationContext* opCtx,
                const NamespaceString& nss,
                const BSONObj& spec,
                const CollatorInterface* collectionCollator) {
    BSONElement vElt = spec["v"];
    if (!vElt) {
        return {ErrorCodes::InternalError,
                str::stream()
                    << "An internal operation failed to specify the 'v' field, which is a required "
                       "property of an index specification: "
                    << spec};
    }

    if (!vElt.isNumber()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "non-numeric value for \"v\" field: " << vElt);
    }

    auto vEltAsInt = representAs<int>(vElt.number());
    if (!vEltAsInt) {
        return {ErrorCodes::CannotCreateIndex,
                str::stream() << "Index version must be representable as a 32-bit integer, but got "
                              << vElt.toString(false, false)};
    }

    auto indexVersion = static_cast<IndexVersion>(*vEltAsInt);

    if (indexVersion >= IndexVersion::kV2) {
        auto status = index_key_validate::validateIndexSpecFieldNames(spec);
        if (!status.isOK()) {
            return status;
        }
    }

    // SERVER-16893 Forbid use of v0 indexes with non-mmapv1 engines
    if (indexVersion == IndexVersion::kV0 &&
        !opCtx->getServiceContext()->getStorageEngine()->isMmapV1()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "use of v0 indexes is only allowed with the "
                                    << "mmapv1 storage engine");
    }

    if (!IndexDescriptor::isIndexVersionSupported(indexVersion)) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "this version of mongod cannot build new indexes "
                                    << "of version number " << static_cast<int>(indexVersion));
    }

    if (nss.isSystemDotIndexes())
        return Status(ErrorCodes::CannotCreateIndex,
                      "cannot have an index on the system.indexes collection");

    if (nss.isOplog())
        return Status(ErrorCodes::CannotCreateIndex, "cannot have an index on the oplog");

    if (nss.coll() == "$freelist") {
        // this isn't really proper, but we never want it and its not an error per se
        return Status(ErrorCodes::IndexAlreadyExists, "cannot index freelist");
    }

    const BSONElement specNamespace = spec["ns"];
    if (specNamespace.type() != String)
        return Status(ErrorCodes::CannotCreateIndex,
                      "the index spec is missing a \"ns\" string field");

    if (nss.ns() != specNamespace.valueStringData())
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream() << "the \"ns\" field of the index spec '"
                                    << specNamespace.valueStringData()
                                    << "' does not match the collection name '" << nss.ns() << "'");

    // logical name of the index
    const BSONElement nameElem = spec["name"];
    if (nameElem.type() != String)
        return Status(ErrorCodes::CannotCreateIndex, "index name must be specified as a string");

    const StringData name = nameElem.valueStringData();
    if (name.find('\0') != std::string::npos)
        return Status(ErrorCodes::CannotCreateIndex, "index name cannot contain NUL bytes");

    if (name.empty())
        return Status(ErrorCodes::CannotCreateIndex, "index name cannot be empty");

    // Drop pending collections are internal to the server and will not be exported to another
    // storage engine. The indexes contained in these collections are not subject to the same
    // namespace length constraints as the ones in created by users.
    if (!nss.isDropPendingNamespace()) {
        auto indexNamespace = IndexDescriptor::makeIndexNamespace(nss.ns(), name);
        if (indexNamespace.length() > NamespaceString::MaxNsLen)
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream() << "namespace name generated from index name \""
                                        << indexNamespace << "\" is too long (127 byte max)");
    }

    const BSONObj key = spec.getObjectField("key");
    const Status keyStatus = index_key_validate::validateKeyPattern(key, indexVersion);
    if (!keyStatus.isOK()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      str::stream()
                          << "bad index key pattern " << key << ": " << keyStatus.reason());
    }

    std::unique_ptr<CollatorInterface> collator;
    BSONElement collationElement = spec.getField("collation");
    if (collationElement) {
        if (collationElement.type() != BSONType::Object) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "\"collation\" for an index must be a document");
        }
        auto statusWithCollator = CollatorFactoryInterface::get(opCtx->getServiceContext())
                                      ->makeFromBSON(collationElement.Obj());
        if (!statusWithCollator.isOK()) {
            return statusWithCollator.getStatus();
        }
        collator = std::move(statusWithCollator.getValue());

        if (!collator) {
            return {ErrorCodes::InternalError,
                    str::stream() << "An internal operation specified the collation "
                                  << CollationSpec::kSimpleSpec
                                  << " explicitly, which should instead be implied by omitting the "
                                     "'collation' field from the index specification"};
        }

        if (static_cast<IndexVersion>(vElt.numberInt()) < IndexVersion::kV2) {
            return {ErrorCodes::CannotCreateIndex,
                    str::stream() << "Index version " << vElt.fieldNameStringData() << "="
                                  << vElt.numberInt() << " does not support the '"
                                  << collationElement.fieldNameStringData() << "' option"};
        }

        string pluginName = IndexNames::findPluginName(key);
        if ((pluginName != IndexNames::BTREE) && (pluginName != IndexNames::GEO_2DSPHERE) &&
            (pluginName != IndexNames::HASHED)) {
            return Status(ErrorCodes::CannotCreateIndex,
                          str::stream()
                              << "Index type '" << pluginName
                              << "' does not support collation: " << collator->getSpec().toBSON());
        }
    }

    const bool isSparse = spec["sparse"].trueValue();

    // Ensure if there is a filter, its valid.
    BSONElement filterElement = spec.getField("partialFilterExpression");
    if (filterElement) {
        if (isSparse) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "cannot mix \"partialFilterExpression\" and \"sparse\" options");
        }

        if (filterElement.type() != Object) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "\"partialFilterExpression\" for an index must be a document");
        }

        // The collator must outlive the constructed MatchExpression.
        boost::intrusive_ptr<ExpressionContext> expCtx(
            new ExpressionContext(opCtx, collator.get()));

        // Parsing the partial filter expression is not expected to fail here since the
        // expression would have been successfully parsed upstream during index creation.
        StatusWithMatchExpression statusWithMatcher =
            MatchExpressionParser::parse(filterElement.Obj(),
                                         std::move(expCtx),
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kBanAllSpecialFeatures);
        if (!statusWithMatcher.isOK()) {
            return statusWithMatcher.getStatus();
        }
        const std::unique_ptr<MatchExpression> filterExpr = std::move(statusWithMatcher.getValue());

        Status status = checkValidFilterExpressions(filterExpr.get());
        if (!status.isOK()) {
            return status;
        }
    }

    if (IndexDescriptor::isIdIndexPattern(key)) {
        BSONElement uniqueElt = spec["unique"];
        if (uniqueElt && !uniqueElt.trueValue()) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be non-unique");
        }

        if (filterElement) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be a partial index");
        }

        if (isSparse) {
            return Status(ErrorCodes::CannotCreateIndex, "_id index cannot be sparse");
        }

        if (collationElement &&
            !CollatorInterface::collatorsMatch(collator.get(), collectionCollator)) {
            return Status(ErrorCodes::CannotCreateIndex,
                          "_id index must have the collection default collation");
        }
    } else {
        // for non _id indexes, we check to see if replication has turned off all indexes
        // we _always_ created _id index
        if (!repl::ReplicationCoordinator::get(opCtx)->buildsIndexes()) {
            // this is not exactly the right error code, but I think will make the most sense
            return Status(ErrorCodes::IndexAlreadyExists, "no indexes per repl");
        }
    }

    // --- only storage engine checks allowed below this ----

    BSONElement storageEngineElement = spec.getField("storageEngine");
    if (storageEngineElement.eoo()) {
        return Status::OK();
    }
    if (storageEngineElement.type() != mongo::Object) {
        return Status(ErrorCodes::CannotCreateIndex,
                      "\"storageEngine\" options must be a document if present");
    }
    BSONObj storageEngineOptions = storageEngineElement.Obj();
    if (storageEngineOptions.isEmpty()) {
        return Status(ErrorCodes::CannotCreateIndex,
                      "Empty \"storageEngine\" options are invalid. "
                      "Please remove the field or include valid options.");
    }
    Status storageEngineStatus = validateStorageOptions(
        opCtx->getServiceContext(), storageEngineOptions, [](const auto& x, const auto& y) {
            return x->validateIndexStorageOptions(y);
        });
    if (!storageEngineStatus.isOK()) {
        return storageEngineStatus;
    }

    return Status::OK();
}

const BSONObj _idObj = BSON("_id" << 1);

BSONObj fixIndexKey(const BSONObj& key) {
    if (IndexDescriptor::isIdIndexPattern(key)) {
        return _idObj;
    }
    if (key["_id"].type() == Bool && key.nFields() == 1) {
        return _idObj;
    }
    return key;
}

StatusWith<BSONObj> fixIndexSpec(OperationContext* opCtx,

                                 const BSONObj& spec) {
    auto statusWithSpec = IndexLegacy::adjustIndexSpecObject(spec);
    if (!statusWithSpec.isOK()) {
        return statusWithSpec;
    }
    BSONObj o = statusWithSpec.getValue();

    BSONObjBuilder b;

    // We've already verified in IndexCatalog::_isSpecOk() that the index version is present and
    // that it is representable as a 32-bit integer.
    auto vElt = o["v"];
    invariant(vElt);

    b.append("v", vElt.numberInt());

    if (o["unique"].trueValue())
        b.appendBool("unique", true);  // normalize to bool true in case was int 1 or something...

    BSONObj key = fixIndexKey(o["key"].Obj());
    b.append("key", key);

    string name = o["name"].String();
    if (IndexDescriptor::isIdIndexPattern(key)) {
        name = "_id_";
    }
    b.append("name", name);

    {
        BSONObjIterator i(o);
        while (i.more()) {
            BSONElement e = i.next();
            string s = e.fieldName();

            if (s == "_id") {
                // skip
            } else if (s == "dropDups") {
                // dropDups is silently ignored and removed from the spec as of SERVER-14710.
            } else if (s == "v" || s == "unique" || s == "key" || s == "name") {
                // covered above
            } else {
                b.append(e);
            }
        }
    }

    return b.obj();
}

// Refer to src/mongo/db/catalog/index_catalog_impl.cpp `IndexCatalogImpl::prepareSpecForCreate`.
//
// MongoDB processes the createCollection command in two distinct steps:
// 1. creates the collection.
// 2. creates the _id index.
// In contrast, Monograph handles the createCollection command in a single step, consolidating the
// process into the initial "create the collection" phase.
//
// However, during the creation of the _id index, MongoDB performs several validation checks that
// may alter the original index specification. To ensure consistency and avoid potential conflicts,
// it is advisable to perform these checks before the collection creation stage.
StatusWith<BSONObj> prepareSpecForCreate(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const BSONObj& collationObj,
                                         const BSONObj& original) {

    std::unique_ptr<CollatorInterface> collator = parseCollation(opCtx, nss, collationObj);

    Status status = isSpecOk(opCtx, nss, original, collator.get());
    if (!status.isOK()) {
        return {status};
    }

    auto fixed = fixIndexSpec(opCtx, original);
    if (!fixed.isOK()) {
        return fixed;
    }

    // we double check with new index spec
    status = isSpecOk(opCtx, nss, fixed.getValue(), collator.get());
    if (!status.isOK()) {
        return {status};
    }

    // The _doesSpecConflictWithExisting method may not be necessary during the "Create Collection"
    // operation, as there are no existing indexes to conflict with. status =
    // _doesSpecConflictWithExisting(opCtx, fixed.getValue()); if (!status.isOK())
    //     return StatusWith<BSONObj>(status);

    return fixed;
}

}  // namespace index_check

Collection* DatabaseImpl::createCollection(OperationContext* opCtx,
                                           StringData ns,
                                           const CollectionOptions& options,
                                           bool createIdIndex,
                                           const BSONObj& idIndex) {
    MONGO_LOG(1) << "[opID]=" << opCtx->getOpID() << " DatabaseImpl::createCollection"
                 << ". ns: " << ns << ". createIdIndex: " << createIdIndex;
    NamespaceString nss{ns};

    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    invariant(!options.isView());

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
            std::string msg = str::stream()
                << "Attempted to create a new collection " << nss.ns() << " without a UUID";
            severe() << msg;
            uasserted(ErrorCodes::InvalidOptions, msg);
        }
        if (canAcceptWrites) {
            optionsWithUUID.uuid.emplace(CollectionUUID::gen());
            generatedUUID = true;
        }
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

    BSONObj idIndexSpec =
        !idIndex.isEmpty() ? idIndex : buildDefaultIdIndexSpec(opCtx, nss, options);

    StatusWith<BSONObj> statusWithSpec =
        index_check::prepareSpecForCreate(opCtx, nss, options.collation, idIndexSpec);
    Status status = statusWithSpec.getStatus();
    uassertStatusOK(status);


    BSONObj specAfterCheck = statusWithSpec.getValue();

    // txservice create table here.
    status = _dbEntry->createCollection(opCtx, nss, optionsWithUUID, specAfterCheck);
    uassertStatusOK(status);

    _dbEntry->createKVCollectionCatalogEntry(opCtx, nss.toStringData());

    // opCtx->recoveryUnit()->registerChange(new AddCollectionChange(opCtx, this, ns));

    Collection* collection = _createCollectionHandler(opCtx, nss, createIdIndex, specAfterCheck);
    invariant(collection);
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

    auto const serviceContext = opCtx->getServiceContext();

    for (const auto& [name, coll] : db->collections(opCtx)) {
        Top::get(serviceContext).collectionDropped(coll->ns().ns(), true);
    }

    DatabaseHolder::getDatabaseHolder().close(opCtx, name, "database dropped");

    auto const storageEngine = serviceContext->getStorageEngine();
    writeConflictRetry(opCtx, "dropDatabase", name, [&] {
        storageEngine->dropDatabase(opCtx, name).transitional_ignore();
    });
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
                                    << maxModelLength << " characters.");
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
                      << collectionNameModel << " after " << numGenerationAttempts
                      << " attempts due to namespace conflicts with existing collections.");
}

DatabaseImpl::CollectionMapView& DatabaseImpl::collections(OperationContext* opCtx) {
    MONGO_LOG(1) << "DatabaseImpl::collections";

    std::vector<std::string> collectionInStorageEngine;
    _dbEntry->getCollectionNamespaces(collectionInStorageEngine);

    _collectionsView.clear();

    for (auto& collectionName : collectionInStorageEngine) {
        NamespaceString nss{std::move(collectionName)};
        MONGO_LOG(1) << "nss: " << nss;
        _createCollectionHandler(opCtx, nss, false, BSONObj{}, true);
    }

    return _collectionsView;
}


MONGO_REGISTER_SHIM(Database::dropDatabase)(OperationContext* opCtx, Database* db)->void {
    return DatabaseImpl::dropDatabase(opCtx, db);
}

MONGO_REGISTER_SHIM(Database::userCreateNS)
(OperationContext* opCtx,
 Database* db,
 StringData ns,
 CollectionOptions collectionOptions,
 bool createDefaultIndexes,
 const BSONObj& idIndex)
    ->Status {
    invariant(db);

    LOG(1) << "create collection " << ns << ' ' << collectionOptions.toBSON();

    if (!NamespaceString::validCollectionComponent(ns))
        return Status(ErrorCodes::InvalidNamespace, str::stream() << "invalid ns: " << ns);

    Collection* collection = db->getCollection(opCtx, ns);

    if (collection)
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a collection '" << ns.toString() << "' already exists");

    if (db->getViewCatalog()->lookup(opCtx, ns))
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a view '" << ns.toString() << "' already exists");

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

        // If the feature compatibility version is not 4.0, and we are validating features as
        // master, ban the use of new agg features introduced in 4.0 to prevent them from being
        // persisted in the catalog.
        if (serverGlobalParams.validateFeaturesAsMaster.load() &&
            currentFCV != ServerGlobalParams::FeatureCompatibility::Version::kFullyUpgradedTo40) {
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

    // See https://www.mongodb.com/docs/v6.0/reference/command/create/
    // `autoIndexId` field has been deprecated.
    if (collectionOptions.autoIndexId == CollectionOptions::NO) {
        return {ErrorCodes::BadValue, "Unsupported value for autoIndexId field: false."};
    }

    if (collectionOptions.isView()) {
        uassertStatusOK(db->createView(opCtx, ns, collectionOptions));
    } else {
        invariant(
            db->createCollection(opCtx, ns, collectionOptions, createDefaultIndexes, idIndex));
    }

    return Status::OK();
}

MONGO_REGISTER_SHIM(Database::dropAllDatabasesExceptLocal)(OperationContext* opCtx)->void {
    Lock::GlobalWrite lk(opCtx);

    std::vector<std::string> dbs;
    StorageEngine* storageEngine = opCtx->getServiceContext()->getStorageEngine();
    storageEngine->listDatabases(&dbs);

    if (dbs.size() == 0)
        return;
    log() << "dropAllDatabasesExceptLocal " << dbs.size();

    repl::ReplicationCoordinator::get(opCtx)->dropAllSnapshots();

    for (const auto& dbName : dbs) {
        if (dbName != "local") {
            writeConflictRetry(opCtx, "dropAllDatabasesExceptLocal", dbName, [&opCtx, &dbName] {
                Database* db = DatabaseHolder::getDatabaseHolder().get(opCtx, dbName);

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
}  // namespace mongo
