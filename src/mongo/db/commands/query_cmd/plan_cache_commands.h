// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status_with.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/modules.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <memory>

namespace mongo::plan_cache_commands {

/**
 * Parses the plan cache command specified by 'ns' and 'cmdObj' and returns the query shape inside
 * that command represented as a CanonicalQuery.
 *
 * TODO SERVER-88503 - Remove canonicalize() when IndexFilters is removed since there is no other
 * consumer.
 */
StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(OperationContext* opCtx,
                                                         const NamespaceString& nss,
                                                         const BSONObj& cmdObj);

/**
 * Remove the plan cache entries whose 'planCacheCommandKey' matches any key in
 * 'planCacheCommandKeys'. Please note that we do not handle 'planCacheCommandKey' hash collisions,
 * namely it's fine to clear a plan cache entry that we technically could have kept around.
 *
 * TODO SERVER-88503 - Move function to plan_cache_clear_command since it will be the
 * only consumer after IndexFilters is removed.
 */
void removePlanCacheEntriesByPlanCacheCommandKeys(
    const stdx::unordered_set<uint32_t>& planCacheCommandKeys, PlanCache* planCache);

/**
 * Similar to removePlanCacheEntriesByPlanCacheCommandKeys() above. This function clears cache
 * entries in a SBE plan cache. There is an extra check on the collection UUID because all
 * collections share one single 'sbe::PlanCache' instance. This will only clear plan cache entries
 * where the given UUID is the main collection (not when it is a secondary collection).
 *
 * TODO SERVER-88503 - Move function to plan_cache_clear_command since it will be the
 * only consumer after IndexFilters is removed.
 */
void removePlanCacheEntriesByPlanCacheCommandKeys(
    const stdx::unordered_set<uint32_t>& planCacheCommandKeys,
    const UUID& collectionUuid,
    sbe::PlanCache* planCache);


/**
 * Validator for the 'collation' field of the 'planCacheClear' command.
 * Given an input BSONObj representing the 'collation' field, it returns ErrorCodes::BadValue if the
 * BSONObj is empty, otherwise returns Status::OK().
 */
Status validateNonEmptyCollation(const BSONObj& obj);
}  // namespace mongo::plan_cache_commands
