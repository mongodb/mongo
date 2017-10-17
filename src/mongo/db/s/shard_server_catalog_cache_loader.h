/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/operation_context_group.h"
#include "mongo/db/s/namespace_metadata_change_notifications.h"
#include "mongo/s/catalog_cache_loader.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

/**
 * Shard implementation of the CatalogCacheLoader used by the CatalogCache. Retrieves chunk metadata
 * for the CatalogCache on shards.
 *
 * If a shard primary, retrieves chunk metadata from the config server and maintains a persisted
 * copy of that chunk metadata so shard secondaries can access the metadata. If a shard secondary,
 * retrieves chunk metadata from the shard persisted chunk metadata.
 */
class ShardServerCatalogCacheLoader : public CatalogCacheLoader {
    MONGO_DISALLOW_COPYING(ShardServerCatalogCacheLoader);

public:
    ShardServerCatalogCacheLoader(std::unique_ptr<CatalogCacheLoader> configServerLoader);
    ~ShardServerCatalogCacheLoader();

    /**
     * Initializes internal state so that the loader behaves as a primary or secondary. This can
     * only be called once, when the sharding state is initialized.
     */
    void initializeReplicaSetRole(bool isPrimary) override;

    /**
     * Updates internal state so that the loader can start behaving like a secondary.
     */
    void onStepDown() override;

    /**
     * Updates internal state so that the loader can start behaving like a primary.
     */
    void onStepUp() override;

    /**
     * Sets any notifications waiting for this version to arrive and invalidates the catalog cache's
     * chunk metadata for collection 'nss' so that the next caller provokes a refresh.
     */
    void notifyOfCollectionVersionUpdate(const NamespaceString& nss) override;

    /**
     * This must be called serially, never in parallel, including waiting for the returned
     * Notification to be signalled.
     *
     * This function is robust to unexpected version requests from the CatalogCache. Requesting
     * versions with epoches that do not match anything on the config server will not affect or
     * clear the locally persisted metadata. Requesting versions higher than anything previous
     * requested, or versions lower than already requested, will not mess up the locally persisted
     * metadata, and will return what was requested if it exists.
     */
    std::shared_ptr<Notification<void>> getChunksSince(
        const NamespaceString& nss,
        ChunkVersion version,
        stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn)
        override;

    void waitForCollectionFlush(OperationContext* opCtx, const NamespaceString& nss) override;

private:
    // Differentiates the server's role in the replica set so that the chunk loader knows whether to
    // load metadata locally or remotely.
    enum class ReplicaSetRole { None, Secondary, Primary };

    /**
     * This represents an update task for the persisted chunk metadata. The task will either be to
     * apply a set up updated chunks to the shard persisted metadata store or to drop the persisted
     * metadata for a specific collection.
     */
    struct Task {
        MONGO_DISALLOW_COPYING(Task);
        Task(Task&&) = default;

        /**
         * Initializes a task for either dropping or updating the persisted metadata for the
         * associated collection. Which type of task is determined by the Status of
         * 'statusWithCollectionAndChangedChunks', whether it is NamespaceNotFound or OK.
         *
         * Note: statusWithCollectionAndChangedChunks must always be NamespaceNotFound or
         * OK, otherwise the constructor will invariant because there is no task to complete.
         *
         * 'collectionAndChangedChunks' is only initialized if 'dropped' is false.
         * 'minimumQueryVersion' sets 'minQueryVersion'.
         * 'maxQueryVersion' is either set to the highest chunk version in
         * 'collectionAndChangedChunks' or ChunkVersion::UNSHARDED().
         */
        Task(StatusWith<CollectionAndChangedChunks> statusWithCollectionAndChangedChunks,
             ChunkVersion minimumQueryVersion,
             long long currentTerm);

        // Always-incrementing task number to uniquely identify different tasks
        uint64_t taskNum;

        // Chunks and Collection updates to be applied to the shard persisted metadata store.
        boost::optional<CollectionAndChangedChunks> collectionAndChangedChunks{boost::none};

        // The highest version that the loader had before going to the config server's metadata
        // store for updated chunks.
        // Used by the TaskList below to enforce consistent updates are applied.
        ChunkVersion minQueryVersion;

        // Either the highest chunk version in 'collectionAndChangedChunks' or the same as
        // 'minQueryVersion' if 'dropped' is true.
        // Used by the TaskList below to enforce consistent updates are applied.
        ChunkVersion maxQueryVersion;

