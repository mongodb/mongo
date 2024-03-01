/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_yield_policy_sbe.h"
#include "mongo/db/query/planner_interface.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/sbe_runtime_planner.h"

namespace mongo {

/**
 * Adapter class to enable SBE runtime planners to work with the common 'PlannerInterface' abstract
 * class.
 */
class SbeRuntimePlanner final : public PlannerInterface {
public:
    SbeRuntimePlanner(
        OperationContext* opCtx,
        const MultipleCollectionAccessor& collections,
        std::unique_ptr<PlanYieldPolicySBE> yieldPolicy,
        QueryPlannerParams plannerParams,
        boost::optional<size_t> cachedPlanHash,
        std::unique_ptr<sbe::RuntimePlanner> runtimePlanner,
        std::vector<std::unique_ptr<QuerySolution>> solutions,
        std::vector<std::pair<std::unique_ptr<sbe::PlanStage>, stage_builder::PlanStageData>> roots,
        std::unique_ptr<RemoteCursorMap> remoteCursors,
        std::unique_ptr<RemoteExplainVector> remoteExplains);

    /**
     * Consumes the canonical query to create the final 'PlanExecutor'. Must be called at most once.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery) override final;

private:
    OperationContext* _opCtx;
    const MultipleCollectionAccessor& _collections;
    std::unique_ptr<PlanYieldPolicySBE> _yieldPolicy;
    QueryPlannerParams _plannerParams;
    sbe::CandidatePlans _candidates;
    boost::optional<size_t> _cachedPlanHash;
    std::unique_ptr<RemoteCursorMap> _remoteCursors;
    std::unique_ptr<RemoteExplainVector> _remoteExplains;
};
}  // namespace mongo
