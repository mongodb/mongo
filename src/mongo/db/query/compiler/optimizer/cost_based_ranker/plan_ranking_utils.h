/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/client.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/classic/multi_plan.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/ce/ce_common.h"
#include "mongo/db/query/compiler/ce/sampling/sampling_estimator_impl.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {

namespace plan_ranking_tests {

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
                                 ce::SamplingEstimatorImpl::SamplingStyle samplingStyle =
                                     ce::SamplingEstimatorImpl::SamplingStyle::kRandom,
                                 boost::optional<int> numChunks = boost::none);

}  // namespace plan_ranking_tests
}  // namespace mongo
