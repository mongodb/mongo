// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/query/plan_executor.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/shard_role/shard_catalog/db_raii.h"
#include "mongo/db/shard_role/shard_role.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Class which is used to access pointers to multiple collections referenced in a query. This class
 * distinguishes between a 'main collection' and 'secondary collections'. While the former
 * represents the collection a given command is run against, the latter represents other collections
 * that the query execution engine may need to access. In case of secondary collections, we only
 * store the namespace strings and fetch 'collectionPtr' on demand, since they can become invalid
 * during query yields. The main collectionPtr is restored through yield so it can be stored.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] MultipleCollectionAccessor final {
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

    const CollectionPtr& lookupCollection(const NamespaceString& nss) const {
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
        for (const auto& [name, acq] : _secondaryAcq) {
            const auto& coll = acq.getCollectionPtr();
            if (coll) {
                func(coll);
            }
        }
    }

private:
    inline const CollectionPtr& _lookupCollectionAcquisitionAndGetCollPtr(
        const NamespaceString& nss) const {
        if (nss == _mainAcq->nss()) {
            return _mainAcq->getCollectionPtr();
        } else if (auto itr = _secondaryAcq.find(nss); itr != _secondaryAcq.end()) {
            return itr->second.getCollectionPtr();
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
};
}  // namespace mongo
