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

#include "mongo/db/catalog/catalog_raii.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/grid.h"
#include "mongo/util/log.h"

namespace mongo {

Status onShardVersionMismatch(OperationContext* opCtx,
                              const NamespaceString& nss,
                              ChunkVersion shardVersionReceived) noexcept {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    auto const shardingState = ShardingState::get(opCtx);
    invariantOK(shardingState->canAcceptShardedCommands());

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
        const auto currentMetadata = CollectionShardingState::get(opCtx, nss)->getMetadata();
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
        forceShardFilteringMetadataRefresh(opCtx, nss);
        return Status::OK();
    } catch (const DBException& ex) {
        log() << "Failed to refresh metadata for collection" << nss << causedBy(redact(ex));
        return ex.toStatus();
    }
}

ChunkVersion forceShardFilteringMetadataRefresh(OperationContext* opCtx,
                                                const NamespaceString& nss) {
    invariant(!opCtx->lockState()->isLocked());
    invariant(!opCtx->getClient()->isInDirectClient());

    auto const shardingState = ShardingState::get(opCtx);
    invariantOK(shardingState->canAcceptShardedCommands());

    const auto routingInfo = uassertStatusOK(
        Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfoWithRefresh(opCtx, nss));
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
        auto css = CollectionShardingState::get(opCtx, nss);

        // We already have newer version
        if (css->getMetadata() &&
            css->getMetadata()->getCollVersion().epoch() == cm->getVersion().epoch() &&
            css->getMetadata()->getCollVersion() >= cm->getVersion()) {
            LOG(1) << "Skipping refresh of metadata for " << nss << " "
                   << css->getMetadata()->getCollVersion() << " with an older " << cm->getVersion();
            return css->getMetadata()->getShardVersion();
        }
    }

    // Exclusive collection lock needed since we're now changing the metadata
    AutoGetCollection autoColl(opCtx, nss, MODE_IX, MODE_X);

    auto css = CollectionShardingState::get(opCtx, nss);

    // We already have newer version
    if (css->getMetadata() &&
        css->getMetadata()->getCollVersion().epoch() == cm->getVersion().epoch() &&
        css->getMetadata()->getCollVersion() >= cm->getVersion()) {
        LOG(1) << "Skipping refresh of metadata for " << nss << " "
               << css->getMetadata()->getCollVersion() << " with an older " << cm->getVersion();
        return css->getMetadata()->getShardVersion();
    }

    std::unique_ptr<CollectionMetadata> newCollectionMetadata =
        stdx::make_unique<CollectionMetadata>(cm, shardingState->getShardName());

    css->refreshMetadata(opCtx, std::move(newCollectionMetadata));

    return css->getMetadata()->getShardVersion();
}

}  // namespace mongo
