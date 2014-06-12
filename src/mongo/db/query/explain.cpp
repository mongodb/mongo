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

#include "mongo/platform/basic.h"

#include "mongo/db/query/explain.h"

#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/version.h"

namespace mongo {

    using mongoutils::str::stream;

    MONGO_EXPORT_SERVER_PARAMETER(enableNewExplain, bool, false);

    // static
    void Explain::explainTree(const PlanStageStats& stats,
                              Explain::Verbosity verbosity,
                              BSONObjBuilder* bob) {
        invariant(bob);

        // Stage name.
        bob->append("stage", stats.common.stageTypeStr);

        if (!stats.common.filter.isEmpty()) {
            bob->append("filter", stats.common.filter);
        }

        // Stage-specific stats
        if (STAGE_IXSCAN == stats.stageType) {
            IndexScanStats* spec = static_cast<IndexScanStats*>(stats.specific.get());
            bob->append("keyPattern", spec->keyPattern);
            bob->appendBool("isMultiKey", spec->isMultiKey);
            bob->append("indexBounds", spec->indexBounds);
        }
        else if (STAGE_GEO_NEAR_2D == stats.stageType) {
            TwoDNearStats* spec = static_cast<TwoDNearStats*>(stats.specific.get());
            bob->append("keyPattern", spec->keyPattern);

            // TODO these things are execution stats
            /*bob->appendNumber("keysExamined", spec->nscanned);
            bob->appendNumber("objectsLoaded", spec->objectsLoaded);*/
        }
        else if (STAGE_TEXT == stats.stageType) {
            TextStats* spec = static_cast<TextStats*>(stats.specific.get());
            bob->append("indexPrefix", spec->indexPrefix);
            bob->append("parsedTextQuery", spec->parsedTextQuery);

            // TODO these things are execution stats
            /*bob->appendNumber("keysExamined", spec->keysExamined);
            bob->appendNumber("fetches", spec->fetches);*/
        }
        else if (STAGE_SORT == stats.stageType) {
            SortStats* spec = static_cast<SortStats*>(stats.specific.get());
            bob->append("sortPattern", spec->sortPattern);
            if (spec->limit > 0) {
                bob->appendNumber("limitAmount", spec->limit);
            }
            bob->appendNumber("memUsage", spec->memUsage);
        }
        else if (STAGE_SORT_MERGE == stats.stageType) {
            MergeSortStats* spec = static_cast<MergeSortStats*>(stats.specific.get());
            bob->append("sortPattern", spec->sortPattern);
        }
        else if (STAGE_PROJECTION == stats.stageType) {
            ProjectionStats* spec = static_cast<ProjectionStats*>(stats.specific.get());
            bob->append("transformBy", spec->projObj);
        }
        else if (STAGE_SKIP == stats.stageType) {
            SkipStats* spec = static_cast<SkipStats*>(stats.specific.get());
            bob->appendNumber("skipAmount", spec->skip);
        }
        else if (STAGE_LIMIT == stats.stageType) {
            LimitStats* spec = static_cast<LimitStats*>(stats.specific.get());
            bob->appendNumber("limitAmount", spec->limit);
        }

        // We're done if there are no children.
        if (stats.children.empty()) {
            return;
        }

        // If there's just one child (a common scenario), avoid making an array. This makes
        // the output more readable by saving a level of nesting. Name the field 'inputStage'
        // rather than 'inputStages'.
        if (1 == stats.children.size()) {
            BSONObjBuilder childBob;
            explainTree(*stats.children[0], verbosity, &childBob);
            bob->append("inputStage", childBob.obj());
            return;
        }

        // There is more than one child. Recursively explainTree(...) on each
        // of them and add them to the 'inputStages' array.

        BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"));
        for (size_t i = 0; i < stats.children.size(); ++i) {
            BSONObjBuilder childBob(childrenBob.subobjStart());
            explainTree(*stats.children[i], verbosity, &childBob);
        }
        childrenBob.doneFast();
    }

