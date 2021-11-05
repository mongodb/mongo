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

#include <functional>
#include <map>
#include <set>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/profile_filter.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/uuid.h"

namespace mongo {

/**
 * This class comprises a UUID to collection catalog, allowing for efficient
 * collection lookup by UUID.
 */
using CollectionUUID = UUID;
class CollectionCatalog;
class Database;

class CollectionCatalog {
    friend class iterator;

public:
    using CollectionInfoFn = std::function<bool(const CollectionPtr& collection)>;

    /**
     * Defines lifetime and behavior of writable Collections.
     */
    enum class LifetimeMode {
        // Lifetime of writable Collection is managed by an active write unit of work. The writable
        // collection is installed in the catalog during commit and discarded on rollback.
        kManagedInWriteUnitOfWork,

        // Inplace writable access to the Collection currently installed in the catalog. This is
        // only safe when the server is in a state where there can be no concurrent readers. Does
        // not require an active write unit of work.
        kInplace
    };

    class iterator {
    public:
        using value_type = CollectionPtr;

        iterator(OperationContext* opCtx, StringData dbName, const CollectionCatalog& catalog);
        iterator(OperationContext* opCtx,
                 std::map<std::pair<std::string, CollectionUUID>,
                          std::shared_ptr<Collection>>::const_iterator mapIter,
                 const CollectionCatalog& catalog);
        value_type operator*();
        iterator operator++();
        iterator operator++(int);
        boost::optional<CollectionUUID> uuid();

        Collection* getWritableCollection(OperationContext* opCtx, LifetimeMode mode);

        /*
         * Equality operators == and != do not attempt to reposition the iterators being compared.
         * The behavior for comparing invalid iterators is undefined.
         */
        bool operator==(const iterator& other) const;
        bool operator!=(const iterator& other) const;

    private:
        bool _exhausted();

        OperationContext* _opCtx;
        std::string _dbName;
        boost::optional<CollectionUUID> _uuid;
        std::map<std::pair<std::string, CollectionUUID>,
                 std::shared_ptr<Collection>>::const_iterator _mapIter;
        const CollectionCatalog* _catalog;
    };

    struct ProfileSettings {
        int level;
        std::shared_ptr<ProfileFilter> filter;  // nullable

        ProfileSettings(int level, std::shared_ptr<ProfileFilter> filter)
            : level(level), filter(filter) {
            // ProfileSettings represents a state, not a request to change the state.
            // -1 is not a valid profiling level: it is only used in requests, to represent
            // leaving the state unchanged.
            invariant(0 <= level && level <= 2,
                      str::stream() << "Invalid profiling level: " << level);
        }

        ProfileSettings() = default;

        bool operator==(const ProfileSettings& other) const {
            return level == other.level && filter == other.filter;
        }
    };

    static std::shared_ptr<const CollectionCatalog> get(ServiceContext* svcCtx);
    static std::shared_ptr<const CollectionCatalog> get(OperationContext* opCtx);

    /**
     * Stashes provided CollectionCatalog pointer on the OperationContext.
     * Will cause get() to return it for this OperationContext.
     */
    static void stash(OperationContext* opCtx, std::shared_ptr<const CollectionCatalog> catalog);

    /**
     * Perform a write to the catalog using copy-on-write. A catalog previously returned by get()
     * will not be modified.
     *
     * This call will block until the modified catalog has been committed. Concurrant writes are
     * batched together and will thus block each other. It is important to not perform blocking
     * operations such as acquiring locks or waiting for I/O in the write job as that would also
     * block other writers.
     *
     * The provided job is allowed to throw which will be propagated through this call.
     *
     * The write job may execute on a different thread.
     */
    using CatalogWriteFn = std::function<void(CollectionCatalog&)>;
    static void write(ServiceContext* svcCtx, CatalogWriteFn job);
    static void write(OperationContext* opCtx, CatalogWriteFn job);

    /**
     * This function is responsible for safely tracking a Collection rename within a
     * WriteUnitOfWork.
     *
     * Must be called within a WriteUnitOfWork.
     */
    void onCollectionRename(OperationContext* opCtx,
                            Collection* coll,
                            const NamespaceString& fromCollection) const;

