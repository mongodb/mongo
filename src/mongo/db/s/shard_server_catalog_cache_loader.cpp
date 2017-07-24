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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_server_catalog_cache_loader.h"

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_group.h"
#include "mongo/db/s/shard_metadata_util.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/s/catalog/type_shard_collection.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/config_server_catalog_cache_loader.h"
#include "mongo/s/grid.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"

namespace mongo {

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;
using namespace shardmetadatautil;

namespace {

/**
 * Constructs the options for the loader thread pool.
 */
ThreadPool::Options makeDefaultThreadPoolOptions() {
    ThreadPool::Options options;
    options.poolName = "ShardServerCatalogCacheLoader";
    options.minThreads = 0;
    options.maxThreads = 6;

    // Ensure all threads have a client.
    options.onCreateThread = [](const std::string& threadName) {
        Client::initThread(threadName.c_str());
    };

    return options;
}

/**
 * Takes a CollectionAndChangedChunks object and persists the changes to the shard's metadata
 * collections.
 *
 * Returns ConflictingOperationInProgress if a chunk is found with a new epoch.
 */
Status persistCollectionAndChangedChunks(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const CollectionAndChangedChunks& collAndChunks) {
    // Update the collections collection entry for 'nss' in case there are any new updates.
    ShardCollectionType update = ShardCollectionType(nss,
                                                     nss,
                                                     collAndChunks.epoch,
                                                     collAndChunks.shardKeyPattern,
                                                     collAndChunks.defaultCollation,
                                                     collAndChunks.shardKeyIsUnique);
    Status status = updateShardCollectionsEntry(
        opCtx, BSON(ShardCollectionType::uuid() << nss.ns()), update.toBSON(), true /*upsert*/);
    if (!status.isOK()) {
        return status;
    }

    // Mark the chunk metadata as refreshing, so that secondaries are aware of refresh.
    status = setPersistedRefreshFlags(opCtx, nss);
    if (!status.isOK()) {
        return status;
    }

    // Update the chunks.
    status = updateShardChunks(opCtx, nss, collAndChunks.changedChunks, collAndChunks.epoch);
    if (!status.isOK()) {
        return status;
    }

    // Mark the chunk metadata as done refreshing.
    status =
        unsetPersistedRefreshFlags(opCtx, nss, collAndChunks.changedChunks.back().getVersion());
    if (!status.isOK()) {
        return status;
    }

    return Status::OK();
}

/**
 * This function will throw on error!
 *
 * Retrieves the persisted max chunk version for 'nss', if there are any persisted chunks. If there
 * are none -- meaning there's no persisted metadata for 'nss' --, returns a
 * ChunkVersion::UNSHARDED() version.
 *
 * It is unsafe to call this when a task for 'nss' is running concurrently because the collection
 * could be dropped and recreated between reading the collection epoch and retrieving the chunk,
 * which would make the returned ChunkVersion corrupt.
 */
ChunkVersion getPersistedMaxVersion(OperationContext* opCtx, const NamespaceString& nss) {
    // Must read the collections entry to get the epoch to pass into ChunkType for shard's chunk
    // collection.
    auto statusWithCollection = readShardCollectionsEntry(opCtx, nss);
    if (statusWithCollection == ErrorCodes::NamespaceNotFound) {
        // There is no persisted metadata.
        return ChunkVersion::UNSHARDED();
    }
    uassert(ErrorCodes::OperationFailed,
            str::stream() << "Failed to read persisted collections entry for collection '"
                          << nss.ns()
                          << "' due to '"
                          << statusWithCollection.getStatus().toString()
                          << "'.",
            statusWithCollection.isOK());

    auto statusWithChunk =
        shardmetadatautil::readShardChunks(opCtx,
                                           nss,
                                           BSONObj(),
                                           BSON(ChunkType::lastmod() << -1),
                                           1LL,
                                           statusWithCollection.getValue().getEpoch());
    uassert(ErrorCodes::OperationFailed,
            str::stream() << "Failed to read highest version persisted chunk for collection '"
                          << nss.ns()
                          << "' due to '"
                          << statusWithChunk.getStatus().toString()
                          << "'.",
            statusWithChunk.isOK());

    return statusWithChunk.getValue().empty() ? ChunkVersion::UNSHARDED()
                                              : statusWithChunk.getValue().front().getVersion();
}

/**
 * This function will throw on error!
 *
 * Tries to find persisted chunk metadata with chunk versions GTE to 'version'.
 *
 *
 * If 'version's epoch matches persisted metadata, returns persisted metadata GTE 'version'.
 * If 'version's epoch doesn't match persisted metadata, returns all persisted metadata.
 * If collections entry does not exist, throws NamespaceNotFound error. Can return an empty
 * chunks vector in CollectionAndChangedChunks without erroring, if collections entry IS found.
 */
CollectionAndChangedChunks getPersistedMetadataSinceVersion(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            ChunkVersion version,
                                                            const bool okToReadWhileRefreshing) {
    ShardCollectionType shardCollectionEntry =
        uassertStatusOK(readShardCollectionsEntry(opCtx, nss));

    // If the persisted epoch doesn't match what the CatalogCache requested, read everything.
    ChunkVersion startingVersion = (shardCollectionEntry.getEpoch() == version.epoch())
        ? version
        : ChunkVersion(0, 0, shardCollectionEntry.getEpoch());

    QueryAndSort diff = createShardChunkDiffQuery(startingVersion);

    auto changedChunks = uassertStatusOK(
        readShardChunks(opCtx, nss, diff.query, diff.sort, boost::none, startingVersion.epoch()));

    return CollectionAndChangedChunks{shardCollectionEntry.getEpoch(),
                                      shardCollectionEntry.getKeyPattern().toBSON(),
                                      shardCollectionEntry.getDefaultCollation(),
                                      shardCollectionEntry.getUnique(),
                                      std::move(changedChunks)};
}

/**
 * Sends forceRoutingTableRefresh to the primary, to force the primary to refresh its routing table
 * entry for 'nss' and to obtain the primary's collectionVersion for 'nss' after the refresh.
 *
 * Returns the primary's returned collectionVersion for 'nss', or throws on error.
 */
ChunkVersion forcePrimaryToRefresh(OperationContext* opCtx, const NamespaceString& nss) {
    auto shardingState = ShardingState::get(opCtx);
    invariant(shardingState->enabled());

    auto selfShard = uassertStatusOK(
        Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardingState->getShardName()));

