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

#include "mongo/db/global_catalog/catalog_cache/shard_server_catalog_cache_loader_impl.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/bson/bson_field.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/database_name.h"
#include "mongo/db/feature_flag.h"
#include "mongo/db/generic_argument_util.h"
#include "mongo/db/global_catalog/ddl/shard_metadata_util.h"
#include "mongo/db/global_catalog/type_chunk.h"
#include "mongo/db/global_catalog/type_collection_common_types_gen.h"
#include "mongo/db/global_catalog/type_shard_collection.h"
#include "mongo/db/global_catalog/type_shard_collection_gen.h"
#include "mongo/db/global_catalog/type_shard_database.h"
#include "mongo/db/global_catalog/type_shard_database_gen.h"
#include "mongo/db/keypattern.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/lock_manager/exception_util.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/op_observer/op_observer.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/operation_context_group.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/client/shard.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/topology/cluster_role.h"
#include "mongo/db/topology/shard_registry.h"
#include "mongo/db/topology/sharding_state.h"
#include "mongo/db/versioning_protocol/database_version.h"
#include "mongo/db/versioning_protocol/shard_version.h"
#include "mongo/db/versioning_protocol/shard_version_factory.h"
#include "mongo/db/versioning_protocol/stale_exception.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/s/resharding/type_collection_fields_gen.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/admission_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/str.h"
#include "mongo/util/timer.h"

#include <iterator>
#include <mutex>
#include <tuple>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

using namespace shardmetadatautil;

using CollectionAndChangedChunks = CatalogCacheLoader::CollectionAndChangedChunks;

namespace {

MONGO_FAIL_POINT_DEFINE(hangCollectionFlush);
MONGO_FAIL_POINT_DEFINE(hangDatabaseFlush);

AtomicWord<unsigned long long> taskIdGenerator{0};

/**
 * Drops all chunks from the persisted metadata whether the collection's epoch has changed.
 */
void dropChunksIfEpochChanged(OperationContext* opCtx,
                              const ChunkVersion& maxLoaderVersion,
                              const OID& currentEpoch,
                              const NamespaceString& nss) {
    if (maxLoaderVersion == ChunkVersion::UNSHARDED() || maxLoaderVersion.epoch() == currentEpoch) {
        return;
    }

    // Drop the 'config.cache.chunks.<ns>' collection
    dropChunks(opCtx, nss);

    LOGV2(5990400,
          "Dropped persisted chunk metadata due to epoch change",
          logAttrs(nss),
          "currentEpoch"_attr = currentEpoch,
          "previousEpoch"_attr = maxLoaderVersion.epoch());
}

/**
 * Takes a CollectionAndChangedChunks object and persists the changes to the shard's metadata
 * collections.
 *
 * Returns ConflictingOperationInProgress if a chunk is found with a new epoch.
 */
Status persistCollectionAndChangedChunks(OperationContext* opCtx,
                                         const NamespaceString& nss,
                                         const CollectionAndChangedChunks& collAndChunks,
                                         const ChunkVersion& maxLoaderVersion) {
    // Update the collection entry in case there are any updates.
    ShardCollectionType update(nss,
                               collAndChunks.epoch,
                               collAndChunks.timestamp,
                               *collAndChunks.uuid,
                               collAndChunks.shardKeyPattern,
                               collAndChunks.shardKeyIsUnique);
    update.setDefaultCollation(collAndChunks.defaultCollation);
    update.setTimeseriesFields(collAndChunks.timeseriesFields);
    update.setReshardingFields(collAndChunks.reshardingFields);
    update.setAllowMigrations(collAndChunks.allowMigrations);
    update.setUnsplittable(collAndChunks.unsplittable);
    update.setRefreshing(true);  // Mark as refreshing so secondaries are aware of it.

    Status status = updateShardCollectionsEntry(
        opCtx,
        BSON(ShardCollectionType::kNssFieldName
             << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
        update.toBSON(),
        true /*upsert*/);
    if (!status.isOK()) {
        return status;
    }

    try {
        dropChunksIfEpochChanged(opCtx, maxLoaderVersion, collAndChunks.epoch, nss);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

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

    LOGV2_DEBUG(3463204, 1, "Persisted collection entry and chunk metadata", logAttrs(nss));

    return Status::OK();
}

/**
 * Takes a DatabaseType object and persists the changes to the shard's metadata
 * collections.
 */
Status persistDbVersion(OperationContext* opCtx, const DatabaseType& dbt) {
    // Update the databases collection entry for 'dbName' in case there are any new updates.
    Status status = updateShardDatabasesEntry(
        opCtx,
        BSON(ShardDatabaseType::kDbNameFieldName
             << DatabaseNameUtil::serialize(dbt.getDbName(), SerializationContext::stateDefault())),
        dbt.toBSON(),
        BSONObj(),
        true /*upsert*/);
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
 * could be dropped and recreated or have its shard key refined between reading the collection epoch
 * and retrieving the chunk, which would make the returned ChunkVersion corrupt.
 */
ChunkVersion getPersistedMaxChunkVersion(OperationContext* opCtx, const NamespaceString& nss) {
    // Must read the collections entry to get the epoch to pass into ChunkType for shard's chunk
    // collection.
    auto statusWithCollection = readShardCollectionsEntry(opCtx, nss);
    if (statusWithCollection == ErrorCodes::NamespaceNotFound) {
        // There is no persisted metadata.
        return ChunkVersion::UNSHARDED();
    }

    uassertStatusOKWithContext(statusWithCollection,
                               str::stream()
                                   << "Failed to read persisted collections entry for collection '"
                                   << nss.toStringForErrorMsg() << "'.");

    auto cachedCollection = statusWithCollection.getValue();
    if (cachedCollection.getRefreshing() && *cachedCollection.getRefreshing()) {
        // Chunks was in the middle of refresh last time and we didn't finish cleanly. The version
        // on the cached collection does not represent the maximum version in the cached chunks.
        // Furthermore, since we don't bump the versions during refineShardKey and we don't store
        // the epoch in the cache chunks, we can't tell whether the chunks are pre or post refined.
        // Therefore, we have no choice but to just throw away the cache and start from scratch.
        uassertStatusOK(dropChunksAndDeleteCollectionsEntry(opCtx, nss));

        return ChunkVersion::UNSHARDED();
    }

    auto statusWithChunk = readShardChunks(opCtx,
                                           nss,
                                           BSONObj(),
                                           BSON(ChunkType::lastmod() << -1),
                                           1LL,
                                           cachedCollection.getEpoch(),
                                           cachedCollection.getTimestamp());
    uassertStatusOKWithContext(
        statusWithChunk,
        str::stream() << "Failed to read highest version persisted chunk for collection '"
                      << nss.toStringForErrorMsg() << "'.");

    return statusWithChunk.getValue().empty() ? ChunkVersion::UNSHARDED()
                                              : statusWithChunk.getValue().front().getVersion();
}

/**
 * This function will throw on error!
 *
 * Tries to find persisted chunk metadata with chunk versions GTE to 'version'.
 *
 * If 'version's epoch matches persisted metadata, returns persisted metadata GTE 'version'.
 * If 'version's epoch doesn't match persisted metadata, returns all persisted metadata.
 * If collections entry does not exist, throws NamespaceNotFound error. Can return an empty
 * chunks vector in CollectionAndChangedChunks without erroring, if collections entry IS found.
 */
CollectionAndChangedChunks getPersistedMetadataSinceVersion(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            ChunkVersion version) {
    ShardCollectionType shardCollectionEntry =
        uassertStatusOK(readShardCollectionsEntry(opCtx, nss));

    // If the persisted epoch doesn't match what the CatalogCache requested, read everything.
    // If the epochs are the same we can safely take the timestamp from the shard coll entry.
    ChunkVersion startingVersion = version.isSameCollection({shardCollectionEntry.getEpoch(),
                                                             shardCollectionEntry.getTimestamp()})
        ? version
        : ChunkVersion({shardCollectionEntry.getEpoch(), shardCollectionEntry.getTimestamp()},
                       {0, 0});

    QueryAndSort diff = createShardChunkDiffQuery(startingVersion);
    auto changedChunks = uassertStatusOK(readShardChunks(opCtx,
                                                         nss,
                                                         diff.query,
                                                         diff.sort,
                                                         boost::none,
                                                         startingVersion.epoch(),
                                                         startingVersion.getTimestamp()));

    return CollectionAndChangedChunks{shardCollectionEntry.getEpoch(),
                                      shardCollectionEntry.getTimestamp(),
                                      shardCollectionEntry.getUuid(),
                                      shardCollectionEntry.getUnsplittable(),
                                      shardCollectionEntry.getKeyPattern().toBSON(),
                                      shardCollectionEntry.getDefaultCollation(),
                                      shardCollectionEntry.getUnique(),
                                      shardCollectionEntry.getTimeseriesFields(),
                                      shardCollectionEntry.getReshardingFields(),
                                      shardCollectionEntry.getAllowMigrations(),
                                      std::move(changedChunks)};
}

/**
 * Attempts to read the collection and chunk metadata. May not read a complete diff if the metadata
 * for the collection is being updated concurrently. This is safe if those updates are appended.
 *
 * If the epoch changes while reading the chunks, returns an empty object.
 */
StatusWith<CollectionAndChangedChunks> getIncompletePersistedMetadataSinceVersion(
    OperationContext* opCtx, const NamespaceString& nss, ChunkVersion version) {

    try {
        CollectionAndChangedChunks collAndChunks =
            getPersistedMetadataSinceVersion(opCtx, nss, version);
        if (collAndChunks.changedChunks.empty()) {
            // Found a collections entry, but the chunks are being updated.
            return CollectionAndChangedChunks();
        }

        // Make sure the collections entry epoch has not changed since we began reading chunks --
        // an epoch change between reading the collections entry and reading the chunk metadata
        // would invalidate the chunks.

        auto afterShardCollectionsEntry = uassertStatusOK(readShardCollectionsEntry(opCtx, nss));
        if (collAndChunks.epoch != afterShardCollectionsEntry.getEpoch()) {
            // The collection was dropped and recreated or had its shard key refined since we began.
            // Return empty results.
            return CollectionAndChangedChunks();
        }

        return collAndChunks;
    } catch (const DBException& ex) {
        Status status = ex.toStatus();
        if (status == ErrorCodes::NamespaceNotFound) {
            return CollectionAndChangedChunks();
        }
        return status.withContext(str::stream() << "Failed to read local metadata.");
    }
}

ShardId getSelfShardId(OperationContext* opCtx) {
    if (serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer)) {
        return ShardId::kConfigServerId;
    }

    auto const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->enabled());
    return shardingState->shardId();
}

/**
 * Runs the given function and throws an exception if the topology term has changed between the
 * start and end of it. If it changes this will uassert with an InterruptedDueToReplStateChange
 * error.
 */
template <typename F>
auto runAndThrowIfTermChanged(OperationContext* opCtx, F&& fn) {
    auto termBeforeOperation = repl::ReplicationCoordinator::get(opCtx)->getTerm();

    if constexpr (!std::is_same_v<void, std::invoke_result_t<F, decltype(termBeforeOperation)>>) {
        auto result = fn(termBeforeOperation);
        auto termAtEndOfOperation = repl::ReplicationCoordinator::get(opCtx)->getTerm();
        uassert(
            ErrorCodes::InterruptedDueToReplStateChange,
            fmt::format("Change of ReplicaSet term detected between start and end of operation. "
                        "Term before operation is {}. Term at end of operation is {}",
                        termBeforeOperation,
                        termAtEndOfOperation),
            termBeforeOperation == termAtEndOfOperation);
        return result;
    } else {
        fn(termBeforeOperation);
        auto termAtEndOfOperation = repl::ReplicationCoordinator::get(opCtx)->getTerm();
        uassert(
            ErrorCodes::InterruptedDueToReplStateChange,
            fmt::format("Change of ReplicaSet term detected between start and end of operation. "
                        "Term before operation is {}. Term at end of operation is {}",
                        termBeforeOperation,
                        termAtEndOfOperation),
            termBeforeOperation == termAtEndOfOperation);
    }
}

/**
 * Sends _flushDatabaseCacheUpdates to the primary to force it to refresh its routing table for
 * database 'dbName' and then waits for the refresh to replicate to this node.
 */
void forcePrimaryDatabaseRefreshAndWaitForReplication(OperationContext* opCtx,
                                                      const DatabaseName& dbName) {
    auto selfShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, getSelfShardId(opCtx)));

