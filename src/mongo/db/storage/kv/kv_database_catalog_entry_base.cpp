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

#include <memory>

#include "mongo/db/storage/kv/kv_database_catalog_entry.h"

#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/storage/kv/kv_catalog_feature_tracker.h"
#include "mongo/db/storage/kv/kv_collection_catalog_entry.h"
#include "mongo/db/storage/kv/kv_engine.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/log.h"

namespace mongo {

KVDatabaseCatalogEntryBase::KVDatabaseCatalogEntryBase(StringData db,
                                                       KVStorageEngineInterface* engine)
    : DatabaseCatalogEntry(db), _engine(engine) {}

KVDatabaseCatalogEntryBase::~KVDatabaseCatalogEntryBase() {
    for (CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it) {
        delete it->second;
    }
    _collections.clear();
}

bool KVDatabaseCatalogEntryBase::exists() const {
    return !isEmpty();
}

bool KVDatabaseCatalogEntryBase::isEmpty() const {
    return _collections.empty();
}

bool KVDatabaseCatalogEntryBase::hasUserData() const {
    return !isEmpty();
}

int64_t KVDatabaseCatalogEntryBase::sizeOnDisk(OperationContext* opCtx) const {
    int64_t size = 0;

    for (CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it) {
        const KVCollectionCatalogEntry* coll = it->second;
        if (!coll)
            continue;
        size += coll->getRecordStore()->storageSize(opCtx);

        std::vector<std::string> indexNames;
        coll->getAllIndexes(opCtx, &indexNames);

        for (size_t i = 0; i < indexNames.size(); i++) {
            std::string ident =
                _engine->getCatalog()->getIndexIdent(opCtx, coll->ns().ns(), indexNames[i]);
            size += _engine->getEngine()->getIdentSize(opCtx, ident);
        }
    }

    return size;
}

void KVDatabaseCatalogEntryBase::appendExtraStats(OperationContext* opCtx,
                                                  BSONObjBuilder* out,
                                                  double scale) const {}

Status KVDatabaseCatalogEntryBase::currentFilesCompatible(OperationContext* opCtx) const {
    // Delegate to the FeatureTracker as to whether the data files are compatible or not.
    return _engine->getCatalog()->getFeatureTracker()->isCompatibleWithCurrentCode(opCtx);
}

void KVDatabaseCatalogEntryBase::getCollectionNamespaces(std::list<std::string>* out) const {
    for (CollectionMap::const_iterator it = _collections.begin(); it != _collections.end(); ++it) {
        out->push_back(it->first);
    }
}

CollectionCatalogEntry* KVDatabaseCatalogEntryBase::getCollectionCatalogEntry(StringData ns) const {
    CollectionMap::const_iterator it = _collections.find(ns.toString());
    if (it == _collections.end()) {
        return NULL;
    }

    return it->second;
}

RecordStore* KVDatabaseCatalogEntryBase::getRecordStore(StringData ns) const {
    CollectionMap::const_iterator it = _collections.find(ns.toString());
    if (it == _collections.end()) {
        return NULL;
    }

    return it->second->getRecordStore();
}

Status KVDatabaseCatalogEntryBase::createCollection(OperationContext* opCtx,
                                                    const NamespaceString& nss,
                                                    const CollectionOptions& options,
                                                    bool allocateDefaultSpace) {
    // TODO(SERVER-39520): Once createCollection does not need database IX lock, 'system.views' will
    // be no longer a special case.
    if (nss.coll().startsWith("system.views")) {
        dassert(opCtx->lockState()->isDbLockedForMode(name(), MODE_IX));
    } else {
        dassert(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    }

    invariant(nss.coll().size() > 0);

    if (_collections.count(nss.toString())) {
        invariant(_collections[nss.toString()]);
        return Status(ErrorCodes::NamespaceExists, "collection already exists");
    }

    KVPrefix prefix = KVPrefix::getNextPrefix(nss);

    // need to create it
    Status status = _engine->getCatalog()->newCollection(opCtx, nss, options, prefix);
    if (!status.isOK())
        return status;

    std::string ident = _engine->getCatalog()->getCollectionIdent(nss.ns());

    status =
        _engine->getEngine()->createGroupedRecordStore(opCtx, nss.ns(), ident, options, prefix);
    if (!status.isOK())
        return status;

    // Mark collation feature as in use if the collection has a non-simple default collation.
    if (!options.collation.isEmpty()) {
        const auto feature = KVCatalog::FeatureTracker::NonRepairableFeature::kCollation;
        if (_engine->getCatalog()->getFeatureTracker()->isNonRepairableFeatureInUse(opCtx,
                                                                                    feature)) {
            _engine->getCatalog()->getFeatureTracker()->markNonRepairableFeatureAsInUse(opCtx,
                                                                                        feature);
        }
    }

    opCtx->recoveryUnit()->onRollback([ opCtx, dce = this, nss, ident ]() {
        // Intentionally ignoring failure
        dce->_engine->getEngine()->dropIdent(opCtx, ident).ignore();

        const CollectionMap::iterator it = dce->_collections.find(nss.ns());
        if (it != dce->_collections.end()) {
            delete it->second;
            dce->_collections.erase(it);
        }
    });

    auto rs = _engine->getEngine()->getGroupedRecordStore(opCtx, nss.ns(), ident, options, prefix);
    invariant(rs);

    _collections[nss.toString()] = new KVCollectionCatalogEntry(
        _engine, _engine->getCatalog(), nss.ns(), ident, std::move(rs));

    return Status::OK();
}

void KVDatabaseCatalogEntryBase::initCollection(OperationContext* opCtx,
                                                const std::string& ns,
                                                bool forRepair) {
    invariant(!_collections.count(ns));

    const std::string ident = _engine->getCatalog()->getCollectionIdent(ns);

    std::unique_ptr<RecordStore> rs;
    if (forRepair) {
        // Using a NULL rs since we don't want to open this record store before it has been
        // repaired. This also ensures that if we try to use it, it will blow up.
        rs = nullptr;
    } else {
        BSONCollectionCatalogEntry::MetaData md = _engine->getCatalog()->getMetaData(opCtx, ns);
        rs = _engine->getEngine()->getGroupedRecordStore(opCtx, ns, ident, md.options, md.prefix);
        invariant(rs);
    }

    // No change registration since this is only for committed collections
    _collections[ns] =
        new KVCollectionCatalogEntry(_engine, _engine->getCatalog(), ns, ident, std::move(rs));
}

void KVDatabaseCatalogEntryBase::reinitCollectionAfterRepair(OperationContext* opCtx,
                                                             const std::string& ns) {
    // Get rid of the old entry.
    CollectionMap::iterator it = _collections.find(ns);
    invariant(it != _collections.end());
    delete it->second;
    _collections.erase(it);

    // Now reopen fully initialized.
    initCollection(opCtx, ns, false);
}

Status KVDatabaseCatalogEntryBase::renameCollection(OperationContext* opCtx,
                                                    StringData fromNS,
                                                    StringData toNS,
                                                    bool stayTemp) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    const NamespaceString fromNss(fromNS);
    const NamespaceString toNss(toNS);

    CollectionMap::const_iterator it = _collections.find(fromNS.toString());
    if (it == _collections.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "rename cannot find collection");
    }

