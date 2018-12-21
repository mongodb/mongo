
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_filtering_metadata_refresh.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

MONGO_FAIL_POINT_DEFINE(skipDatabaseVersionMetadataRefresh);
MONGO_FAIL_POINT_DEFINE(skipShardFilteringMetadataRefresh);

namespace {

void onShardVersionMismatch(OperationContext* opCtx,
                            const NamespaceString& nss,
                            ChunkVersion shardVersionReceived,
                            bool forceRefreshFromThisThread) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    invariant(ShardingState::get(opCtx)->canAcceptShardedCommands());

    LOG(2) << "Metadata refresh requested for " << nss.ns() << " at shard version "
           << shardVersionReceived;

    ShardingStatistics::get(opCtx).countStaleConfigErrors.addAndFetch(1);

    // Ensure any ongoing migrations have completed before trying to do the refresh. This wait is
    // just an optimization so that mongos does not exhaust its maximum number of StaleShardVersion
    // retry attempts while the migration is being committed.
    OperationShardingState::get(opCtx).waitForMigrationCriticalSectionSignal(opCtx);

    const auto currentShardVersion = [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        return CollectionShardingState::get(opCtx, nss)->getCurrentShardVersionIfKnown();
    }();

    if (currentShardVersion) {
        if (currentShardVersion->epoch() == shardVersionReceived.epoch() &&
            currentShardVersion->majorVersion() >= shardVersionReceived.majorVersion()) {
            // Don't need to remotely reload if we're in the same epoch and the requested version is
            // smaller than the one we know about. This means that the remote side is behind.
            return;
        }
    }

    if (MONGO_FAIL_POINT(skipShardFilteringMetadataRefresh)) {
        return;
    }

    forceShardFilteringMetadataRefresh(opCtx, nss, forceRefreshFromThisThread);
}

void onDbVersionMismatch(OperationContext* opCtx,
                         const StringData dbName,
                         const DatabaseVersion& clientDbVersion,
                         const boost::optional<DatabaseVersion>& serverDbVersion) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    invariant(ShardingState::get(opCtx)->canAcceptShardedCommands());

    if (serverDbVersion && serverDbVersion->getUuid() == clientDbVersion.getUuid() &&
        serverDbVersion->getLastMod() >= clientDbVersion.getLastMod()) {
        // The client was stale; do not trigger server-side refresh.
        return;
    }

    // Ensure any ongoing movePrimary's have completed before trying to do the refresh. This wait is
    // just an optimization so that mongos does not exhaust its maximum number of
    // StaleDatabaseVersion retry attempts while the movePrimary is being committed.
    OperationShardingState::get(opCtx).waitForMovePrimaryCriticalSectionSignal(opCtx);

    if (MONGO_FAIL_POINT(skipDatabaseVersionMetadataRefresh)) {
        return;
    }

    forceDatabaseRefresh(opCtx, dbName);
}

}  // namespace

Status onShardVersionMismatchNoExcept(OperationContext* opCtx,
                                      const NamespaceString& nss,
                                      ChunkVersion shardVersionReceived,
                                      bool forceRefreshFromThisThread) noexcept {
    try {
        onShardVersionMismatch(opCtx, nss, shardVersionReceived, forceRefreshFromThisThread);
        return Status::OK();
    } catch (const DBException& ex) {
        log() << "Failed to refresh metadata for collection" << nss << causedBy(redact(ex));
        return ex.toStatus();
    }
}