    // Run the operation on the primary and await the result to be replicated to this node. To avoid
    // issues with rollback/term changes this is wrapped in a runAndThrowIfTermChanged since we
    // currently have no way to detect a topology change/rollback after the return from primary is
    // received.
    runAndThrowIfTermChanged(opCtx, [&](auto term) {
        auto cmdResponse = uassertStatusOK(selfShard->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            BSON("_flushDatabaseCacheUpdates"
                 << DatabaseNameUtil::serialize(dbName, SerializationContext::stateDefault())),
            Seconds{30},
            Shard::RetryPolicy::kIdempotent));

        // If the error is `DatabaseMetadataRefreshCanceledDueToFCVTransition` it means that the
        // primary is already relying on the authoritative model to acknowledge filtering metadata
        // and will not serve more refreshes. In order to follow the same protocol, this node has to
        // wait for the seen opTime from the last call (same behaviour as today), and then fail, so
        // an upper layer will retry this refresh using the authoritative protocol.

        auto status = cmdResponse.commandStatus;
        auto waitForOptime = [&]() {
            uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->waitUntilOpTimeForRead(
                opCtx,
                {repl::OpTime{cmdResponse.response.getField(LogicalTime::kOperationTimeFieldName)
                                  .timestamp(),
                              term},
                 boost::none}));
        };

        if (status == ErrorCodes::DatabaseMetadataRefreshCanceledDueToFCVTransition) {
            waitForOptime();
        }

        uassertStatusOK(status);

        waitForOptime();
    });
}

void performNoopMajorityWriteLocally(OperationContext* opCtx, StringData msg) {
    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);

    {
        AutoGetOplogFastPath oplogWrite(opCtx, OplogAccessMode::kWrite);
        uassert(ErrorCodes::NotWritablePrimary,
                fmt::format("Not primary when performing noop write for {}", msg),
                replCoord->canAcceptWritesForDatabase(opCtx, DatabaseName::kAdmin));

        writeConflictRetry(
            opCtx, "performNoopWrite", NamespaceString::kRsOplogNamespace, [&opCtx, &msg] {
                WriteUnitOfWork wuow(opCtx);
                opCtx->getClient()->getServiceContext()->getOpObserver()->onOpMessage(
                    opCtx, BSON("msg" << msg));
                wuow.commit();
            });
    }
    auto& replClient = repl::ReplClientInfo::forClient(opCtx->getClient());
    WriteConcernResult writeConcernResult;
    uassertStatusOK(waitForWriteConcern(
        opCtx, replClient.getLastOp(), defaultMajorityWriteConcernDoNotUse(), &writeConcernResult));
}

}  // namespace

