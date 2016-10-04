// kv_database_catalog_entry.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/db/storage/kv/kv_database_catalog_entry.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/recovery_unit.h"

namespace mongo {

using std::string;
using std::vector;

class KVDatabaseCatalogEntry::AddCollectionChange : public RecoveryUnit::Change {
public:
    AddCollectionChange(OperationContext* opCtx,
                        KVDatabaseCatalogEntry* dce,
                        StringData collection,
                        StringData ident,
                        bool dropOnRollback)
        : _opCtx(opCtx),
          _dce(dce),
          _collection(collection.toString()),
          _ident(ident.toString()),
          _dropOnRollback(dropOnRollback) {}

    virtual void commit() {}
    virtual void rollback() {
        if (_dropOnRollback) {
            // Intentionally ignoring failure
            _dce->_engine->getEngine()->dropIdent(_opCtx, _ident);
        }

        const CollectionMap::iterator it = _dce->_collections.find(_collection);
        if (it != _dce->_collections.end()) {
            delete it->second;
            _dce->_collections.erase(it);
        }
    }

    OperationContext* const _opCtx;
    KVDatabaseCatalogEntry* const _dce;
    const std::string _collection;
    const std::string _ident;
    const bool _dropOnRollback;
};

class KVDatabaseCatalogEntry::RemoveCollectionChange : public RecoveryUnit::Change {
public:
    RemoveCollectionChange(OperationContext* opCtx,
                           KVDatabaseCatalogEntry* dce,
                           StringData collection,
                           StringData ident,
                           KVCollectionCatalogEntry* entry,
                           bool dropOnCommit)
        : _opCtx(opCtx),
          _dce(dce),
          _collection(collection.toString()),
          _ident(ident.toString()),
          _entry(entry),
          _dropOnCommit(dropOnCommit) {}

    virtual void commit() {
        delete _entry;

        // Intentionally ignoring failure here. Since we've removed the metadata pointing to the
        // collection, we should never see it again anyway.
        if (_dropOnCommit)
            _dce->_engine->getEngine()->dropIdent(_opCtx, _ident);
    }

    virtual void rollback() {
        _dce->_collections[_collection] = _entry;
    }

    OperationContext* const _opCtx;
    KVDatabaseCatalogEntry* const _dce;
    const std::string _collection;
    const std::string _ident;
    KVCollectionCatalogEntry* const _entry;
    const bool _dropOnCommit;
};

KVDatabaseCatalogEntry::KVDatabaseCatalogEntry(StringData db, KVStorageEngine* engine)
    : DatabaseCatalogEntry(db), _engine(engine) {}

KVDatabaseCatalogEntry::~KVDatabaseCatalogEntry() {
    for (CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it) {
        delete it->second;
    }
    _collections.clear();
}

bool KVDatabaseCatalogEntry::exists() const {
    return !isEmpty();
}

bool KVDatabaseCatalogEntry::isEmpty() const {
    return _collections.empty();
}

bool KVDatabaseCatalogEntry::hasUserData() const {
    return !isEmpty();
}

int64_t KVDatabaseCatalogEntry::sizeOnDisk(OperationContext* opCtx) const {
    int64_t size = 0;

    for (CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it) {
        const KVCollectionCatalogEntry* coll = it->second;
        if (!coll)
            continue;
        size += coll->getRecordStore()->storageSize(opCtx);

        vector<string> indexNames;
        coll->getAllIndexes(opCtx, &indexNames);

        for (size_t i = 0; i < indexNames.size(); i++) {
            string ident =
                _engine->getCatalog()->getIndexIdent(opCtx, coll->ns().ns(), indexNames[i]);
            size += _engine->getEngine()->getIdentSize(opCtx, ident);
        }
    }

    return size;
}

void KVDatabaseCatalogEntry::appendExtraStats(OperationContext* opCtx,
                                              BSONObjBuilder* out,
                                              double scale) const {}

Status KVDatabaseCatalogEntry::currentFilesCompatible(OperationContext* opCtx) const {
    // Delegate to the FeatureTracker as to whether the data files are compatible or not.
    return _engine->getCatalog()->getFeatureTracker()->isCompatibleWithCurrentCode(opCtx);
}

void KVDatabaseCatalogEntry::getCollectionNamespaces(std::list<std::string>* out) const {
    for (CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it) {
        out->push_back(it->first);
    }
}

CollectionCatalogEntry* KVDatabaseCatalogEntry::getCollectionCatalogEntry(StringData ns) const {
    CollectionMap::const_iterator it = _collections.find(ns.toString());
    if (it == _collections.end()) {
        return NULL;
    }

    return it->second;
}

RecordStore* KVDatabaseCatalogEntry::getRecordStore(StringData ns) const {
    CollectionMap::const_iterator it = _collections.find(ns.toString());
    if (it == _collections.end()) {
        return NULL;
    }

    return it->second->getRecordStore();
}

Status KVDatabaseCatalogEntry::createCollection(OperationContext* txn,
                                                StringData ns,
                                                const CollectionOptions& options,
                                                bool allocateDefaultSpace) {
    invariant(txn->lockState()->isDbLockedForMode(name(), MODE_X));

    if (ns.empty()) {
        return Status(ErrorCodes::BadValue, "Collection namespace cannot be empty");
    }

    if (_collections.count(ns.toString())) {
        invariant(_collections[ns.toString()]);
        return Status(ErrorCodes::NamespaceExists, "collection already exists");
    }

    // need to create it
    Status status = _engine->getCatalog()->newCollection(txn, ns, options);
    if (!status.isOK())
        return status;

    string ident = _engine->getCatalog()->getCollectionIdent(ns);

    status = _engine->getEngine()->createRecordStore(txn, ns, ident, options);
    if (!status.isOK())
        return status;

    RecordStore* rs = _engine->getEngine()->getRecordStore(txn, ns, ident, options);
    invariant(rs);

    // Mark collation feature as in use if the collection has a non-simple default collation.
    if (!options.collation.isEmpty()) {
        const auto feature = KVCatalog::FeatureTracker::NonRepairableFeature::kCollation;
        if (_engine->getCatalog()->getFeatureTracker()->isNonRepairableFeatureInUse(txn, feature)) {
            _engine->getCatalog()->getFeatureTracker()->markNonRepairableFeatureAsInUse(txn,
                                                                                        feature);
        }
    }

    txn->recoveryUnit()->registerChange(new AddCollectionChange(txn, this, ns, ident, true));
    _collections[ns.toString()] =
        new KVCollectionCatalogEntry(_engine->getEngine(), _engine->getCatalog(), ns, ident, rs);

    return Status::OK();
}

void KVDatabaseCatalogEntry::initCollection(OperationContext* opCtx,
                                            const std::string& ns,
                                            bool forRepair) {
    invariant(!_collections.count(ns));

    const std::string ident = _engine->getCatalog()->getCollectionIdent(ns);

    RecordStore* rs;
    if (forRepair) {
        // Using a NULL rs since we don't want to open this record store before it has been
        // repaired. This also ensures that if we try to use it, it will blow up.
        rs = NULL;
    } else {
        BSONCollectionCatalogEntry::MetaData md = _engine->getCatalog()->getMetaData(opCtx, ns);
        rs = _engine->getEngine()->getRecordStore(opCtx, ns, ident, md.options);
        invariant(rs);
    }

    // No change registration since this is only for committed collections
    _collections[ns] =
        new KVCollectionCatalogEntry(_engine->getEngine(), _engine->getCatalog(), ns, ident, rs);
}

void KVDatabaseCatalogEntry::reinitCollectionAfterRepair(OperationContext* opCtx,
                                                         const std::string& ns) {
    // Get rid of the old entry.
    CollectionMap::iterator it = _collections.find(ns);
    invariant(it != _collections.end());
    delete it->second;
    _collections.erase(it);

    // Now reopen fully initialized.
    initCollection(opCtx, ns, false);
}

Status KVDatabaseCatalogEntry::renameCollection(OperationContext* txn,
                                                StringData fromNS,
                                                StringData toNS,
                                                bool stayTemp) {
    invariant(txn->lockState()->isDbLockedForMode(name(), MODE_X));

    RecordStore* originalRS = NULL;

    CollectionMap::const_iterator it = _collections.find(fromNS.toString());
    if (it == _collections.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "rename cannot find collection");
    }

