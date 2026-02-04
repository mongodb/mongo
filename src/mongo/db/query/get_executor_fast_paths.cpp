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

#include "mongo/db/query/get_executor_fast_paths.h"

#include <boost/container/flat_set.hpp>
#include <boost/container/small_vector.hpp>
#include <boost/container/vector.hpp>
#include <boost/cstdint.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/sort_key_generator.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/exec/express/plan_executor_express.h"
#include "mongo/db/exec/runtime_planners/planner_interface.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

namespace {

boost::optional<ScopedCollectionFilter> getScopedCollectionFilter(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    const QueryPlannerParams& plannerParams) {
    if (plannerParams.mainCollectionInfo.options & QueryPlannerParams::INCLUDE_SHARD_FILTER) {
        auto collFilter = collections.getMainCollectionPtrOrAcquisition().getShardingFilter();
        tassert(11321302,
                "Attempting to use shard filter when there's no shard filter available for "
                "the collection",
                collFilter);
        return collFilter;
    }
    return boost::none;
}

}  // namespace

ExpressResult tryExpress(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    std::unique_ptr<CanonicalQuery>& canonicalQuery,
    std::size_t plannerOptions,
    const std::function<std::unique_ptr<QueryPlannerParams>(size_t)>& makePlannerParams) {
    // First try to use the express id point query fast path.
    const auto& mainColl = collections.getMainCollection();
    const auto expressEligibility = isExpressEligible(opCtx, mainColl, *canonicalQuery);
    if (expressEligibility == ExpressEligibility::IdPointQueryEligible) {
        planCacheCounters.incrementClassicSkippedCounter();
        auto plannerParams =
            std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForExpress{
                opCtx, *canonicalQuery, collections, plannerOptions});
        auto collectionFilter = getScopedCollectionFilter(opCtx, collections, *plannerParams);
        const bool isClusteredOnId = plannerParams->clusteredInfo
            ? clustered_util::isClusteredOnId(plannerParams->clusteredInfo)
            : false;

        auto expressExecutor = isClusteredOnId
            ? makeExpressExecutorForFindByClusteredId(
                  opCtx,
                  std::move(canonicalQuery),
                  collections.getMainCollectionPtrOrAcquisition(),
                  std::move(collectionFilter),
                  plannerOptions & QueryPlannerParams::RETURN_OWNED_DATA)
            : makeExpressExecutorForFindById(opCtx,
                                             std::move(canonicalQuery),
                                             collections.getMainCollectionPtrOrAcquisition(),
                                             std::move(collectionFilter),
                                             plannerOptions &
                                                 QueryPlannerParams::RETURN_OWNED_DATA);

        return {.executor = std::move(expressExecutor)};
    }

    // The query might still be eligible for express execution via the index equality fast path.
    // However, that requires the full set of planner parameters for the main collection to be
    // available and creating those now allows them to be reused for subsequent strategies if
    // the express index equality one fails.
    auto paramsForSingleCollectionQuery = makePlannerParams(plannerOptions);
    if (expressEligibility == ExpressEligibility::IndexedEqualityEligible) {
        if (auto indexEntry =
                getIndexForExpressEquality(*canonicalQuery, *paramsForSingleCollectionQuery)) {
            auto expressExecutor = makeExpressExecutorForFindByUserIndex(
                opCtx,
                std::move(canonicalQuery),
                collections.getMainCollectionPtrOrAcquisition(),
                *indexEntry,
                getScopedCollectionFilter(opCtx, collections, *paramsForSingleCollectionQuery),
                plannerOptions & QueryPlannerParams::RETURN_OWNED_DATA);

            return {.executor = std::move(expressExecutor)};
        }
    }

    // Allow reuse of the planner params, in case other planning logic needs it.
    return {.plannerParams = std::move(paramsForSingleCollectionQuery)};
}

std::unique_ptr<classic_runtime_planner::IdHackPlanner> tryIdHack(
    OperationContext* opCtx,
    const MultipleCollectionAccessor& collections,
    CanonicalQuery* cq,
    const std::function<PlannerData()>& makePlannerData) {
    const auto& mainCollection = collections.getMainCollection();
    if (!isIdHackEligibleQuery(mainCollection, *cq)) {
        return nullptr;
    }

    const auto indexEntry = mainCollection->getIndexCatalog()->findIdIndex(opCtx);
    if (!indexEntry) {
        return nullptr;
    }

    LOGV2_DEBUG(20922,
                2,
                "Using classic engine idhack",
                "canonicalQuery"_attr = redact(cq->toStringShort()));
    planCacheCounters.incrementClassicSkippedCounter();
    fastPathQueryCounters.incrementIdHackQueryCounter();
    return std::make_unique<classic_runtime_planner::IdHackPlanner>(makePlannerData(), indexEntry);
}

}  // namespace mongo
