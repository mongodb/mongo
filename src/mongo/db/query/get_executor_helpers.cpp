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

#include "mongo/db/query/get_executor_helpers.h"

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
#include "mongo/db/matcher/extensions_callback_real.h"
#include "mongo/db/pipeline/sbe_pushdown.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/internal_plans.h"
#include "mongo/db/query/planner_analysis.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_utils.h"
#include "mongo/db/query/wildcard_multikey_paths.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/service_context.h"
#include "mongo/db/shard_role/shard_catalog/index_catalog.h"
#include "mongo/db/shard_role/shard_catalog/index_descriptor.h"
#include "mongo/db/storage/record_store.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/update/update_driver.h"
#include "mongo/logv2/log.h"
#include "mongo/util/assert_util.h"


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

void setOpDebugPlanCacheInfo(OperationContext* opCtx, const PlanCacheInfo& cacheInfo) {
    OpDebug& opDebug = CurOp::get(opCtx)->debug();
    if (!opDebug.planCacheShapeHash && cacheInfo.planCacheShapeHash) {
        opDebug.planCacheShapeHash = *cacheInfo.planCacheShapeHash;
    }
    if (!opDebug.planCacheKey && cacheInfo.planCacheKey) {
        opDebug.planCacheKey = *cacheInfo.planCacheKey;
    }
}

std::unique_ptr<PlannerInterface> retryMakePlanner(
    std::unique_ptr<QueryPlannerParams> plannerParams,
    const MakePlannerParamsFn& makeQueryPlannerParams,
    const MakePlannerFn& makePlanner,
    CanonicalQuery* canonicalQuery,
    std::size_t plannerOptions,
    Pipeline* pipeline) {
    static constexpr size_t kMaxIterations = 5;
    for (size_t iter = 0; iter < kMaxIterations; ++iter) {
        try {
            // First try the single collection query parameters, as these would have been
            // generated with query settings if present.
            return makePlanner(std::move(plannerParams));
        } catch (const ExceptionFor<ErrorCodes::NoDistinctScansForDistinctEligibleQuery>&) {
            // The planner failed to generate a DISTINCT_SCAN for a distinct-like query. Remove
            // the distinct property and replan using SBE or subplanning as applicable.
            canonicalQuery->resetDistinct();
            if (canonicalQuery->isSbeCompatible()) {
                // Stages still need to be finalized for SBE since classic was used previously.
                finalizePipelineStages(pipeline, canonicalQuery);
            }
            return makePlanner(makeQueryPlannerParams(*canonicalQuery, plannerOptions));
        } catch (const ExceptionFor<ErrorCodes::NoQueryExecutionPlans>& exception) {
            // The planner failed to generate a viable plan. Remove the query settings and
            // retry if any are present. Otherwise just propagate the exception.
            const auto& querySettings = canonicalQuery->getExpCtx()->getQuerySettings();
            const bool hasQuerySettings = querySettings.getIndexHints().has_value();
            // Planning has been tried without query settings and no execution plan was found.
            const bool ignoreQuerySettings =
                plannerOptions & QueryPlannerParams::IGNORE_QUERY_SETTINGS;
            if (!hasQuerySettings || ignoreQuerySettings) {
                throw;
            }
            LOGV2_DEBUG(8524200,
                        2,
                        "Encountered planning error while running with query settings. Retrying "
                        "without query settings.",
                        "query"_attr = redact(canonicalQuery->toStringForErrorMsg()),
                        "querySettings"_attr = querySettings,
                        "reason"_attr = exception.reason(),
                        "code"_attr = exception.codeString());

            plannerOptions |= QueryPlannerParams::IGNORE_QUERY_SETTINGS;
            // Propagate the params to the next iteration.
            plannerParams = makeQueryPlannerParams(*canonicalQuery, plannerOptions);
        } catch (const ExceptionFor<ErrorCodes::RetryMultiPlanning>&) {
            // Propagate the params to the next iteration.
            plannerParams = makeQueryPlannerParams(*canonicalQuery, plannerOptions);
            canonicalQuery->getExpCtx()->setWasRateLimited(true);
        }
    }
    tasserted(8712800, "Exceeded retry iterations for making a planner");
}

}  // namespace mongo
