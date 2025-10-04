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

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/database_name.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/durable_catalog_entry.h"
#include "mongo/db/local_catalog/historical_catalogid_tracker.h"
#include "mongo/db/local_catalog/index_catalog_entry.h"
#include "mongo/db/local_catalog/views_for_database.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/views/view.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/functional.h"
#include "mongo/util/immutable/map.h"
#include "mongo/util/immutable/unordered_map.h"
#include "mongo/util/immutable/unordered_set.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <absl/container/flat_hash_set.h>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <immer/detail/util.hpp>

namespace mongo {

extern const SharedCollectionDecorations::Decoration<AtomicWord<bool>>
    historicalIDTrackerAllowsMixedModeWrites;

namespace catalog {
// Forward declaration of a friend class for the CollectionCatalog since it resides in another
// namespace. This is a special class that controls opening/closing the catalog and resides in
// catalog_control.cpp
class CatalogControlUtils;

/**
 * Must be called after MDBCatalog is loaded.
 */
void initializeCollectionCatalog(OperationContext* opCtx,
                                 StorageEngine* engine,
                                 boost::optional<Timestamp> stableTs);
void initializeCollectionCatalog(OperationContext* opCtx, StorageEngine* engine);

/**
 * Creates a Collection object and registers it in the CollectionCatalog.
 */
void initCollectionObject(OperationContext* opCtx,
                          StorageEngine* engine,
                          RecordId catalogId,
                          const NamespaceString& nss,
                          bool forRepair,
                          Timestamp minValidTs);

/**
 * Lists the databases.
 * This function doesn't return databases whose creation has committed durably but hasn't been
 * published yet in the CollectionCatalog.
 */
std::vector<DatabaseName> listDatabases(boost::optional<TenantId> tenantId = boost::none);
}  // namespace catalog

class CollectionCatalog {
    friend class iterator;
    using OrderedCollectionMap =
        immutable::map<std::pair<DatabaseName, UUID>, std::shared_ptr<Collection>>;

public:
    using CollectionInfoFn = std::function<bool(const Collection* collection)>;


    class iterator {
    public:
        using value_type = const Collection*;

        iterator(const DatabaseName& dbName,
                 OrderedCollectionMap::iterator it,
                 const OrderedCollectionMap& catalog);
        value_type operator*();
        iterator operator++();
        bool operator==(const iterator& other) const;
        bool operator!=(const iterator& other) const;

    private:
        const OrderedCollectionMap& _map;
        immutable::map<std::pair<DatabaseName, UUID>, std::shared_ptr<Collection>>::iterator
            _mapIter;
    };

    class Range {
    public:
        Range(const OrderedCollectionMap&, const DatabaseName& dbName);
        iterator begin() const;
        iterator end() const;
        bool empty() const;

    private:
        OrderedCollectionMap _map;
        DatabaseName _dbName;
    };

    /**
     * Returns a CollectionCatalog instance capable of returning Collection instances consistent
     * with the storage snapshot. Is the same as latest() below if no snapshot is opened.
     *
     * Is the default method of acquiring a CollectionCatalog instance.
     */
    static std::shared_ptr<const CollectionCatalog> get(OperationContext* opCtx);

    /**
     * Returns a CollectionCatalog instance that reflects the latest state of the server.
     *
     * Used to confirm whether Collection instances are write eligible.
     */
    static std::shared_ptr<const CollectionCatalog> latest(OperationContext* opCtx);

    /**
     * Like latest() above.
     */
    static std::shared_ptr<const CollectionCatalog> latest(ServiceContext* svcCtx);

    /**
     * Stashes provided CollectionCatalog pointer on the RecoveryUnit snapshot.
     * Will cause get() to return this instance while the snapshot remains open.
     */
    static void stash(OperationContext* opCtx, std::shared_ptr<const CollectionCatalog> catalog);

