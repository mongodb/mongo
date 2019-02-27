/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

class Collection;
class OperationContext;
struct PlanSummaryStats;

/**
 * Namespace for the collection of static methods used to generate explain information.
 */
class Explain {
public:
    /**
     * Get explain BSON for the execution stages contained by 'exec'. Use this function if you
     * have a PlanExecutor and want to convert it into a human readable explain format. Any
     * operation which has a query component (e.g. find, update, group) can be explained via
     * this function.
     *
     * The explain information is extracted from 'exec' and added to the out-parameter 'out'.
     *
     * The explain information is generated with the level of detail specified by 'verbosity'.
     *
     * The 'extraInfo' parameter specifies additional information to include into the output.
     *
     * Does not take ownership of its arguments.
     *
     * The caller should hold at least an IS lock on the collection the that the query runs on,
     * even if 'collection' is nullptr.
     *
     * If there is an error during the execution of the query, the error message and code are
     * added to the "executionStats" section of the explain.
     */
    static void explainStages(PlanExecutor* exec,
                              const Collection* collection,
                              ExplainOptions::Verbosity verbosity,
                              BSONObj extraInfo,
                              BSONObjBuilder* out);
    /**
     * Adds "queryPlanner" and "executionStats" (if requested in verbosity) fields to 'out'. Unlike
     * the other overload of explainStages() above, this one does not add the "serverInfo" section.
     *
     * - 'exec' is the stage tree for the operation being explained.
     * - 'collection' is the relevant collection. The caller should hold at least an IS lock on the
     * collection which the query ran on, even 'collection' is nullptr.
     * - 'verbosity' is the verbosity level of the explain.
     * - 'extraInfo' specifies additional information to include into the output.
     * - 'executePlanStatus' is the status returned after executing the query (Status::OK if the
     * query wasn't executed).
     * - 'winningPlanTrialStats' is the stats of the winning plan during the trial period. May be
     * nullptr.
     * - 'out' is the builder for the explain output.
     **/
    static void explainStages(PlanExecutor* exec,
                              const Collection* collection,
                              ExplainOptions::Verbosity verbosity,
                              Status executePlanStatus,
                              PlanStageStats* winningPlanTrialStats,
                              BSONObj extraInfo,
                              BSONObjBuilder* out);

    /**
     * Gets explain BSON for the document sources contained by 'exec'. Use this function if you
     * have a PlanExecutor whose root is a PipelineProxyStage and want to turn it into a human
     * readable explain format.
     *
     * The explain information is generated with the level of detail specified by 'verbosity'.
     **/
    static void explainPipelineExecutor(PlanExecutor* exec,
                                        ExplainOptions::Verbosity verbosity,
                                        BSONObjBuilder* out);

    /**
     * Converts the PlanExecutor's winning plan stats tree to BSON and returns to the caller.
     */
    static BSONObj getWinningPlanStats(const PlanExecutor* exec);

    /**
     * Converts the PlanExecutor's winning plan stats tree to BSON and returns the result through
     * the out-parameter 'bob'.
     */
    static void getWinningPlanStats(const PlanExecutor* exec, BSONObjBuilder* bob);

    /**
     * Converts the stats tree 'stats' into a corresponding BSON object containing
     * explain information.
     *
     * Generates the BSON stats at a verbosity specified by 'verbosity'. Defaults
     * to execution stats verbosity.
     */
    static BSONObj statsToBSON(
        const PlanStageStats& stats,
        ExplainOptions::Verbosity verbosity = ExplainOptions::Verbosity::kExecStats);

    /**
     * This version of stats tree to BSON conversion returns the result through the
     * out-parameter 'bob' rather than returning a BSONObj.
     *
     * Generates the BSON stats at a verbosity specified by 'verbosity'. Defaults
     * to execution stats verbosity.
     */
    static void statsToBSON(
        const PlanStageStats& stats,
        BSONObjBuilder* bob,
        ExplainOptions::Verbosity verbosity = ExplainOptions::Verbosity::kExecStats);

