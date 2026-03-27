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

#include "mongo/db/query/get_executor_deferred_engine_choice.h"

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
#include "mongo/db/exec/classic/cached_plan.h"
#include "mongo/db/exec/classic/delete_stage.h"
#include "mongo/db/exec/classic/plan_stage.h"
#include "mongo/db/exec/classic/subplan.h"
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/get_executor_deferred_engine_choice_lowering.h"
#include "mongo/db/query/get_executor_deferred_engine_choice_planning.h"
#include "mongo/db/query/get_executor_helpers.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/update/update_driver.h"

#include <utility>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::exec_deferred_engine_choice {

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
getExecutorFindDeferredEngineChoice(OperationContext* opCtx,
                                    const MultipleCollectionAccessor& collections,
                                    std::unique_ptr<CanonicalQuery> canonicalQuery,
                                    PlanYieldPolicy::YieldPolicy yieldPolicy,
                                    const MakePlannerParamsFn& makeQueryPlannerParams,
                                    std::size_t plannerOptions,
                                    Pipeline* pipeline) {
    PlanRankingResult rankerResult = planRanking(opCtx,
                                                 collections,
                                                 canonicalQuery,
                                                 yieldPolicy,
                                                 plannerOptions,
                                                 pipeline,
                                                 makeQueryPlannerParams);
    if (rankerResult.expressExecutor) {
        return {std::move(rankerResult.expressExecutor)};
    }
    // If we replanned and the old plan and new plan are the same, update the counter.
    if (rankerResult.plannerParams->replanningData && !rankerResult.solutions.empty()) {
        const auto replanningData = rankerResult.plannerParams->replanningData;
        const auto qsn = rankerResult.solutions.at(0).get();
        if (qsn && replanningData->oldPlanHash == qsn->hash()) {
            planCacheCounters.incrementClassicReplannedPlanIsCachedPlanCounter();
        }
    }
    return lowerPlanRankingResult(std::move(canonicalQuery),
                                  std::move(rankerResult),
                                  opCtx,
                                  collections,
                                  yieldPolicy,
                                  pipeline);
}

}  // namespace mongo::exec_deferred_engine_choice