    /**
     * Perform a write to the catalog using copy-on-write. A catalog previously returned by get()
     * will not be modified.
     *
     * This call will block until the modified catalog has been committed. Concurrent writes for a
     * single ServiceContext will block each other. It is important to not perform blocking
     * operations such as acquiring locks or waiting for I/O in the write job as that would also
     * block other writers.
     *
     * The provided job is allowed to throw which will be propagated through this call.
     */
    using CatalogWriteFn = function_ref<void(CollectionCatalog&)>;
    static void write(ServiceContext* svcCtx, CatalogWriteFn job);
    static void write(OperationContext* opCtx, CatalogWriteFn job);

    /**
     * Create a new view 'viewName' with contents defined by running the specified aggregation
     * 'pipeline' with collation 'collation' on a collection or view 'viewOn'. May insert this view
     * into the system.views collection depending on 'durability'.
     *
     * Must be in WriteUnitOfWork. View creation rolls back if the unit of work aborts.
     *
     * Expects db.system.views MODE_X lock and view namespace MODE_IX lock (unless 'durability' is
     * set to kAlreadyDurable).
     */
    Status createView(OperationContext* opCtx,
                      const NamespaceString& viewName,
                      const NamespaceString& viewOn,
                      const BSONArray& pipeline,
                      const ViewsForDatabase::PipelineValidatorFn& validatePipeline,
                      const BSONObj& collation,
                      ViewsForDatabase::Durability durability =
                          ViewsForDatabase::Durability::kNotYetDurable) const;

    /**
     * Drop the view named 'viewName'.
     *
     * Must be in WriteUnitOfWork. The drop rolls back if the unit of work aborts.
     */
    Status dropView(OperationContext* opCtx, const NamespaceString& viewName) const;

    /**
     * Modify the view named 'viewName' to have the new 'viewOn' and 'pipeline'.
     *
     * Must be in WriteUnitOfWork. The modification rolls back if the unit of work aborts.
     */
    Status modifyView(OperationContext* opCtx,
                      const NamespaceString& viewName,
                      const NamespaceString& viewOn,
                      const BSONArray& pipeline,
                      const ViewsForDatabase::PipelineValidatorFn& validatePipeline) const;

    /**
     * Reloads the in-memory state of the view catalog from the 'system.views' collection. The
     * durable view definitions will be validated. Reading stops on the first invalid entry with
     * errors logged and returned. Performs no cycle detection, etc.
     *
     * This is implicitly called by other methods when write operations are performed on the
     * view catalog, on external changes to the 'system.views' collection and on the first
     * opening of a database.
     *
     * Callers must re-fetch the catalog to observe changes.
     *
     * Requires an X lock on the 'system.views' collection'.
     */
    void reloadViews(OperationContext* opCtx, const DatabaseName& dbName) const;

    /**
     * Establish a collection instance consistent with the opened storage snapshot.
     *
     * Returns the collection pointer representative of 'nssOrUUID' at the provided read timestamp.
     * If no timestamp is provided, returns instance of the latest collection. When called
     * concurrently with a DDL operation the latest collection returned may be the instance being
     * committed by the concurrent DDL operation.
     *
     * Returns nullptr when reading from a point-in-time where the collection did not exist.
     *
     * The returned collection instance is only valid while a reference to this catalog instance is
     * held or stashed and as long as the storage snapshot remains open. Releasing catalog reference
     * or closing the storage snapshot invalidates the instance.
     *
     * Future calls to lookupCollection, lookupNSS, lookupUUID on this namespace/UUID will return
     * results consistent with the opened storage snapshot.
     *
     * Depending on the internal state of the CollectionCatalog a read from the durable catalog may
     * be performed and this call may block on I/O. No mutex should be held while calling this
     * function.
     *
     * Multikey state is not guaranteed to be consistent with the storage snapshot. It may indicate
     * an index to be multikey where it is not multikey in the storage snapshot. However, it will
     * never be wrong in the other direction.
     *
     * No collection level lock is required to call this function.
     */
    ConsistentCollection establishConsistentCollection(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nssOrUUID,
        boost::optional<Timestamp> readTimestamp) const;

    // Establish a consistent view of the database. This method will only work against the latest
    // timestamp. It is equivalent to calling establishConsistentCollection with no timestamp on all
    // collections of the database.
    std::vector<ConsistentCollection> establishConsistentCollections(
        OperationContext* opCtx, const DatabaseName& dbName) const;

