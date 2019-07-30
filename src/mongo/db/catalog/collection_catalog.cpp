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

#include "collection_catalog.h"

#include "mongo/db/catalog/database.h"
#include "mongo/db/concurrency/lock_manager_defs.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/log.h"
#include "mongo/util/uuid.h"

namespace mongo {
namespace {
const ServiceContext::Decoration<CollectionCatalog> getCatalog =
    ServiceContext::declareDecoration<CollectionCatalog>();

class FinishDropCollectionChange : public RecoveryUnit::Change {
public:
    FinishDropCollectionChange(CollectionCatalog* catalog,
                               std::unique_ptr<Collection> coll,
                               CollectionUUID uuid)
        : _catalog(catalog), _coll(std::move(coll)), _uuid(uuid) {}

    void commit(boost::optional<Timestamp>) override {
        _coll.reset();
    }

    void rollback() override {
        _catalog->registerCollection(_uuid, std::move(_coll));
    }

private:
    CollectionCatalog* _catalog;
    std::unique_ptr<Collection> _coll;
    CollectionUUID _uuid;
};

}  // namespace

CollectionCatalog::iterator::iterator(StringData dbName,
                                      uint64_t genNum,
                                      const CollectionCatalog& catalog)
    : _dbName(dbName), _genNum(genNum), _catalog(&catalog) {
    auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();

    stdx::lock_guard<stdx::mutex> lock(_catalog->_catalogLock);
    _mapIter = _catalog->_orderedCollections.lower_bound(std::make_pair(_dbName, minUuid));

    if (_mapIter != _catalog->_orderedCollections.end() && _mapIter->first.first == _dbName) {
        _uuid = _mapIter->first.second;
    }
}

CollectionCatalog::iterator::iterator(
    std::map<std::pair<std::string, CollectionUUID>, Collection*>::const_iterator mapIter)
    : _mapIter(mapIter) {}

const CollectionCatalog::iterator::value_type CollectionCatalog::iterator::operator*() {
    stdx::lock_guard<stdx::mutex> lock(_catalog->_catalogLock);
    _repositionIfNeeded();
    if (_exhausted()) {
        return _nullCollection;
    }

    return _mapIter->second;
}

boost::optional<CollectionUUID> CollectionCatalog::iterator::uuid() {
    return _uuid;
}

CollectionCatalog::iterator CollectionCatalog::iterator::operator++() {
    stdx::lock_guard<stdx::mutex> lock(_catalog->_catalogLock);

    if (!_repositionIfNeeded()) {
        _mapIter++;  // If the position was not updated, increment iterator to next element.
    }

    if (_exhausted()) {
        // If the iterator is at the end of the map or now points to an entry that does not
        // correspond to the correct database.
        _mapIter = _catalog->_orderedCollections.end();
        _uuid = boost::none;
        return *this;
    }

    _uuid = _mapIter->first.second;
    return *this;
}

CollectionCatalog::iterator CollectionCatalog::iterator::operator++(int) {
    auto oldPosition = *this;
    ++(*this);
    return oldPosition;
}

bool CollectionCatalog::iterator::operator==(const iterator& other) {
    stdx::lock_guard<stdx::mutex> lock(_catalog->_catalogLock);

    if (other._mapIter == _catalog->_orderedCollections.end()) {
        return _uuid == boost::none;
    }

    return _uuid == other._uuid;
}

bool CollectionCatalog::iterator::operator!=(const iterator& other) {
    return !(*this == other);
}

bool CollectionCatalog::iterator::_repositionIfNeeded() {
    if (_genNum == _catalog->_generationNumber) {
        return false;
    }

    _genNum = _catalog->_generationNumber;
    // If the map has been modified, find the entry the iterator was on, or the one right after it.
    _mapIter = _catalog->_orderedCollections.lower_bound(std::make_pair(_dbName, *_uuid));

    if (_exhausted()) {
        return true;
    }

    // If the old pair matches the previous DB name and UUID, the iterator was not repositioned.
    auto dbUuidPair = _mapIter->first;
    bool repositioned = !(dbUuidPair.first == _dbName && dbUuidPair.second == _uuid);
    _uuid = dbUuidPair.second;

    return repositioned;
}

bool CollectionCatalog::iterator::_exhausted() {
    return _mapIter == _catalog->_orderedCollections.end() || _mapIter->first.first != _dbName;
}

CollectionCatalog& CollectionCatalog::get(ServiceContext* svcCtx) {
    return getCatalog(svcCtx);
}
CollectionCatalog& CollectionCatalog::get(OperationContext* opCtx) {
    return getCatalog(opCtx->getServiceContext());
}

void CollectionCatalog::setCollectionNamespace(OperationContext* opCtx,
                                               Collection* coll,
                                               const NamespaceString& fromCollection,
                                               const NamespaceString& toCollection) {
    // Rather than maintain, in addition to the UUID -> Collection* mapping, an auxiliary
    // data structure with the UUID -> namespace mapping, the CollectionCatalog relies on
    // Collection::ns() to provide UUID to namespace lookup. In addition, the CollectionCatalog
    // does not require callers to hold locks.
    //
    // This means that Collection::ns() may be called while only '_catalogLock' (and no lock
    // manager locks) are held. The purpose of this function is ensure that we write to the
    // Collection's namespace string under '_catalogLock'.
    invariant(coll);
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);