ShardServerCatalogCacheLoaderImpl::ShardServerCatalogCacheLoaderImpl(
    std::unique_ptr<ConfigServerCatalogCacheLoader> configServerLoader)
    : _configServerLoader(std::move(configServerLoader)),
      _executor(std::make_shared<ThreadPool>([] {
          ThreadPool::Options options;
          options.poolName = "ShardServerCatalogCacheLoaderImpl";
          options.minThreads = 0;
          options.maxThreads = 6;
          return options;
      }())) {
    _executor->startup();
}

ShardServerCatalogCacheLoaderImpl::~ShardServerCatalogCacheLoaderImpl() {
    shutDown();
}

void ShardServerCatalogCacheLoaderImpl::notifyOfCollectionRefreshEndMarkerSeen(
    const NamespaceString& nss, const Timestamp& commitTime) {
    _namespaceNotifications.notifyChange(nss, commitTime);
}

void ShardServerCatalogCacheLoaderImpl::initializeReplicaSetRole(bool isPrimary) {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    invariant(_role == ReplicaSetRole::None);

    if (isPrimary) {
        _role = ReplicaSetRole::Primary;
    } else {
        _role = ReplicaSetRole::Secondary;
    }
}

void ShardServerCatalogCacheLoaderImpl::onStepDown() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    invariant(_role != ReplicaSetRole::None);
    _contexts.interrupt(ErrorCodes::PrimarySteppedDown);
    ++_term;
    _role = ReplicaSetRole::Secondary;
}

void ShardServerCatalogCacheLoaderImpl::onStepUp() {
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    invariant(_role != ReplicaSetRole::None);
    _contexts.interrupt(ErrorCodes::InterruptedDueToReplStateChange);
    ++_term;
    _role = ReplicaSetRole::Primary;
}

void ShardServerCatalogCacheLoaderImpl::onReplicationRollback() {
    // No need to increment the term since this interruption is only to prevent the secondary
    // refresh thread from getting stuck or waiting on an incorrect opTime.
    stdx::lock_guard<stdx::mutex> lg(_mutex);
    _contexts.interrupt(ErrorCodes::Interrupted);
}

void ShardServerCatalogCacheLoaderImpl::shutDown() {
    {
        stdx::lock_guard<stdx::mutex> lg(_mutex);
        if (_inShutdown) {
            return;
        }

        _inShutdown = true;
    }

    // Prevent further scheduling, then interrupt ongoing tasks.
    _executor->shutdown();
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        _contexts.interrupt(ErrorCodes::InterruptedAtShutdown);
        ++_term;
    }

    _executor->join();
    invariant(_contexts.isEmpty());

    _configServerLoader->shutDown();
}

SemiFuture<CollectionAndChangedChunks> ShardServerCatalogCacheLoaderImpl::getChunksSince(
    const NamespaceString& nss, ChunkVersion version) {
    // If the collecction is never registered on the sharding catalog there is no need to refresh.
    // Further, attempting to refesh config.collections or config.chunks would trigger recursive
    // refreshes, and, if this is running on a config server secondary, the refresh would not
    // succeed if the primary is unavailable, unnecessarily reducing availability.
    if (nss.isNamespaceAlwaysUntracked()) {
        return Status(ErrorCodes::NamespaceNotFound,
                      str::stream() << "Collection " << nss.toStringForErrorMsg() << " not found");
    }

    bool isPrimary;
    long long term;
    std::tie(isPrimary, term) = [&] {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return std::make_tuple(_role == ReplicaSetRole::Primary, _term);
    }();

    return ExecutorFuture<void>(_executor)
        .then([=, this]() {
            // TODO(SERVER-111753): Please revisit if this thread could be made killable.
            ThreadClient tc("ShardServerCatalogCacheLoader::getChunksSince",
                            getGlobalServiceContext()->getService(ClusterRole::ShardServer),
                            ClientOperationKillableByStepdown{false});
            auto context = _contexts.makeOperationContext(*tc);

            {
                // We may have missed an OperationContextGroup interrupt since this operation
                // began but before the OperationContext was added to the group. So we'll check
                // that we're still in the same _term.
                stdx::lock_guard<stdx::mutex> lock(_mutex);
                uassert(ErrorCodes::InterruptedDueToReplStateChange,
                        "Unable to refresh routing table because replica set state changed or "
                        "the node is shutting down.",
                        _term == term);
            }

            if (isPrimary) {
                return _schedulePrimaryGetChunksSince(context.opCtx(), nss, version, term);
            } else {
                return _runSecondaryGetChunksSince(context.opCtx(), nss, version);
            }
        })
        .semi();
}

SemiFuture<DatabaseType> ShardServerCatalogCacheLoaderImpl::getDatabase(
    const DatabaseName& dbName) {
    tassert(9131801,
            "Unexpected request for 'admin' or 'config' database, which have fixed metadata",
            !dbName.isAdminDB() && !dbName.isConfigDB());

    const auto [isPrimary, term] = [&] {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return std::make_tuple(_role == ReplicaSetRole::Primary, _term);
    }();

    return ExecutorFuture<void>(_executor)
        .then([this, dbName, isPrimary = isPrimary, term = term]() {
            // TODO(SERVER-111753): Please revisit if this thread could be made killable.
            ThreadClient tc("ShardServerCatalogCacheLoader::getDatabase",
                            getGlobalServiceContext()->getService(ClusterRole::ShardServer),
                            ClientOperationKillableByStepdown{false});
            auto context = _contexts.makeOperationContext(*tc);

            {
                // We may have missed an OperationContextGroup interrupt since this operation began
                // but before the OperationContext was added to the group. So we'll check that we're
                // still in the same _term.
                stdx::lock_guard<stdx::mutex> lock(_mutex);
                uassert(ErrorCodes::InterruptedDueToReplStateChange,
                        "Unable to refresh database because replica set state changed or the node "
                        "is shutting down.",
                        _term == term);
            }

            if (isPrimary) {
                return _schedulePrimaryGetDatabase(context.opCtx(), dbName, term);
            } else {
                return _runSecondaryGetDatabase(context.opCtx(), dbName);
            }
        })
        .semi();
}

