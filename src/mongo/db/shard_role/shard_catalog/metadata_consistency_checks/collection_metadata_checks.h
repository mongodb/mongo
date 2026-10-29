// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/database_name.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
namespace [[MONGO_MOD_PARENT_PRIVATE]] collection_metadata_consistency_checks {

/**
 * Checks for durable and in-memory collection metadata in the shard catalog for databases that do
 * not exist in the global catalog.
 * `shardId` is the shard this check is running on.
 * `dbNamesInGlobalCatalog` is the set of databases known to the global catalog at the start of the
 * check.
 */
std::vector<MetadataInconsistencyItem> checkNoMetadataForNonExistentDatabase(
    OperationContext* opCtx,
    const ShardId& shardId,
    metadata_consistency_util::RSNodeMode rsMode,
    const stdx::unordered_set<DatabaseName>& dbNamesInGlobalCatalog);

}  // namespace collection_metadata_consistency_checks
}  // namespace mongo
