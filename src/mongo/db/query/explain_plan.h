/**
 *    Copyright (C) 2013 10gen Inc.
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

#include "mongo/base/status.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/query/query_solution.h"

namespace mongo {

    class TypeExplain;
    struct PlanInfo;

    /**
     * Returns OK, allocating and filling in '*explainOut' describing the access paths used in
     * the 'stats' tree of a given query solution. The caller has the ownership of
     * '*explainOut', on success. Otherwise return an error status describing the problem.
     *
     * If 'fullDetails' was requested, the explain will return all available information about
     * the plan, otherwise, just a summary. The fields in the summary are: 'cursor', 'n',
     * 'nscannedObjects', 'nscanned', and 'indexBounds'. The remaining fields are: 'isMultKey',
     * 'nscannedObjectsAllPlans', 'nscannedAllPlans', 'scanAndOrder', 'indexOnly', 'nYields',
     * 'nChunkSkips', 'millis', 'allPlans', and 'oldPlan'.
     *
     * All these fields are documented in type_explain.h
     *
     * TODO: Currently, only working for single-leaf plans.
     */
    Status explainPlan(const PlanStageStats& stats, TypeExplain** explainOut, bool fullDetails);

    /**
     * Returns OK, allocating and filling in '*explain' with details of
     * the "winner" plan. Caller takes ownership of '*explain'. Otherwise,
     * return a status describing the error.
     *
     * 'bestStats', 'candidateStats' and 'solution' are used to fill in '*explain'.
     * Used by both MultiPlanRunner and CachedPlanRunner.
     */
    Status explainMultiPlan(const PlanStageStats& stats,
                            const std::vector<PlanStageStats*>& candidateStats,
                            QuerySolution* solution,
                            TypeExplain** explain);

    /**
     * Returns a short plan summary std::string describing the leaves of the query solution.
     *
     * Used for logging.
     */
    std::string getPlanSummary(const QuerySolution& soln);

    /**
     * If the out-parameter 'info' is non-null, fills in '*infoOut' with information
     * from the query solution tree 'soln' that can be determined before the query is done
     * running. Whereas 'explainPlan(...)' above is for collecting runtime debug information,
     * this function is for collecting static debug information that is known prior
     * to query runtime.
     *
     * The caller is responsible for deleting '*infoOut'.
     */
    void getPlanInfo(const QuerySolution& soln, PlanInfo** infoOut);

    void statsToBSON(const PlanStageStats& stats,
                     BSONObjBuilder* bob,
                     BSONObjBuilder* topLevelBob);

    void statsToBSON(const PlanStageStats& stats, BSONObjBuilder* bob);

    BSONObj statsToBSON(const PlanStageStats& stats);

} // namespace mongo