void ShardServerCatalogCacheLoaderImpl::waitForCollectionFlush(OperationContext* opCtx,
                                                               const NamespaceString& nss) {
    stdx::unique_lock<stdx::mutex> lg(_mutex);
    const auto initialTerm = _term;

    boost::optional<uint64_t> taskNumToWait;

    while (true) {
        uassert(ErrorCodes::NotWritablePrimary,
                str::stream() << "Unable to wait for collection metadata flush for "
                              << nss.toStringForErrorMsg()
                              << " because the node's replication role changed.",
                _role == ReplicaSetRole::Primary && _term == initialTerm);

        auto it = _collAndChunkTaskLists.find(nss);

        // If there are no tasks for the specified namespace, everything must have been completed
        if (it == _collAndChunkTaskLists.end())
            return;

        auto& taskList = it->second;

        if (!taskNumToWait) {
            const auto& lastTask = taskList.back();
            taskNumToWait = lastTask.taskNum;
        } else {
            const auto& activeTask = taskList.front();

            if (activeTask.taskNum > *taskNumToWait) {
                auto secondTaskIt = std::next(taskList.begin());

                // Because of an optimization where a namespace drop clears all tasks except the
                // active it is possible that the task number we are waiting on will never actually
                // be written. Because of this we move the task number to the drop which can only be
                // in the active task or in the one after the active.
                if (activeTask.dropped) {
                    taskNumToWait = activeTask.taskNum;
                } else if (secondTaskIt != taskList.end() && secondTaskIt->dropped) {
                    taskNumToWait = secondTaskIt->taskNum;
                } else {
                    return;
                }
            }
        }

        // Wait for the active task to complete
        {
            const auto activeTaskNum = taskList.front().taskNum;

            // Increase the use_count of the condition variable shared pointer, because the entire
            // task list might get deleted during the unlocked interval
            auto condVar = taskList._activeTaskCompletedCondVar;

            // It is not safe to use taskList after this call, because it will unlock and lock the
            // tasks mutex, so we just loop around.
            // It is only correct to wait again on condVar if the taskNum has not changed, meaning
            // that it must still be the same task list.
            opCtx->waitForConditionOrInterrupt(*condVar, lg, [&]() {
                const auto it = _collAndChunkTaskLists.find(nss);
                return it == _collAndChunkTaskLists.end() || it->second.empty() ||
                    it->second.front().taskNum != activeTaskNum;
            });
        }
    }
}

void ShardServerCatalogCacheLoaderImpl::waitForDatabaseFlush(OperationContext* opCtx,
                                                             const DatabaseName& dbName) {

    stdx::unique_lock<stdx::mutex> lg(_mutex);
    const auto initialTerm = _term;

    boost::optional<uint64_t> taskNumToWait;

    while (true) {
        uassert(ErrorCodes::NotWritablePrimary,
                str::stream() << "Unable to wait for database metadata flush for "
                              << dbName.toStringForErrorMsg()
                              << " because the node's replication role changed.",
                _role == ReplicaSetRole::Primary && _term == initialTerm);

        auto it = _dbTaskLists.find(dbName);

        // If there are no tasks for the specified namespace, everything must have been completed
        if (it == _dbTaskLists.end())
            return;

        auto& taskList = it->second;

        if (!taskNumToWait) {
            const auto& lastTask = taskList.back();
            taskNumToWait = lastTask.taskNum;
        } else {
            const auto& activeTask = taskList.front();

            if (activeTask.taskNum > *taskNumToWait) {
                auto secondTaskIt = std::next(taskList.begin());

                // Because of an optimization where a namespace drop clears all tasks except the
                // active it is possible that the task number we are waiting on will never actually
                // be written. Because of this we move the task number to the drop which can only be
                // in the active task or in the one after the active.
                if (!activeTask.dbType) {
                    // The task is for a drop.
                    taskNumToWait = activeTask.taskNum;
                } else if (secondTaskIt != taskList.end() && !secondTaskIt->dbType) {
                    taskNumToWait = secondTaskIt->taskNum;
                } else {
                    return;
                }
            }
        }

        // Wait for the active task to complete
        {
            const auto activeTaskNum = taskList.front().taskNum;

            // Increase the use_count of the condition variable shared pointer, because the entire
            // task list might get deleted during the unlocked interval
            auto condVar = taskList._activeTaskCompletedCondVar;

            // It is not safe to use taskList after this call, because it will unlock and lock the
            // tasks mutex, so we just loop around.
            // It is only correct to wait again on condVar if the taskNum has not changed, meaning
            // that it must still be the same task list.
            opCtx->waitForConditionOrInterrupt(*condVar, lg, [&]() {
                const auto it = _dbTaskLists.find(dbName);
                return it == _dbTaskLists.end() || it->second.empty() ||
                    it->second.front().taskNum != activeTaskNum;
            });
        }
    }
}

StatusWith<CollectionAndChangedChunks>
ShardServerCatalogCacheLoaderImpl::_runSecondaryGetChunksSince(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion) {
    Timer t;
    auto nssNotif = _forcePrimaryCollectionRefreshAndWaitForReplication(opCtx, nss);
    LOGV2_FOR_CATALOG_REFRESH(5965800,
                              2,
                              "Cache loader on secondary successfully waited for primary refresh "
                              "and replication of collection",
                              logAttrs(nss),
                              "duration"_attr = Milliseconds(t.millis()));

    // Read the local metadata.
    return _getCompletePersistedMetadataForSecondarySinceVersion(
        opCtx, std::move(nssNotif), nss, catalogCacheSinceVersion);
}

StatusWith<CollectionAndChangedChunks>
ShardServerCatalogCacheLoaderImpl::_schedulePrimaryGetChunksSince(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    long long termScheduled) {
    // Get the max version the loader has.
    const auto maxLoaderVersion = [&] {
        {
            stdx::lock_guard<stdx::mutex> lock(_mutex);
            auto taskListIt = _collAndChunkTaskLists.find(nss);

            if (taskListIt != _collAndChunkTaskLists.end() &&
                taskListIt->second.hasTasksFromThisTerm(termScheduled)) {
                // Enqueued tasks have the latest metadata
                return taskListIt->second.getHighestVersionEnqueued();
            }
        }

        try {
            // If there are no enqueued tasks, get the max persisted
            return getPersistedMaxChunkVersion(opCtx, nss);
        } catch (const ExceptionFor<ErrorCategory::IDLParseError>& parseError) {
            LOGV2_WARNING(9580700,
                          "Clearing up corrupted cached collection metadata. "
                          "The cache will be eventually repopulated by a full refresh.",
                          "error"_attr = redact(parseError.toStatus()),
                          logAttrs(nss));
            uassertStatusOK(dropChunksAndDeleteCollectionsEntry(opCtx, nss));
            return ChunkVersion::UNSHARDED();
        }
    }();

    // Refresh the loader's metadata from the config server. The caller's request will
    // then be serviced from the loader's up-to-date metadata.
    auto swCollectionAndChangedChunks =
        _configServerLoader->getChunksSince(nss, maxLoaderVersion).getNoThrow();

    if (swCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound) {
        _ensureMajorityPrimaryAndScheduleCollAndChunksTask(
            opCtx,
            nss,
            CollAndChunkTask{swCollectionAndChangedChunks, maxLoaderVersion, termScheduled});

        LOGV2_FOR_CATALOG_REFRESH(
            24107,
            1,
            "Cache loader remotely refreshed for collection and no metadata was found",
            logAttrs(nss),
            "oldCollectionPlacementVersion"_attr = maxLoaderVersion);
        return swCollectionAndChangedChunks;
    }

    if (!swCollectionAndChangedChunks.isOK()) {
        return swCollectionAndChangedChunks;
    }

    auto& collAndChunks = swCollectionAndChangedChunks.getValue();

    if (!collAndChunks.changedChunks.back().getVersion().isSameCollection(
            {collAndChunks.epoch, collAndChunks.timestamp})) {
        return Status{ErrorCodes::ConflictingOperationInProgress,
                      str::stream()
                          << "Invalid chunks found when reloading '" << nss.toStringForErrorMsg()
                          << "' Previous collection timestamp was '" << collAndChunks.timestamp
                          << "', but found a new timestamp '"
                          << collAndChunks.changedChunks.back().getVersion().getTimestamp()
                          << "'."};
    }

    auto compareResult = maxLoaderVersion <=> collAndChunks.changedChunks.back().getVersion();
    if (compareResult == std::partial_ordering::unordered ||
        compareResult == std::partial_ordering::less) {
        _ensureMajorityPrimaryAndScheduleCollAndChunksTask(
            opCtx,
            nss,
            CollAndChunkTask{swCollectionAndChangedChunks, maxLoaderVersion, termScheduled});
    }

    LOGV2_FOR_CATALOG_REFRESH(24108,
                              1,
                              "Cache loader remotely refreshed for collection",
                              logAttrs(nss),
                              "oldCollectionPlacementVersion"_attr = maxLoaderVersion,
                              "refreshedCollectionPlacementVersion"_attr =
                                  collAndChunks.changedChunks.back().getVersion());

    // Metadata was found remotely
    // -- otherwise we would have received NamespaceNotFound rather than Status::OK().
    // Return metadata for CatalogCache that's GTE catalogCacheSinceVersion,
    // from the loader's persisted and enqueued metadata.

    swCollectionAndChangedChunks =
        _getLoaderMetadata(opCtx, nss, catalogCacheSinceVersion, termScheduled);
    if (!swCollectionAndChangedChunks.isOK()) {
        return swCollectionAndChangedChunks;
    }

    const auto termAfterRefresh = [&] {
        stdx::lock_guard<stdx::mutex> lock(_mutex);
        return _term;
    }();

    if (termAfterRefresh != termScheduled) {
        // Raising a ConflictingOperationInProgress error here will cause the CatalogCache
        // to attempt the refresh as secondary instead of failing the operation
        return Status(ErrorCodes::ConflictingOperationInProgress,
                      str::stream() << "Replication stepdown occurred during refresh for  '"
                                    << nss.toStringForErrorMsg());
    }

    // After finding metadata remotely, we must have found metadata locally.
    tassert(7032350,
            str::stream() << "No chunks metadata found for collection '"
                          << nss.toStringForErrorMsg()
                          << "' despite the config server returned actual information",
            !collAndChunks.changedChunks.empty());

    return swCollectionAndChangedChunks;
};