    auto cmdResponse = uassertStatusOK(selfShard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting{ReadPreference::PrimaryOnly},
        "admin",
        BSON("forceRoutingTableRefresh" << nss.ns()),
        Seconds{30},
        Shard::RetryPolicy::kIdempotent));
    uassertStatusOK(cmdResponse.commandStatus);

    return uassertStatusOK(
        ChunkVersion::parseFromBSONWithFieldForCommands(cmdResponse.response, "collectionVersion"));
}

/**
 * Reads the local chunk metadata to obtain the current ChunkVersion. If there is no local
 * metadata for the namespace, returns ChunkVersion::UNSHARDED(), since only metadata for sharded
 * collections is persisted.
 */
ChunkVersion getLocalVersion(OperationContext* opCtx, const NamespaceString& nss) {
    auto swRefreshState = getPersistedRefreshFlags(opCtx, nss);
    if (swRefreshState == ErrorCodes::NamespaceNotFound)
        return ChunkVersion::UNSHARDED();
    return uassertStatusOK(std::move(swRefreshState)).lastRefreshedCollectionVersion;
}

}  // namespace

void ShardServerCatalogCacheLoader::notifyOfCollectionVersionUpdate(OperationContext* opCtx,
                                                                    const NamespaceString& nss,
                                                                    const ChunkVersion& version) {
    Grid::get(opCtx)->catalogCache()->invalidateShardedCollection(nss);

    _namespaceNotifications.notifyChange(nss);
}

Status ShardServerCatalogCacheLoader::waitForCollectionVersion(OperationContext* opCtx,
                                                               const NamespaceString& nss,
                                                               const ChunkVersion& version) {
    invariant(!opCtx->lockState()->isLocked());
    while (true) {
        auto scopedNotification = _namespaceNotifications.createNotification(nss);

        auto swRefreshState = getPersistedRefreshFlags(opCtx, nss);
        if (!swRefreshState.isOK()) {
            return swRefreshState.getStatus();
        }
        RefreshState refreshState = swRefreshState.getValue();

        if (refreshState.lastRefreshedCollectionVersion.epoch() != version.epoch() ||
            refreshState.lastRefreshedCollectionVersion >= version) {
            return Status::OK();
        }

        scopedNotification.get(opCtx);
    }
}