    /**
     * Handles committing a collection to the catalog within a WriteUnitOfWork.
     *
     * Must be called within a WriteUnitOfWork.
     */
    void onCreateCollection(OperationContext* opCtx, std::shared_ptr<Collection> coll) const;

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

    /**
     * Removes the view records associated with 'dbName', if any, from the in-memory
     * representation of the catalog. Should be called when Database instance is closed. Requires X
     * lock on database namespace.
     */
    void onCloseDatabase(OperationContext* opCtx, DatabaseName dbName);

    /**
     * Register the collection with `uuid` at a given commitTime.
     *
     * The global lock must be held in exclusive mode.
     */
    void registerCollection(OperationContext* opCtx,
                            std::shared_ptr<Collection> collection,
                            boost::optional<Timestamp> commitTime);

    /**
     * Deregister the collection.
     */
    std::shared_ptr<Collection> deregisterCollection(OperationContext* opCtx,
                                                     const UUID& uuid,
                                                     boost::optional<Timestamp> commitTime);

    /**
     * Create a temporary record of an uncommitted view namespace to aid in detecting a
     * simultaneous attempt to create a collection with the same namespace.
     */
    void registerUncommittedView(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Remove the temporary record for an uncommitted view namespace, either on commit or rollback.
     */
    void deregisterUncommittedView(const NamespaceString& nss);

    /**
     * Deregister all the collection objects and view namespaces.
     */
    void deregisterAllCollectionsAndViews(ServiceContext* svcCtx);

    /**
     * Clears the in-memory state for the views associated with a particular database.
     *
     * Callers must re-fetch the catalog to observe changes.
     */
    void clearViews(OperationContext* opCtx, const DatabaseName& dbName) const;

    /**
     * Returns a Collection pointer that corresponds to the provided
     * NamespaceString/UUID/NamespaceOrUUID.
     *
     * For the returned collection instance to remain valid remains valid, one of two preconditions
     * needs to be met:
     * 1. A collection lock of at least MODE_IS is being held.
     * 2. A reference to this catalog instance is held or stashed AND the storage snapshot remains
     *    open.
     *
     * Releasing the collection lock, catalog instance or storage snapshot will invalidate the
     * returned collection instance.
     *
     * A read or write AutoGetCollection style RAII object meets the requirements and ensures
     * validity for collection instances during its lifetime.
     *
     * It is NOT safe to cache this pointer or any pointer obtained from this instance across
     * storage snapshots such as query yield.
     *
     * Returns nullptr if no collection is known.
     */
    const Collection* lookupCollectionByUUID(OperationContext* opCtx, UUID uuid) const;
    const Collection* lookupCollectionByNamespace(OperationContext* opCtx,
                                                  const NamespaceString& nss) const;
    const Collection* lookupCollectionByNamespaceOrUUID(
        OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) const;

    /**
     * This function gets the NamespaceString from the collection catalog entry that
     * corresponds to UUID uuid. If no collection exists with the uuid, return
     * boost::none. See onCloseCatalog/onOpenCatalog for more info.
     */
    boost::optional<NamespaceString> lookupNSSByUUID(OperationContext* opCtx,
                                                     const UUID& uuid) const;

    /**
     * Returns the UUID if `nss` exists in CollectionCatalog.
     */
    boost::optional<UUID> lookupUUIDByNSS(OperationContext* opCtx,
                                          const NamespaceString& nss) const;

    /**
     * Checks if the provided instance is the latest version for this catalog version. This check
     * should be used to determine if the collection instance is safe to perform CRUD writes on. For
     * the check to be meaningful it should be performed against CollectionCatalog::latest.
     * NOTE: the check is based on pointer equality between the 'collection' parameter and
     * the object stored by the catalog under the same UUID; this may lead to unexpected false
     * results when a different, but structurally equivalent parameter is passed in (like a
     * 'collection' obtained via read-through). Consider using checkIfUUIDExistsAtLatest when such a
     * behavior is not desirable.
     */
    bool isLatestCollection(OperationContext* opCtx, const Collection* collection) const;

    /**
     * Checks if the provided UUID is compatible with the latest version for this catalog version
     * (plus uncommitted catalog updates).
     * The function only returns a meaningful result when called against CollectionCatalog::latest()
     * and it is exclusively meant to support the ShardRole API in the resource acquisition for
     * unsharded collection.
     */
    bool checkIfUUIDExistsAtLatest(OperationContext* opCtx, UUID uuid) const;

    /**
     * Verifies that the provided collection name doesn't exist in the catalog and is exclusively
     * present in the uncommitted updates of the operation. For the check to be meaningful it should
     * be performed against CollectionCatalog::latest.
     */
    void ensureCollectionIsNew(OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Iterates through the views in the catalog associated with database `dbName`, applying
     * 'callback' to each view.  If the 'callback' returns false, the iterator exits early.
     */
    void iterateViews(OperationContext* opCtx,
                      const DatabaseName& dbName,
                      const std::function<bool(const ViewDefinition& view)>& callback) const;

    /**
     * Look up the 'nss' in the view catalog, returning a shared pointer to a View definition,
     * or nullptr if it doesn't exist.
     */
    std::shared_ptr<const ViewDefinition> lookupView(OperationContext* opCtx,
                                                     const NamespaceString& nss) const;

    /**
     * Same functionality as above, except this function skips validating durable views in the
     * view catalog.
     */
    std::shared_ptr<const ViewDefinition> lookupViewWithoutValidatingDurable(
        OperationContext* opCtx, const NamespaceString& nss) const;

    /**
     * Without acquiring any locks resolves the given NamespaceStringOrUUID to an actual namespace.
     * Throws NamespaceNotFound if the collection UUID cannot be resolved to a name, or if the UUID
     * can be resolved, but the resulting collection is in the wrong database.
     */
    NamespaceString resolveNamespaceStringOrUUID(OperationContext* opCtx,
                                                 const NamespaceStringOrUUID& nsOrUUID) const;

    /**
     * Resolves the given NamespaceStringOrUUID to an actual namespace without acquiring any locks.
     * Throws NamespaceNotFound if the collection UUID cannot be resolved to a name, or if the UUID
     * can be resolved, but the resulting collection is in the wrong database.
     *
     * Note that this will lookup in commit pending entries and can result in wrong results if not
     * paired with a CollectionCatalog::establishConsistentCollection call. This is safe to do if it
     * is intended as a way to perform UUID->NSS mapping stability with the usual mechanism (resolve
     * -> lock NSS -> resolve again and check if it's still the same NSS) OR if the result will be
     * used for read source selection as they only require a check on whether the NSS is replicated
     * or the oplog, both of which are going to be stable since we can't make a collection
     * non-replicated nor replace the oplog collection. Any other usages are probably not safe.
     *
     * TODO SERVER-93555: See if we can remove this method.
     */
    NamespaceString resolveNamespaceStringOrUUIDWithCommitPendingEntries_UNSAFE(
        OperationContext* opCtx, const NamespaceStringOrUUID& nsOrUUID) const;


    /**
     * Resolves and validates the namespace from the given DatabaseName and UUID.
     */
    NamespaceString resolveNamespaceStringFromDBNameAndUUID(OperationContext* opCtx,
                                                            const DatabaseName& dbName,
                                                            const UUID& uuid) const;

    /**
     * Returns whether the collection with 'uuid' satisfies the provided 'predicate'. If the
     * collection with 'uuid' is not found, false is returned.
     */
    bool checkIfCollectionSatisfiable(UUID uuid, const CollectionInfoFn& predicate) const;

    /**
     * This function gets the UUIDs of all collections from `dbName`.
     *
     * If the caller does not take a strong database lock, some of UUIDs might no longer exist (due
     * to collection drop) after this function returns.
     *
     * Returns empty vector if the 'dbName' is not known.
     */
    std::vector<UUID> getAllCollectionUUIDsFromDb(const DatabaseName& dbName) const;

    /**
     * This function gets the ns of all collections from `dbName`. The result is not sorted.
     *
     * Caller must take a strong database lock; otherwise, collections returned could be dropped or
     * renamed.
     *
     * Returns empty vector if the 'dbName' is not known.
     */
    std::vector<NamespaceString> getAllCollectionNamesFromDb(OperationContext* opCtx,
                                                             const DatabaseName& dbName) const;

    /**
     * This function gets all the database names. The result is sorted in alphabetical ascending
     * order.
     *
     * Callers of this method must hold the global lock in at least MODE_IS.
     *
     * Unlike DatabaseHolder::getNames(), this does not return databases that are empty.
     */
    std::vector<DatabaseName> getAllDbNames() const;

    /**
     * This function gets all the database names associated with tenantId. The result is sorted in
     * alphabetical ascending order.
     *
     * Callers of this method must hold the global lock in at least MODE_IS.
     *
     * Unlike DatabaseHolder::getNames(), this does not return databases that are empty.
     */
    std::vector<DatabaseName> getAllDbNamesForTenant(boost::optional<TenantId> tenantId) const;

    /**
     * This function gets all tenantIds in the database in ascending order.
     *
     * Callers of this method must hold the global lock in at least MODE_IS.
     *
     * Only returns tenantIds which are attached to at least one non-empty database.
     */
    std::set<TenantId> getAllTenants() const;

    /**
     * This function gets all the database names. The result is sorted in alphabetical ascending
     * order. The returned list is consistent with the storage snapshot.
     *
     * Callers of this method must hold an active storage snapshot. This method takes a global lock
     * in MODE_IS.
     *
     * Unlike DatabaseHolder::getNames(), this does not return databases that are empty.
     */
    std::vector<DatabaseName> getAllConsistentDbNames(OperationContext* opCtx) const;

    /**
     * This function gets all the database names associated with tenantId. The result is sorted in
     * alphabetical ascending order. The returned list is consistent with the storage snapshot.
     *
     * Callers of this method must hold an active storage snapshot. This method takes a global lock
     * in MODE_IS.
     *
     * Unlike DatabaseHolder::getNames(), this does not return databases that are empty.
     */
    std::vector<DatabaseName> getAllConsistentDbNamesForTenant(
        OperationContext* opCtx, boost::optional<TenantId> tenantId) const;

    /**
     * Marks the given database as drop pending.
     */
    void addDropPending(const DatabaseName&);

    /**
     * Unmarks the given database as drop pending.
     */
    void removeDropPending(const DatabaseName&);

    /**
     * Returns whether the given database is marked as drop pending.
     */
    bool isDropPending(const DatabaseName&) const;

    /**
     * Statistics for the types of collections in the catalog.
     * Total collections = 'internal' + 'userCollections' + 'userTimeseries'
     */
    struct Stats {
        // Non-system collections on non-internal databases
        // Note timeseries collections are not accounted here.
        int userCollections = 0;
        // Non-system capped collections on non-internal databases
        int userCapped = 0;
        // Non-system clustered collection on non-internal databases.
        // Note timeseries collections are not accounted here.
        int userClustered = 0;
        // Non-system timeseries collections on non-internal databases.
        int userTimeseries = 0;
        // System collections or collections on internal databases
        int internal = 0;
        // Client Side Field Level Encryption collections on non-internal databases
        int csfle = 0;
        // Queryable Encryption collections on non-internal databases
        int queryableEncryption = 0;
        // <>.system.profile collections on non-internal databases. Counted as `internal`.
        int systemProfile = 0;
    };

    /**
     * Returns statistics for the collection catalog.
     */
    Stats getStats() const;

    /**
     * Returns view statistics for the specified database.
     */
    boost::optional<ViewsForDatabase::Stats> getViewStatsForDatabase(
        OperationContext* opCtx, const DatabaseName& dbName) const;

    /**
     * Returns a set of databases, by name, that have view catalogs.
     */
    using ViewCatalogSet = absl::flat_hash_set<DatabaseName>;
    ViewCatalogSet getViewCatalogDbNames(OperationContext* opCtx) const;

    /**
     * Puts the catalog in closed state. In this state, the lookupNSSByUUID method will fall back to
     * the pre-close state to resolve queries for currently unknown UUIDs. This allows processes,
     * like authorization and replication, which need to do lookups outside of database locks, to
     * proceed.
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    void onCloseCatalog();

    /**
     * Puts the catalog back in open state, removing the pre-close state. See onCloseCatalog.
     *
     * Must be called with the global lock acquired in exclusive mode.
     */
    void onOpenCatalog();

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

    /**
     * Provides an iterable range for the collections belonging to the specified database.
     *
     * Will not observe any updates made to the catalog after the creation of the 'Range'. The
     * 'Range' object just remain in scope for the duration of the iteration.
     */
    Range range(const DatabaseName& dbName) const;

    /**
     * Ensures we have a MODE_X lock on a collection or MODE_IX lock for newly created collections.
     */
    static void invariantHasExclusiveAccessToCollection(OperationContext* opCtx,
                                                        const NamespaceString& nss);
    static bool hasExclusiveAccessToCollection(OperationContext* opCtx, const NamespaceString& nss);

    /**
     * Returns HistoricalCatalogIdTracker for historical namespace/uuid mappings to catalogId based
     * on timestamp.
     */
    const HistoricalCatalogIdTracker& catalogIdTracker() const;
    HistoricalCatalogIdTracker& catalogIdTracker();

    class BatchedCollectionWrite;

private:
    friend class CollectionCatalog::iterator;

    // We only allow the CollectionWriter class to interface with the catalog. This is to prevent
    // misuse (i.e. bypassing existing infrastructure) and to only provide a single way to interact
    // with the catalog for writers.
    friend class CollectionWriter;

    // We allow this class to access the internal methods as it deals with opening/closing the
    // catalog and will perform live modifications to the catalog.
    friend class catalog::CatalogControlUtils;

    /**
     * Returns a non-const Collection pointer that corresponds to the provided NamespaceString/UUID
     * for a DDL operation.
     *
     * A MODE_X collection lock is required to call this function, unless the namespace/UUID
     * corresponds to an uncommitted collection creation in which case a MODE_IX lock is sufficient.
     *
     * A WriteUnitOfWork must be active and the instance returned will be created using
     * copy-on-write and will be different than prior calls to lookupCollection. However, subsequent
     * calls to lookupCollection will return the same instance as this function as long as the
     * WriteUnitOfWork remains active.
     *
     * When the WriteUnitOfWork commits future versions of the CollectionCatalog will return this
     * instance. If the WriteUnitOfWork rolls back the instance will be discarded.
     *
     * It is safe to write to the returned instance in onCommit handlers but not in onRollback
     * handlers.
     *
     * Returns nullptr if the 'uuid' is not known.
     */
    Collection* lookupCollectionByUUIDForMetadataWrite(OperationContext* opCtx,
                                                       const UUID& uuid) const;
    Collection* lookupCollectionByNamespaceForMetadataWrite(OperationContext* opCtx,
                                                            const NamespaceString& nss) const;

    class PublishCatalogUpdates;

    /**
     * Returns whether the collection has pending commits.
     */
    bool _collectionHasPendingCommits(const NamespaceStringOrUUID& nssOrUUID) const;

    /**
     * Gets Collections by UUID/Namespace.
     */
    std::shared_ptr<const Collection> _getCollectionByNamespace(OperationContext* opCtx,
                                                                const NamespaceString& nss) const;

    std::shared_ptr<const Collection> _getCollectionByUUID(OperationContext* opCtx,
                                                           const UUID& uuid) const;

    /**
     * Resolves and validates the namespace from the given DatabaseName and UUID.
     *
     * This will also lookup in the commit pending entries if passed true for withCommitPending.
     */
    NamespaceString _resolveNamespaceStringFromDBNameAndUUID(OperationContext* opCtx,
                                                             const DatabaseName& dbName,
                                                             const UUID& uuid,
                                                             bool withCommitPending) const;

    /**
     * This function gets the NamespaceString from the collection catalog entry that
     * corresponds to UUID uuid. If no collection exists with the uuid, return
     * boost::none. See onCloseCatalog/onOpenCatalog for more info.
     *
     * This will also lookup in the commit pending entries if passed true for withCommitPending.
     */
    boost::optional<NamespaceString> _lookupNSSByUUID(OperationContext* opCtx,
                                                      const UUID& uuid,
                                                      bool withCommitPending) const;

    /**
     * Checks if an instance of the given namespace or UUID has already been instantiated for the
     * current snapshot, either in `OpenedCollections` or in `UncommittedCatalogUpdates`.
     *
     * A non-empty optional containing nullptr indicates the collection has been renamed or dropped.
     */
    boost::optional<std::shared_ptr<const Collection>> _findInstantiatedCollectionByNamespace(
        OperationContext* opCtx, const NamespaceString& nss) const;
    boost::optional<std::shared_ptr<const Collection>> _findInstantiatedCollectionByUUID(
        OperationContext* opCtx, const UUID& uuid) const;
    boost::optional<std::shared_ptr<const Collection>> _findInstantiatedCollectionByNamespaceOrUUID(
        OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) const;

    /**
     * Looks up the Collection corresponding to the provided namespace or UUID in the catalog of
     * committed/registered collections.
     * Does not find previously instantiated collections for this snapshot in `OpenedCollections`
     * or `UncommittedCatalogUpdates`. In particular, it will not return changes from the active
     * `WriteUnitOfWork`, which are generally expected to be visible to users of the catalog.
     * The caller is expected to have previously checked for those.
     */
    std::shared_ptr<Collection> _lookupCollectionByNamespaceNoFindInstantiated(
        const NamespaceString& nss) const;
    std::shared_ptr<Collection> _lookupCollectionByUUIDNoFindInstantiated(UUID uuid) const;
    std::shared_ptr<Collection> _lookupCollectionByNamespaceOrUUIDNoFindInstantiated(
        const NamespaceStringOrUUID& nssOrUUID) const;

    /**
     * Register the collection.
     */
    void _registerCollection(OperationContext* opCtx,
                             std::shared_ptr<Collection> collection,
                             boost::optional<Timestamp> commitTime);

    const Collection* _lookupSystemViews(OperationContext* opCtx, const DatabaseName& dbName) const;

    /**
     * Searches for a catalog entry at a point-in-time.
     */
    boost::optional<durable_catalog::CatalogEntry> _fetchPITCatalogEntry(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nssOrUUID,
        boost::optional<Timestamp> readTimestamp) const;

    /**
     * Tries to create a Collection instance using existing shared collection state. Returns nullptr
     * if unable to do so.
     */
    std::shared_ptr<Collection> _createCompatibleCollection(
        OperationContext* opCtx,
        const std::shared_ptr<const Collection>& latestCollection,
        boost::optional<Timestamp> readTimestamp,
        const durable_catalog::CatalogEntry& catalogEntry) const;

    /**
     * Creates a Collection instance from scratch if the ident has not yet been dropped.
     */
    std::shared_ptr<Collection> _createNewPITCollection(
        OperationContext* opCtx,
        boost::optional<Timestamp> readTimestamp,
        const durable_catalog::CatalogEntry& catalogEntry) const;

    /**
     * Retrieves the views for a given database, including any uncommitted changes for this
     * operation.
     */
    boost::optional<const ViewsForDatabase&> _getViewsForDatabase(OperationContext* opCtx,
                                                                  const DatabaseName& dbName) const;

    /**
     * Iterates over databases, and performs a callback on each database. If any callback fails,
     * returns its error code. If tenantId is set, will iterate only over databases with that
     * tenantId. nextUpperBound is a callback that controls how we iterate -- given the current
     * database name, returns a <DatabaseName, UUID> pair which must be strictly less than the next
     * entry we iterate to.
     */
    Status _iterAllDbNamesHelper(
        const boost::optional<TenantId>& tenantId,
        const std::function<Status(const DatabaseName&)>& callback,
        const std::function<std::pair<DatabaseName, UUID>(const DatabaseName&)>& nextLowerBound)
        const;

    /**
     * Sets all namespaces used by views for a database. Will uassert if there is a conflicting
     * collection name in the catalog.
     */
    void _replaceViewsForDatabase(const DatabaseName& dbName, ViewsForDatabase&& views);

    /**
     * Throws 'WriteConflictException' if given namespace is already registered with the catalog, as
     * either a view or collection. The results will include namespaces which have been registered
     * by preCommitHooks on other threads, but which have not truly been committed yet.
     *
     * If 'type' is set to 'NamespaceType::kCollection', we will only check for collisions with
     * collections. If set to 'NamespaceType::kAll', we will check against both collections and
     * views.
     */
    enum class NamespaceType { kAll, kCollection };
    void _ensureNamespaceDoesNotExist(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      NamespaceType type) const;

    /**
     * Returns true if catalog information about this namespace or UUID should be looked up from the
     * durable catalog rather than using the in-memory state of the catalog.
     *
     * This is true when either:
     *  - The readTimestamp is prior to the minimum valid timestamp for the collection corresponding
     *    to this namespace, or
     *  - There's no read timestamp provided and this namespace has a pending DDL operation that has
     *    not completed yet (which would imply that the latest version of the catalog may or may not
     *    match the state of the durable catalog for this collection).
     */
    bool _needsOpenCollection(OperationContext* opCtx,
                              const NamespaceStringOrUUID& nsOrUUID,
                              boost::optional<Timestamp> readTimestamp) const;

    /**
     * Returns the collection pointer representative of 'nssOrUUID' at the provided read timestamp.
     * If no timestamp is provided, returns instance of the latest collection. The returned
     * collection instance is only valid while the storage snapshot is open and becomes invalidated
     * when the snapshot is closed.
     *
     * Returns nullptr when reading from a point-in-time where the collection did not exist.
     */
    const Collection* _openCollection(OperationContext* opCtx,
                                      const NamespaceStringOrUUID& nssOrUUID,
                                      boost::optional<Timestamp> readTimestamp) const;

    // Helpers to perform openCollection at latest or at point-in-time on Namespace/UUID.
    const Collection* _openCollectionAtLatestByNamespaceOrUUID(
        OperationContext* opCtx, const NamespaceStringOrUUID& nssOrUUID) const;
    const Collection* _openCollectionAtPointInTimeByNamespaceOrUUID(
        OperationContext* opCtx,
        const NamespaceStringOrUUID& nssOrUUID,
        Timestamp readTimestamp) const;

    /**
     * When present, indicates that the catalog is in closed state, and contains a map from UUID
     * to pre-close NSS. See also onCloseCatalog.
     */
    std::shared_ptr<const mongo::stdx::unordered_map<UUID, NamespaceString, UUID::Hash>>
        _shadowCatalog;

    using CollectionCatalogMap =
        immutable::unordered_map<UUID, std::shared_ptr<Collection>, UUID::Hash>;
    using NamespaceCollectionMap =
        immutable::unordered_map<NamespaceString, std::shared_ptr<Collection>>;
    using UncommittedViewsSet = immutable::unordered_set<NamespaceString>;
    using ViewsForDatabaseMap = immutable::unordered_map<DatabaseName, ViewsForDatabase>;

    CollectionCatalogMap _catalog;
    OrderedCollectionMap _orderedCollections;  // Ordered by <dbName, collUUID> pair
    NamespaceCollectionMap _collections;
    UncommittedViewsSet _uncommittedViews;

    // Namespaces and UUIDs in pending commit. The opened storage snapshot must be consulted to
    // confirm visibility. The instance may be used if the namespace/uuid are otherwise unoccupied
    // in the CollectionCatalog.
    immutable::unordered_map<NamespaceString, std::shared_ptr<Collection>> _pendingCommitNamespaces;
    immutable::unordered_map<UUID, std::shared_ptr<Collection>, UUID::Hash> _pendingCommitUUIDs;

    // Provides functionality to lookup catalogId by namespace/uuid for a given timestamp.
    HistoricalCatalogIdTracker _catalogIdTracker;

    // Map of database names to their corresponding views and other associated state.
    ViewsForDatabaseMap _viewsForDatabase;

    // Set of databases which are currently in the process of being dropped.
    immutable::unordered_set<DatabaseName> _dropPendingDatabases;

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

    // Tracks usage of collection usage features (e.g. capped).
    Stats _stats;
};

}  // namespace mongo