ChunkVersion forceShardFilteringMetadataRefresh(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                bool forceRefreshFromThisThread) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    auto* const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->canAcceptShardedCommands());

    auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
            opCtx, nss, forceRefreshFromThisThread));
    auto cm = routingInfo.cm();

    if (!cm) {
        // No chunk manager, so unsharded.

        // Exclusive collection lock needed since we're now changing the metadata
        AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);
        CollectionShardingRuntime::get(opCtx, nss)
            ->setFilteringMetadata(opCtx, CollectionMetadata());

        return ChunkVersion::UNSHARDED();
    }

    // Optimistic check with only IS lock in order to avoid threads piling up on the collection X
    // lock below
    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        auto optMetadata = CollectionShardingState::get(opCtx, nss)->getCurrentMetadataIfKnown();

        // We already have newer version
        if (optMetadata) {
            const auto& metadata = *optMetadata;
            if (metadata->isSharded() &&
                metadata->getCollVersion().epoch() == cm->getVersion().epoch() &&
                metadata->getCollVersion() >= cm->getVersion()) {
                LOG(1) << "Skipping refresh of metadata for " << nss << " "
                       << metadata->getCollVersion() << " with an older " << cm->getVersion();
                return metadata->getShardVersion();
            }
        }
    }

    // Exclusive collection lock needed since we're now changing the metadata
    AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);
    auto* const css = CollectionShardingRuntime::get(opCtx, nss);

    {
        auto optMetadata = CollectionShardingState::get(opCtx, nss)->getCurrentMetadataIfKnown();

        // We already have newer version
        if (optMetadata) {
            const auto& metadata = *optMetadata;
            if (metadata->isSharded() &&
                metadata->getCollVersion().epoch() == cm->getVersion().epoch() &&
                metadata->getCollVersion() >= cm->getVersion()) {
                LOG(1) << "Skipping refresh of metadata for " << nss << " "
                       << metadata->getCollVersion() << " with an older " << cm->getVersion();
                return metadata->getShardVersion();
            }
        }
    }

    CollectionMetadata metadata(std::move(cm), shardingState->shardId());
    const auto newShardVersion = metadata.getShardVersion();

    css->setFilteringMetadata(opCtx, std::move(metadata));
    return newShardVersion;
}

Status onDbVersionMismatchNoExcept(
    OperationContext* opCtx,
    const StringData dbName,
    const DatabaseVersion& clientDbVersion,
    const boost::optional<DatabaseVersion>& serverDbVersion) noexcept {
    try {
        onDbVersionMismatch(opCtx, dbName, clientDbVersion, serverDbVersion);
        return Status::OK();
    } catch (const DBException& ex) {
        log() << "Failed to refresh databaseVersion for database " << dbName
              << causedBy(redact(ex));
        return ex.toStatus();
    }
}

void forceDatabaseRefresh(OperationContext* opCtx, const StringData dbName) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    auto const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->canAcceptShardedCommands());

    const auto refreshedDbVersion =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getDatabaseWithRefresh(opCtx, dbName))
            .databaseVersion();

    // First, check under a shared lock if another thread already updated the cached version.
    // This is a best-effort optimization to make as few threads as possible to convoy on the
    // exclusive lock below.
    auto databaseHolder = DatabaseHolder::get(opCtx);
    {
        // Take the DBLock directly rather than using AutoGetDb, to prevent a recursive call
        // into checkDbVersion().
        Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
        auto db = databaseHolder->getDb(opCtx, dbName);
        if (!db) {
            log() << "Database " << dbName
                  << " has been dropped; not caching the refreshed databaseVersion";
            return;
        }

        const auto cachedDbVersion = DatabaseShardingState::get(db).getDbVersion(opCtx);
        if (cachedDbVersion && cachedDbVersion->getUuid() == refreshedDbVersion.getUuid() &&
            cachedDbVersion->getLastMod() >= refreshedDbVersion.getLastMod()) {
            LOG(2) << "Skipping setting cached databaseVersion for " << dbName
                   << " to refreshed version " << refreshedDbVersion.toBSON()
                   << " because current cached databaseVersion is already "
                   << cachedDbVersion->toBSON();
            return;
        }
    }

    // The cached version is older than the refreshed version; update the cached version.
    Lock::DBLock dbLock(opCtx, dbName, MODE_X);
    auto db = databaseHolder->getDb(opCtx, dbName);
    if (!db) {
        log() << "Database " << dbName
              << " has been dropped; not caching the refreshed databaseVersion";
        return;
    }

    DatabaseShardingState::get(db).setDbVersion(opCtx, std::move(refreshedDbVersion));
}

}  // namespace mongo