ShardServerCatalogCacheLoader::ShardServerCatalogCacheLoader(
    std::unique_ptr<CatalogCacheLoader> configLoader)
    : _configServerLoader(std::move(configLoader)), _threadPool(makeDefaultThreadPoolOptions()) {
    _threadPool.startup();
}

ShardServerCatalogCacheLoader::~ShardServerCatalogCacheLoader() {
    _contexts.interrupt(ErrorCodes::InterruptedAtShutdown);
    _threadPool.shutdown();
    _threadPool.join();
    invariant(_contexts.isEmpty());
}

void ShardServerCatalogCacheLoader::initializeReplicaSetRole(bool isPrimary) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_role == ReplicaSetRole::None);

    if (isPrimary) {
        _role = ReplicaSetRole::Primary;
    } else {
        _role = ReplicaSetRole::Secondary;
    }
}

void ShardServerCatalogCacheLoader::onStepDown() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_role != ReplicaSetRole::None);
    _contexts.interrupt(ErrorCodes::PrimarySteppedDown);
    ++_term;
    _role = ReplicaSetRole::Secondary;
}

void ShardServerCatalogCacheLoader::onStepUp() {
    stdx::lock_guard<stdx::mutex> lock(_mutex);
    invariant(_role != ReplicaSetRole::None);
    _contexts.resetInterrupt();
    ++_term;
    _role = ReplicaSetRole::Primary;
}

std::shared_ptr<Notification<void>> ShardServerCatalogCacheLoader::getChunksSince(
    const NamespaceString& nss,
    ChunkVersion version,
    stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn) {
    long long currentTerm;
    bool isPrimary;
    {
        // Take the mutex so that we can discern whether we're primary or secondary and schedule a
        // task with the corresponding _term value.
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        invariant(_role != ReplicaSetRole::None);

        currentTerm = _term;
        isPrimary = (_role == ReplicaSetRole::Primary);
    }

    auto notify = std::make_shared<Notification<void>>();

    uassertStatusOK(_threadPool.schedule(
        [ this, nss, version, callbackFn, notify, isPrimary, currentTerm ]() noexcept {
            auto context = _contexts.makeOperationContext(*Client::getCurrent());
            try {
                if (isPrimary) {
                    _schedulePrimaryGetChunksSince(
                        context.opCtx(), nss, version, currentTerm, callbackFn, notify);
                } else {
                    _runSecondaryGetChunksSince(context.opCtx(), nss, version, callbackFn);
                }
            } catch (const DBException& ex) {
                callbackFn(context.opCtx(), ex.toStatus());
                notify->set();
            }
        }));

    return notify;
}

void ShardServerCatalogCacheLoader::_runSecondaryGetChunksSince(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn) {
    _forcePrimaryRefreshAndWaitForReplication(opCtx, nss);

    // Read the local metadata.
    auto swCollAndChunks =
        _getCompletePersistedMetadataForSecondarySinceVersion(opCtx, nss, catalogCacheSinceVersion);
    callbackFn(opCtx, std::move(swCollAndChunks));
}

/**
 * "Waiting for replication" by waiting to see a local version equal or greater to the primary's
 * collectionVersion is not so straightforward. A few key insights:
 *
 * 1) ChunkVersions are ordered, so within an epoch, we can wait for a particular ChunkVersion.
 *
 * 2) Epochs are not ordered. If we are waiting for epochB and see epochA locally, we can't know if
 *    the update for epochB already replicated or has yet to replicate.
 *
 *    To deal with this, on seeing epochA, we wait for one update. If we are now in epochB (e.g., if
 *    epochA was UNSHARDED) we continue waiting for updates until our version meets or exceeds the
 *    primary's. Otherwise, we throw an error. A caller can retry, which will cause us to ask the
 *    primary for a new collectionVersion to wait for. If we were behind, we continue waiting; if we
 *    were ahead, we now have a new target.
 *
 *    This only occurs if collections are being created, sharded, and dropped quickly.
 *
 * 3) Unsharded collections do not have epochs at all. A unique identifier for all collections,
 *    including unsharded, will be introduced in 3.6. Until then, we cannot differentiate between
 *    different incarnations of unsharded collections of the same name.
 *
 *    We do not deal with this at all. We report that we are "up to date" even if we are at an
 *    earlier incarnation of the unsharded collection.
 */
