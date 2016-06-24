// database.cpp

/**
*    Copyright (C) 2008-2014 MongoDB Inc.
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

#include "mongo/db/catalog/database.h"

#include <algorithm>
#include <boost/filesystem/operations.hpp>
#include <memory>

#include "mongo/db/audit.h"
#include "mongo/db/auth/auth_index_d.h"
#include "mongo/db/background.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/database_catalog_entry.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/views/view_catalog.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"

namespace mongo {

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

class Database::AddCollectionChange : public RecoveryUnit::Change {
public:
    AddCollectionChange(OperationContext* txn, Database* db, StringData ns)
        : _txn(txn), _db(db), _ns(ns.toString()) {}

    virtual void commit() {
        CollectionMap::const_iterator it = _db->_collections.find(_ns);
        if (it == _db->_collections.end())
            return;

        // Ban reading from this collection on committed reads on snapshots before now.
        auto replCoord = repl::ReplicationCoordinator::get(_txn);
        auto snapshotName = replCoord->reserveSnapshotName(_txn);
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

    OperationContext* const _txn;
    Database* const _db;
    const std::string _ns;
};

class Database::RemoveCollectionChange : public RecoveryUnit::Change {
public:
    // Takes ownership of coll (but not db).
    RemoveCollectionChange(Database* db, Collection* coll) : _db(db), _coll(coll) {}

    virtual void commit() {
        delete _coll;
    }

    virtual void rollback() {
        Collection*& inMap = _db->_collections[_coll->ns().ns()];
        invariant(!inMap);
        inMap = _coll;
    }

    Database* const _db;
    Collection* const _coll;
};

Database::~Database() {
    for (CollectionMap::const_iterator i = _collections.begin(); i != _collections.end(); ++i)
        delete i->second;
}

void Database::close(OperationContext* txn) {
    // XXX? - Do we need to close database under global lock or just DB-lock is sufficient ?
    invariant(txn->lockState()->isW());
    // oplog caches some things, dirty its caches
    repl::oplogCheckCloseDatabase(txn, this);

    if (BackgroundOperation::inProgForDb(_name)) {
        log() << "warning: bg op in prog during close db? " << _name << endl;
    }
}

Status Database::validateDBName(StringData dbname) {
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

Collection* Database::_getOrCreateCollectionInstance(OperationContext* txn, StringData fullns) {
    Collection* collection = getCollection(fullns);
    if (collection) {
        return collection;
    }

    unique_ptr<CollectionCatalogEntry> cce(_dbEntry->getCollectionCatalogEntry(fullns));
    invariant(cce.get());

    unique_ptr<RecordStore> rs(_dbEntry->getRecordStore(fullns));
    invariant(rs.get());  // if cce exists, so should this

    // Not registering AddCollectionChange since this is for collections that already exist.
    Collection* c = new Collection(txn, fullns, cce.release(), rs.release(), _dbEntry);
    return c;
}

Database::Database(OperationContext* txn, StringData name, DatabaseCatalogEntry* dbEntry)
    : _name(name.toString()),
      _dbEntry(dbEntry),
      _profileName(_name + ".system.profile"),
      _indexesName(_name + ".system.indexes"),
      _viewsName(_name + ".system.views"),
      _views(txn, this) {
    Status status = validateDBName(_name);
    if (!status.isOK()) {
        warning() << "tried to open invalid db: " << _name << endl;
        uasserted(10028, status.toString());
    }

    _profile = serverGlobalParams.defaultProfile;

    list<string> collections;
    _dbEntry->getCollectionNamespaces(&collections);
    for (list<string>::const_iterator it = collections.begin(); it != collections.end(); ++it) {
        const string ns = *it;
        _collections[ns] = _getOrCreateCollectionInstance(txn, ns);
    }
}


/*static*/
string Database::duplicateUncasedName(const string& name, set<string>* duplicates) {
    if (duplicates) {
        duplicates->clear();
    }

    set<string> allShortNames;
    dbHolder().getAllShortNames(allShortNames);

    for (const auto& dbname : allShortNames) {
        if (strcasecmp(dbname.c_str(), name.c_str()))
            continue;

        if (strcmp(dbname.c_str(), name.c_str()) == 0)
            continue;

        if (duplicates) {
            duplicates->insert(dbname);
        } else {
            return dbname;
        }
    }
    if (duplicates) {
        return duplicates->empty() ? "" : *duplicates->begin();
    }
    return "";
}