    coll->setNs(toCollection);

    _collections[toCollection] = _collections[fromCollection];
    _collections.erase(fromCollection);

    ResourceId oldRid = ResourceId(RESOURCE_COLLECTION, fromCollection.ns());
    ResourceId newRid = ResourceId(RESOURCE_COLLECTION, toCollection.ns());

    removeResource(oldRid, fromCollection.ns());
    addResource(newRid, toCollection.ns());

    opCtx->recoveryUnit()->onRollback([this, coll, fromCollection, toCollection] {
        stdx::lock_guard<stdx::mutex> lock(_catalogLock);
        coll->setNs(std::move(fromCollection));

        _collections[fromCollection] = _collections[toCollection];
        _collections.erase(toCollection);

        ResourceId oldRid = ResourceId(RESOURCE_COLLECTION, fromCollection.ns());
        ResourceId newRid = ResourceId(RESOURCE_COLLECTION, toCollection.ns());

        removeResource(newRid, toCollection.ns());
        addResource(oldRid, fromCollection.ns());
    });
}

void CollectionCatalog::onCloseDatabase(OperationContext* opCtx, std::string dbName) {
    invariant(opCtx->lockState()->isW());
    auto rid = ResourceId(RESOURCE_DATABASE, dbName);
    removeResource(rid, dbName);
}

void CollectionCatalog::onCloseCatalog(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    invariant(!_shadowCatalog);
    _shadowCatalog.emplace();
    for (auto& entry : _catalog)
        _shadowCatalog->insert({entry.first, entry.second->ns()});
}

void CollectionCatalog::onOpenCatalog(OperationContext* opCtx) {
    invariant(opCtx->lockState()->isW());
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    invariant(_shadowCatalog);
    _shadowCatalog.reset();
}

Collection* CollectionCatalog::lookupCollectionByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    return _lookupCollectionByUUID(lock, uuid);
}

Collection* CollectionCatalog::_lookupCollectionByUUID(WithLock, CollectionUUID uuid) const {
    auto foundIt = _catalog.find(uuid);
    return foundIt == _catalog.end() ? nullptr : foundIt->second.get();
}

Collection* CollectionCatalog::lookupCollectionByNamespace(const NamespaceString& nss) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto it = _collections.find(nss);
    return it == _collections.end() ? nullptr : it->second;
}

boost::optional<NamespaceString> CollectionCatalog::lookupNSSByUUID(CollectionUUID uuid) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto foundIt = _catalog.find(uuid);
    if (foundIt != _catalog.end()) {
        NamespaceString ns = foundIt->second->ns();
        invariant(!ns.isEmpty());
        return ns;
    }

    // Only in the case that the catalog is closed and a UUID is currently unknown, resolve it
    // using the pre-close state. This ensures that any tasks reloading the catalog can see their
    // own updates.
    if (_shadowCatalog) {
        auto shadowIt = _shadowCatalog->find(uuid);
        if (shadowIt != _shadowCatalog->end())
            return shadowIt->second;
    }
    return boost::none;
}