void ShardServerCatalogCacheLoader::_forcePrimaryRefreshAndWaitForReplication(
    OperationContext* opCtx, const NamespaceString& nss) {
    // Start listening for metadata updates before obtaining the primary's version, in case we
    // replicate an epoch change past the primary's version before reading locally.
    boost::optional<NamespaceMetadataChangeNotifications::ScopedNotification> notif(
        _namespaceNotifications.createNotification(nss));

    auto primaryVersion = forcePrimaryToRefresh(opCtx, nss);

    bool waitedForUpdate = false;
    while (true) {
        auto secondaryVersion = getLocalVersion(opCtx, nss);

        if (secondaryVersion.hasEqualEpoch(primaryVersion) && secondaryVersion >= primaryVersion) {
            return;
        }

        if (waitedForUpdate) {
            // If we still aren't in the primary's epoch, throw.
            uassert(ErrorCodes::ConflictingOperationInProgress,
                    "The collection has recently been dropped and recreated",
                    secondaryVersion.epoch() == primaryVersion.epoch());
        }

        // Wait for a chunk metadata update (either ChunkVersion increment or epoch change).
        notif->get(opCtx);
        notif.emplace(_namespaceNotifications.createNotification(nss));
        waitedForUpdate = true;
    }
}

void ShardServerCatalogCacheLoader::_schedulePrimaryGetChunksSince(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    long long termScheduled,
    stdx::function<void(OperationContext*, StatusWith<CollectionAndChangedChunks>)> callbackFn,
    std::shared_ptr<Notification<void>> notify) {

    // Get the max version the loader has.
    const ChunkVersion maxLoaderVersion = [&] {
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            auto taskListIt = _taskLists.find(nss);

            if (taskListIt != _taskLists.end() &&
                taskListIt->second.hasTasksFromThisTerm(termScheduled)) {
                // Enqueued tasks have the latest metadata
                return taskListIt->second.getHighestVersionEnqueued();
            }
        }

        // If there are no enqueued tasks, get the max persisted
        return getPersistedMaxVersion(opCtx, nss);
    }();

    auto remoteRefreshCallbackFn = [this,
                                    nss,
                                    catalogCacheSinceVersion,
                                    maxLoaderVersion,
                                    termScheduled,
                                    callbackFn,
                                    notify](
        OperationContext* opCtx,
        StatusWith<CollectionAndChangedChunks> swCollectionAndChangedChunks) {

        if (!swCollectionAndChangedChunks.isOK() &&
            swCollectionAndChangedChunks != ErrorCodes::NamespaceNotFound) {
            // No updates to apply. Do nothing.
        } else {
            // Enqueue a Task to apply the update retrieved from the config server, if new data was
            // retrieved.
            if (!swCollectionAndChangedChunks.isOK() ||
                (swCollectionAndChangedChunks.getValue()
                     .changedChunks.back()
                     .getVersion()
                     .epoch() != maxLoaderVersion.epoch()) ||
                (swCollectionAndChangedChunks.getValue().changedChunks.back().getVersion() >
                 maxLoaderVersion)) {
                Status scheduleStatus = _scheduleTask(
                    nss, Task{swCollectionAndChangedChunks, maxLoaderVersion, termScheduled});
                if (!scheduleStatus.isOK()) {
                    callbackFn(opCtx, scheduleStatus);
                    notify->set();
                    return;
                }
            }


            if (swCollectionAndChangedChunks.isOK()) {
                log() << "Cache loader remotely refreshed for collection " << nss
                      << " from collection version " << maxLoaderVersion
                      << " and found collection version "
                      << swCollectionAndChangedChunks.getValue().changedChunks.back().getVersion();

                // Metadata was found remotely -- otherwise would have received
                // NamespaceNotFound rather than Status::OK(). Return metadata for CatalogCache
                // that's GTE catalogCacheSinceVersion, from the loader's persisted and enqueued
                // metadata.

                swCollectionAndChangedChunks =
                    _getLoaderMetadata(opCtx, nss, catalogCacheSinceVersion, termScheduled);
                if (swCollectionAndChangedChunks.isOK()) {
                    // After finding metadata remotely, we must have found metadata locally.
                    invariant(!swCollectionAndChangedChunks.getValue().changedChunks.empty());
                }
            } else {  // NamespaceNotFound
                invariant(swCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound);
                log() << "Cache loader remotely refreshed for collection " << nss
                      << " from version " << maxLoaderVersion << " and no metadata was found.";
            }
        }

        // Complete the callbackFn work.
        callbackFn(opCtx, std::move(swCollectionAndChangedChunks));
        notify->set();
    };

    // Refresh the loader's metadata from the config server. The caller's request will
    // then be serviced from the loader's up-to-date metadata.
    _configServerLoader->getChunksSince(nss, maxLoaderVersion, remoteRefreshCallbackFn);
}