        // Indicates whether the collection metadata must be cleared.
        bool dropped{false};

        // The term in which the loader scheduled this task.
        uint32_t termCreated;
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
    class TaskList {
    public:
        TaskList();

        /**
         * Adds 'task' to the back of the 'tasks' list.
         *
         * If 'task' is a drop task, clears 'tasks' except for the front active task, so that we
         * don't waste time applying changes we will just delete. If the one remaining task in the
         * list is already a drop task, the new one isn't added because it is redundant.
         */
        void addTask(Task task);

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
         * Must only be called if there is an active task. Behaves like a condition variable and
         * will be signaled when the active task has been completed.
         *
         * NOTE: Because this call unlocks and locks the provided mutex, it is not safe to use the
         * same task object on which it was called because it might have been deleted during the
         * unlocked period.
         */
        void waitForActiveTaskCompletion(stdx::unique_lock<stdx::mutex>& lg);

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
         * Iterates over the task list to retrieve the enqueued metadata. Only retrieves collects
         * data from tasks that have terms matching the specified 'term'.
         */
        CollectionAndChangedChunks getEnqueuedMetadataForTerm(const long long term) const;

    private:
        std::list<Task> _tasks{};

        // Condition variable which will be signaled whenever the active task from the tasks list is
        // completed. Must be used in conjunction with the loader's mutex.
        std::shared_ptr<stdx::condition_variable> _activeTaskCompletedCondVar;
    };

    typedef std::map<NamespaceString, TaskList> TaskLists;

    /**
     * Forces the primary to refresh its metadata for 'nss' and waits until this node's metadata
     * has caught up to the primary's.
     * Then retrieves chunk metadata from this node's persisted metadata store and passes it to
     * 'callbackFn'.
     */
    void _runSecondaryGetChunksSince(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& catalogCacheSinceVersion,
        stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn);

    /**
     * Refreshes chunk metadata from the config server's metadata store, and schedules maintenance
     * of the shard's persisted metadata store with the latest updates retrieved from the config
     * server.
     *
     * Then calls 'callbackFn' with metadata retrived locally from the shard persisted metadata
     * store and any in-memory tasks with terms matching 'currentTerm' enqueued to update that
     * store, GTE to 'catalogCacheSinceVersion'.
     *
     * Only run on the shard primary.
     */
    void _schedulePrimaryGetChunksSince(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& catalogCacheSinceVersion,
        long long currentTerm,
        stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn,
        std::shared_ptr<Notification<void>> notify);


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
        const long long term);

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
        const NamespaceString& nss,
        const ChunkVersion& catalogCacheSinceVersion,
        const long long term);

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
    Status _ensureMajorityPrimaryAndScheduleTask(OperationContext* opCtx,
                                                 const NamespaceString& nss,
                                                 Task task);

    /**
     * Schedules tasks in the 'nss' task list to execute until the task list is depleted.
     *
     * Only run on the shard primary.
     */
    void _runTasks(const NamespaceString& nss);

    /**
     * Executes the task at the front of the task list for 'nss'. The task will either drop 'nss's
     * metadata or apply a set of updates.
     *
     * Only run on the shard primary.
     */
    void _updatePersistedMetadata(OperationContext* opCtx, const NamespaceString& nss);

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
        OperationContext* opCtx, const NamespaceString& nss, const ChunkVersion& version);

    // Used by the shard primary to retrieve chunk metadata from the config server.
    const std::unique_ptr<CatalogCacheLoader> _configServerLoader;

    // Thread pool used to load chunk metadata.
    ThreadPool _threadPool;

    NamespaceMetadataChangeNotifications _namespaceNotifications;

    // Protects the class state below.
    stdx::mutex _mutex;

    // Map to track in progress persisted cache updates on the shard primary.
    TaskLists _taskLists;

    // This value is bumped every time the set of currently scheduled tasks should no longer be
    // running. This includes, replica set state transitions and shutdown.
    long long _term{0};

    // Indicates whether this server is the primary or not, so that the appropriate loading action
    // can be taken.
    ReplicaSetRole _role{ReplicaSetRole::None};

    // The collection of operation contexts in use by all threads.
    OperationContextGroup _contexts;
};

}  // namespace mongo
