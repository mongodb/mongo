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

#include "mongo/s/catalog_cache_loader.h"
#include "mongo/util/concurrency/thread_pool.h"

namespace mongo {

class ConfigServerCatalogCacheLoader;
class ThreadPoolInterface;

/**
 * Shard implementation of the CatalogCacheLoader used by the CatalogCache. Retrieves chunk metadata
 * for the CatalogCache on shards.
 *
 * If a shard primary, retrieves chunk metadata from the config server and maintains a persisted
 * copy of that chunk metadata so shard secondaries can access the metadata. If a shard secondary,
 * retrieves chunk metadata from the shard persisted chunk metadata.
 */
class ShardServerCatalogCacheLoader : public CatalogCacheLoader {
public:
    ShardServerCatalogCacheLoader(std::unique_ptr<CatalogCacheLoader> configLoader);
    ~ShardServerCatalogCacheLoader();

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

private:
    /**
     * This represents an update task for the persisted chunk metadata. The task will either be to
     * apply a set up updated chunks to the shard persisted metadata store or to drop the persisted
     * metadata for a specific collection.
     */
    struct Task {
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
             ChunkVersion minimumQueryVersion);

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
        /**
         * Adds 'task' to the back of the 'tasks' list.
         *
         * If 'task' is a drop task, clears 'tasks' except for the front active task, so that we
         * don't waste time applying changes we will just delete. If the one remaining task in the
         * list is already a drop task, the new one isn't added because it is redundant.
         */
        void addTask(Task task);

        /**
         * Returns the front of the 'tasks' list. Invariants if 'tasks' is empty.
         */
        const Task& getActiveTask() const;

        /**
         * Erases the current active task and updates 'activeTask' to the next task in 'tasks'.
         */
        void removeActiveTask();

        /**
         * Checks whether there are any tasks left.
         */
        const bool empty() {
            return _tasks.empty();
        }

        /**
         * Gets the last task's highest version -- this is the most up to date version.
         */
        ChunkVersion getHighestVersionEnqueued() const;

        /**
         * Iterates over the task list to retrieve the enqueued metadata.
         */
        CollectionAndChangedChunks getEnqueuedMetadata() const;

    private:
        std::list<Task> _tasks{};
    };

    typedef std::map<NamespaceString, TaskList> TaskLists;

    /**
     * Refreshes chunk metadata from the config server's metadata store, and schedules maintenance
     * of the shard's persisted metadata store with the latest updates retrieved from the config
     * server.
     *
     * Then calls 'callbackFn' with metadata loaded from the shard persisted metadata store, and any
     * in-memory task enqueued to update that store, GTE to 'catalogCacheSinceVersion'
     *
     * Only run on the shard primary.
     */
    void _schedulePrimayGetChunksSince(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& catalogCacheSinceVersion,
        stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn,
        std::shared_ptr<Notification<void>> notify);


    /**
     * Loads chunk metadata from the shard persisted metadata store, and any in-memory task enqueued
     * to update that store, GTE to 'catalogCacheSinceVersion'.
     *
     * Will return an empty CollectionAndChangedChunks object if no metadata is found (collection
     * was dropped).
     *
     * Only run on the shard primary.
     */
    StatusWith<CollectionAndChangedChunks> _getLoaderMetadata(
        OperationContext* opCtx,
        const NamespaceString& nss,
        const ChunkVersion& catalogCacheSinceVersion);

    /**
     * Loads chunk metadata from all in-memory tasks enqueued to update the shard persisted metadata
     * store for collection 'nss' that is GTE 'catalogCacheSinceVersion'. If
     * 'catalogCacheSinceVersion's epoch does not match that of the metadata enqueued, returns all
     * metadata.
     *
     * The bool returned in the pair indicates whether there are any tasks enqueued. If none are, it
     * is false. If it is true, and the CollectionAndChangedChunks returned is empty, this indicates
     * a drop was enqueued and there is no metadata.
     *
     * Only run on the shard primary.
     */
    std::pair<bool, CollectionAndChangedChunks> _getEnqueuedMetadata(
        const NamespaceString& nss, const ChunkVersion& catalogCacheSinceVersion);

    /**
     * Adds 'task' to the task list for 'nss'. If this creates a new task list, then '_runTasks' is
     * started on another thread to execute the tasks.
     *
     * Only run on the shard primary.
     */
    Status _scheduleTask(const NamespaceString& nss, Task task);

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
    bool _updatePersistedMetadata(OperationContext* opCtx, const NamespaceString& nss);

    // Used by the shard primary to retrieve chunk metadata from the config server.
    const std::unique_ptr<CatalogCacheLoader> _configServerLoader;

    // Thread pool used to load chunk metadata.
    ThreadPool _threadPool;

    // Protects the class state below.
    stdx::mutex _mutex;

    // Map to track in progress persisted cache updates on the shard primary.
    TaskLists _taskLists;
};

}  // namespace mongo
