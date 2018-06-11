/**
 *    Copyright (C) 2018 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/shard_filtering_metadata_refresh.h"

#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/commands/feature_compatibility_version.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/database_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

Status onShardVersionMismatch(OperationContext* opCtx,
                              const NamespaceString& nss,
                              ChunkVersion shardVersionReceived,
                              bool forceRefreshFromThisThread) noexcept {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    auto const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->canAcceptShardedCommands());

    LOG(2) << "Metadata refresh requested for " << nss.ns() << " at shard version "
           << shardVersionReceived;

    ShardingStatistics::get(opCtx).countStaleConfigErrors.addAndFetch(1);

    // Ensure any ongoing migrations have completed before trying to do the refresh. This wait is
    // just an optimization so that MongoS does not exhaust its maximum number of StaleConfig retry
    // attempts while the migration is being committed.
    try {
        auto& oss = OperationShardingState::get(opCtx);
        oss.waitForMigrationCriticalSectionSignal(opCtx);
    } catch (const DBException& ex) {
        return ex.toStatus();
    }

    const auto currentShardVersion = [&] {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        const auto currentMetadata = CollectionShardingState::get(opCtx, nss)->getMetadata(opCtx);
        if (currentMetadata) {
            return currentMetadata->getShardVersion();
        }

        return ChunkVersion::UNSHARDED();
    }();

    if (currentShardVersion.epoch() == shardVersionReceived.epoch() &&
        currentShardVersion.majorVersion() >= shardVersionReceived.majorVersion()) {
        // Don't need to remotely reload if we're in the same epoch and the requested version is
        // smaller than the one we know about. This means that the remote side is behind.
        return Status::OK();
    }

    try {
        forceShardFilteringMetadataRefresh(opCtx, nss, forceRefreshFromThisThread);
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

    auto const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->canAcceptShardedCommands());

    const auto routingInfo =
        uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(
            opCtx, nss, forceRefreshFromThisThread));
    const auto cm = routingInfo.cm();

    if (!cm) {
        // No chunk manager, so unsharded.

        // Exclusive collection lock needed since we're now changing the metadata
        AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);

        auto css = CollectionShardingState::get(opCtx, nss);
        css->refreshMetadata(opCtx, nullptr);

        return ChunkVersion::UNSHARDED();
    }

    {
        AutoGetCollection autoColl(opCtx, nss, MODE_IS);
        auto metadata = CollectionShardingState::get(opCtx, nss)->getMetadata(opCtx);

        // We already have newer version
        if (metadata && metadata->getCollVersion().epoch() == cm->getVersion().epoch() &&
            metadata->getCollVersion() >= cm->getVersion()) {
            LOG(1) << "Skipping refresh of metadata for " << nss << " "
                   << metadata->getCollVersion() << " with an older " << cm->getVersion();
            return metadata->getShardVersion();
        }
    }

    // Exclusive collection lock needed since we're now changing the metadata
    AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);

    auto css = CollectionShardingState::get(opCtx, nss);
    auto metadata = css->getMetadata(opCtx);

    // We already have newer version
    if (metadata && metadata->getCollVersion().epoch() == cm->getVersion().epoch() &&
        metadata->getCollVersion() >= cm->getVersion()) {
        LOG(1) << "Skipping refresh of metadata for " << nss << " " << metadata->getCollVersion()
               << " with an older " << cm->getVersion();
        return metadata->getShardVersion();
    }

    std::unique_ptr<CollectionMetadata> newCollectionMetadata =
        stdx::make_unique<CollectionMetadata>(cm, shardingState->getShardName());

    css->refreshMetadata(opCtx, std::move(newCollectionMetadata));

    return css->getMetadata(opCtx)->getShardVersion();
}

void onDbVersionMismatch(OperationContext* opCtx,
                         const StringData dbName,
                         const DatabaseVersion& clientDbVersion,
                         const boost::optional<DatabaseVersion>& serverDbVersion) noexcept {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    auto const shardingState = ShardingState::get(opCtx);
    invariant(shardingState->canAcceptShardedCommands());

    if (serverDbVersion && serverDbVersion->getUuid() == clientDbVersion.getUuid() &&
        serverDbVersion->getLastMod() >= clientDbVersion.getLastMod()) {
        // The client was stale; do not trigger server-side refresh.
        return;
    }

    try {
        // TODO SERVER-33773 if the 'waitForMovePrimaryCriticalSection' flag is set on the
        // OperationShardingState, wait for the movePrimary critical section to complete before
        // attempting a refresh.
    } catch (const DBException& ex) {
        log() << "Failed to wait for movePrimary critical section to complete "
              << causedBy(redact(ex));
        return;
    }

    try {
        forceDatabaseRefresh(opCtx, dbName);
    } catch (const DBException& ex) {
        log() << "Failed to refresh databaseVersion for database " << dbName
              << causedBy(redact(ex));
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
    {
        // Take the DBLock directly rather than using AutoGetDb, to prevent a recursive call
        // into checkDbVersion().
        Lock::DBLock dbLock(opCtx, dbName, MODE_IS);
        const auto db = DatabaseHolder::getDatabaseHolder().get(opCtx, dbName);
        if (!db) {
            log() << "Database " << dbName
                  << " has been dropped; not caching the refreshed databaseVersion";
            return;
        }

        const auto cachedDbVersion = DatabaseShardingState::get(db).getDbVersion(opCtx);
        if (cachedDbVersion && refreshedDbVersion &&
            cachedDbVersion->getUuid() == refreshedDbVersion->getUuid() &&
            cachedDbVersion->getLastMod() >= refreshedDbVersion->getLastMod()) {
            LOG(2) << "Skipping setting cached databaseVersion for " << dbName
                   << " to refreshed version " << refreshedDbVersion->toBSON()
                   << " because current cached databaseVersion is already "
                   << cachedDbVersion->toBSON();
            return;
        }
    }

    // The cached version is older than the refreshed version; update the cached version.
    Lock::DBLock dbLock(opCtx, dbName, MODE_X);
    const auto db = DatabaseHolder::getDatabaseHolder().get(opCtx, dbName);
    if (!db) {
        log() << "Database " << dbName
              << " has been dropped; not caching the refreshed databaseVersion";
        return;
    }

    DatabaseShardingState::get(db).setDbVersion(opCtx, std::move(refreshedDbVersion));
}

}  // namespace mongo