    RecordStore* originalRS = it->second->getRecordStore();

    it = _collections.find(toNS.toString());
    if (it != _collections.end()) {
        return Status(ErrorCodes::NamespaceExists, "for rename to already exists");
    }

    const std::string identFrom = _engine->getCatalog()->getCollectionIdent(fromNS);

    Status status = _engine->getEngine()->okToRename(opCtx, fromNS, toNS, identFrom, originalRS);
    if (!status.isOK())
        return status;

    status = _engine->getCatalog()->renameCollection(opCtx, fromNS, toNS, stayTemp);
    if (!status.isOK())
        return status;

    const std::string identTo = _engine->getCatalog()->getCollectionIdent(toNS);
    invariant(identFrom == identTo);

    // Add the destination collection to _collections before erasing the source collection. This
    // is to ensure that _collections doesn't erroneously appear empty during listDatabases if
    // a database consists of a single collection and that collection gets renamed (see
    // SERVER-34531). There is no locking to prevent listDatabases from looking into
    // _collections as a rename is taking place.
    auto itFrom = _collections.find(fromNS.toString());
    invariant(itFrom != _collections.end());
    auto* collectionCatalogEntry = itFrom->second;
    invariant(collectionCatalogEntry);
    _collections[toNS.toString()] = collectionCatalogEntry;
    _collections.erase(itFrom);

