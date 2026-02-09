/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/base/status_with.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_distinct.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/engine_selection.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/write_ops/canonical_update.h"
#include "mongo/db/query/write_ops/parsed_delete.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog_entry.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>

#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>


namespace mongo {

/**
 * Struct to hold information about a query plan's cache info.
 */
struct PlanCacheInfo {
    boost::optional<uint32_t> planCacheKey;
    boost::optional<uint32_t> planCacheShapeHash;
};

/**
 * Fills in the given information on the CurOp::OpDebug object, if it has not already been filled in
 * by an outer pipeline.
 */
void setOpDebugPlanCacheInfo(OperationContext* opCtx, const PlanCacheInfo& cacheInfo);


using MakePlannerParamsFn =
    std::function<std::unique_ptr<QueryPlannerParams>(const CanonicalQuery&, size_t)>;
using MakePlannerFn =
    std::function<std::unique_ptr<PlannerInterface>(std::unique_ptr<QueryPlannerParams>)>;

/*
 * Calls `makePlanner` with five retries in case exceptions are thrown. Uses the given plannerParams
 * at first to avoid additional calls to `makeQueryPlannerParams`.
 */
std::unique_ptr<PlannerInterface> retryMakePlanner(
    std::unique_ptr<QueryPlannerParams> plannerParams,
    const MakePlannerParamsFn& makeQueryPlannerParams,
    const MakePlannerFn& makePlanner,
    CanonicalQuery* canonicalQuery,
    std::size_t plannerOptions,
    Pipeline* pipeline);

}  // namespace mongo
