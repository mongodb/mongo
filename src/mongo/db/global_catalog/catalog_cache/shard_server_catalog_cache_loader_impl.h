/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/db/global_catalog/catalog_cache/config_server_catalog_cache_loader.h"
#include "mongo/db/global_catalog/catalog_cache/namespace_metadata_change_notifications.h"
#include "mongo/db/global_catalog/catalog_cache/shard_server_catalog_cache_loader.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_group.h"
#include "mongo/db/versioning_protocol/chunk_version.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/thread_pool.h"
#include "mongo/util/future.h"

#include <cstdint>
#include <list>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

/**
 * Shard implementation of the CatalogCacheLoader used by the CatalogCache. Retrieves chunk metadata
 * for the CatalogCache on shards.
 *
 * If a shard primary, retrieves chunk metadata from the config server and maintains a persisted
 * copy of that chunk metadata so shard secondaries can access the metadata. If a shard secondary,
 * retrieves chunk metadata from the shard persisted chunk metadata.
 */
class ShardServerCatalogCacheLoaderImpl : public ShardServerCatalogCacheLoader {
    ShardServerCatalogCacheLoaderImpl(const ShardServerCatalogCacheLoaderImpl&) = delete;
    ShardServerCatalogCacheLoaderImpl& operator=(const ShardServerCatalogCacheLoaderImpl&) = delete;

public:
    ShardServerCatalogCacheLoaderImpl(
        std::unique_ptr<ConfigServerCatalogCacheLoader> configServerLoader);
    ~ShardServerCatalogCacheLoaderImpl() override;

    void initializeReplicaSetRole(bool isPrimary) override;
    void onStepDown() override;
    void onStepUp() override;
    void shutDown() override;
    void onReplicationRollback() override;
    void notifyOfCollectionRefreshEndMarkerSeen(const NamespaceString& nss,
                                                const Timestamp& commitTime) override;

    SemiFuture<CollectionAndChangedChunks> getChunksSince(const NamespaceString& nss,
                                                          ChunkVersion version) override;

    SemiFuture<DatabaseType> getDatabase(const DatabaseName& dbName) override;

    void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) override;
    void waitForDatabaseFlush(OperationContext* opCtx, const DatabaseName& dbName) override;