    /**
     * Marks a collection as dropped for this OperationContext. Will cause the collection
     * to appear dropped for this OperationContext. The drop will be committed into the catalog on
     * commit.
     *
     * Must be called within a WriteUnitOfWork.
     */
    void dropCollection(OperationContext* opCtx, Collection* coll) const;

    void onCloseDatabase(OperationContext* opCtx, std::string dbName);

    /**
     * Register the collection with `uuid`.
     */
    void registerCollection(OperationContext* opCtx,
                            CollectionUUID uuid,
                            std::shared_ptr<Collection> collection);

    /**
     * Deregister the collection.
     */
    std::shared_ptr<Collection> deregisterCollection(OperationContext* opCtx, CollectionUUID uuid);

    /**
     * Deregister all the collection objects and view namespaces.
     */
    void deregisterAllCollectionsAndViews();

    /**
     * Register the namespace to be used as a view.
     *
     * Throws WriteConflictException if namespace is used by a Collection
     */
    void registerView(const NamespaceString& ns);

    /**
     * Deregister the namespace from being used as a view.
     */
    void deregisterView(const NamespaceString& ns);

    /**
     * Sets all namespaces used by views for a database. Does not validate if they are used by
     * Collections. When creating new view its namespace should be registered with registerView()
     * above.
     */
    void replaceViewsForDatabase(StringData dbName, absl::flat_hash_set<NamespaceString> views);

    /**
     * This function gets the Collection pointer that corresponds to the CollectionUUID.
     *
     * The required locks must be obtained prior to calling this function, or else the found
     * Collection pointer might no longer be valid when the call returns.
     *
     * 'lookupCollectionByUUIDForMetadataWrite' requires a MODE_X collection lock, returns a copy to
     * the caller because catalog updates are copy-on-write.
     *
     * 'lookupCollectionByUUID' requires a MODE_IS collection lock.
     *
     * 'lookupCollectionByUUIDForRead' does not require locks and should only be used in the context
     * of a lock-free read wherein we also have a consistent storage snapshot.
     *
     * Returns nullptr if the 'uuid' is not known.
     */
    Collection* lookupCollectionByUUIDForMetadataWrite(OperationContext* opCtx,
                                                       LifetimeMode mode,
                                                       CollectionUUID uuid) const;
    CollectionPtr lookupCollectionByUUID(OperationContext* opCtx, CollectionUUID uuid) const;
    std::shared_ptr<const Collection> lookupCollectionByUUIDForRead(OperationContext* opCtx,
                                                                    CollectionUUID uuid) const;

    /**
     * Returns true if the collection has been registered in the CollectionCatalog but not yet made
     * visible.
     */
    bool isCollectionAwaitingVisibility(CollectionUUID uuid) const;

