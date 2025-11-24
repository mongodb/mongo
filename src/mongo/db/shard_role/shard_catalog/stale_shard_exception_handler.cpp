/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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
    uassertStatusOK(FilteringMetadataCache::get(opCtx)->onCollectionPlacementVersionMismatch(
        opCtx, sci.getNss(), sci.getVersionReceived().placementVersion()));

    // Get the new installed shard metadata version and return it.
    auto scopedCsr = CollectionShardingRuntime::acquireShared(opCtx, sci.getNss());
    const auto maybeCurrentMetadata = scopedCsr->getCurrentMetadataIfKnown();
    return maybeCurrentMetadata
        ? boost::optional<ChunkVersion>(maybeCurrentMetadata->getShardPlacementVersion())
        : boost::none;
}

}  // namespace mongo