void Database::clearTmpCollections(OperationContext* txn) {
    invariant(txn->lockState()->isDbLockedForMode(name(), MODE_X));

    list<string> collections;
    _dbEntry->getCollectionNamespaces(&collections);

    for (list<string>::iterator i = collections.begin(); i != collections.end(); ++i) {
        string ns = *i;
        invariant(NamespaceString::normal(ns));

        CollectionCatalogEntry* coll = _dbEntry->getCollectionCatalogEntry(ns);

        CollectionOptions options = coll->getCollectionOptions(txn);
        if (!options.temp)
            continue;
        try {
            WriteUnitOfWork wunit(txn);
            Status status = dropCollection(txn, ns);
            if (!status.isOK()) {
                warning() << "could not drop temp collection '" << ns << "': " << status;
                continue;
            }

            wunit.commit();
        } catch (const WriteConflictException& exp) {
            warning() << "could not drop temp collection '" << ns << "' due to "
                                                                     "WriteConflictException";
            txn->recoveryUnit()->abandonSnapshot();
        }
    }
}

Status Database::setProfilingLevel(OperationContext* txn, int newLevel) {
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

    Status status = createProfileCollection(txn, this);
    if (!status.isOK()) {
        return status;
    }

    _profile = newLevel;

    return Status::OK();
}

void Database::getStats(OperationContext* opCtx, BSONObjBuilder* output, double scale) {
    list<string> collections;
    _dbEntry->getCollectionNamespaces(&collections);

    long long ncollections = 0;
    long long objects = 0;
    long long size = 0;
    long long storageSize = 0;
    long long numExtents = 0;
    long long indexes = 0;
    long long indexSize = 0;

    for (list<string>::const_iterator it = collections.begin(); it != collections.end(); ++it) {
        const string ns = *it;

        Collection* collection = getCollection(ns);
        if (!collection)
            continue;

        ncollections += 1;
        objects += collection->numRecords(opCtx);
        size += collection->dataSize(opCtx);

        BSONObjBuilder temp;
        storageSize += collection->getRecordStore()->storageSize(opCtx, &temp);
        numExtents += temp.obj()["numExtents"].numberInt();  // XXX

        indexes += collection->getIndexCatalog()->numIndexesTotal(opCtx);
        indexSize += collection->getIndexSize(opCtx);
    }

    output->appendNumber("collections", ncollections);
    output->appendNumber("objects", objects);
    output->append("avgObjSize", objects == 0 ? 0 : double(size) / double(objects));
    output->appendNumber("dataSize", size / scale);
    output->appendNumber("storageSize", storageSize / scale);
    output->appendNumber("numExtents", numExtents);
    output->appendNumber("indexes", indexes);
    output->appendNumber("indexSize", indexSize / scale);

    _dbEntry->appendExtraStats(opCtx, output, scale);
}

Status Database::dropCollection(OperationContext* txn, StringData fullns) {
    invariant(txn->lockState()->isDbLockedForMode(name(), MODE_X));

    LOG(1) << "dropCollection: " << fullns << endl;
    massertNamespaceNotIndex(fullns, "dropCollection");

    Collection* collection = getCollection(fullns);
    if (!collection) {
        // collection doesn't exist
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
            } else {
                return Status(ErrorCodes::IllegalOperation, "can't drop system ns");
            }
        }
    }

    BackgroundOperation::assertNoBgOpInProgForNs(fullns);

    audit::logDropCollection(&cc(), fullns);

    Status s = collection->getIndexCatalog()->dropAllIndexes(txn, true);
    if (!s.isOK()) {
        warning() << "could not drop collection, trying to drop indexes" << fullns << " because of "
                  << s.toString();
        return s;
    }

    verify(collection->_details->getTotalIndexCount(txn) == 0);
    LOG(1) << "\t dropIndexes done" << endl;

    Top::get(txn->getClient()->getServiceContext()).collectionDropped(fullns);

    s = _dbEntry->dropCollection(txn, fullns);

    // we want to do this always
    _clearCollectionCache(txn, fullns, "collection dropped");

    if (!s.isOK())
        return s;

    DEV {
        // check all index collection entries are gone
        string nstocheck = fullns.toString() + ".$";
        for (CollectionMap::const_iterator i = _collections.begin(); i != _collections.end(); ++i) {
            string temp = i->first;
            if (temp.find(nstocheck) != 0)
                continue;
            log() << "after drop, bad cache entries for: " << fullns << " have " << temp;
            verify(0);
        }
    }

    auto opObserver = getGlobalServiceContext()->getOpObserver();
    if (opObserver)
        opObserver->onDropCollection(txn, nss);

    return Status::OK();
}

