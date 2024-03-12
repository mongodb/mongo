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

#include "mongo/db/operation_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner_params.h"

namespace mongo {

/**
 * Data that any runtime planner needs to perform the planning.
 */
struct PlannerData {
    PlannerData(OperationContext* opCtx,
                CanonicalQuery* cq,
                std::unique_ptr<WorkingSet> workingSet,
                const MultipleCollectionAccessor& collections,
                QueryPlannerParams plannerParams,
                PlanYieldPolicy::YieldPolicy yieldPolicy,
                boost::optional<size_t> cachedPlanHash)
        : opCtx(opCtx),
          cq(cq),
          workingSet(std::move(workingSet)),
          collections(collections),
          plannerParams(std::move(plannerParams)),
          yieldPolicy(yieldPolicy),
          cachedPlanHash(cachedPlanHash) {}

    PlannerData(const PlannerData&) = delete;
    PlannerData& operator=(const PlannerData&) = delete;
    PlannerData(PlannerData&&) = default;
    PlannerData& operator=(PlannerData&&) = default;

    virtual ~PlannerData() = default;

    OperationContext* opCtx;
    CanonicalQuery* cq;
    std::unique_ptr<WorkingSet> workingSet;
    const MultipleCollectionAccessor& collections;
    QueryPlannerParams plannerParams;
    PlanYieldPolicy::YieldPolicy yieldPolicy;
    boost::optional<size_t> cachedPlanHash;
};

/*
 *  Common interface for planner implementations.
 */
class PlannerInterface {
public:
    virtual ~PlannerInterface() = default;

    /**
     * Function that creates a PlanExecutor for the selected plan. Can be called only once, as it
     * may transfer ownership of some data to returned PlanExecutor.
     */
    virtual std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> makeExecutor(
        std::unique_ptr<CanonicalQuery> canonicalQuery) = 0;
};
}  // namespace mongo