private:
    // Differentiates the server's role in the replica set so that the chunk loader knows whether to
    // load metadata locally or remotely.
    enum class ReplicaSetRole { None, Secondary, Primary };

    /**
     * This represents an update task for the persisted chunk metadata. The task will either be to
     * apply a set up updated chunks to the shard persisted metadata store or to drop the persisted
     * metadata for a specific collection.
     */
    struct CollAndChunkTask {
        CollAndChunkTask(const CollAndChunkTask&) = delete;
        CollAndChunkTask& operator=(const CollAndChunkTask&) = delete;
        CollAndChunkTask(CollAndChunkTask&&) = default;

        /**
         * Initializes a task for either dropping or updating the persisted metadata for the
         * associated collection. Which type of task is determined by the Status of
         * 'statusWithCollectionAndChangedChunks', whether it is NamespaceNotFound or OK.
         *
         * Note: statusWithCollectionAndChangedChunks must always be NamespaceNotFound or
         * OK, otherwise the constructor will invariant because there is no task to complete.
         *
         *
         * 'collectionAndChangedChunks' is only initialized if 'dropped' is false.
         * 'minimumQueryVersion' sets 'minQueryVersion'.
         * 'maxQueryVersion' is either set to the highest chunk version in
         * 'collectionAndChangedChunks' or ChunkVersion::UNSHARDED().
         */
        CollAndChunkTask(
            StatusWith<CollectionAndChangedChunks> statusWithCollectionAndChangedChunks,
            ChunkVersion minimumQueryVersion,
            long long currentTerm);

        // Always-incrementing task number to uniquely identify different tasks
        uint64_t taskNum;

        // Chunks and Collection updates to be applied to the shard persisted metadata store.
        boost::optional<CollectionAndChangedChunks> collectionAndChangedChunks{boost::none};

        // The highest version that the loader had before going to the config server's metadata
        // store for updated chunks.
        // Used by the CollAndChunkTaskList below to enforce consistent updates are applied.
        ChunkVersion minQueryVersion;

        // Either the highest chunk version in 'collectionAndChangedChunks' or the same as
        // 'minQueryVersion' if 'dropped' is true.
        // Used by the CollAndChunkTaskList below to enforce consistent updates are
        // applied.
        ChunkVersion maxQueryVersion;

        // Indicates whether the collection metadata must be cleared.
        bool dropped{false};

        // The term in which the loader scheduled this task.
        uint32_t termCreated;

        std::string toString() const {
            std::stringstream ss;
            ss << "CollAndChunkTask -"
               << " taskNum: " << taskNum << ", collectionAndChangedChunksSize: "
               << (collectionAndChangedChunks ? collectionAndChangedChunks->changedChunks.size()
                                              : -1)
               << ", minQueryVersion: "
               << (minQueryVersion.isSet() ? minQueryVersion.toString() : "(unset)")
               << ", maxQueryVersion: "
               << (maxQueryVersion.isSet() ? maxQueryVersion.toString() : "(unset)")
               << ", dropped: " << dropped << ", termCreated: " << termCreated;
            return ss.str();
        }
    };

    /**
     * A list (work queue) of updates to apply to the shard persisted metadata store for a specific
     * collection. Enforces that tasks that are added to the list are either consistent:
     *
     *     tasks[i].minQueryVersion == tasks[i-1].maxQueryVersion.
     *
     * or applying a complete update from the minumum version, where
     *
     *     minQueryVersion == ChunkVersion::UNSHARDED().
     */
    class CollAndChunkTaskList {
    public:
        CollAndChunkTaskList();

        /**
         * Adds 'task' to the back of the 'tasks' list.
         *
         * If 'task' is a drop task, clears 'tasks' except for the front active task, so that we
         * don't waste time applying changes we will just delete. If the one remaining task in the
         * list is already a drop task, the new one isn't added because it is redundant.
         */
        void addTask(CollAndChunkTask task);

        auto& front() {
            invariant(!_tasks.empty());
            return _tasks.front();
        }

        auto& back() {
            invariant(!_tasks.empty());
            return _tasks.back();
        }

        auto begin() {
            invariant(!_tasks.empty());
            return _tasks.begin();
        }

        auto end() {
            invariant(!_tasks.empty());
            return _tasks.end();
        }

        void pop_front();

        bool empty() const {
            return _tasks.empty();
        }

        /**
         * Checks whether 'term' matches the term of the latest task in the task list. This is
         * useful to check whether the task list has outdated data that's no longer valid to use in
         * the current/new term specified by 'term'.
         */
        bool hasTasksFromThisTerm(long long term) const;

        /**
         * Gets the last task's highest version -- this is the most up to date version.
         */
        ChunkVersion getHighestVersionEnqueued() const;

        /**
         * Iterates over the task list to retrieve the enqueued metadata. Only retrieves
         * collects data from tasks that have terms matching the specified 'term'.
         */
        CollectionAndChangedChunks getEnqueuedMetadataForTerm(long long term) const;


    private:
        friend class ShardServerCatalogCacheLoaderImpl;

        std::list<CollAndChunkTask> _tasks{};

        // Condition variable which will be signaled whenever the active task from the tasks list is
        // completed. Must be used in conjunction with the loader's mutex.
        std::shared_ptr<stdx::condition_variable> _activeTaskCompletedCondVar;
    };

    /**
     * This represents an update task for the persisted database metadata. The task will either be
     * to persist an update to the shard persisted metadata store or to drop the persisted
     * metadata for a specific database.
     */
    struct DBTask {
        DBTask(const DBTask&) = delete;
        DBTask& operator=(const DBTask&) = delete;
        DBTask(DBTask&&) = default;

        /**
         * Initializes a task for either dropping or updating the persisted metadata for the
         * associated database. Which type of task is determined by the Status of 'swDatabaseType',
         * whether it is NamespaceNotFound or OK.
         *
         * Note: swDatabaseType must always be NamespaceNotFound or OK, otherwise the constructor
         * will invariant because there is no task to complete.
         */
        DBTask(StatusWith<DatabaseType> swDatabaseType, long long currentTerm);

        // Always-incrementing task number to uniquely identify different tasks
        uint64_t taskNum;

        // If boost::none, indicates this task is for a drop. Otherwise, contains the refreshed
        // database entry.
        boost::optional<DatabaseType> dbType;

        // The term in which the loader scheduled this task.
        uint32_t termCreated;
    };

    /**
     * A list (work queue) of updates to apply to the shard persisted metadata store for a specific
     * database.
     */
    class DbTaskList {
    public:
        DbTaskList();

        /**
         * Adds 'task' to the back of the 'tasks' list.
         *
         * If 'task' is a drop task, clears 'tasks' except for the front active task, so that we
         * don't waste time applying changes we will just delete. If the one remaining task in the
         * list is already a drop task, the new one isn't added because it is redundant.
         */
        void addTask(DBTask task);

        auto& front() {
            invariant(!_tasks.empty());
            return _tasks.front();
        }

        auto& back() {
            invariant(!_tasks.empty());
            return _tasks.back();
        }

        auto begin() {
            invariant(!_tasks.empty());
            return _tasks.begin();
        }

        auto end() {
            invariant(!_tasks.empty());
            return _tasks.end();
        }

        void pop_front();

        bool empty() const {
            return _tasks.empty();
        }

    private:
        friend class ShardServerCatalogCacheLoaderImpl;

        std::list<DBTask> _tasks{};

        // Condition variable which will be signaled whenever the active task from the tasks list is
        // completed. Must be used in conjunction with the loader's mutex.
        std::shared_ptr<stdx::condition_variable> _activeTaskCompletedCondVar;
    };
    typedef std::map<DatabaseName, DbTaskList> DbTaskLists;

    typedef std::map<NamespaceString, CollAndChunkTaskList> CollAndChunkTaskLists;

    /**
     * Forces the primary to refresh its metadata for 'nss' and waits until this node's metadata
     * has caught up to the primary's.
     *
     * Returns chunk metadata from this node's persisted metadata store.
     */
    StatusWith<CollectionAndChangedChunks> _runSecondaryGetChunksSince(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& catalogCacheSinceVersion);

    /**
     * Refreshes chunk metadata from the config server's metadata store, and schedules maintenance
     * of the shard's persisted metadata store with the latest updates retrieved from the config
     * server.
     *
     * Returns the metadata retrieved locally from the shard persisted metadata
     * store and any in-memory enqueued tasks to update that store that match the given term,
     * greater then or equal to the given chunk version.
     *
     * Only run on the shard primary.
     */
    StatusWith<CollectionAndChangedChunks> _schedulePrimaryGetChunksSince(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& catalogCacheSinceVersion,
        long long termScheduled);

    /**
     * Forces the primary to refresh its metadata for 'dbName' and waits until this node's metadata
     * has caught up to the primary's.
     * Returns the database version from this node's persisted metadata store.
     */
    StatusWith<DatabaseType> _runSecondaryGetDatabase(OperationContext* opCtx,
                                                      const DatabaseName& dbName);

    /**
     * Refreshes db version from the config server's metadata store, and schedules maintenance
     * of the shard's persisted metadata store with the latest updates retrieved from the config
     * server.
     *
     * Returns the metadata retrieved locally from the shard persisted metadata to update that
     * store.
     *
     * Only run on the shard primary.
     */
    StatusWith<DatabaseType> _schedulePrimaryGetDatabase(OperationContext* opCtx,
                                                         const DatabaseName& dbName,
                                                         long long termScheduled);

    /**
     * Loads chunk metadata from the shard persisted metadata store and any in-memory tasks with
     * terms matching 'term' enqueued to update that store, GTE to 'catalogCacheSinceVersion'.
     *
     * Will return an empty CollectionAndChangedChunks object if no metadata is found (collection
     * was dropped).
     *
     * Only run on the shard primary.
     */
    StatusWith<CollectionAndChangedChunks> _getLoaderMetadata(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& catalogCacheSinceVersion,
        long long expectedTerm);

    /**
     * Loads chunk metadata from all in-memory tasks enqueued to update the shard persisted metadata
     * store for collection 'nss' that is GTE 'catalogCacheSinceVersion'. If
     * 'catalogCacheSinceVersion's epoch does not match that of the metadata enqueued, returns all
     * metadata. Ignores tasks with terms that do not match 'term': these are no longer valid.
     *
     * The bool returned in the pair indicates whether there are any tasks enqueued. If none are, it
     * is false. If it is true, and the CollectionAndChangedChunks returned is empty, this indicates
     * a drop was enqueued and there is no metadata.
     *
     * Only run on the shard primary.
     */
    std::pair<bool, CollectionAndChangedChunks> _getEnqueuedMetadata(
        const NamespaceString& nss, const ChunkVersion& catalogCacheSinceVersion, long long term);

    /**
     * First ensures that this server is a majority primary in the case of a replica set with two
     * primaries: we do not want a minority primary to see majority side routing table changes for
     * which the minority does not have the corresponding data.
     *
     * Then adds 'task' to the task list for 'nss'. If this creates a new task list, then
     * '_runTasks' is started on another thread to execute the tasks.
     *
     * Only run on the shard primary.
     */
    void _ensureMajorityPrimaryAndScheduleCollAndChunksTask(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            CollAndChunkTask task);

    void _ensureMajorityPrimaryAndScheduleDbTask(OperationContext* opCtx,
                                                 const DatabaseName& dbName,
                                                 DBTask task);
    /**
     * Schedules tasks in the 'nss' task list to execute until the task list is depleted.
     *
     * Only run on the shard primary.
     */
    void _runCollAndChunksTasks(const NamespaceString& nss);

    void _runDbTasks(const DatabaseName& dbName);

    /**
     * Executes the task at the front of the task list for 'nss'. The task will either drop 'nss's
     * metadata or apply a set of updates.
     *
     * Only run on the shard primary.
     */
    void _updatePersistedCollAndChunksMetadata(OperationContext* opCtx, const NamespaceString& nss);

    void _updatePersistedDbMetadata(OperationContext* opCtx, const DatabaseName& dbName);

    /**
     * Sends _flushRoutingTableCacheUpdates to the primary to force it to refresh its routing table
     * for collection 'nss' and then waits for the refresh to replicate to this node. Returns a
     * notification that can be used to wait for the refreshing flag to be set to true in the
     * config.collections entry to provide a consistent view of config.chunks.
     */
    NamespaceMetadataChangeNotifications::ScopedNotification
    _forcePrimaryCollectionRefreshAndWaitForReplication(OperationContext* opCtx,
                                                        const NamespaceString& nss);

    /**
     * Attempts to read the collection and chunk metadata since 'version' from the shard persisted
     * metadata store. Continues to retry reading the metadata until a complete diff is read
     * uninterrupted by concurrent updates.
     *
     * Returns a complete metadata update since 'version', which when applied to a complete metadata
     * store up to 'version' again produces a complete metadata store. Throws on error --
     * NamespaceNotFound error means the collection does not exist.
     */
    CollectionAndChangedChunks _getCompletePersistedMetadataForSecondarySinceVersion(
        OperationContext* opCtx,
        NamespaceMetadataChangeNotifications::ScopedNotification&& notif,
        const NamespaceString& nss,
        const ChunkVersion& version);

    // Loader used by the shard primary to retrieve the authoritative routing metadata from the
    // config server
    std::unique_ptr<ConfigServerCatalogCacheLoader> _configServerLoader;

    // Thread pool used to run blocking tasks which perform disk reads and writes
    std::shared_ptr<ThreadPool> _executor;

    // Registry of notifications for changes happening to the shard's on-disk routing information
    NamespaceMetadataChangeNotifications _namespaceNotifications;

    // Protects the class state below
    stdx::mutex _mutex;

    // True if shutDown was called.
    bool _inShutdown{false};

    // This value is bumped every time the set of currently scheduled tasks should no longer be
    // running. This includes, replica set state transitions and shutdown.
    long long _term{0};

    // Indicates whether this server is the primary or not, so that the appropriate loading action
    // can be taken.
    ReplicaSetRole _role{ReplicaSetRole::None};

    // The collection of operation contexts in use by all threads.
    OperationContextGroup _contexts;

    CollAndChunkTaskLists _collAndChunkTaskLists;
    DbTaskLists _dbTaskLists;
};

}  // namespace mongo