StatusWith<DatabaseType> ShardServerCatalogCacheLoaderImpl::_runSecondaryGetDatabase(
    OperationContext* opCtx, const DatabaseName& dbName) {
    Timer t;
    forcePrimaryDatabaseRefreshAndWaitForReplication(opCtx, dbName);
    LOGV2_FOR_CATALOG_REFRESH(5965801,
                              2,
                              "Cache loader on secondary successfully waited for primary refresh "
                              "and replication of database",
                              "db"_attr = dbName,
                              "duration"_attr = Milliseconds(t.millis()));
    auto swShardDatabase = readShardDatabasesEntry(opCtx, dbName);
    if (!swShardDatabase.isOK())
        return swShardDatabase.getStatus();

    const auto& shardDatabase = swShardDatabase.getValue();
    DatabaseType dbt;
    dbt.setDbName(shardDatabase.getDbName());
    dbt.setPrimary(shardDatabase.getPrimary());
    dbt.setVersion(shardDatabase.getVersion());

    return dbt;
}

StatusWith<DatabaseType> ShardServerCatalogCacheLoaderImpl::_schedulePrimaryGetDatabase(
    OperationContext* opCtx, const DatabaseName& dbName, long long termScheduled) {
    auto swDatabaseType = _configServerLoader->getDatabase(dbName).getNoThrow();
    if (swDatabaseType == ErrorCodes::NamespaceNotFound) {
        _ensureMajorityPrimaryAndScheduleDbTask(
            opCtx, dbName, DBTask{swDatabaseType, termScheduled});

        LOGV2_FOR_CATALOG_REFRESH(
            24109,
            1,
            "Cache loader remotely refreshed for database and found the database has been dropped",
            "db"_attr = dbName);
        return swDatabaseType;
    }

    if (!swDatabaseType.isOK()) {
        return swDatabaseType;
    }

    _ensureMajorityPrimaryAndScheduleDbTask(opCtx, dbName, DBTask{swDatabaseType, termScheduled});

    LOGV2_FOR_CATALOG_REFRESH(24110,
                              1,
                              "Cache loader remotely refreshed for database",
                              "db"_attr = dbName,
                              "refreshedDatabaseType"_attr = swDatabaseType.getValue().toBSON());

    return swDatabaseType;
}

StatusWith<CollectionAndChangedChunks> ShardServerCatalogCacheLoaderImpl::_getLoaderMetadata(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    long long expectedTerm) {

    // Get the enqueued metadata first. Otherwise we could miss data between reading persisted and
    // enqueued, if an enqueued task finished after the persisted read but before the enqueued read.
    auto [tasksAreEnqueued, enqueued] =
        _getEnqueuedMetadata(nss, catalogCacheSinceVersion, expectedTerm);

    auto swPersisted =
        getIncompletePersistedMetadataSinceVersion(opCtx, nss, catalogCacheSinceVersion);
    CollectionAndChangedChunks persisted;
    if (swPersisted == ErrorCodes::NamespaceNotFound) {
        // No persisted metadata found
    } else if (!swPersisted.isOK()) {
        return swPersisted;
    } else {
        persisted = std::move(swPersisted.getValue());
    }

    bool lastTaskIsADrop = tasksAreEnqueued && enqueued.changedChunks.empty();

    LOGV2_FOR_CATALOG_REFRESH(
        24111,
        1,
        "Cache loader state since the latest cached version",
        "enqueuedTasksDesc"_attr =
            (enqueued.changedChunks.empty()
                 ? (tasksAreEnqueued
                        ? (lastTaskIsADrop ? "a drop is enqueued"
                                           : "an update of the metadata format is enqueued")
                        : "no enqueued metadata")
                 : ("enqueued metadata from " +
                    enqueued.changedChunks.front().getVersion().toString() + " to " +
                    enqueued.changedChunks.back().getVersion().toString())),
        "persistedMetadataDesc"_attr =
            (persisted.changedChunks.empty()
                 ? "no persisted metadata"
                 : ("persisted metadata from " +
                    persisted.changedChunks.front().getVersion().toString() + " to " +
                    persisted.changedChunks.back().getVersion().toString())),
        "latestCachedVersion"_attr = catalogCacheSinceVersion);

    if (!tasksAreEnqueued) {
        // There are no tasks in the queue. Return the persisted metadata.
        return persisted;
    } else if (persisted.changedChunks.empty() || lastTaskIsADrop ||
               enqueued.epoch != persisted.epoch) {
        // There is a task in the queue and:
        // - nothing is persisted, OR
        // - the last task in the queue was a drop, OR
        // - the epoch changed in the enqueued metadata.
        // Whichever the cause, the persisted metadata is out-dated/non-existent. Return enqueued
        // results.
        return enqueued;
    } else {

        // There can be overlap between persisted and enqueued metadata because enqueued work can
        // be applied while persisted was read. We must remove this overlap.
        if (!enqueued.changedChunks.empty()) {
            const ChunkVersion minEnqueuedVersion = enqueued.changedChunks.front().getVersion();

            // Remove chunks from 'persisted' that are GTE the minimum in 'enqueued' -- this is
            // the overlap.
            auto persistedChangedChunksIt = persisted.changedChunks.begin();
            while (persistedChangedChunksIt != persisted.changedChunks.end() &&
                   (persistedChangedChunksIt->getVersion() <=> minEnqueuedVersion) ==
                       std::partial_ordering::less) {
                ++persistedChangedChunksIt;
            }
            persisted.changedChunks.erase(persistedChangedChunksIt, persisted.changedChunks.end());

            // Append 'enqueued's chunks to 'persisted', which no longer overlaps
            persisted.changedChunks.insert(persisted.changedChunks.end(),
                                           enqueued.changedChunks.begin(),
                                           enqueued.changedChunks.end());
        }

        // The collection info in enqueued metadata may be more recent than the persisted metadata
        persisted.timestamp = enqueued.timestamp;
        persisted.timeseriesFields = std::move(enqueued.timeseriesFields);
        persisted.reshardingFields = std::move(enqueued.reshardingFields);
        persisted.allowMigrations = enqueued.allowMigrations;
        persisted.unsplittable = enqueued.unsplittable;

        return persisted;
    }
}