StatusWith<CollectionAndChangedChunks> ShardServerCatalogCacheLoader::_getLoaderMetadata(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    const long long term) {

    // Get the enqueued metadata first. Otherwise we could miss data between reading persisted and
    // enqueued, if an enqueued task finished after the persisted read but before the enqueued read.

    auto enqueuedRes = _getEnqueuedMetadata(nss, catalogCacheSinceVersion, term);
    bool tasksAreEnqueued = std::move(enqueuedRes.first);
    CollectionAndChangedChunks enqueued = std::move(enqueuedRes.second);

    // TODO: a primary can load metadata while updates are being applied once we have indexes on the
    // chunk collections that ensure new data is seen after query yields. This function keeps
    // retrying until no updates are applied concurrently. Waiting on SERVER-27714 to add indexes.
    auto swPersisted =
        _getCompletePersistedMetadataForSecondarySinceVersion(opCtx, nss, catalogCacheSinceVersion);
    CollectionAndChangedChunks persisted;
    if (swPersisted == ErrorCodes::NamespaceNotFound) {
        // No persisted metadata found, create an empty object.
        persisted = CollectionAndChangedChunks();
    } else if (!swPersisted.isOK()) {
        return swPersisted;
    } else {
        persisted = std::move(swPersisted.getValue());
    }

    log() << "Cache loader found "
          << (enqueued.changedChunks.empty()
                  ? (tasksAreEnqueued ? "a drop enqueued" : "no enqueued metadata")
                  : ("enqueued metadata from " +
                     enqueued.changedChunks.front().getVersion().toString() + " to " +
                     enqueued.changedChunks.back().getVersion().toString()))
          << " and " << (persisted.changedChunks.empty()
                             ? "no persisted metadata"
                             : ("persisted metadata from " +
                                persisted.changedChunks.front().getVersion().toString() + " to " +
                                persisted.changedChunks.back().getVersion().toString()))
          << ", GTE cache version " << catalogCacheSinceVersion;

    if (!tasksAreEnqueued) {
        // There are no tasks in the queue. Return the persisted metadata.
        return persisted;
    } else if (enqueued.changedChunks.empty() || enqueued.epoch != persisted.epoch) {
        // There is a task queue and either:
        // - nothing was returned, which means the last task enqueued is a drop task.
        // - the epoch changed in the enqueued metadata, which means there's a drop operation
        //   enqueued somewhere.
        // Either way, the persisted metadata is out-dated. Return enqueued results.
        return enqueued;
    } else if (persisted.changedChunks.empty()) {
        // Nothing is persisted. Return enqueued results.
        return enqueued;
    } else {
        // There can be overlap between persisted and enqueued metadata because enqueued work can
        // be applied while persisted was read. We must remove this overlap.

        const ChunkVersion minEnqueuedVersion = enqueued.changedChunks.front().getVersion();

        // Remove chunks from 'persisted' that are GTE the minimum in 'enqueued' -- this is
        // the overlap.
        auto persistedChangedChunksIt = persisted.changedChunks.begin();
        while (persistedChangedChunksIt != persisted.changedChunks.end() &&
               persistedChangedChunksIt->getVersion() < minEnqueuedVersion) {
            ++persistedChangedChunksIt;
        }
        persisted.changedChunks.erase(persistedChangedChunksIt, persisted.changedChunks.end());

        // Append 'enqueued's chunks to 'persisted', which no longer overlaps.
        persisted.changedChunks.insert(persisted.changedChunks.end(),
                                       enqueued.changedChunks.begin(),
                                       enqueued.changedChunks.end());

        return persisted;
    }
}