    /**
     * These functions fetch a Collection pointer that corresponds to the NamespaceString.
     *
     * The required locks must be obtained prior to calling this function, or else the found
     * Collection pointer may no longer be valid when the call returns.
     *
     * 'lookupCollectionByNamespaceForMetadataWrite' requires a MODE_X collection lock, returns a
     * copy to the caller because catalog updates are copy-on-write.
     *
     * 'lookupCollectionByNamespace' requires a MODE_IS collection lock.
     *
     * 'lookupCollectionByNamespaceForRead' does not require locks and should only be used in the
     * context of a lock-free read wherein we also have a consistent storage snapshot.
     *
     * Returns nullptr if the namespace is unknown.
     */
    Collection* lookupCollectionByNamespaceForMetadataWrite(OperationContext* opCtx,
                                                            LifetimeMode mode,
                                                            const NamespaceString& nss) const;
    CollectionPtr lookupCollectionByNamespace(OperationContext* opCtx,
                                              const NamespaceString& nss) const;
    std::shared_ptr<const Collection> lookupCollectionByNamespaceForRead(
        OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * This function gets the NamespaceString from the collection catalog entry that
     * corresponds to CollectionUUID uuid. If no collection exists with the uuid, return
     * boost::none. See onCloseCatalog/onOpenCatalog for more info.
     */
    boost::optional<NamespaceString> lookupNSSByUUID(OperationContext* opCtx,
                                                     CollectionUUID uuid) const;

    /**
     * Returns the UUID if `nss` exists in CollectionCatalog.
     */
    boost::optional<CollectionUUID> lookupUUIDByNSS(OperationContext* opCtx,
                                                    const NamespaceString& nss) const;

    /**
     * Without acquiring any locks resolves the given NamespaceStringOrUUID to an actual namespace.
     * Throws NamespaceNotFound if the collection UUID cannot be resolved to a name, or if the UUID
     * can be resolved, but the resulting collection is in the wrong database.
     */
    NamespaceString resolveNamespaceStringOrUUID(OperationContext* opCtx,
                                                 NamespaceStringOrUUID nsOrUUID) const;

    /**
     * Returns whether the collection with 'uuid' satisfies the provided 'predicate'. If the
     * collection with 'uuid' is not found, false is returned.
     */
    bool checkIfCollectionSatisfiable(CollectionUUID uuid, CollectionInfoFn predicate) const;

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
     *
     * Unlike DatabaseHolder::getNames(), this does not return databases that are empty.
     */
    std::vector<std::string> getAllDbNames() const;

    /**
     * Sets 'newProfileSettings' as the profiling settings for the database 'dbName'.
     */
    void setDatabaseProfileSettings(StringData dbName, ProfileSettings newProfileSettings);

    /**
     * Fetches the profiling settings for database 'dbName'.
     *
     * Returns the server's default database profile settings if the database does not exist.
     */
    ProfileSettings getDatabaseProfileSettings(StringData dbName) const;

    /**
     * Fetches the profiling level for database 'dbName'.
     *
     * Returns the server's default database profile settings if the database does not exist.
     *
     * There is no corresponding setDatabaseProfileLevel; use setDatabaseProfileSettings instead.
     * This method only exists as a convenience.
     */
    int getDatabaseProfileLevel(StringData dbName) const {
        return getDatabaseProfileSettings(dbName).level;
    }

    /**
     * Clears the database profile settings entry for 'dbName'.
     */
    void clearDatabaseProfileSettings(StringData dbName);

    /**
     * Statistics for the types of collections in the catalog.
     * Total collections = 'internal' + 'userCollections'
     */
    struct Stats {
        // Non-system collections on non-internal databases
        int userCollections = 0;
        // Non-system capped collections on non-internal databases
        int userCapped = 0;
        // System collections or collections on internal databases
        int internal = 0;
    };

    /**
     * Returns statistics for the collection catalog.
     */
    Stats getStats() const;

    /**
     * Returns a set of databases, by name, that have view catalogs.
     */
    using ViewCatalogSet = StringSet;
    ViewCatalogSet getViewCatalogDbNames() const;

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

    /**
     * The epoch is incremented whenever the catalog is closed and re-opened.
     *
     * Callers of this method must hold the global lock in at least MODE_IS.
     *
     * This allows callers to detect an intervening catalog close. For example, closing the catalog
     * must kill all active queries. This is implemented by checking that the epoch has not changed
     * during query yield recovery.
     */
    uint64_t getEpoch() const;

    iterator begin(OperationContext* opCtx, StringData db) const;
    iterator end(OperationContext* opCtx) const;

    /**
     * Lookup the name of a resource by its ResourceId. If there are multiple namespaces mapped to
     * the same ResourceId entry, we return the boost::none for those namespaces until there is
     * only one namespace in the set. If the ResourceId is not found, boost::none is returned.
     */
    boost::optional<std::string> lookupResourceName(const ResourceId& rid) const;

    /**
     * Removes an existing ResourceId 'rid' with namespace 'entry' from the map.
     */
    void removeResource(const ResourceId& rid, const std::string& entry);

    /**
     * Inserts a new ResourceId 'rid' into the map with namespace 'entry'.
     */
    void addResource(const ResourceId& rid, const std::string& entry);

private:
    friend class CollectionCatalog::iterator;
    class PublishCatalogUpdates;

    std::shared_ptr<Collection> _lookupCollectionByUUID(CollectionUUID uuid) const;

    /**
     * When present, indicates that the catalog is in closed state, and contains a map from UUID
     * to pre-close NSS. See also onCloseCatalog.
     */
    boost::optional<
        mongo::stdx::unordered_map<CollectionUUID, NamespaceString, CollectionUUID::Hash>>
        _shadowCatalog;

    using CollectionCatalogMap =
        stdx::unordered_map<CollectionUUID, std::shared_ptr<Collection>, CollectionUUID::Hash>;
    using OrderedCollectionMap =
        std::map<std::pair<std::string, CollectionUUID>, std::shared_ptr<Collection>>;
    using NamespaceCollectionMap =
        stdx::unordered_map<NamespaceString, std::shared_ptr<Collection>>;
    using DatabaseProfileSettingsMap = StringMap<ProfileSettings>;

    CollectionCatalogMap _catalog;
    OrderedCollectionMap _orderedCollections;  // Ordered by <dbName, collUUID> pair
    NamespaceCollectionMap _collections;

    // Map of database names to a set of their views. Only databases with views are present.
    StringMap<absl::flat_hash_set<NamespaceString>> _views;

    // Incremented whenever the CollectionCatalog gets closed and reopened (onCloseCatalog and
    // onOpenCatalog).
    //
    // Catalog objects are destroyed and recreated when the catalog is closed and re-opened. We
    // increment this counter to track when the catalog is reopened. This permits callers to detect
    // after yielding whether their catalog pointers are still valid. Collection UUIDs are not
    // sufficient, since they remain stable across catalog re-opening.
    //
    // A thread must hold the global exclusive lock to write to this variable, and must hold the
    // global lock in at least MODE_IS to read it.
    uint64_t _epoch = 0;

    // Mapping from ResourceId to a set of strings that contains collection and database namespaces.
    std::map<ResourceId, std::set<std::string>> _resourceInformation;

    /**
     * Contains non-default database profile settings. New collections, current collections and
     * views must all be able to access the correct profile settings for the database in which they
     * reside. Simple database name to struct ProfileSettings map.
     */
    DatabaseProfileSettingsMap _databaseProfileSettings;

    // Tracks usage of collection usage features (e.g. capped).
    Stats _stats;
};

/**
 * RAII style object to stash a versioned CollectionCatalog on the OperationContext.
 * Calls to CollectionCatalog::get(OperationContext*) will return this instance.
 *
 * Unstashes the CollectionCatalog at destruction if the OperationContext::isLockFreeReadsOp()
 * flag is no longer set. This is handling for the nested Stasher use case.
 */
class CollectionCatalogStasher {
public:
    CollectionCatalogStasher(OperationContext* opCtx);
    CollectionCatalogStasher(OperationContext* opCtx,
                             std::shared_ptr<const CollectionCatalog> catalog);

