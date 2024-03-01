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

#include "mongo/db/query/classic_runtime_planner_for_sbe/planner_interface.h"

#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo::classic_runtime_planner_for_sbe {

CachedPlanner::CachedPlanner(PlannerDataForSBE plannerData,
                             std::unique_ptr<sbe::CachedPlanHolder> cachedPlanHolder)
    : PlannerBase(std::move(plannerData)), _cachedPlanHolder(std::move(cachedPlanHolder)) {}

std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> CachedPlanner::plan() {
    LOGV2_DEBUG(8523404, 5, "Recovering SBE plan from the cache");
    _cachedPlanHolder->cachedPlan->planStageData.debugInfo = _cachedPlanHolder->debugInfo;
    return prepareSbePlanExecutor(nullptr /*solution*/,
                                  {std::move(_cachedPlanHolder->cachedPlan->root),
                                   std::move(_cachedPlanHolder->cachedPlan->planStageData)},
                                  true /*isFromPlanCache*/,
                                  boost::none /*cachedPlanHash*/,
                                  nullptr /*classicRuntimePlannerStage*/);
}
}  // namespace mongo::classic_runtime_planner_for_sbe