std::pair<bool, CollectionAndChangedChunks> ShardServerCatalogCacheLoader::_getEnqueuedMetadata(
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    const long long term) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    auto taskListIt = _taskLists.find(nss);

    if (taskListIt == _taskLists.end()) {
        return std::make_pair(false, CollectionAndChangedChunks());
    } else if (!taskListIt->second.hasTasksFromThisTerm(term)) {
        // If task list does not have a term that matches, there's no valid task data to collect.
        return std::make_pair(false, CollectionAndChangedChunks());
    }

    // Only return task data of tasks scheduled in the same term as the given 'term': older term
    // task data is no longer valid.
    CollectionAndChangedChunks collAndChunks = taskListIt->second.getEnqueuedMetadataForTerm(term);

    // Return all the results if 'catalogCacheSinceVersion's epoch does not match. Otherwise, trim
    // the results to be GTE to 'catalogCacheSinceVersion'.

    if (collAndChunks.epoch != catalogCacheSinceVersion.epoch()) {
        return std::make_pair(true, collAndChunks);
    }

    auto changedChunksIt = collAndChunks.changedChunks.begin();
    while (changedChunksIt != collAndChunks.changedChunks.end() &&
           changedChunksIt->getVersion() < catalogCacheSinceVersion) {
        ++changedChunksIt;
    }
    collAndChunks.changedChunks.erase(collAndChunks.changedChunks.begin(), changedChunksIt);

    return std::make_pair(true, collAndChunks);
}

Status ShardServerCatalogCacheLoader::_scheduleTask(const NamespaceString& nss, Task task) {
    stdx::lock_guard<stdx::mutex> lock(_mutex);

    const bool wasEmpty = _taskLists[nss].empty();
    _taskLists[nss].addTask(std::move(task));

    if (wasEmpty) {
        Status status = _threadPool.schedule([this, nss]() { _runTasks(nss); });
        if (!status.isOK()) {
            log() << "Cache loader failed to schedule persisted metadata update"
                  << " task for namespace '" << nss << "' due to '" << redact(status)
                  << "'. Clearing task list so that scheduling"
                  << " will be attempted by the next caller to refresh this namespace.";
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            _taskLists.erase(nss);
        }
        return status;
    }

    return Status::OK();
}

void ShardServerCatalogCacheLoader::_runTasks(const NamespaceString& nss) {
    auto context = _contexts.makeOperationContext(*Client::getCurrent());

    bool taskFinished = false;
    try {
        _updatePersistedMetadata(context.opCtx(), nss);
        taskFinished = true;
    } catch (const DBException& ex) {
        Status exceptionStatus = ex.toStatus();

        // This thread must stop if we are shutting down
        if (ErrorCodes::isShutdownError(exceptionStatus.code())) {
            log() << "Failed to persist chunk metadata update for collection '" << nss
                  << "' due to shutdown.";
            return;
        }

        log() << redact(exceptionStatus);
    }

    stdx::lock_guard<stdx::mutex> lock(_mutex);

    // If task completed successfully, remove it from work queue
    if (taskFinished) {
        _taskLists[nss].removeActiveTask();
    }

    // Schedule more work if there is any
    if (!_taskLists[nss].empty()) {
        Status status = _threadPool.schedule([this, nss]() { _runTasks(nss); });
        if (!status.isOK()) {
            log() << "Cache loader failed to schedule a persisted metadata update"
                  << " task for namespace '" << nss << "' due to '" << redact(status)
                  << "'. Clearing task list so that scheduling will be attempted by the next"
                  << " caller to refresh this namespace.";
            _taskLists.erase(nss);
        }
    } else {
        _taskLists.erase(nss);
    }
}