std::pair<bool, CollectionAndChangedChunks> ShardServerCatalogCacheLoaderImpl::_getEnqueuedMetadata(
    const NamespaceString& nss,
    const ChunkVersion& catalogCacheSinceVersion,
    const long long term) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);
    auto taskListIt = _collAndChunkTaskLists.find(nss);

    if (taskListIt == _collAndChunkTaskLists.end()) {
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
        return std::make_pair(true, std::move(collAndChunks));
    }

    auto changedChunksIt = collAndChunks.changedChunks.begin();
    while (changedChunksIt != collAndChunks.changedChunks.end() &&
           (changedChunksIt->getVersion() <=> catalogCacheSinceVersion) ==
               std::partial_ordering::less) {
        ++changedChunksIt;
    }
    collAndChunks.changedChunks.erase(collAndChunks.changedChunks.begin(), changedChunksIt);

    return std::make_pair(true, std::move(collAndChunks));
}

void ShardServerCatalogCacheLoaderImpl::_ensureMajorityPrimaryAndScheduleCollAndChunksTask(
    OperationContext* opCtx, const NamespaceString& nss, CollAndChunkTask task) {

    // Ensure that this node is primary before using or persisting the information fetched from the
    // config server. This prevents using incorrect filtering information in split brain scenarios.
    performNoopMajorityWriteLocally(opCtx, "ensureMajorityPrimaryAndScheduleCollAndChunksTask");

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        auto& list = _collAndChunkTaskLists[nss];
        auto wasEmpty = list.empty();
        list.addTask(std::move(task));

        if (!wasEmpty)
            return;
    }

    _executor->schedule([this, nss](auto status) {
        if (!status.isOK()) {
            if (ErrorCodes::isCancellationError(status)) {
                return;
            }
            fassertFailedWithStatus(4826400, status);
        }

        _runCollAndChunksTasks(nss);
    });
}

void ShardServerCatalogCacheLoaderImpl::_ensureMajorityPrimaryAndScheduleDbTask(
    OperationContext* opCtx, const DatabaseName& dbName, DBTask task) {

    // Ensure that this node is primary before using or persisting the information fetched from the
    // config server. This prevents using incorrect filtering information in split brain scenarios.
    performNoopMajorityWriteLocally(opCtx, "ensureMajorityPrimaryAndScheduleDbTask");
    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        auto& list = _dbTaskLists[dbName];
        auto wasEmpty = list.empty();
        list.addTask(std::move(task));

        if (!wasEmpty)
            return;
    }

    _executor->schedule([this, dbName](auto status) {
        if (!status.isOK()) {
            if (ErrorCodes::isCancellationError(status)) {
                return;
            }

            fassertFailedWithStatus(4826401, status);
        }

        _runDbTasks(dbName);
    });
}

void ShardServerCatalogCacheLoaderImpl::_runCollAndChunksTasks(const NamespaceString& nss) {
    // TODO(SERVER-111753): Please revisit if this thread could be made killable.
    ThreadClient tc("ShardServerCatalogCacheLoader::runCollAndChunksTasks",
                    getGlobalServiceContext()->getService(ClusterRole::ShardServer),
                    ClientOperationKillableByStepdown{false});

    auto context = _contexts.makeOperationContext(*tc);
    bool taskFinished = false;
    bool inShutdown = false;
    try {
        if (MONGO_unlikely(hangCollectionFlush.shouldFail())) {
            LOGV2(5710200, "Hit hangCollectionFlush failpoint");
            hangCollectionFlush.pauseWhileSet(context.opCtx());
        }

        _updatePersistedCollAndChunksMetadata(context.opCtx(), nss);
        taskFinished = true;
    } catch (const ExceptionFor<ErrorCategory::ShutdownError>&) {
        LOGV2(22094,
              "Failed to persist chunk metadata update for collection due to shutdown",
              logAttrs(nss));
        inShutdown = true;
    } catch (const DBException& ex) {
        LOGV2(22095,
              "Failed to persist chunk metadata update for collection",
              logAttrs(nss),
              "error"_attr = redact(ex));
    }

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        // If task completed successfully, remove it from work queue.
        if (taskFinished) {
            _collAndChunkTaskLists[nss].pop_front();
        }

        // Return if have no more work
        if (_collAndChunkTaskLists[nss].empty()) {
            _collAndChunkTaskLists.erase(nss);
            return;
        }

        // If shutting down need to remove tasks to end waiting on its completion.
        if (inShutdown) {
            while (!_collAndChunkTaskLists[nss].empty()) {
                _collAndChunkTaskLists[nss].pop_front();
            }
            _collAndChunkTaskLists.erase(nss);
            return;
        }
    }

    _executor->schedule([this, nss](Status status) {
        if (status.isOK()) {
            _runCollAndChunksTasks(nss);
            return;
        }

        if (ErrorCodes::isCancellationError(status.code())) {
            LOGV2(22096,
                  "Cache loader failed to schedule a persisted metadata update task. Clearing task "
                  "list so that scheduling will be attempted by the next caller to refresh this "
                  "namespace",
                  logAttrs(nss),
                  "error"_attr = redact(status));

            {
                stdx::lock_guard<stdx::mutex> lock(_mutex);
                _collAndChunkTaskLists.erase(nss);
            }
        } else {
            fassertFailedWithStatus(4826402, status);
        }
    });
}

