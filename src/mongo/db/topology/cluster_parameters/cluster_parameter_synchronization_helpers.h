// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/shard_role/shard_catalog/collection.h"
#include "mongo/db/tenant_id.h"
#include "mongo/util/modules.h"

#include <string_view>

#include <boost/optional/optional.hpp>

namespace mongo {
namespace cluster_parameters {

[[MONGO_MOD_PRIVATE]] void validateParameter(BSONObj doc,
                                             const boost::optional<TenantId>& tenantId);

[[MONGO_MOD_PRIVATE]] void updateParameter(OperationContext* opCtx,
                                           BSONObj doc,
                                           std::string_view mode,
                                           const boost::optional<TenantId>& tenantId);

[[MONGO_MOD_PRIVATE]] void clearParameter(OperationContext* opCtx,
                                          std::string_view id,
                                          const boost::optional<TenantId>& tenantId);

[[MONGO_MOD_PRIVATE]] void clearAllTenantParameters(OperationContext* opCtx,
                                                    const boost::optional<TenantId>& tenantId);

/**
 * Used to initialize in-memory cluster parameter state based on the on-disk contents after startup
 * recovery or initial sync is complete.
 */
[[MONGO_MOD_NEEDS_REPLACEMENT]] void initializeAllTenantParametersFromCollection(
    OperationContext* opCtx, const Collection& coll);

/**
 * Used on rollback. Updates settings which are present and clears settings which are not.
 */
[[MONGO_MOD_PRIVATE]] void resynchronizeAllTenantParametersFromCollection(OperationContext* opCtx,
                                                                          const Collection& coll);

}  // namespace cluster_parameters
}  // namespace mongo