void ShardServerCatalogCacheLoader::_updatePersistedMetadata(OperationContext* opCtx,
                                                             const NamespaceString& nss) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    const Task& task = _taskLists[nss].getActiveTask();
    invariant(task.dropped || !task.collectionAndChangedChunks->changedChunks.empty());

    // If this task is from an old term and no longer valid, do not execute and return true so that
    // the task gets removed from the task list
    if (task.termCreated != _term) {
        return;
    }

    lock.unlock();

    // Check if this is a drop task
    if (task.dropped) {
        // The namespace was dropped. The persisted metadata for the collection must be cleared.
        Status status = dropChunksAndDeleteCollectionsEntry(opCtx, nss);
        uassert(status.code(),
                str::stream() << "Failed to clear persisted chunk metadata for collection '"
                              << nss.ns()
                              << "' due to '"
                              << status.reason()
                              << "'. Will be retried.",
                status.isOK());
        return;
    }

    ChunkVersion persistedMaxVersion = getPersistedMaxVersion(opCtx, nss);

    // If the epoch of the update task does not match the persisted metadata, the persisted metadata
    // -- from an old collection that was recreated -- must be cleared before applying the changes.
    if (persistedMaxVersion.isSet() &&
        persistedMaxVersion.epoch() != task.maxQueryVersion.epoch()) {
        Status status = dropChunksAndDeleteCollectionsEntry(opCtx, nss);
        uassert(status.code(),
                str::stream() << "Failed to clear persisted chunk metadata for collection '"
                              << nss.ns()
                              << "' due to '"
                              << status.reason()
                              << "'. Will be retried.",
                status.isOK());
    }

    Status status =
        persistCollectionAndChangedChunks(opCtx, nss, task.collectionAndChangedChunks.get());
    if (status == ErrorCodes::ConflictingOperationInProgress) {
        // A new epoch was discovered while updating the persisted metadata. The getChunksSince
        // which enqueued this task would have discovered that independently and also returned
        // ConflictingOperationInProgress to the catalog cache, which means that the next enqueued
        // task should have the new epoch, which in turn means that on the next invocation, the
        // old collection entry will be dropped and recreated.
        return;
    }

    uassert(status.code(),
            str::stream() << "Failed to update the persisted chunk metadata for collection '"
                          << nss.ns()
                          << "' from '"
                          << task.minQueryVersion.toString()
                          << "' to '"
                          << task.maxQueryVersion.toString()
                          << "' due to '"
                          << status.reason()
                          << "'. Will be retried.",
            status.isOK());

    LOG(1) << "Successfully updated persisted chunk metadata for collection '" << nss << "' from '"
           << task.minQueryVersion << "' to collection version '" << task.maxQueryVersion << "'.";
}

StatusWith<CollectionAndChangedChunks>
ShardServerCatalogCacheLoader::_getCompletePersistedMetadataForSecondarySinceVersion(
    OperationContext* opCtx, const NamespaceString& nss, const ChunkVersion& version) {
    try {
        // Keep trying to load the metadata until we get a complete view without updates being
        // concurrently applied.
        while (true) {
            const auto beginRefreshState = [&]() {
                while (true) {
                    auto notif = _namespaceNotifications.createNotification(nss);

                    auto refreshState = uassertStatusOK(getPersistedRefreshFlags(opCtx, nss));

                    if (!refreshState.refreshing) {
                        return refreshState;
                    }

                    notif.get(opCtx);
                }
            }();

            // Load the metadata.
            CollectionAndChangedChunks collAndChangedChunks =
                getPersistedMetadataSinceVersion(opCtx, nss, version, true);

            // Check that no updates were concurrently applied while we were loading the metadata:
            // this could cause the loaded metadata to provide an incomplete view of the chunk
            // ranges.
            const auto endRefreshState = uassertStatusOK(getPersistedRefreshFlags(opCtx, nss));

            if (beginRefreshState == endRefreshState) {
                return collAndChangedChunks;
            }

            LOG(1) << "Cache loader read meatadata while updates were being applied: this"
                   << " metadata may be incomplete. Retrying. Refresh state before read: "
                   << beginRefreshState << ". Current refresh state: '" << endRefreshState << "'.";
        }
    } catch (const DBException& ex) {
        Status status = ex.toStatus();

        // NamespaceNotFound errors are expected and must be returned.
        if (status == ErrorCodes::NamespaceNotFound) {
            return status;
        }

        // All other errors are unhandled.
        return Status(ErrorCodes::OperationFailed,
                      str::stream() << "Failed to load local metadata due to '"
                                    << ex.toStatus().toString()
                                    << "'.");
    }
}

