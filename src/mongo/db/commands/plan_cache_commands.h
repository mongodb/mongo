/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/sbe_plan_cache.h"

namespace mongo::plan_cache_commands {

/**
 * Parses the plan cache command specified by 'ns' and 'cmdObj' and returns the query shape inside
 * that command represented as a CanonicalQuery.
 */
StatusWith<std::unique_ptr<CanonicalQuery>> canonicalize(OperationContext* opCtx,
                                                         StringData ns,
                                                         const BSONObj& cmdObj);

/**
 * Remove the plan cache entries whose 'planCacheCommandKey' matches any key in
 * 'planCacheCommandKeys'. Please note that we do not handle 'planCacheCommandKey' hash collisions,
 * namely it's fine to clear a plan cache entry that we technically could have kept around.
 */
void removePlanCacheEntriesByPlanCacheCommandKeys(
    const stdx::unordered_set<uint32_t>& planCacheCommandKeys, PlanCache* planCache);

/**
 * Similar to removePlanCacheEntriesByPlanCacheCommandKeys() above. This function clears cache
 * entries in a SBE plan cache. There is an extra check on the collection UUID because all
 * collections share one single 'sbe::PlanCache' instance. This will only clear plan cache entries
 * where the given UUID is the main collection (not when it is a secondary collection).
 */
void removePlanCacheEntriesByPlanCacheCommandKeys(
    const stdx::unordered_set<uint32_t>& planCacheCommandKeys,
    const UUID& collectionUuid,
    sbe::PlanCache* planCache);
}  // namespace mongo::plan_cache_commands