    // static
    void Explain::generatePlannerInfo(CanonicalQuery* query,
                                      PlanStageStats* winnerStats,
                                      vector<PlanStageStats*>& rejectedStats,
                                      BSONObjBuilder* out) {
        BSONObjBuilder plannerBob(out->subobjStart("queryPlanner"));;

        plannerBob.append("plannerVersion", QueryPlanner::kPlannerVersion);

        BSONObjBuilder parsedQueryBob(plannerBob.subobjStart("parsedQuery"));
        query->root()->toBSON(&parsedQueryBob);
        parsedQueryBob.doneFast();

        BSONObjBuilder winningPlanBob(plannerBob.subobjStart("winningPlan"));
        explainTree(*winnerStats, Explain::QUERY_PLANNER, &winningPlanBob);
        winningPlanBob.doneFast();

        // Genenerate array of rejected plans.
        BSONArrayBuilder allPlansBob(plannerBob.subarrayStart("rejectedPlans"));
        for (size_t i = 0; i < rejectedStats.size(); i++) {
            BSONObjBuilder childBob(allPlansBob.subobjStart());
            explainTree(*rejectedStats.at(i), Explain::QUERY_PLANNER, &childBob);
        }
        allPlansBob.doneFast();

        plannerBob.doneFast();
    }

    // static
    void Explain::generateServerInfo(BSONObjBuilder* out) {
        BSONObjBuilder serverBob(out->subobjStart("serverInfo"));
        out->append("host", getHostNameCached());
        out->appendNumber("port", serverGlobalParams.port);
        out->append("version", versionString);
        out->append("gitVersion", gitVersion());

        ProcessInfo p;
        BSONObjBuilder bOs;
        bOs.append("type", p.getOsType());
        bOs.append("name", p.getOsName());
        bOs.append("version", p.getOsVersion());
        serverBob.append(StringData("os"), bOs.obj());

        serverBob.doneFast();
    }

    // static
    Status Explain::explainSinglePlan(Collection* collection,
                                      CanonicalQuery* rawCanonicalQuery,
                                      QuerySolution* solution,
                                      Explain::Verbosity verbosity,
                                      BSONObjBuilder* out) {
        // Only one possible plan. Build the stages from the solution.
        WorkingSet* ws = new WorkingSet();
        PlanStage* root;
        verify(StageBuilder::build(collection, *solution, ws, &root));

        // Wrap the exec stages in a plan executor. Takes ownership of 'ws' and 'root'.
        scoped_ptr<PlanExecutor> exec(new PlanExecutor(ws, root, collection));

        // If we need execution stats, then we should run the plan.
        if (verbosity > Explain::QUERY_PLANNER) {
            Runner::RunnerState state;
            BSONObj obj;
            while (Runner::RUNNER_ADVANCED == (state = exec->getNext(&obj, NULL)));

            if (Runner::RUNNER_ERROR == state) {
                return Status(ErrorCodes::BadValue,
                              "Exec error: " + WorkingSetCommon::toStatusString(obj));
            }
        }

        scoped_ptr<PlanStageStats> stats(exec->getStats());

        // Actually generate the explain results.

        if (verbosity >= Explain::QUERY_PLANNER) {
            vector<PlanStageStats*> rejected;
            generatePlannerInfo(rawCanonicalQuery, stats.get(), rejected, out);
            generateServerInfo(out);
        }

        if (verbosity >= Explain::EXEC_STATS) {
            // TODO: generate executionStats section
        }

        if (verbosity >= Explain::EXEC_ALL_PLANS) {
            // TODO: generate rejected plans execution stats
        }

        return Status::OK();
    }

