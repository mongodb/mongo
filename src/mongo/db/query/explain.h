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
#include "mongo/db/exec/sbe/stages/plan_stats.h"
#include "mongo/db/exec/sbe/stages/stages.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

class Collection;
class OperationContext;
class PlanExecutorPipeline;
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
     */
    static void explainStages(PlanExecutor* exec,
                              const Collection* collection,
                              ExplainOptions::Verbosity verbosity,
                              Status executePlanStatus,
                              PlanStageStats* winningPlanTrialStats,
                              BSONObj extraInfo,
                              BSONObjBuilder* out);

    /**
     * Gets explain BSON for the document sources contained by 'exec'. Use this function if you have
     * a PlanExecutor for a pipeline and want to turn it into a human readable explain format.
     *
     * The explain information is generated with the level of detail specified by 'verbosity'.
     */
    static void explainPipelineExecutor(PlanExecutorPipeline* exec,
                                        ExplainOptions::Verbosity verbosity,
                                        BSONObjBuilder* out);

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
    static BSONObj statsToBSON(
        const sbe::PlanStageStats& stats,
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
    static std::string getPlanSummary(const PlanStage* root);
    static std::string getPlanSummary(const sbe::PlanStage* root);

    /**
     * If exec's root stage is a MultiPlanStage, returns the stats for the trial period of of the
     * winning plan. Otherwise, returns nullptr.
     *
     * Must be called _before_ executing the plan with PlanExecutor::getNext()
     * or the PlanExecutor::execute*() methods.
     */
    static std::unique_ptr<PlanStageStats> getWinningPlanTrialStats(PlanExecutor* exec);

    /**
     * Serializes a PlanCacheEntry to the provided BSON object builder. The output format is
     * intended to be human readable, and useful for debugging query performance problems related to
     * the plan cache.
     */
    static void planCacheEntryToBSON(const PlanCacheEntry& entry, BSONObjBuilder* out);

    /**
     * Traverses the tree rooted at 'root', and adds all nodes into the list 'flattened'. If a
     * MultiPlanStage is encountered, only adds the best plan and its children to 'flattened'.
     */
    static void flattenExecTree(const PlanStage* root, std::vector<const PlanStage*>* flattened);

    /**
     * Given the SpecificStats object for a stage and the type of the stage, returns the number of
     * index keys examined by the stage.
     */
    static size_t getKeysExamined(StageType type, const SpecificStats* specific);

    /**
     * Given the SpecificStats object for a stage and the type of the stage, returns the number of
     * documents examined by the stage.
     */
    static size_t getDocsExamined(StageType type, const SpecificStats* specific);

private:
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
     * Adds the "executionStats" field to out. Assumes that the PlanExecutor has already been
     * executed to the point of reaching EOF. Also assumes that verbosity >= kExecStats.
     *
     * If verbosity >= kExecAllPlans, it will include the "allPlansExecution" array.
     *
     * - 'execPlanStatus' is OK if the query was exected successfully, or a non-OK status if there
     *   was a runtime error.
     * - 'winningPlanTrialStats' may be nullptr.
     **/
    static void generateExecutionInfo(PlanExecutor* exec,
                                      ExplainOptions::Verbosity verbosity,
                                      Status executePlanStatus,
                                      PlanStageStats* winningPlanTrialStats,
                                      BSONObjBuilder* out);
};

}  // namespace mongo