    /**
     * Unstashes the catalog if _opCtx->isLockFreeReadsOp() is no longer set.
     */
    ~CollectionCatalogStasher();

    /**
     * Moves ownership of the stash to the new instance, and marks the old one unstashed.
     */
    CollectionCatalogStasher(CollectionCatalogStasher&& other);

    CollectionCatalogStasher(const CollectionCatalogStasher&) = delete;
    CollectionCatalogStasher& operator=(const CollectionCatalogStasher&) = delete;
    CollectionCatalogStasher& operator=(CollectionCatalogStasher&&) = delete;

    /**
     * Stashes 'catalog' on the _opCtx.
     */
    void stash(std::shared_ptr<const CollectionCatalog> catalog);

    /**
     * Resets the OperationContext so CollectionCatalog::get() returns latest catalog again
     */
    void reset();

private:
    OperationContext* _opCtx;
    bool _stashed;
};

/**
 * Functor for looking up Collection by UUID from the Collection Catalog. This is the default yield
 * restore implementation for CollectionPtr when acquired from the catalog.
 */
struct LookupCollectionForYieldRestore {
    const Collection* operator()(OperationContext* opCtx, CollectionUUID uuid) const;
};

/**
 * RAII class to perform multiple writes to the CollectionCatalog on a single copy of the
 * CollectionCatalog instance. Requires the global lock to be held in exclusive write mode (MODE_X)
 * for the lifetime of this object.
 */
class BatchedCollectionCatalogWriter {
public:
    BatchedCollectionCatalogWriter(OperationContext* opCtx);
    ~BatchedCollectionCatalogWriter();

    BatchedCollectionCatalogWriter(const BatchedCollectionCatalogWriter&) = delete;
    BatchedCollectionCatalogWriter(BatchedCollectionCatalogWriter&&) = delete;

private:
    OperationContext* _opCtx;
    // Store base when we clone the CollectionCatalog so we can verify that there has been no other
    // writers during the batching.
    std::shared_ptr<CollectionCatalog> _base = nullptr;
};

}  // namespace mongo
