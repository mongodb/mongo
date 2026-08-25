// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
namespace [[MONGO_MOD_PARENT_PRIVATE]] non_existing_database_metadata_consistency_checks {

/**
 * Checks for inconsistencies where this shard holds database or collection metadata (durable shard
 * catalog or in-memory DSS/CSS) for a database that does not exist in the global catalog.
 * `shardId` is the shard this check is running on.
 *
 * Returns the list of detected inconsistencies.
 */
std::vector<MetadataInconsistencyItem> checkNonExistingDatabaseMetadataConsistency(
    OperationContext* opCtx,
    const ShardId& shardId,
    metadata_consistency_util::RSNodeMode rsMode = metadata_consistency_util::RSNodeMode::kPrimary);

}  // namespace non_existing_database_metadata_consistency_checks
}  // namespace mongo
