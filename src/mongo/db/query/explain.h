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

namespace mongo {

    class Collection;

    // Temporarily hide the new explain implementation behind a setParameter.
    // TODO: take this out, and make the new implementation the default.
    extern bool enableNewExplain;

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

            // At this highest verbosity level, we generate the execution stats for each rejected
            // plan as well as the winning plan. String alias is "allPlansExecution".
            EXEC_ALL_PLANS = 2
        };

        /**
         * Adds the 'queryPlanner' explain section to the BSON object being built
         * by 'out'.
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
         */
        static void generateExecStats(PlanStageStats* stats,
                                      BSONObjBuilder* out);

        /**
         * Adds the 'serverInfo' explain section to the BSON object being build
         * by 'out'.
         */
        static void generateServerInfo(BSONObjBuilder* out);

        /**
         * Converts the stats tree 'stats' into a corresponding BSON object containing
         * explain information.
         *
         * Explain info is added to 'bob' according to the verbosity level passed in
         * 'verbosity'.
         */
        static void explainStatsTree(const PlanStageStats& stats,
                                     Explain::Verbosity verbosity,
                                     BSONObjBuilder* bob);

        /**
         * Generate explain info for the execution plan 'exec', adding the results
         * to the BSONObj being built by 'out'.
         *
         * The query part of the operation is contained in 'canonicalQuery', but
         * 'exec' can contain any tree of execution stages. We can explain any
         * operation that executes as stages by calling into this function.
         *
         * The explain information is generated according with a level of detail
         * specified by 'verbosity'.
         */
        static Status explainStages(PlanExecutor* exec,
                                    CanonicalQuery* canonicalQuery,
                                    Explain::Verbosity verbosity,
                                    BSONObjBuilder* out);

        //
        // Helpers for special-case explains.
        //

        /**
         * If you have an empty query with a count, then there are no execution stages.
         * We just get the number of records and then apply skip/limit. Since there
         * are no stages, this requires a special explain format.
         */
        static void explainCountEmptyQuery(BSONObjBuilder* out);

    };

} // namespace