    // static
    Status Explain::explainMultiPlan(Collection* collection,
                                     CanonicalQuery* rawCanonicalQuery,
                                     vector<QuerySolution*>& solutions,
                                     Explain::Verbosity verbosity,
                                     BSONObjBuilder* out) {
        scoped_ptr<WorkingSet> sharedWorkingSet(new WorkingSet());

        scoped_ptr<MultiPlanStage> multiPlanStage(
            new MultiPlanStage(collection, rawCanonicalQuery));

        for (size_t ix = 0; ix < solutions.size(); ++ix) {
            // version of StageBuild::build when WorkingSet is shared
            PlanStage* nextPlanRoot;
            verify(StageBuilder::build(collection, *solutions[ix],
                                       sharedWorkingSet.get(), &nextPlanRoot));

            // Takes ownership of the solution and the root PlanStage, but not the working set.
            multiPlanStage->addPlan(solutions[ix], nextPlanRoot, sharedWorkingSet.get());
        }

        // Run the plan / do the plan selection as required by the requested verbosity.
        multiPlanStage->pickBestPlan();
        if (Explain::EXEC_STATS == verbosity) {
            Status execStatus = multiPlanStage->executeWinningPlan();
            if (!execStatus.isOK()) {
                return execStatus;
            }
        }
        else if (Explain::EXEC_ALL_PLANS == verbosity) {
            Status execStatus = multiPlanStage->executeAllPlans();
            if (!execStatus.isOK()) {
                return execStatus;
            }
        }

        // Get stats for the winning plan.
        scoped_ptr<PlanStageStats> stats(multiPlanStage->getStats());

        // Actually generate the explain results.

        if (verbosity >= Explain::QUERY_PLANNER) {
            vector<PlanStageStats*>* rejected = multiPlanStage->generateCandidateStats();
            generatePlannerInfo(rawCanonicalQuery, stats.get(), *rejected, out);
            generateServerInfo(out);
        }

        if (verbosity >= Explain::EXEC_STATS) {
            // TODO: generate executionStats section
        }

        if (verbosity >= Explain::EXEC_ALL_PLANS) {
            // TODO: generate rejected plans execution stats
        }

        return Status::OK();
    }

    // static
    void Explain::explainEmptyColl(CanonicalQuery* rawCanonicalQuery,
                                   BSONObjBuilder* out) {
        BSONObjBuilder plannerBob(out->subobjStart("queryPlanner"));

        plannerBob.append("plannerVersion", QueryPlanner::kPlannerVersion);

        BSONObjBuilder parsedQueryBob(plannerBob.subobjStart("parsedQuery"));
        rawCanonicalQuery->root()->toBSON(&parsedQueryBob);
        parsedQueryBob.doneFast();

        plannerBob.appendBool("emptyCollection", true);

        plannerBob.append("winningPlan", "EOF");

        // Empty array of rejected plans.
        BSONArrayBuilder allPlansBob(plannerBob.subarrayStart("rejectedPlans"));
        allPlansBob.doneFast();

        plannerBob.doneFast();

        generateServerInfo(out);
    }

    // static
    Status Explain::explain(Collection* collection,
                            CanonicalQuery* rawCanonicalQuery,
                            size_t plannerOptions,
                            Explain::Verbosity verbosity,
                            BSONObjBuilder* out) {
        invariant(rawCanonicalQuery);
        auto_ptr<CanonicalQuery> canonicalQuery(rawCanonicalQuery);

        if (NULL == collection) {
            explainEmptyColl(rawCanonicalQuery, out);
            return Status::OK();
        }

        QueryPlannerParams plannerParams;
        plannerParams.options = plannerOptions;
        fillOutPlannerParams(collection, rawCanonicalQuery, &plannerParams);

        vector<QuerySolution*> solutions;
        Status status = QueryPlanner::plan(*canonicalQuery, plannerParams, &solutions);
        if (!status.isOK()) {
            return Status(ErrorCodes::BadValue,
                          "error processing explain: " + canonicalQuery->toString() +
                          " planner returned error: " + status.reason());
        }

        // We cannot figure out how to answer the query.  Perhaps it requires an index
        // we do not have?
        if (0 == solutions.size()) {
            stream ss;
            ss << "error processing explain: " << canonicalQuery->toString()
               << " No query solutions";
            return Status(ErrorCodes::BadValue, ss);
        }
        else if (1 == solutions.size()) {
            return explainSinglePlan(collection, rawCanonicalQuery, solutions[0], verbosity, out);
        }
        else {
            return explainMultiPlan(collection, rawCanonicalQuery, solutions, verbosity, out);
        }
    }

} // namespace mongo