ShardServerCatalogCacheLoader::Task::Task(
    StatusWith<CollectionAndChangedChunks> statusWithCollectionAndChangedChunks,
    ChunkVersion minimumQueryVersion,
    long long currentTerm)
    : minQueryVersion(minimumQueryVersion), termCreated(currentTerm) {
    if (statusWithCollectionAndChangedChunks.isOK()) {
        collectionAndChangedChunks = statusWithCollectionAndChangedChunks.getValue();
        invariant(!collectionAndChangedChunks->changedChunks.empty());
        maxQueryVersion = collectionAndChangedChunks->changedChunks.back().getVersion();
    } else {
        invariant(statusWithCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound);
        dropped = true;
        maxQueryVersion = ChunkVersion::UNSHARDED();
    }
}

void ShardServerCatalogCacheLoader::TaskList::addTask(Task task) {
    if (_tasks.empty()) {
        _tasks.emplace_back(std::move(task));
        return;
    }

    if (task.dropped) {
        invariant(_tasks.back().maxQueryVersion.equals(task.minQueryVersion));

        // As an optimization, on collection drop, clear any pending tasks in order to prevent any
        // throw-away work from executing. Because we have no way to differentiate whether the
        // active tasks is currently being operated on by a thread or not, we must leave the front
        // intact.
        _tasks.erase(std::next(_tasks.begin()), _tasks.end());

        // No need to schedule a drop if one is already currently active.
        if (!_tasks.front().dropped) {
            _tasks.emplace_back(std::move(task));
        }
    } else {
        // Tasks must have contiguous versions, unless a complete reload occurs.
        invariant(_tasks.back().maxQueryVersion.equals(task.minQueryVersion) ||
                  !task.minQueryVersion.isSet());

        _tasks.emplace_back(std::move(task));
    }
}

const ShardServerCatalogCacheLoader::Task& ShardServerCatalogCacheLoader::TaskList::getActiveTask()
    const {
    invariant(!_tasks.empty());
    return _tasks.front();
}

void ShardServerCatalogCacheLoader::TaskList::removeActiveTask() {
    invariant(!_tasks.empty());
    _tasks.pop_front();
}

bool ShardServerCatalogCacheLoader::TaskList::hasTasksFromThisTerm(long long term) const {
    invariant(!_tasks.empty());
    return _tasks.back().termCreated == term;
}

ChunkVersion ShardServerCatalogCacheLoader::TaskList::getHighestVersionEnqueued() const {
    invariant(!_tasks.empty());
    return _tasks.back().maxQueryVersion;
}

CollectionAndChangedChunks ShardServerCatalogCacheLoader::TaskList::getEnqueuedMetadataForTerm(
    const long long term) const {
    CollectionAndChangedChunks collAndChunks;
    for (const auto& task : _tasks) {
        if (task.termCreated != term) {
            // Task data is no longer valid. Go on to the next task in the list.
            continue;
        }

        if (task.dropped) {
            // A drop task should reset the metadata.
            collAndChunks = CollectionAndChangedChunks();
        } else {
            if (task.collectionAndChangedChunks->epoch != collAndChunks.epoch) {
                // An epoch change should reset the metadata and start from the new.
                collAndChunks = task.collectionAndChangedChunks.get();
            } else {
                // Epochs match, so the new results should be appended.
                //
                // Note: it's okay if the new chunks change to a new version epoch in the middle of
                // the chunks vector. This will be either reset by the next task with a total reload
                // with a new epoch, or cause the original getChunksSince caller to throw out the
                // results and refresh again.
                collAndChunks.changedChunks.insert(
                    collAndChunks.changedChunks.end(),
                    task.collectionAndChangedChunks->changedChunks.begin(),
                    task.collectionAndChangedChunks->changedChunks.end());
            }
        }
    }
    return collAndChunks;
}

}  // namespace mongo
