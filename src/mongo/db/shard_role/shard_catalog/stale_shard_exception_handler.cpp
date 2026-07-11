// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/shard_role/shard_catalog/stale_shard_exception_handler.h"

#include "mongo/db/s/resharding/resharding_metrics_helpers.h"
#include "mongo/db/shard_role/shard_catalog/collection_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/database_sharding_runtime.h"
#include "mongo/db/shard_role/shard_catalog/shard_filtering_metadata_refresh.h"
#include "mongo/db/versioning_protocol/stale_exception.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {

boost::optional<DatabaseVersion>
StaleShardDatabaseMetadataHandlerImpl::handleStaleDatabaseVersionException(
    OperationContext* opCtx, const StaleDbRoutingVersion& staleDbException) const {
    // Recover or refresh the shard metadata.
    uassertStatusOK(FilteringMetadataCache::get(opCtx)->onDbVersionMismatch(
        opCtx, staleDbException.getDb(), staleDbException.getVersionReceived()));

    BypassDatabaseMetadataAccess bypassDbMetadataAccess(
        opCtx, BypassDatabaseMetadataAccess::Type::kReadOnly);  // NOLINT
    auto scopedDss = DatabaseShardingRuntime::acquireShared(opCtx, staleDbException.getDb());
    return scopedDss->getDbVersion(opCtx);
}

boost::optional<ChunkVersion>
StaleShardCollectionMetadataHandlerImpl::handleStaleShardVersionException(
    OperationContext* opCtx, const StaleConfigInfo& sci) const {
    // Track resharding metrics.
    resharding_metrics::onCriticalSectionError(opCtx, sci);

    // Recover or refresh the shard metadata.
    uassertStatusOK(FilteringMetadataCache::get(opCtx)->onShardVersionMismatch(
        opCtx, sci.getNss(), sci.getVersionReceived().placementVersion()));

    // Get the new installed shard metadata version and return it.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, sci.getNss());
    const auto maybeCurrentMetadata = scopedCsr->getCurrentMetadataIfKnown();
    return maybeCurrentMetadata
        ? boost::optional<ChunkVersion>(maybeCurrentMetadata->getShardPlacementVersion())
        : boost::none;
}

}  // namespace mongo
