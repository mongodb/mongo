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

#pragma once

#include <map>
#include <memory>
#include <vector>

#include "mongo/bson/timestamp.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"

namespace mongo {
class UncommittedCollections {
public:
    /**
     * Wrapper class for the resources used by UncommittedCollections
     */
    class UncommittedCollectionsMap {
        friend class UncommittedCollections;

    private:
        bool empty() {
            return _collections.empty() && _nssIndex.empty();
        }
        void erase(UUID uuid, NamespaceString nss) {
            _collections.erase(uuid);
            _nssIndex.erase(nss);
        }

        std::map<UUID, std::unique_ptr<Collection>> _collections;
        std::map<NamespaceString, UUID> _nssIndex;
    };

    UncommittedCollections() {
        _resourcesPtr = std::make_shared<UncommittedCollectionsMap>();
    }
    ~UncommittedCollections() = default;

    std::weak_ptr<UncommittedCollectionsMap> getResources() {
        return std::weak_ptr<UncommittedCollectionsMap>(_resourcesPtr);
    }

    std::shared_ptr<UncommittedCollectionsMap> shareResources() {
        return _resourcesPtr;
    }

    void receiveResources(std::shared_ptr<UncommittedCollectionsMap> resources) {
        invariant(_resourcesPtr->empty());
        _resourcesPtr = resources;
    }

    static UncommittedCollections& get(OperationContext* opCtx);

    static void addToTxn(OperationContext* opCtx, std::unique_ptr<Collection> coll);

    static Collection* getForTxn(OperationContext* opCtx, const NamespaceStringOrUUID& nss);
    static Collection* getForTxn(OperationContext* opCtx, const NamespaceString& nss);
    static Collection* getForTxn(OperationContext* opCtx, const UUID& uuid);

    /**
     * Registers any uncommitted collections with the CollectionCatalog. If registering a collection
     * name conflicts with an existing entry, this method will throw a `WriteConflictException`.
     * This method also clears the entries for the collection identified by `uuid` from
     * UncommittedCollections.
     */
    static void commit(OperationContext* opCtx, UUID uuid, UncommittedCollectionsMap* map);

    /**
     * Deregisters the collection with uuid `uuid` from the CollectionCatalog, and re-adds the
     * entries for the collection identified by `uuid` to UncommittedCollections. This function
     * assumes `commit` has previously been called for `uuid`.
     */
    static void rollback(ServiceContext* svcCtx,
                         CollectionUUID uuid,
                         UncommittedCollectionsMap* map);


    /**
     * Erases the UUID/NamespaceString entries corresponding to `uuid` and `nss` from `map`.
     */
    static void erase(UUID uuid, NamespaceString nss, UncommittedCollectionsMap* map);

    bool isUncommittedCollection(OperationContext* opCtx, const NamespaceString& nss) const;
    void invariantHasExclusiveAccessToCollection(OperationContext* opCtx,
                                                 const NamespaceString& nss) const;
    bool isEmpty();

    static void clear(UncommittedCollectionsMap* map) {
        map->_collections.clear();
        map->_nssIndex.clear();
    }

private:
    std::shared_ptr<UncommittedCollectionsMap> _resourcesPtr;
};
}  // namespace mongo