void ShardServerCatalogCacheLoaderImpl::_runDbTasks(const DatabaseName& dbName) {
    // TODO(SERVER-111753): Please revisit if this thread could be made killable.
    ThreadClient tc("ShardServerCatalogCacheLoader::runDbTasks",
                    getGlobalServiceContext()->getService(ClusterRole::ShardServer),
                    ClientOperationKillableByStepdown{false});
    auto context = _contexts.makeOperationContext(*tc);

    bool taskFinished = false;
    bool inShutdown = false;
    try {
        if (MONGO_unlikely(hangDatabaseFlush.shouldFail())) {
            hangDatabaseFlush.pauseWhileSet(context.opCtx());
        }

        _updatePersistedDbMetadata(context.opCtx(), dbName);
        taskFinished = true;
    } catch (const ExceptionFor<ErrorCategory::ShutdownError>&) {
        LOGV2(
            22097, "Failed to persist metadata update for db due to shutdown", "db"_attr = dbName);
        inShutdown = true;
    } catch (const DBException& ex) {
        LOGV2(22098,
              "Failed to persist chunk metadata update for database",
              "db"_attr = dbName,
              "error"_attr = redact(ex));
    }

    {
        stdx::lock_guard<stdx::mutex> lock(_mutex);

        // If task completed successfully, remove it from work queue.
        if (taskFinished) {
            _dbTaskLists[dbName].pop_front();
        }

        // Return if have no more work
        if (_dbTaskLists[dbName].empty()) {
            _dbTaskLists.erase(dbName);
            return;
        }

        // If shutting down need to remove tasks to end waiting on its completion.
        if (inShutdown) {
            while (!_dbTaskLists[dbName].empty()) {
                _dbTaskLists[dbName].pop_front();
            }
            _dbTaskLists.erase(dbName);
            return;
        }
    }

    _executor->schedule([this, dbName](auto status) {
        if (status.isOK()) {
            _runDbTasks(dbName);
            return;
        }

        if (ErrorCodes::isCancellationError(status.code())) {
            LOGV2(22099,
                  "Cache loader failed to schedule a persisted metadata update task. Clearing task "
                  "list so that scheduling will be attempted by the next caller to refresh this "
                  "database",
                  "database"_attr = dbName,
                  "error"_attr = redact(status));

            {
                stdx::lock_guard<stdx::mutex> lock(_mutex);
                _dbTaskLists.erase(dbName);
            }
        } else {
            fassertFailedWithStatus(4826403, status);
        }
    });
}

void ShardServerCatalogCacheLoaderImpl::_updatePersistedCollAndChunksMetadata(
    OperationContext* opCtx, const NamespaceString& nss) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    const CollAndChunkTask& task = _collAndChunkTaskLists[nss].front();
    tassert(7032351,
            "Invalid CollAndChunkTask state",
            task.dropped || !task.collectionAndChangedChunks->changedChunks.empty());

    // If this task is from an old term and no longer valid, do not execute and return true so that
    // the task gets removed from the task list
    if (task.termCreated != _term) {
        return;
    }

    lock.unlock();

    // Check if this is a drop task
    if (task.dropped) {
        // The namespace was dropped. The persisted metadata for the collection must be cleared.
        uassertStatusOKWithContext(
            dropChunksAndDeleteCollectionsEntry(opCtx, nss),
            str::stream() << "Failed to clear persisted chunk metadata for collection '"
                          << nss.toStringForErrorMsg() << "'. Will be retried.");
        return;
    }

    uassertStatusOKWithContext(
        persistCollectionAndChangedChunks(
            opCtx, nss, *task.collectionAndChangedChunks, task.minQueryVersion),
        str::stream() << "Failed to update the persisted chunk metadata for collection '"
                      << nss.toStringForErrorMsg() << "' from '" << task.minQueryVersion.toString()
                      << "' to '" << task.maxQueryVersion.toString() << "'. Will be retried.");

    LOGV2_FOR_CATALOG_REFRESH(24112,
                              1,
                              "Successfully updated persisted chunk metadata for collection",
                              logAttrs(nss),
                              "oldCollectionPlacementVersion"_attr = task.minQueryVersion,
                              "newCollectionPlacementVersion"_attr = task.maxQueryVersion);
}

void ShardServerCatalogCacheLoaderImpl::_updatePersistedDbMetadata(OperationContext* opCtx,
                                                                   const DatabaseName& dbName) {
    stdx::unique_lock<stdx::mutex> lock(_mutex);

    const DBTask& task = _dbTaskLists[dbName].front();

    // If this task is from an old term and no longer valid, do not execute and return true so that
    // the task gets removed from the task list
    if (task.termCreated != _term) {
        return;
    }

    lock.unlock();

    // Check if this is a drop task
    if (!task.dbType) {
        // The database was dropped. The persisted metadata for the collection must be cleared.
        uassertStatusOKWithContext(deleteDatabasesEntry(opCtx, dbName),
                                   str::stream()
                                       << "Failed to clear persisted metadata for db '"
                                       << dbName.toStringForErrorMsg() << "'. Will be retried.");
        return;
    }

    uassertStatusOKWithContext(persistDbVersion(opCtx, *task.dbType),
                               str::stream()
                                   << "Failed to update the persisted metadata for db '"
                                   << dbName.toStringForErrorMsg() << "'. Will be retried.");

    LOGV2_FOR_CATALOG_REFRESH(24113,
                              1,
                              "Successfully updated persisted metadata for db",
                              "db"_attr = dbName.toStringForErrorMsg());
}

NamespaceMetadataChangeNotifications::ScopedNotification
ShardServerCatalogCacheLoaderImpl::_forcePrimaryCollectionRefreshAndWaitForReplication(
    OperationContext* opCtx, const NamespaceString& nss) {
    auto selfShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, getSelfShardId(opCtx)));

    // Run the operation on the primary and await the result to be replicated to this node. To avoid
    // issues with stepdown this is wrapped in a runAndThrowIfTermChanged since we currently have no
    // way to detect a rollback after the return from primary is received.
    return runAndThrowIfTermChanged(opCtx, [&](auto term) {
        auto notif = _namespaceNotifications.createNotification(nss);

        auto cmdResponse = uassertStatusOK(selfShard->runCommand(
            opCtx,
            ReadPreferenceSetting{ReadPreference::PrimaryOnly},
            DatabaseName::kAdmin,
            BSON("_flushRoutingTableCacheUpdates"
                 << NamespaceStringUtil::serialize(nss, SerializationContext::stateDefault())),
            Seconds{30},
            Shard::RetryPolicy::kIdempotent));

        uassertStatusOK(cmdResponse.commandStatus);

        uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->waitUntilOpTimeForRead(
            opCtx,
            {repl::OpTime{
                 cmdResponse.response.getField(LogicalTime::kOperationTimeFieldName).timestamp(),
                 term},
             boost::none}));
        return notif;
    });
}

CollectionAndChangedChunks
ShardServerCatalogCacheLoaderImpl::_getCompletePersistedMetadataForSecondarySinceVersion(
    OperationContext* opCtx,
    NamespaceMetadataChangeNotifications::ScopedNotification&& notif,
    const NamespaceString& nss,
    const ChunkVersion& version) {
    // Keep trying to load the metadata until we get a complete view without updates being
    // concurrently applied.
    while (true) {
        const auto beginRefreshState = [&]() {
            while (true) {
                auto refreshState = uassertStatusOK(getPersistedRefreshFlags(opCtx, nss));

                if (!refreshState.refreshing) {
                    return refreshState;
                }

                // Blocking call to wait for the notification, get the most recent value, and
                // recreate the notification under lock so that we don't miss any notifications.
                auto notificationTime = _namespaceNotifications.get(opCtx, notif);
                // Wait until the local lastApplied timestamp is the one from the notification.
                uassertStatusOK(repl::ReplicationCoordinator::get(opCtx)->waitUntilOpTimeForRead(
                    opCtx, {LogicalTime(notificationTime), boost::none}));
            }
        }();

        // Load the metadata.
        CollectionAndChangedChunks collAndChangedChunks =
            getPersistedMetadataSinceVersion(opCtx, nss, version);

        // Check that no updates were concurrently applied while we were loading the metadata: this
        // could cause the loaded metadata to provide an incomplete view of the chunk ranges.
        const auto endRefreshState = uassertStatusOK(getPersistedRefreshFlags(opCtx, nss));

        if (beginRefreshState == endRefreshState) {
            return collAndChangedChunks;
        }

        LOGV2_FOR_CATALOG_REFRESH(
            24114,
            1,
            "Cache loader read metadata while updates were being applied: this metadata may be "
            "incomplete. Retrying",
            "beginRefreshState"_attr = beginRefreshState,
            "endRefreshState"_attr = endRefreshState);
    }
}