    originalRS = it->second->getRecordStore();

    it = _collections.find(toNS.toString());
    if (it != _collections.end()) {
        return Status(ErrorCodes::NamespaceExists, "for rename to already exists");
    }

    const std::string identFrom = _engine->getCatalog()->getCollectionIdent(fromNS);

    Status status = _engine->getEngine()->okToRename(txn, fromNS, toNS, identFrom, originalRS);
    if (!status.isOK())
        return status;

    status = _engine->getCatalog()->renameCollection(txn, fromNS, toNS, stayTemp);
    if (!status.isOK())
        return status;

    const std::string identTo = _engine->getCatalog()->getCollectionIdent(toNS);

    invariant(identFrom == identTo);

    BSONCollectionCatalogEntry::MetaData md = _engine->getCatalog()->getMetaData(txn, toNS);
    RecordStore* rs = _engine->getEngine()->getRecordStore(txn, toNS, identTo, md.options);

    const CollectionMap::iterator itFrom = _collections.find(fromNS.toString());
    invariant(itFrom != _collections.end());
    txn->recoveryUnit()->registerChange(
        new RemoveCollectionChange(txn, this, fromNS, identFrom, itFrom->second, false));
    _collections.erase(itFrom);

    txn->recoveryUnit()->registerChange(new AddCollectionChange(txn, this, toNS, identTo, false));
    _collections[toNS.toString()] = new KVCollectionCatalogEntry(
        _engine->getEngine(), _engine->getCatalog(), toNS, identTo, rs);

    return Status::OK();
}

Status KVDatabaseCatalogEntry::dropCollection(OperationContext* opCtx, StringData ns) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));

    CollectionMap::const_iterator it = _collections.find(ns.toString());
    if (it == _collections.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "cannnot find collection to drop");
    }

    KVCollectionCatalogEntry* const entry = it->second;

    invariant(entry->getTotalIndexCount(opCtx) == entry->getCompletedIndexCount(opCtx));

    {
        std::vector<std::string> indexNames;
        entry->getAllIndexes(opCtx, &indexNames);
        for (size_t i = 0; i < indexNames.size(); i++) {
            entry->removeIndex(opCtx, indexNames[i]);
        }
    }

    invariant(entry->getTotalIndexCount(opCtx) == 0);

    const std::string ident = _engine->getCatalog()->getCollectionIdent(ns);

    Status status = _engine->getCatalog()->dropCollection(opCtx, ns);
    if (!status.isOK()) {
        return status;
    }

    // This will lazily delete the KVCollectionCatalogEntry and notify the storageEngine to
    // drop the collection only on WUOW::commit().
    opCtx->recoveryUnit()->registerChange(
        new RemoveCollectionChange(opCtx, this, ns, ident, it->second, true));

    _collections.erase(ns.toString());

    return Status::OK();
}
}