void Database::_clearCollectionCache(OperationContext* txn,
                                     StringData fullns,
                                     const std::string& reason) {
    verify(_name == nsToDatabaseSubstring(fullns));
    CollectionMap::const_iterator it = _collections.find(fullns.toString());
    if (it == _collections.end())
        return;

    // Takes ownership of the collection
    txn->recoveryUnit()->registerChange(new RemoveCollectionChange(this, it->second));

    it->second->_cursorManager.invalidateAll(false, reason);
    _collections.erase(it);
}

Collection* Database::getCollection(StringData ns) const {
    invariant(_name == nsToDatabaseSubstring(ns));
    CollectionMap::const_iterator it = _collections.find(ns);
    if (it != _collections.end() && it->second) {
        return it->second;
    }

    return NULL;
}

Status Database::renameCollection(OperationContext* txn,
                                  StringData fromNS,
                                  StringData toNS,
                                  bool stayTemp) {
    audit::logRenameCollection(&cc(), fromNS, toNS);
    invariant(txn->lockState()->isDbLockedForMode(name(), MODE_X));
    BackgroundOperation::assertNoBgOpInProgForNs(fromNS);
    BackgroundOperation::assertNoBgOpInProgForNs(toNS);

    {  // remove anything cached
        Collection* coll = getCollection(fromNS);
        if (!coll)
            return Status(ErrorCodes::NamespaceNotFound, "collection not found to rename");

        string clearCacheReason = str::stream() << "renamed collection '" << fromNS << "' to '"
                                                << toNS << "'";
        IndexCatalog::IndexIterator ii = coll->getIndexCatalog()->getIndexIterator(txn, true);
        while (ii.more()) {
            IndexDescriptor* desc = ii.next();
            _clearCollectionCache(txn, desc->indexNamespace(), clearCacheReason);
        }

        _clearCollectionCache(txn, fromNS, clearCacheReason);
        _clearCollectionCache(txn, toNS, clearCacheReason);

        Top::get(txn->getClient()->getServiceContext()).collectionDropped(fromNS.toString());
    }

    txn->recoveryUnit()->registerChange(new AddCollectionChange(txn, this, toNS));
    Status s = _dbEntry->renameCollection(txn, fromNS, toNS, stayTemp);
    _collections[toNS] = _getOrCreateCollectionInstance(txn, toNS);
    return s;
}

Collection* Database::getOrCreateCollection(OperationContext* txn, StringData ns) {
    Collection* c = getCollection(ns);
    if (!c) {
        c = createCollection(txn, ns);
    }
    return c;
}

void Database::_checkCanCreateCollection(const NamespaceString& nss,
                                         const CollectionOptions& options) {
    massert(17399, "collection already exists", getCollection(nss.ns()) == nullptr);
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
}

Status Database::createView(OperationContext* txn,
                            StringData ns,
                            const CollectionOptions& options) {
    invariant(txn->lockState()->isDbLockedForMode(name(), MODE_X));
    invariant(options.isView());

    NamespaceString nss(ns);
    NamespaceString viewOnNss(nss.db(), options.viewOn);
    _checkCanCreateCollection(nss, options);
    audit::logCreateCollection(&cc(), ns);

    if (nss.isOplog())
        return Status(ErrorCodes::InvalidNamespace,
                      str::stream() << "invalid namespace name for a view: " + nss.toString());

    return _views.createView(txn, nss, viewOnNss, options.pipeline);
}