    collectionCatalogEntry->setNs(toNss);

    // Register a Change which, on rollback, will reinstall the collection catalog entry in the
    // collections map so that it is associated with 'fromNS', not 'toNS'.
    opCtx->recoveryUnit()->onRollback([ dce = this, collectionCatalogEntry, fromNss, toNss ]() {
        auto it = dce->_collections.find(toNss.ns());
        invariant(it != dce->_collections.end());
        invariant(it->second == collectionCatalogEntry);
        dce->_collections[fromNss.ns()] = collectionCatalogEntry;
        dce->_collections.erase(toNss.ns());
        collectionCatalogEntry->setNs(fromNss);

    });

    return Status::OK();
}

Status KVDatabaseCatalogEntryBase::dropCollection(OperationContext* opCtx, StringData ns) {
    invariant(opCtx->lockState()->isDbLockedForMode(name(), MODE_X));
    NamespaceString nss(ns);

    CollectionMap::const_iterator it = _collections.find(nss.toString());
    if (it == _collections.end()) {
        return Status(ErrorCodes::NamespaceNotFound, "cannnot find collection to drop");
    }

    KVCollectionCatalogEntry* const entry = it->second;

    invariant(entry->getTotalIndexCount(opCtx) == entry->getCompletedIndexCount(opCtx));

    {
        std::vector<std::string> indexNames;
        entry->getAllIndexes(opCtx, &indexNames);
        for (size_t i = 0; i < indexNames.size(); i++) {
            entry->removeIndex(opCtx, indexNames[i]).transitional_ignore();
        }
    }

    invariant(entry->getTotalIndexCount(opCtx) == 0);

    BSONCollectionCatalogEntry::MetaData md = _engine->getCatalog()->getMetaData(opCtx, ns);
    OptionalCollectionUUID uuid = md.options.uuid;
    const std::string ident = _engine->getCatalog()->getCollectionIdent(ns);

    Status status = _engine->getCatalog()->dropCollection(opCtx, ns);
    if (!status.isOK()) {
        return status;
    }

    // This will lazily delete the KVCollectionCatalogEntry and notify the storageEngine to
    // drop the collection only on WUOW::commit().
    opCtx->recoveryUnit()->onCommit([ opCtx, dce = this, nss, uuid, ident, entry = it->second ](
        boost::optional<Timestamp> commitTimestamp) {
        delete entry;

        auto engine = dce->_engine;
        auto storageEngine = engine->getStorageEngine();
        if (storageEngine->supportsPendingDrops() && commitTimestamp) {
            log() << "Deferring table drop for collection '" << nss << "' (" << uuid << ")"
                  << ". Ident: " << ident << ", commit timestamp: " << commitTimestamp;
            engine->addDropPendingIdent(*commitTimestamp, nss, ident);
        } else {
            // Intentionally ignoring failure here. Since we've removed the metadata pointing to the
            // collection, we should never see it again anyway.
            auto kvEngine = engine->getEngine();
            kvEngine->dropIdent(opCtx, ident).ignore();
        }
    });

    opCtx->recoveryUnit()->onRollback(
        [ dce = this, nss, entry = it->second ]() { dce->_collections[nss.toString()] = entry; });
    _collections.erase(nss.toString());

    return Status::OK();
}
}  // namespace mongo
