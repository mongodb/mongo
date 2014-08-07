/**
 *    Copyright (C) 2013-2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
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
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/type_explain.h"

namespace mongo {

    class Collection;
    class OperationContext;

    // Temporarily hide the new explain implementation behind a setParameter.
    // TODO: take this out, and make the new implementation the default.
    extern bool enableNewExplain;

    /**
     * A container for the summary statistics that the profiler, slow query log, and
     * other non-explain debug mechanisms may want to collect.
     */
    struct PlanSummaryStats {

        PlanSummaryStats() : nReturned(0),
                             totalKeysExamined(0),
                             totalDocsExamined(0),
                             isIdhack(false),
                             hasSortStage(false),
                             summaryStr("") { }

        // The number of results returned by the plan.
        size_t nReturned;

        // The total number of index keys examined by the plan.
        size_t totalKeysExamined;

        // The total number of documents examined by the plan.
        size_t totalDocsExamined;

        // The number of milliseconds spent inside the root stage's work() method.
        long long executionTimeMillis;

        // Did this plan use the fast path for key-value retrievals on the _id index?
        bool isIdhack;

        // Did this plan use an in-memory sort stage?
        bool hasSortStage;

        // A string summarizing the plan selected.
        std::string summaryStr;
    };

    /**
     * Namespace for the collection of static methods used to generate explain information.
     */
    class Explain {
    public:
        /**
         * The various supported verbosity levels for explain. The order is
         * significant: the enum values are assigned in order of increasing verbosity.
         */
        enum Verbosity {
            // At all verbosities greater than or equal to QUERY_PLANNER, we display information
            // about the plan selected and alternate rejected plans. Does not include any execution-
            // related info. String alias is "queryPlanner".
            QUERY_PLANNER = 0,

            // At all verbosities greater than or equal to EXEC_STATS, we display a section of
            // output containing both overall execution stats, and stats per stage in the
            // execution tree. String alias is "execStats".
            EXEC_STATS = 1,

            // At this second-highest verbosity level, we generate the execution stats for each
            // rejected plan as well as the winning plan. String alias is "allPlansExecution".
            EXEC_ALL_PLANS = 2,

            // This is the highest verbosity level. It has the same behavior as EXEC_ALL_PLANS,
            // except it includes more detailed stats. String alias is "full".
            //
            // The FULL verbosity level is used to generate detailed debug information for the
            // plan cache and for logging. It includes metrics like "works", "isEOF", and "advanced"
            // that are omitted at lesser verbosities.
            FULL = 3,
        };

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
         * Does not take ownership of its arguments.
         */
        static Status explainStages(PlanExecutor* exec,
                                    Explain::Verbosity verbosity,
                                    BSONObjBuilder* out);

        /**
         * Converts the stats tree 'stats' into a corresponding BSON object containing
         * explain information.
         *
         * Generates the BSON stats at a verbosity specified by 'verbosity'. Defaults
         * to the highest verbosity (FULL).
         */
        static BSONObj statsToBSON(const PlanStageStats& stats,
                                   Explain::Verbosity verbosity = FULL);

        /**
         * This version of stats tree to BSON conversion returns the result through the
         * out-parameter 'bob' rather than returning a BSONObj.
         *
         * Generates the BSON stats at a verbosity specified by 'verbosity'. Defaults
         * to the highest verbosity (FULL).
         */
        static void statsToBSON(const PlanStageStats& stats,
                                BSONObjBuilder* bob,
                                Explain::Verbosity verbosity = FULL);

        /**
         * Returns a short plan summary std::string describing the leaves of the query plan.
         */
        static std::string getPlanSummary(PlanStage* root);

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
        static void getSummaryStats(PlanExecutor* exec, PlanSummaryStats* statsOut);

        //
        // Helpers for special-case explains.
        //

        /**
         * If you have an empty query with a count, then there are no execution stages.
         * We just get the number of records and then apply skip/limit. Since there
         * are no stages, this requires a special explain format.
         */
        static void explainCountEmptyQuery(BSONObjBuilder* out);

        /**
         * Generate the legacy explain format from a PlanExecutor.
         *
         * On success, the caller owns 'explain'.
         *
         * TODO: THIS IS TEMPORARY. Once the legacy explain code is deleted, we won't
         * need this anymore.
         */
        static Status legacyExplain(PlanExecutor* exec, TypeExplain** explain);

    private:
        /**
         * Private helper that does the heavy-lifting for the public statsToBSON(...) functions
         * declared above.
         *
         * Not used except as a helper to the public statsToBSON(...) functions.
         */
        static void statsToBSON(const PlanStageStats& stats,
                                Explain::Verbosity verbosity,
                                BSONObjBuilder* bob,
                                BSONObjBuilder* topLevelBob);

        /**
         * Adds the 'queryPlanner' explain section to the BSON object being built
         * by 'out'.
         *
         * This is a helper for generating explain BSON. It is used by explainStages(...).
         *
         * @param query -- the query part of the operation being explained.
         * @param winnerStats -- the stats tree for the winning plan.
         * @param rejectedStats -- an array of stats trees, one per rejected plan
         */
        static void generatePlannerInfo(CanonicalQuery* query,
                                        PlanStageStats* winnerStats,
                                        const vector<PlanStageStats*>& rejectedStats,
                                        BSONObjBuilder* out);

        /**
         * Generates the execution stats section for the stats tree 'stats',
         * adding the resulting BSON to 'out'.
         *
         * Stats are generated at the verbosity specified by 'verbosity'.
         *
         * This is a helper for generating explain BSON. It is used by explainStages(...).
         */
        static void generateExecStats(PlanStageStats* stats,
                                      Explain::Verbosity verbosity,
                                      BSONObjBuilder* out);

        /**
         * Adds the 'serverInfo' explain section to the BSON object being build
         * by 'out'.
         *
         * This is a helper for generating explain BSON. It is used by explainStages(...).
         */
        static void generateServerInfo(BSONObjBuilder* out);

    };

} // namespace
