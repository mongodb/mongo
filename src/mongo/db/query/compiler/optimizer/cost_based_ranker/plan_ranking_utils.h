// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/util/modules.h"

#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace plan_ranking_tests {

struct PlanningTimeProfile {
    double sampleGenerationTimeMS;
    double planRankingTimeMS;
};

/**
 * Use the MultiPlanRunner to pick the best plan for the query 'cq'.  Goes through
 * normal planning to generate solutions and feeds them to the 'MultiPlanStage'.
 *
 * Does NOT take ownership of 'cq'.  Caller DOES NOT own the returned QuerySolution*.
 */
const QuerySolution* pickBestPlan(CanonicalQuery* cq,
                                  OperationContext& opCtx,
                                  boost::intrusive_ptr<ExpressionContext> expCtx,
                                  std::unique_ptr<MultiPlanStage>& mps,
                                  NamespaceString nss);

/**
 * Use the cost-based ranker (CBR) to find an optimal plan for the query 'cq'.
 * It is expected that the tests using this function ensure that CBR always
 * finds an optimal plan. In this case the method returns a pointer tp that
 * optimal plan.
 */
const QuerySolution* bestCBRPlan(CanonicalQuery* cq,
                                 size_t numDocs,
                                 OperationContext& opCtx,
                                 int sampleSize,
                                 std::vector<std::unique_ptr<QuerySolution>>& bestCBRPlan,
                                 NamespaceString nss,
                                 SamplingCEMethodEnum samplingStyle = SamplingCEMethodEnum::kRandom,
                                 boost::optional<int> numChunks = boost::none,
                                 boost::optional<PlanningTimeProfile&> times = boost::none);

}  // namespace plan_ranking_tests
}  // namespace mongo
