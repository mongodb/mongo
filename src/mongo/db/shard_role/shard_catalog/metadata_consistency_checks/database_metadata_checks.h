// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_types_gen.h"
#include "mongo/db/global_catalog/metadata_consistency_validation/metadata_consistency_util.h"
#include "mongo/db/global_catalog/type_database_gen.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/util/modules.h"

#include <vector>

namespace mongo {
namespace [[MONGO_MOD_PARENT_PRIVATE]] database_metadata_consistency_checks {

/**
 * Checks for inconsistencies in the database's metadata between the global catalog and the
 * shard catalog. `shardId` is the shard this check is running on.
 *
 * The list of inconsistencies is returned as a vector of MetadataInconsistencies objects. If
 * there is no inconsistency, it returns an empty vector.
 */
std::vector<MetadataInconsistencyItem> checkDatabaseMetadataConsistency(
    OperationContext* opCtx,
    const DatabaseType& dbInGlobalCatalog,
    const ShardId& shardId,
    metadata_consistency_util::RSNodeMode rsMode = metadata_consistency_util::RSNodeMode::kPrimary);

}  // namespace database_metadata_consistency_checks
}  // namespace mongo