boost::optional<CollectionUUID> CollectionCatalog::lookupUUIDByNSS(
    const NamespaceString& nss) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto it = _orderedCollections.lower_bound(std::make_pair(nss.db().toString(), minUuid));

    // The entry _mapIter points to is valid if it's not at the end of _orderedCollections and
    // the entry's database is the same as dbName.
    while (it != _orderedCollections.end() && it->first.first == nss.db()) {
        if (it->second->ns() == nss) {
            return it->first.second;
        }
        ++it;
    }
    return boost::none;
}

NamespaceString CollectionCatalog::resolveNamespaceStringOrUUID(NamespaceStringOrUUID nsOrUUID) {
    if (auto& nss = nsOrUUID.nss()) {
        uassert(ErrorCodes::InvalidNamespace,
                str::stream() << "Namespace " << *nss << " is not a valid collection name",
                nss->isValid());
        return std::move(*nss);
    }

    auto resolvedNss = lookupNSSByUUID(*nsOrUUID.uuid());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "Unable to resolve " << nsOrUUID.toString(),
            resolvedNss && resolvedNss->isValid());

    uassert(ErrorCodes::NamespaceNotFound,
            str::stream() << "UUID " << nsOrUUID.toString() << " specified in " << nsOrUUID.dbname()
                          << " resolved to a collection in a different database: " << *resolvedNss,
            resolvedNss->db() == nsOrUUID.dbname());

    return std::move(*resolvedNss);
}

bool CollectionCatalog::checkIfCollectionSatisfiable(CollectionUUID uuid,
                                                     CollectionInfoFn predicate) const {
    invariant(predicate);

    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto collection = _lookupCollectionByUUID(lock, uuid);

    if (!collection) {
        return false;
    }

    return predicate(collection);
}

std::vector<CollectionUUID> CollectionCatalog::getAllCollectionUUIDsFromDb(
    StringData dbName) const {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();
    auto it = _orderedCollections.lower_bound(std::make_pair(dbName.toString(), minUuid));

    std::vector<CollectionUUID> ret;
    while (it != _orderedCollections.end() && it->first.first == dbName) {
        ret.push_back(it->first.second);
        ++it;
    }
    return ret;
}

std::vector<NamespaceString> CollectionCatalog::getAllCollectionNamesFromDb(
    OperationContext* opCtx, StringData dbName) const {
    invariant(opCtx->lockState()->isDbLockedForMode(dbName, MODE_S));

    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto minUuid = UUID::parse("00000000-0000-0000-0000-000000000000").getValue();

    std::vector<NamespaceString> ret;
    for (auto it = _orderedCollections.lower_bound(std::make_pair(dbName.toString(), minUuid));
         it != _orderedCollections.end() && it->first.first == dbName;
         ++it) {
        ret.push_back(it->second->ns());
    }
    return ret;
}

std::vector<std::string> CollectionCatalog::getAllDbNames() const {
    std::vector<std::string> ret;
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);
    auto maxUuid = UUID::parse("FFFFFFFF-FFFF-FFFF-FFFF-FFFFFFFFFFFF").getValue();
    auto iter = _orderedCollections.upper_bound(std::make_pair("", maxUuid));
    while (iter != _orderedCollections.end()) {
        auto dbName = iter->first.first;
        ret.push_back(dbName);
        iter = _orderedCollections.upper_bound(std::make_pair(dbName, maxUuid));
    }
    return ret;
}

void CollectionCatalog::registerCollection(CollectionUUID uuid, std::unique_ptr<Collection> coll) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);

    LOG(1) << "Registering collection " << coll->ns() << " with UUID " << uuid;

    auto ns = coll->ns();
    auto dbName = ns.db().toString();
    auto dbIdPair = std::make_pair(dbName, uuid);

    // Make sure no entry related to this uuid.
    invariant(_catalog.find(uuid) == _catalog.end());
    invariant(_collections.find(ns) == _collections.end());
    invariant(_orderedCollections.find(dbIdPair) == _orderedCollections.end());

    _catalog[uuid] = std::move(coll);
    _collections[ns] = _catalog[uuid].get();
    _orderedCollections[dbIdPair] = _catalog[uuid].get();

    auto dbRid = ResourceId(RESOURCE_DATABASE, dbName);
    addResource(dbRid, dbName);

    auto collRid = ResourceId(RESOURCE_COLLECTION, ns.ns());
    addResource(collRid, ns.ns());
}

