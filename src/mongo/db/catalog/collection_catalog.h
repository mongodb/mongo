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
#include <set>
#include <unordered_map>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog_entry.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/functional.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * This class comprises a UUID to collection catalog, allowing for efficient
 * collection lookup by UUID.
 */
using CollectionUUID = UUID;
class Database;

class CollectionCatalog {
    CollectionCatalog(const CollectionCatalog&) = delete;
    CollectionCatalog& operator=(const CollectionCatalog&) = delete;

    friend class iterator;
    struct CollectionInfo;

public:
    class iterator {
    public:
        using value_type = Collection*;
        using pointer = const value_type*;
        using reference = const value_type&;

        iterator(StringData dbName, uint64_t genNum, const CollectionCatalog& catalog);
        iterator(std::map<std::pair<std::string, CollectionUUID>,
                          CollectionCatalog::CollectionInfo*>::const_iterator mapIter);
        pointer operator->();
        reference operator*();
        iterator operator++();
        iterator operator++(int);
        boost::optional<CollectionUUID> uuid();

        /*
         * Equality operators == and != do not attempt to reposition the iterators being compared.
         * The behavior for comparing invalid iterators is undefined.
         */
        bool operator==(const iterator& other);
        bool operator!=(const iterator& other);

    private:
        /**
         * Check if _mapIter has been invalidated due to a change in the _orderedCollections map. If
         * it has, restart iteration through a call to lower_bound. If the element that the iterator
         * is currently pointing to has been deleted, the iterator will be repositioned to the
         * element that follows it.
         *
         * Returns true if iterator got repositioned.
         */
        bool _repositionIfNeeded();
        bool _exhausted();

        std::string _dbName;
        boost::optional<CollectionUUID> _uuid;
        uint64_t _genNum;
        std::map<std::pair<std::string, CollectionUUID>, CollectionInfo*>::const_iterator _mapIter;
        const CollectionCatalog* _catalog;
        static constexpr Collection* _nullCollection = nullptr;
    };

    static CollectionCatalog& get(ServiceContext* svcCtx);
    static CollectionCatalog& get(OperationContext* opCtx);
    CollectionCatalog() = default;

    /**
     * This function inserts the entry for uuid, coll into the collection catalog. It is called by
     * the op observer when a collection is created.
     */
    void onCreateCollection(OperationContext* opCtx,
                            std::unique_ptr<Collection> coll,
                            CollectionUUID uuid);

    /**
     * This function removes the entry for uuid from the collection catalog. It is called by the op
     * observer when a collection is dropped.
     */
    void onDropCollection(OperationContext* opCtx, CollectionUUID uuid);

    /**
     * This function is responsible for safely setting the namespace string inside 'coll' to the
     * value of 'toCollection'. The caller need not hold locks on the collection.
     *
     * Must be called within a WriteUnitOfWork. The Collection namespace will be set back to
     * 'fromCollection' if the WriteUnitOfWork aborts.
     */
    void setCollectionNamespace(OperationContext* opCtx,
                                Collection* coll,
                                const NamespaceString& fromCollection,
                                const NamespaceString& toCollection);

    /**
     * Implies onDropCollection for all collections in db, but is not transactional.
     */
    void onCloseDatabase(OperationContext* opCtx, std::string dbName);

    /**
     * Register the collection catalog entry with `uuid`. The collection object with `uuid` must not
     * exist in the CollectionCatalog yet.
     */
    void registerCatalogEntry(CollectionUUID uuid,
                              std::unique_ptr<CollectionCatalogEntry> collectionCatalogEntry);

    /**
     * Deregister the collection catalog entry. The collection object with `uuid` is already gone,
     * so this function completely removes any info about uuid.
     */
    std::unique_ptr<CollectionCatalogEntry> deregisterCatalogEntry(CollectionUUID uuid);

    /**
     * Register the collection object with `uuid`. The collection catalog entry with `uuid` already
     * exists in the CollectionCatalog.
     */
    void registerCollectionObject(CollectionUUID uuid, std::unique_ptr<Collection> coll);

    /**
     * Deregister the collection object. The collection catalog entry still exists and will be
     * deregistered later.
     */
    std::unique_ptr<Collection> deregisterCollectionObject(CollectionUUID uuid);


    /**
     * Deregister all the collection objects and catalog entries.
     */
    void deregisterAllCatalogEntriesAndCollectionObjects();

    /**
     * This function gets the Collection pointer that corresponds to the CollectionUUID.
     * The required locks must be obtained prior to calling this function, or else the found
     * Collection pointer might no longer be valid when the call returns.
     *
     * Returns nullptr if the 'uuid' is not known.
     */
    Collection* lookupCollectionByUUID(CollectionUUID uuid) const;

    /**
     * This function gets the CollectionCatalogEntry pointer that corresponds to the
     * CollectionUUID.
     * The required locks must be obtained prior to calling this function, or else the found
     * CollectionCatalogEntry pointer might no longer be valid when the call returns.
     *
     * Returns nullptr if the 'uuid' is not known.
     */
    CollectionCatalogEntry* lookupCollectionCatalogEntryByUUID(CollectionUUID uuid) const;

