/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#pragma once

#include "mongo/db/query/plan_executor.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_role.h"

namespace mongo {

/**
 * Class which is used to access pointers to multiple collections referenced in a query. This class
 * distinguishes between a 'main collection' and 'secondary collections'. While the former
 * represents the collection a given command is run against, the latter represents other collections
 * that the query execution engine may need to access. In case of secondary collections, we only
 * store the namespace strings and fetch 'collectionPtr' on demand, since they can become invalid
 * during query yields. The main collectionPtr is restored through yield so it can be stored.
 */
class MultipleCollectionAccessor final {
public:
    MultipleCollectionAccessor() = default;

    explicit MultipleCollectionAccessor(CollectionAcquisition mainAcq)
        : _mainAcq(CollectionOrViewAcquisition(std::move(mainAcq))) {}

    explicit MultipleCollectionAccessor(CollectionOrViewAcquisition mainAcq) : _mainAcq(mainAcq) {}

    explicit MultipleCollectionAccessor(const boost::optional<CollectionOrViewAcquisition>& mainAcq)
        : _mainAcq(mainAcq) {}


    MultipleCollectionAccessor(const CollectionOrViewAcquisition& mainAcq,
                               const CollectionOrViewAcquisitionMap& secondaryAcquisitions,
                               bool isAnySecondaryNamespaceAViewOrNotFullyLocal)
        : _mainAcq(mainAcq),
          _secondaryAcq(secondaryAcquisitions),
          _isAnySecondaryNamespaceAViewOrNotFullyLocal(
              isAnySecondaryNamespaceAViewOrNotFullyLocal) {}

    bool hasMainCollection() const {
        return _mainAcq && _mainAcq->collectionExists();
    }

    bool hasNonExistentMainCollection() const {
        return _mainAcq && _mainAcq->isCollection() && !_mainAcq->collectionExists();
    }

    const CollectionPtr& getMainCollection() const {
        return _mainAcq ? _mainAcq->getCollectionPtr() : CollectionPtr::null;
    }

    std::map<NamespaceString, CollectionPtr> getSecondaryCollections() const {
        return _getSecondaryCollections();
    }

    const CollectionOrViewAcquisitionMap& getSecondaryCollectionAcquisitions() const {
        return _secondaryAcq;
    }

    bool isAnySecondaryNamespaceAViewOrNotFullyLocal() const {
        return _isAnySecondaryNamespaceAViewOrNotFullyLocal;
    }

    const CollectionAcquisition& getMainCollectionAcquisition() const {
        return _mainAcq->getCollection();
    }

    CollectionAcquisition getMainCollectionPtrOrAcquisition() const {
        return CollectionAcquisition(_mainAcq->getCollection());
    }

    CollectionPtr lookupCollection(const NamespaceString& nss) const {
        return _lookupCollectionAcquisitionAndGetCollPtr(nss);
    }

    bool knowsNamespace(const NamespaceString& nss) const {
        if (nss == _mainAcq->nss()) {
            return true;
        }
        return _secondaryAcq.find(nss) != _secondaryAcq.end();
    }

    boost::optional<CollectionAcquisition> getCollectionAcquisitionFromUuid(const UUID uuid) const {
        return _lookupCollectionAcquisition(uuid);
    }

    void clear() {
        _mainAcq.reset();
        _secondaryAcq.clear();
    }

    void forEach(std::function<void(const CollectionPtr&)> func) const {
        if (hasMainCollection()) {
            func(getMainCollection());
        }
        for (const auto& [name, coll] : getSecondaryCollections()) {
            if (coll) {
                func(coll);
            }
        }
    }

private:
    inline std::map<NamespaceString, CollectionPtr> _getSecondaryCollections() const {
        std::map<NamespaceString, CollectionPtr> collMap;
        for (const auto& [nss, acq] : _secondaryAcq) {
            // TODO(SERVER-103403): Investigate usage validity of
            // CollectionPtr::CollectionPtr_UNSAFE
            collMap.emplace(nss, CollectionPtr::CollectionPtr_UNSAFE(acq.getCollectionPtr().get()));
        }
        return collMap;
    }

    inline CollectionPtr _lookupCollectionAcquisitionAndGetCollPtr(
        const NamespaceString& nss) const {
        if (nss == _mainAcq->nss()) {
            // TODO(SERVER-103403): Investigate usage validity of
            // CollectionPtr::CollectionPtr_UNSAFE
            return CollectionPtr::CollectionPtr_UNSAFE(_mainAcq->getCollectionPtr().get());
        } else if (auto itr = _secondaryAcq.find(nss); itr != _secondaryAcq.end()) {
            // TODO(SERVER-103403): Investigate usage validity of
            // CollectionPtr::CollectionPtr_UNSAFE
            return CollectionPtr::CollectionPtr_UNSAFE(itr->second.getCollectionPtr().get());
        }
        tasserted(
            10096102,
            str::stream() << "MultipleCollectionAccessor::_lookupCollectionAcquisition: requested "
                             "unexpected collection nss: "
                          << nss.toStringForErrorMsg());
    }

    inline boost::optional<CollectionAcquisition> _lookupCollectionAcquisition(UUID uuid) const {
        if (_mainAcq && _mainAcq->collectionExists() && uuid == _mainAcq->getCollection().uuid()) {
            return _mainAcq->getCollection();
        }
        // Since _secondaryAcq is keyed by NamespaceString, iterate over all secondary
        // acquisitions.
        for (const auto& entry : _secondaryAcq) {
            if (entry.second.collectionExists() && entry.second.getCollection().uuid() == uuid) {
                return entry.second.getCollection();
            }
        }
        MONGO_UNREACHABLE_TASSERT(9367601);
    }

    // Shard role api collection access.
    boost::optional<CollectionOrViewAcquisition> _mainAcq;
    CollectionOrViewAcquisitionMap _secondaryAcq;

    // Tracks whether any secondary namespace is a view or is not fully local based on
    // information captured at the time of AutoGet* object acquisition. This is used to
    // determine if a $lookup is eligible for pushdown into the query execution subsystem as
    // $lookup against a foreign view or a non-local collection is not currently supported
    // by the execution subsystem.
    bool _isAnySecondaryNamespaceAViewOrNotFullyLocal = false;

    OperationContext* _opCtx = nullptr;
};
}  // namespace mongo