    /**
     * Returns a short plan summary std::string describing the leaves of the query plan.
     */
    static std::string getPlanSummary(const PlanExecutor* exec);
    static std::string getPlanSummary(const PlanStage* root);

    /**
     * Fills out 'statsOut' with summary stats using the execution tree contained
     * in 'exec'.
     *
     * The summary stats are consumed by debug mechanisms such as the profiler and
     * the slow query log.
     *
     * This is a lightweight alternative for explainStages(...) above which is useful
     * when operations want to request debug information without doing all the work
     * to generate a full explain.
     *
     * Does not take ownership of its arguments.
     */
    static void getSummaryStats(const PlanExecutor& exec, PlanSummaryStats* statsOut);

    /**
     * If exec's root stage is a MultiPlanStage, returns the stats for the trial period of of the
     * winning plan. Otherwise, returns nullptr.
     *
     * Must be called _before_ calling PlanExecutor::executePlan() or PlanExecutor::getNext().
     **/
    static std::unique_ptr<PlanStageStats> getWinningPlanTrialStats(PlanExecutor* exec);

    /**
     * Generates the execution stats section for the stats tree 'stats', adding the resulting BSON
     * to 'out'.
     *
     * The 'totalTimeMillis' value passed here will be added to the top level of the execution stats
     * section, but will not affect the reporting of timing for individual stages. If
     * 'totalTimeMillis' is not set, we use the approximate timing information collected by the
     * stages.
     *
     * Stats are generated at the verbosity specified by 'verbosity'.
     **/
    static void generateSinglePlanExecutionInfo(const PlanStageStats* stats,
                                                ExplainOptions::Verbosity verbosity,
                                                boost::optional<long long> totalTimeMillis,
                                                BSONObjBuilder* out);

    /**
     * Serializes a PlanCacheEntry to the provided BSON object builder. The output format is
     * intended to be human readable, and useful for debugging query performance problems related to
     * the plan cache.
     */
    static void planCacheEntryToBSON(const PlanCacheEntry& entry, BSONObjBuilder* out);

private:
    /**
     * Adds the 'queryPlanner' explain section to the BSON object being built
     * by 'out'.
     *
     * This is a helper for generating explain BSON. It is used by explainStages(...).
     *
     * - 'exec' is the stage tree for the operation being explained.
     * - 'collection' is the collection used in the operation. The caller should hold an IS lock on
     * the collection which the query is for, even if 'collection' is nullptr.
     * - 'extraInfo' specifies additional information to include into the output.
     * - 'out' is a builder for the explain output.
     */
    static void generatePlannerInfo(PlanExecutor* exec,
                                    const Collection* collection,
                                    BSONObj extraInfo,
                                    BSONObjBuilder* out);

    /**
     * Private helper that does the heavy-lifting for the public statsToBSON(...) functions
     * declared above.
     *
     * Not used except as a helper to the public statsToBSON(...) functions.
     */
    static void statsToBSON(const PlanStageStats& stats,
                            ExplainOptions::Verbosity verbosity,
                            BSONObjBuilder* bob,
                            BSONObjBuilder* topLevelBob);

    /**
     * Adds the "executionStats" field to out. Assumes PlanExecutor::executePlan() has been called
     * and that verbosity >= kExecStats.
     *
     * If verbosity >= kExecAllPlans, it will include the "allPlansExecution" array.
     *
     * - 'execPlanStatus' is the value returned after executing the query.
     * - 'winningPlanTrialStats' may be nullptr.
     **/
    static void generateExecutionInfo(PlanExecutor* exec,
                                      ExplainOptions::Verbosity verbosity,
                                      Status executePlanStatus,
                                      PlanStageStats* winningPlanTrialStats,
                                      BSONObjBuilder* out);

    /**
     * Adds the 'serverInfo' explain section to the BSON object being build
     * by 'out'.
     *
     * This is a helper for generating explain BSON. It is used by explainStages(...).
     */
    static void generateServerInfo(BSONObjBuilder* out);
};

}  // namespace