    /**
     * This function gets the Collection pointer that corresponds to the NamespaceString.
     * The required locks must be obtained prior to calling this function, or else the found
     * Collection pointer may no longer be valid when the call returns.
     *
     * Returns nullptr if the namespace is unknown.
     */
    Collection* lookupCollectionByNamespace(const NamespaceString& nss) const;

    /**
     * This function gets the CollectionCatalogEntry pointer that corresponds to the
     * CollectionUUID.
     * The required locks must be obtained prior to calling this function, or else the found
     * CollectionCatalogEntry pointer might no longer be valid when the call returns.
     *
     * Returns nullptr if the 'uuid' is not known.
     */
    CollectionCatalogEntry* lookupCollectionCatalogEntryByNamespace(
        const NamespaceString& nss) const;

    /**
     * This function gets the NamespaceString from the collection catalog entry that
     * corresponds to CollectionUUID uuid. If no collection exists with the uuid, return
     * boost::none. See onCloseCatalog/onOpenCatalog for more info.
     */
    boost::optional<NamespaceString> lookupNSSByUUID(CollectionUUID uuid) const;

    /**
     * Returns the UUID if `nss` exists in CollectionCatalog. The time complexity of
     * this function is linear to the number of collections in `nss.db()`.
     */
    boost::optional<CollectionUUID> lookupUUIDByNSS(const NamespaceString& nss) const;

    /**
     * This function gets the UUIDs of all collections from `dbName`.
     *
     * If the caller does not take a strong database lock, some of UUIDs might no longer exist (due
     * to collection drop) after this function returns.
     *
     * Returns empty vector if the 'dbName' is not known.
     */
    std::vector<CollectionUUID> getAllCollectionUUIDsFromDb(StringData dbName) const;

    /**
     * This function gets the ns of all collections from `dbName`. The result is not sorted.
     *
     * Caller must take a strong database lock; otherwise, collections returned could be dropped or
     * renamed.
     *
     * Returns empty vector if the 'dbName' is not known.
     */
    std::vector<NamespaceString> getAllCollectionNamesFromDb(OperationContext* opCtx,
                                                             StringData dbName) const;

    /**
     * This functions gets all the database names. The result is sorted in alphabetical ascending
     * order.
     */
    std::vector<std::string> getAllDbNames() const;

    /**
     * Puts the catalog in closed state. In this state, the lookupNSSByUUID method will fall back
     * to the pre-close state to resolve queries for currently unknown UUIDs. This allows processes,
     * like authorization and replication, which need to do lookups outside of database locks, to
     * proceed.
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    void onCloseCatalog(OperationContext* opCtx);

    /**
     * Puts the catatlog back in open state, removing the pre-close state. See onCloseCatalog.
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    void onOpenCatalog(OperationContext* opCtx);

    iterator begin(StringData db) const;
    iterator end() const;

    /**
     * Lookup the name of a resource by its ResourceId. If there are multiple namespaces mapped to
     * the same ResourceId entry, we return the boost::none for those namespaces until there is
     * only one namespace in the set. If the ResourceId is not found, boost::none is returned.
     */
    boost::optional<std::string> lookupResourceName(const ResourceId& rid);

    /**
     * Removes an existing ResourceId 'rid' with namespace 'entry' from the map.
     */
    void removeResource(const ResourceId& rid, const std::string& entry);

    /**
     * Inserts a new ResourceId 'rid' into the map with namespace 'entry'.
     */
    void addResource(const ResourceId& rid, const std::string& entry);

private:
    class FinishDropChange;
    friend class CollectionCatalog::iterator;

    const std::vector<CollectionUUID>& _getOrdering_inlock(const StringData& db,
                                                           const stdx::lock_guard<stdx::mutex>&);
    mutable mongo::stdx::mutex _catalogLock;
    /**
     * When present, indicates that the catalog is in closed state, and contains a map from UUID
     * to pre-close NSS. See also onCloseCatalog.
     */
    boost::optional<
        mongo::stdx::unordered_map<CollectionUUID, NamespaceString, CollectionUUID::Hash>>
        _shadowCatalog;

    /**
     * Unordered map from Collection UUID to the corresponding Collection object and
     * CollectionCatalogEntry object.
     */
    struct CollectionInfo {
        std::unique_ptr<Collection> collection;
        Collection* collectionPtr;  // This store the address to the collection object
        std::unique_ptr<CollectionCatalogEntry> collectionCatalogEntry;
    };
    mongo::stdx::unordered_map<CollectionUUID, CollectionInfo, CollectionUUID::Hash> _catalog;

    /**
     * Ordered map from <database name, collection UUID> to a Collection object.
     */
    std::map<std::pair<std::string, CollectionUUID>, CollectionInfo*> _orderedCollections;

    mongo::stdx::unordered_map<NamespaceString, CollectionInfo*> _collections;

    /**
     * Generation number to track changes to the catalog that could invalidate iterators.
     */
    uint64_t _generationNumber;

    // Protects _resourceInformation.
    mutable stdx::mutex _resourceLock;

    // Mapping from ResourceId to a set of strings that contains collection and database namespaces.
    std::map<ResourceId, std::set<std::string>> _resourceInformation;
};
}  // namespace mongo