std::unique_ptr<Collection> CollectionCatalog::deregisterCollection(CollectionUUID uuid) {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);

    invariant(_catalog.find(uuid) != _catalog.end());

    auto coll = std::move(_catalog[uuid]);
    auto ns = coll->ns();
    auto dbName = ns.db().toString();
    auto dbIdPair = std::make_pair(dbName, uuid);

    LOG(1) << "Deregistering collection " << ns << " with UUID " << uuid;

    // Make sure collection object exists.
    invariant(_collections.find(ns) != _collections.end());
    invariant(_orderedCollections.find(dbIdPair) != _orderedCollections.end());

    _orderedCollections.erase(dbIdPair);
    _collections.erase(ns);
    _catalog.erase(uuid);

    auto collRid = ResourceId(RESOURCE_COLLECTION, ns.ns());
    removeResource(collRid, ns.ns());

    // Removal from an ordered map will invalidate iterators and potentially references to the
    // references to the erased element.
    _generationNumber++;

    return coll;
}

RecoveryUnit::Change* CollectionCatalog::makeFinishDropCollectionChange(
    std::unique_ptr<Collection> coll, CollectionUUID uuid) {
    return new FinishDropCollectionChange(this, std::move(coll), uuid);
}

void CollectionCatalog::deregisterAllCollections() {
    stdx::lock_guard<stdx::mutex> lock(_catalogLock);

    LOG(0) << "Deregistering all the collections";
    for (auto& entry : _catalog) {
        auto uuid = entry.first;
        auto ns = entry.second->ns();
        auto dbName = ns.db().toString();
        auto dbIdPair = std::make_pair(dbName, uuid);

        LOG(1) << "Deregistering collection " << ns << " with UUID " << uuid;

        entry.second.reset();
    }

    _collections.clear();
    _orderedCollections.clear();
    _catalog.clear();

    stdx::lock_guard<stdx::mutex> resourceLock(_resourceLock);
    _resourceInformation.clear();

    _generationNumber++;
}

CollectionCatalog::iterator CollectionCatalog::begin(StringData db) const {
    return iterator(db, _generationNumber, *this);
}

CollectionCatalog::iterator CollectionCatalog::end() const {
    return iterator(_orderedCollections.end());
}

boost::optional<std::string> CollectionCatalog::lookupResourceName(const ResourceId& rid) {
    invariant(rid.getType() == RESOURCE_DATABASE || rid.getType() == RESOURCE_COLLECTION);
    stdx::lock_guard<stdx::mutex> lock(_resourceLock);

    auto search = _resourceInformation.find(rid);
    if (search == _resourceInformation.end()) {
        return boost::none;
    }

    std::set<std::string>& namespaces = search->second;

    // When there are multiple namespaces mapped to the same ResourceId, return boost::none as the
    // ResourceId does not identify a single namespace.
    if (namespaces.size() > 1) {
        return boost::none;
    }

    return *namespaces.begin();
}

void CollectionCatalog::removeResource(const ResourceId& rid, const std::string& entry) {
    invariant(rid.getType() == RESOURCE_DATABASE || rid.getType() == RESOURCE_COLLECTION);
    stdx::lock_guard<stdx::mutex> lock(_resourceLock);

    auto search = _resourceInformation.find(rid);
    if (search == _resourceInformation.end()) {
        return;
    }

    std::set<std::string>& namespaces = search->second;
    namespaces.erase(entry);

    // Remove the map entry if this is the last namespace in the set for the ResourceId.
    if (namespaces.size() == 0) {
        _resourceInformation.erase(search);
    }
}

void CollectionCatalog::addResource(const ResourceId& rid, const std::string& entry) {
    invariant(rid.getType() == RESOURCE_DATABASE || rid.getType() == RESOURCE_COLLECTION);
    stdx::lock_guard<stdx::mutex> lock(_resourceLock);

    auto search = _resourceInformation.find(rid);
    if (search == _resourceInformation.end()) {
        std::set<std::string> newSet = {entry};
        _resourceInformation.insert(std::make_pair(rid, newSet));
        return;
    }

    std::set<std::string>& namespaces = search->second;
    if (namespaces.count(entry) > 0) {
        return;
    }

    namespaces.insert(entry);
}

}  // namespace mongo