Collection* Database::createCollection(OperationContext* txn,
                                       StringData ns,
                                       const CollectionOptions& options,
                                       bool createIdIndex) {
    invariant(txn->lockState()->isDbLockedForMode(name(), MODE_X));
    invariant(!options.isView());

    NamespaceString nss(ns);
    _checkCanCreateCollection(nss, options);
    audit::logCreateCollection(&cc(), ns);

    txn->recoveryUnit()->registerChange(new AddCollectionChange(txn, this, ns));

    Status status = _dbEntry->createCollection(txn, ns, options, true /*allocateDefaultSpace*/);
    massertNoTraceStatusOK(status);


    Collection* collection = _getOrCreateCollectionInstance(txn, ns);
    invariant(collection);
    _collections[ns] = collection;

    if (createIdIndex) {
        if (collection->requiresIdIndex()) {
            if (options.autoIndexId == CollectionOptions::YES ||
                options.autoIndexId == CollectionOptions::DEFAULT) {
                IndexCatalog* ic = collection->getIndexCatalog();
                uassertStatusOK(ic->createIndexOnEmptyCollection(txn, ic->getDefaultIdIndexSpec()));
            }
        }

        if (nss.isSystem()) {
            authindex::createSystemIndexes(txn, collection);
        }
    }

    auto opObserver = getGlobalServiceContext()->getOpObserver();
    if (opObserver)
        opObserver->onCreateCollection(txn, nss, options);

    return collection;
}

const DatabaseCatalogEntry* Database::getDatabaseCatalogEntry() const {
    return _dbEntry;
}

void dropAllDatabasesExceptLocal(OperationContext* txn) {
    ScopedTransaction transaction(txn, MODE_X);
    Lock::GlobalWrite lk(txn->lockState());

    vector<string> n;
    StorageEngine* storageEngine = getGlobalServiceContext()->getGlobalStorageEngine();
    storageEngine->listDatabases(&n);

    if (n.size() == 0)
        return;
    log() << "dropAllDatabasesExceptLocal " << n.size() << endl;

    repl::getGlobalReplicationCoordinator()->dropAllSnapshots();
    for (vector<string>::iterator i = n.begin(); i != n.end(); i++) {
        if (*i != "local") {
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                Database* db = dbHolder().get(txn, *i);
                // This is needed since dropDatabase can't be rolled back.
                // This is safe be replaced by "invariant(db);dropDatabase(txn, db);" once fixed
                if (db == nullptr) {
                    log() << "database disappeared after listDatabases but before drop: " << *i;
                } else {
                    Database::dropDatabase(txn, db);
                }
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "dropAllDatabasesExceptLocal", *i);
        }
    }
}

void Database::dropDatabase(OperationContext* txn, Database* db) {
    invariant(db);

    // Store the name so we have if for after the db object is deleted
    const string name = db->name();
    LOG(1) << "dropDatabase " << name;

    invariant(txn->lockState()->isDbLockedForMode(name, MODE_X));

    BackgroundOperation::assertNoBgOpInProgForDb(name);

    audit::logDropDatabase(txn->getClient(), name);

    dbHolder().close(txn, name);
    db = NULL;  // d is now deleted

    getGlobalServiceContext()->getGlobalStorageEngine()->dropDatabase(txn, name);
}

/** { ..., capped: true, size: ..., max: ... }
 * @param createDefaultIndexes - if false, defers id (and other) index creation.
 * @return true if successful
*/
Status userCreateNS(OperationContext* txn,
                    Database* db,
                    StringData ns,
                    BSONObj options,
                    bool createDefaultIndexes) {
    invariant(db);

    LOG(1) << "create collection " << ns << ' ' << options;

    if (!NamespaceString::validCollectionComponent(ns))
        return Status(ErrorCodes::InvalidNamespace, str::stream() << "invalid ns: " << ns);

    Collection* collection = db->getCollection(ns);

    if (collection)
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a collection '" << ns.toString() << "' already exists");

    if (db->getViewCatalog()->lookup(ns))
        return Status(ErrorCodes::NamespaceExists,
                      str::stream() << "a view '" << ns.toString() << "' already exists");

    CollectionOptions collectionOptions;
    Status status = collectionOptions.parse(options);
    if (!status.isOK())
        return status;

    // Validate the collation, if there is one.
    if (!collectionOptions.collation.isEmpty()) {
        auto collator = CollatorFactoryInterface::get(txn->getServiceContext())
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
        uassertStatusOK(db->createView(txn, ns, collectionOptions));
    } else {
        invariant(db->createCollection(txn, ns, collectionOptions, createDefaultIndexes));
    }

    return Status::OK();
}
}  // namespace mongo
