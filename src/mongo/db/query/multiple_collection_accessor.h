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

#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/query/plan_executor.h"

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

    MultipleCollectionAccessor(OperationContext* opCtx,
                               const CollectionPtr* mainColl,
                               const NamespaceString& mainCollNss,
                               bool isAnySecondaryNamespaceAViewOrNotFullyLocal,
                               const std::vector<NamespaceStringOrUUID>& secondaryExecNssList)
        : _mainColl(mainColl),
          _isAnySecondaryNamespaceAViewOrNotFullyLocal(isAnySecondaryNamespaceAViewOrNotFullyLocal),
          _opCtx(opCtx) {
        auto catalog = CollectionCatalog::get(opCtx);
        for (const auto& secondaryNssOrUuid : secondaryExecNssList) {
            auto readTimestamp =
                shard_role_details::getRecoveryUnit(opCtx)->getPointInTimeReadTimestamp();
            // We ignore the collection pointer returned as we don't need it.
            catalog->establishConsistentCollection(opCtx, secondaryNssOrUuid, readTimestamp);
            auto secondaryNss = catalog->resolveNamespaceStringOrUUID(opCtx, secondaryNssOrUuid);

            // Don't store secondaryNss if it is also the main nss.
            if (secondaryNss != mainCollNss) {
                _secondaryColls.emplace(secondaryNss,
                                        catalog->lookupUUIDByNSS(opCtx, secondaryNss));
            }
        }
    }

    explicit MultipleCollectionAccessor(const CollectionPtr* mainColl) : _mainColl(mainColl) {}

    explicit MultipleCollectionAccessor(const CollectionPtr& mainColl)
        : MultipleCollectionAccessor(&mainColl) {}

    explicit MultipleCollectionAccessor(CollectionAcquisition mainAcq)
        : _mainAcq(CollectionOrViewAcquisition(std::move(mainAcq))) {}

    explicit MultipleCollectionAccessor(CollectionOrViewAcquisition mainAcq) : _mainAcq(mainAcq) {}

    MultipleCollectionAccessor(const CollectionOrViewAcquisition& mainAcq,
                               const CollectionOrViewAcquisitionMap& secondaryAcquisitions,
                               bool isAnySecondaryNamespaceAViewOrNotFullyLocal)
        : _mainAcq(mainAcq),
          _secondaryAcq(secondaryAcquisitions),
          _isAnySecondaryNamespaceAViewOrNotFullyLocal(
              isAnySecondaryNamespaceAViewOrNotFullyLocal) {}

    bool hasMainCollection() const {
        return (_mainColl && _mainColl->get()) || (_mainAcq && _mainAcq->collectionExists());
    }

    const CollectionPtr& getMainCollection() const {
        return _mainAcq ? _mainAcq->getCollectionPtr() : *_mainColl;
    }

    std::map<NamespaceString, CollectionPtr> getSecondaryCollections() const {
        if (isAcquisition()) {
            return _getSecondaryAcquisitions();
        } else {
            return _getSecondaryCollectionsAutoGetters();
        }
    }

    bool isAnySecondaryNamespaceAViewOrNotFullyLocal() const {
        return _isAnySecondaryNamespaceAViewOrNotFullyLocal;
    }

    bool isAcquisition() const {
        return bool(_mainAcq);
    }

    const CollectionAcquisition& getMainCollectionAcquisition() const {
        return _mainAcq->getCollection();
    }

    VariantCollectionPtrOrAcquisition getMainCollectionPtrOrAcquisition() const {
        return isAcquisition() ? VariantCollectionPtrOrAcquisition(_mainAcq->getCollection())
                               : VariantCollectionPtrOrAcquisition(_mainColl);
    }

    CollectionPtr lookupCollection(const NamespaceString& nss) const {
        if (isAcquisition()) {
            return _lookupCollectionAcquisitionAndGetCollPtr(nss);
        } else {
            return _lookupCollectionAutoGetters(nss);
        }
    }

    boost::optional<CollectionAcquisition> getCollectionAcquisitionFromUuid(const UUID uuid) const {
        if (isAcquisition()) {
            return _lookupCollectionAcquisition(uuid);
        }
        tasserted(9367602, "No collection acquisition with associated UUID");
        return boost::none;
    }

    void clear() {
        _mainColl = &CollectionPtr::null;
        _mainAcq.reset();
        _secondaryColls.clear();
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
    inline std::map<NamespaceString, CollectionPtr> _getSecondaryAcquisitions() const {
        std::map<NamespaceString, CollectionPtr> collMap;
        for (const auto& [nss, acq] : _secondaryAcq) {
            // TODO(SERVER-103403): Investigate usage validity of
            // CollectionPtr::CollectionPtr_UNSAFE
            collMap.emplace(nss, CollectionPtr::CollectionPtr_UNSAFE(acq.getCollectionPtr().get()));
        }
        return collMap;
    }

    inline std::map<NamespaceString, CollectionPtr> _getSecondaryCollectionsAutoGetters() const {
        std::map<NamespaceString, CollectionPtr> collMap;
        for (const auto& [nss, uuid] : _secondaryColls) {
            collMap.emplace(nss,
                            uuid ? CollectionCatalog::get(_opCtx)->establishConsistentCollection(
                                       _opCtx,
                                       NamespaceStringOrUUID{nss.dbName(), *uuid},
                                       shard_role_details::getRecoveryUnit(_opCtx)
                                           ->getPointInTimeReadTimestamp())
                                 : ConsistentCollection{});
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

    inline CollectionPtr _lookupCollectionAutoGetters(const NamespaceString& nss) const {
        if (_mainColl && _mainColl->get() && nss == _mainColl->get()->ns()) {
            // TODO(SERVER-103403): Investigate usage validity of
            // CollectionPtr::CollectionPtr_UNSAFE
            return CollectionPtr::CollectionPtr_UNSAFE(_mainColl->get());
        } else if (auto itr = _secondaryColls.find(nss);
                   itr != _secondaryColls.end() && itr->second) {
            auto timestamp =
                shard_role_details::getRecoveryUnit(_opCtx)->getPointInTimeReadTimestamp();
            return CollectionPtr{CollectionCatalog::get(_opCtx)->establishConsistentCollection(
                _opCtx, NamespaceStringOrUUID{nss.dbName(), *itr->second}, timestamp)};
        }
        // TODO(SERVER-103403): Investigate usage validity of CollectionPtr::CollectionPtr_UNSAFE
        return CollectionPtr::CollectionPtr_UNSAFE(nullptr);
    }

    // Shard role api collection access.
    boost::optional<CollectionOrViewAcquisition> _mainAcq;
    CollectionOrViewAcquisitionMap _secondaryAcq;

    // Manual collection access state
    const CollectionPtr* _mainColl{&CollectionPtr::null};
    stdx::unordered_map<NamespaceString, boost::optional<UUID>> _secondaryColls{};

    // Tracks whether any secondary namespace is a view or is not fully local based on
    // information captured at the time of AutoGet* object acquisition. This is used to
    // determine if a $lookup is eligible for pushdown into the query execution subsystem as
    // $lookup against a foreign view or a non-local collection is not currently supported
    // by the execution subsystem.
    bool _isAnySecondaryNamespaceAViewOrNotFullyLocal = false;

    OperationContext* _opCtx = nullptr;
};
}  // namespace mongo