ShardServerCatalogCacheLoaderImpl::CollAndChunkTask::CollAndChunkTask(
    StatusWith<CollectionAndChangedChunks> statusWithCollectionAndChangedChunks,
    ChunkVersion minimumQueryVersion,
    long long currentTerm)
    : taskNum(taskIdGenerator.fetchAndAdd(1)),
      minQueryVersion(std::move(minimumQueryVersion)),
      termCreated(currentTerm) {
    if (statusWithCollectionAndChangedChunks.isOK()) {
        collectionAndChangedChunks = std::move(statusWithCollectionAndChangedChunks.getValue());
        tassert(7032354,
                "Found no chunks in retrieved collection metadata",
                !collectionAndChangedChunks->changedChunks.empty());
        maxQueryVersion = collectionAndChangedChunks->changedChunks.back().getVersion();
    } else {
        tassert(7032358,
                fmt::format("Encountered unexpected error while fetching collection metadata: {}",
                            statusWithCollectionAndChangedChunks.getStatus().toString()),
                statusWithCollectionAndChangedChunks == ErrorCodes::NamespaceNotFound);
        dropped = true;
        maxQueryVersion = ChunkVersion::UNSHARDED();
    }
}

ShardServerCatalogCacheLoaderImpl::DBTask::DBTask(StatusWith<DatabaseType> swDatabaseType,
                                                  long long currentTerm)
    : taskNum(taskIdGenerator.fetchAndAdd(1)), termCreated(currentTerm) {
    if (swDatabaseType.isOK()) {
        dbType = std::move(swDatabaseType.getValue());
    } else {
        tassert(7032355,
                fmt::format("Encountered unexpected error while fetching database metadata: {}",
                            swDatabaseType.getStatus().toString()),
                swDatabaseType == ErrorCodes::NamespaceNotFound);
    }
}

ShardServerCatalogCacheLoaderImpl::CollAndChunkTaskList::CollAndChunkTaskList()
    : _activeTaskCompletedCondVar(std::make_shared<stdx::condition_variable>()) {}

ShardServerCatalogCacheLoaderImpl::DbTaskList::DbTaskList()
    : _activeTaskCompletedCondVar(std::make_shared<stdx::condition_variable>()) {}

void ShardServerCatalogCacheLoaderImpl::CollAndChunkTaskList::addTask(CollAndChunkTask task) {
    if (_tasks.empty()) {
        _tasks.emplace_back(std::move(task));
        return;
    }

    const auto& lastTask = _tasks.back();
    if (lastTask.termCreated != task.termCreated) {
        _tasks.emplace_back(std::move(task));
        return;
    }

    if (task.dropped) {
        tassert(7032356,
                str::stream() << "The version of the added task is not contiguous with that of "
                              << "the previous one: LastTask {" << lastTask.toString() << "}, "
                              << "AddedTask {" << task.toString() << "}",
                lastTask.maxQueryVersion == task.minQueryVersion);

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
        tassert(7032357,
                str::stream() << "The added task is not the first and its version is not "
                              << "contiguous with that of the previous one: LastTask {"
                              << lastTask.toString() << "}, AddedTask {" << task.toString() << "}",
                lastTask.maxQueryVersion == task.minQueryVersion || !task.minQueryVersion.isSet());

        _tasks.emplace_back(std::move(task));
    }
}

void ShardServerCatalogCacheLoaderImpl::DbTaskList::addTask(DBTask task) {
    if (_tasks.empty()) {
        _tasks.emplace_back(std::move(task));
        return;
    }

    if (!task.dbType) {
        // As an optimization, on database drop, clear any pending tasks in order to prevent any
        // throw-away work from executing. Because we have no way to differentiate whether the
        // active tasks is currently being operated on by a thread or not, we must leave the front
        // intact.
        _tasks.erase(std::next(_tasks.begin()), _tasks.end());

        // No need to schedule a drop if one is already currently active.
        if (_tasks.front().dbType) {
            _tasks.emplace_back(std::move(task));
        }
    } else {
        _tasks.emplace_back(std::move(task));
    }
}

void ShardServerCatalogCacheLoaderImpl::CollAndChunkTaskList::pop_front() {
    invariant(!_tasks.empty());
    _tasks.pop_front();
    _activeTaskCompletedCondVar->notify_all();
}

void ShardServerCatalogCacheLoaderImpl::DbTaskList::pop_front() {
    invariant(!_tasks.empty());
    _tasks.pop_front();
    _activeTaskCompletedCondVar->notify_all();
}

bool ShardServerCatalogCacheLoaderImpl::CollAndChunkTaskList::hasTasksFromThisTerm(
    long long term) const {
    invariant(!_tasks.empty());
    return _tasks.back().termCreated == term;
}

ChunkVersion ShardServerCatalogCacheLoaderImpl::CollAndChunkTaskList::getHighestVersionEnqueued()
    const {
    invariant(!_tasks.empty());
    return _tasks.back().maxQueryVersion;
}

CollectionAndChangedChunks
ShardServerCatalogCacheLoaderImpl::CollAndChunkTaskList::getEnqueuedMetadataForTerm(
    const long long term) const {
    CollectionAndChangedChunks collAndChunks;
    for (const auto& task : _tasks) {
        if (task.termCreated != term) {
            // Task data is no longer valid. Go on to the next task in the list.
            continue;
        }

        if (task.dropped) {
            // The current task is a drop -> the aggregated results aren't interesting so we
            // overwrite the CollAndChangedChunks and unset the flag
            collAndChunks = CollectionAndChangedChunks();
        } else if (task.collectionAndChangedChunks->epoch != collAndChunks.epoch) {
            // The current task has a new epoch -> the aggregated results aren't interesting so we
            // overwrite them
            collAndChunks = *task.collectionAndChangedChunks;
        } else {
            // The current task is not a drop neither an update and the epochs match -> we add its
            // results to the aggregated results

            // Make sure we do not append a duplicate chunk. The diff query is GTE, so there can
            // be duplicates of the same exact versioned chunk across tasks. This is no problem
            // for our diff application algorithms, but it can return unpredictable numbers of
            // chunks for testing purposes. Eliminate unpredictable duplicates for testing
            // stability.
            auto taskCollectionAndChangedChunksIt =
                task.collectionAndChangedChunks->changedChunks.begin();
            if (!collAndChunks.changedChunks.empty() &&
                collAndChunks.changedChunks.back().getVersion() ==
                    taskCollectionAndChangedChunksIt->getVersion()) {
                ++taskCollectionAndChangedChunksIt;
            }

            collAndChunks.changedChunks.insert(
                collAndChunks.changedChunks.end(),
                taskCollectionAndChangedChunksIt,
                task.collectionAndChangedChunks->changedChunks.end());

            // Keep the most recent version of these fields
            collAndChunks.unsplittable = task.collectionAndChangedChunks->unsplittable;
            collAndChunks.allowMigrations = task.collectionAndChangedChunks->allowMigrations;
            collAndChunks.reshardingFields = task.collectionAndChangedChunks->reshardingFields;
            collAndChunks.timeseriesFields = task.collectionAndChangedChunks->timeseriesFields;
        }
    }
    return collAndChunks;
}

}  // namespace mongo
