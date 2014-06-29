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
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameters.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/processinfo.h"
#include "mongo/util/version.h"

namespace {

    using namespace mongo;

    /**
     * Do a depth-first traversal of the tree rooted at 'root', and flatten the tree nodes
     * into the list 'flattened'.
     */
    void flattenStatsTree(PlanStageStats* root, vector<PlanStageStats*>* flattened) {
        flattened->push_back(root);
        for (size_t i = 0; i < root->children.size(); ++i) {
            flattenStatsTree(root->children[i], flattened);
        }
    }

    /**
     * Get a pointer to the MultiPlanStage inside the stage tree rooted at 'root'.
     * Returns NULL if there is no MPS.
     */
    MultiPlanStage* getMultiPlanStage(PlanStage* root) {
        if (root->stageType() == STAGE_MULTI_PLAN) {
            MultiPlanStage* mps = static_cast<MultiPlanStage*>(root);
            return mps;
        }

        vector<PlanStage*> children = root->getChildren();
        for (size_t i = 0; i < children.size(); i++) {
            MultiPlanStage* mps = getMultiPlanStage(children[i]);
            if (mps != NULL) {
                return mps;
            }
        }

        return NULL;
    }

} // namespace

namespace mongo {

    using mongoutils::str::stream;

    MONGO_EXPORT_SERVER_PARAMETER(enableNewExplain, bool, false);

    // static
    void Explain::explainStatsTree(const PlanStageStats& stats,
                                   Explain::Verbosity verbosity,
                                   BSONObjBuilder* bob) {
        invariant(bob);

        // Stage name.
        bob->append("stage", stats.common.stageTypeStr);

        // Display the BSON representation of the filter, if there is one.
        if (!stats.common.filter.isEmpty()) {
            bob->append("filter", stats.common.filter);
        }

        // Some top-level exec stats get pulled out of the root stage.
        if (verbosity >= Explain::EXEC_STATS) {
            bob->appendNumber("nReturned", stats.common.advanced);
            bob->appendNumber("executionTimeMillis", stats.common.executionTimeMillis);
        }

        // Stage-specific stats
        if (STAGE_IXSCAN == stats.stageType) {
            IndexScanStats* spec = static_cast<IndexScanStats*>(stats.specific.get());

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("keysExamined", spec->keysExamined);
            }

            bob->append("keyPattern", spec->keyPattern);
            bob->appendBool("isMultiKey", spec->isMultiKey);
            bob->append("indexBounds", spec->indexBounds);
        }
        else if (STAGE_COLLSCAN == stats.stageType) {
            CollectionScanStats* spec = static_cast<CollectionScanStats*>(stats.specific.get());
            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("docsExamined", spec->docsTested);
            }
        }
        else if (STAGE_COUNT == stats.stageType) {
            CountStats* spec = static_cast<CountStats*>(stats.specific.get());

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("keysExamined", spec->keysExamined);
            }

            bob->append("keyPattern", spec->keyPattern);
            bob->appendBool("isMultiKey", spec->isMultiKey);
        }
        else if (STAGE_FETCH == stats.stageType) {
            FetchStats* spec = static_cast<FetchStats*>(stats.specific.get());
            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("docsExamined", spec->docsExamined);
            }
        }
        else if (STAGE_GEO_NEAR_2D == stats.stageType) {
            TwoDNearStats* spec = static_cast<TwoDNearStats*>(stats.specific.get());

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("keysExamined", spec->nscanned);
                bob->appendNumber("docsExamined", spec->objectsLoaded);
            }

            bob->append("keyPattern", spec->keyPattern);
        }
        else if (STAGE_IDHACK == stats.stageType) {
            IDHackStats* spec = static_cast<IDHackStats*>(stats.specific.get());
            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("keysExamined", spec->keysExamined);
                bob->appendNumber("docsExamined", spec->docsExamined);
            }
        }
        else if (STAGE_TEXT == stats.stageType) {
            TextStats* spec = static_cast<TextStats*>(stats.specific.get());

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("keysExamined", spec->keysExamined);
                bob->appendNumber("docsExamined", spec->fetches);
            }

            bob->append("indexPrefix", spec->indexPrefix);
            bob->append("parsedTextQuery", spec->parsedTextQuery);
        }
        else if (STAGE_SORT == stats.stageType) {
            SortStats* spec = static_cast<SortStats*>(stats.specific.get());
            bob->append("sortPattern", spec->sortPattern);

            if (verbosity >= Explain::EXEC_STATS) {
                bob->appendNumber("memUsage", spec->memUsage);
            }

            if (spec->limit > 0) {
                bob->appendNumber("limitAmount", spec->limit);
            }
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
            explainStatsTree(*stats.children[0], verbosity, &childBob);
            bob->append("inputStage", childBob.obj());
            return;
        }

        // There is more than one child. Recursively explainStatsTree(...) on each
        // of them and add them to the 'inputStages' array.

        BSONArrayBuilder childrenBob(bob->subarrayStart("inputStages"));
        for (size_t i = 0; i < stats.children.size(); ++i) {
            BSONObjBuilder childBob(childrenBob.subobjStart());
            explainStatsTree(*stats.children[i], verbosity, &childBob);
        }
        childrenBob.doneFast();
    }

    // static
    void Explain::generatePlannerInfo(CanonicalQuery* query,
                                      PlanStageStats* winnerStats,
                                      const vector<PlanStageStats*>& rejectedStats,
                                      BSONObjBuilder* out) {
        BSONObjBuilder plannerBob(out->subobjStart("queryPlanner"));;

        plannerBob.append("plannerVersion", QueryPlanner::kPlannerVersion);

        BSONObjBuilder parsedQueryBob(plannerBob.subobjStart("parsedQuery"));
        query->root()->toBSON(&parsedQueryBob);
        parsedQueryBob.doneFast();

        BSONObjBuilder winningPlanBob(plannerBob.subobjStart("winningPlan"));
        explainStatsTree(*winnerStats, Explain::QUERY_PLANNER, &winningPlanBob);
        winningPlanBob.doneFast();

        // Genenerate array of rejected plans.
        BSONArrayBuilder allPlansBob(plannerBob.subarrayStart("rejectedPlans"));
        for (size_t i = 0; i < rejectedStats.size(); i++) {
            BSONObjBuilder childBob(allPlansBob.subobjStart());
            explainStatsTree(*rejectedStats[i], Explain::QUERY_PLANNER, &childBob);
        }
        allPlansBob.doneFast();

        plannerBob.doneFast();
    }

    // static
    void Explain::generateExecStats(PlanStageStats* stats,
                                    BSONObjBuilder* out) {

        out->appendNumber("nReturned", stats->common.advanced);
        out->appendNumber("executionTimeMillis", stats->common.executionTimeMillis);

        // Flatten the stats tree into a list.
        vector<PlanStageStats*> statsNodes;
        flattenStatsTree(stats, &statsNodes);

        // Iterate over all stages in the tree and get the total number of keys/docs examined.
        // These are just aggregations of information already available in the stats tree.
        size_t totalKeysExamined = 0;
        size_t totalDocsExamined = 0;
        for (size_t i = 0; i < statsNodes.size(); ++i) {
            if (STAGE_IXSCAN == statsNodes[i]->stageType) {
                IndexScanStats* spec = static_cast<IndexScanStats*>(statsNodes[i]->specific.get());
                totalKeysExamined += spec->keysExamined;
            }
            else if (STAGE_GEO_NEAR_2D == statsNodes[i]->stageType) {
                TwoDNearStats* spec = static_cast<TwoDNearStats*>(statsNodes[i]->specific.get());
                totalKeysExamined += spec->nscanned;
                totalDocsExamined += spec->objectsLoaded;
            }
            else if (STAGE_IDHACK == statsNodes[i]->stageType) {
                IDHackStats* spec = static_cast<IDHackStats*>(statsNodes[i]->specific.get());
                totalKeysExamined += spec->keysExamined;
                totalDocsExamined += spec->docsExamined;
            }
            else if (STAGE_TEXT == statsNodes[i]->stageType) {
                TextStats* spec = static_cast<TextStats*>(statsNodes[i]->specific.get());
                totalKeysExamined += spec->keysExamined;
                totalDocsExamined += spec->fetches;
            }
            else if (STAGE_FETCH == statsNodes[i]->stageType) {
                FetchStats* spec = static_cast<FetchStats*>(statsNodes[i]->specific.get());
                totalDocsExamined += spec->docsExamined;
            }
            else if (STAGE_COLLSCAN == statsNodes[i]->stageType) {
                CollectionScanStats* spec =
                    static_cast<CollectionScanStats*>(statsNodes[i]->specific.get());
                totalDocsExamined += spec->docsTested;
            }
            else if (STAGE_COUNT == statsNodes[i]->stageType) {
                CountStats* spec = static_cast<CountStats*>(statsNodes[i]->specific.get());
                totalKeysExamined += spec->keysExamined;
            }
        }

        out->appendNumber("totalKeysExamined", totalKeysExamined);
        out->appendNumber("totalDocsExamined", totalDocsExamined);

        // Add the tree of stages, with individual execution stats for each stage.
        BSONObjBuilder stagesBob(out->subobjStart("executionStages"));
        explainStatsTree(*stats, Explain::EXEC_STATS, &stagesBob);
        stagesBob.doneFast();
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
    void Explain::explainCountEmptyQuery(BSONObjBuilder* out) {
        BSONObjBuilder plannerBob(out->subobjStart("queryPlanner"));

        plannerBob.append("plannerVersion", QueryPlanner::kPlannerVersion);

        plannerBob.append("winningPlan", "TRIVIAL_COUNT");

        // Empty array of rejected plans.
        BSONArrayBuilder allPlansBob(plannerBob.subarrayStart("rejectedPlans"));
        allPlansBob.doneFast();

        plannerBob.doneFast();

        generateServerInfo(out);
    }

    // static
    Status Explain::explainStages(PlanExecutor* exec,
                                  CanonicalQuery* canonicalQuery,
                                  Explain::Verbosity verbosity,
                                  BSONObjBuilder* out) {
        //
        // Step 1: run the stages as required by the verbosity level.
        //

        // Inspect the tree to see if there is a MultiPlanStage.
        MultiPlanStage* mps = getMultiPlanStage(exec->getStages());

        // The queryPlanner verbosity level requires that we know the winning plan,
        // if there are multiple. There are multiple candidates iff we have a MultiPlanStage.
        if (verbosity >= Explain::QUERY_PLANNER && NULL != mps) {
            mps->pickBestPlan();
        }

        // The executionStats verbosity level requires that we run the winning plan
        // until if finishes.
        if (verbosity >= Explain::EXEC_STATS) {
            Status s = exec->executePlan();
            if (!s.isOK()) {
                return s;
            }
        }

        // The allPlansExecution verbosity level requires that we run all plans to completion,
        // if there are multiple candidates. If 'mps' is NULL, then there was only one candidate,
        // and we don't have to worry about gathering stats for rejected plans.
        if (verbosity == Explain::EXEC_ALL_PLANS && NULL != mps) {
            Status s = mps->executeAllPlans();
            if (!s.isOK()) {
                return s;
            }
        }

        //
        // Step 2: collect plan stats (which also give the structure of the plan tree).
        //

        // Get stats for the winning plan.
        scoped_ptr<PlanStageStats> winningStats(exec->getStats());

        // Get stats for the rejected plans, if there were rehected plans.
        vector<PlanStageStats*> rejectedStats;
        if (NULL != mps) {
            rejectedStats = mps->generateCandidateStats();
        }

        //
        // Step 3: use the stats trees to produce explain BSON.
        //

        if (verbosity >= Explain::QUERY_PLANNER) {
            generatePlannerInfo(canonicalQuery, winningStats.get(), rejectedStats, out);
        }

        if (verbosity >= Explain::EXEC_STATS) {
            BSONObjBuilder execBob(out->subobjStart("executionStats"));

            // Generate exec stats BSON for the winning plan.
            generateExecStats(winningStats.get(), &execBob);

            // Also generate exec stats for each rejected plan, if the verbosity level
            // is high enough.
            if (verbosity >= Explain::EXEC_ALL_PLANS) {
                BSONArrayBuilder rejectedBob(execBob.subarrayStart("rejectedPlansExecution"));
                for (size_t i = 0; i < rejectedStats.size(); ++i) {
                    BSONObjBuilder planBob(rejectedBob.subobjStart());
                    generateExecStats(rejectedStats[i], &planBob);
                    planBob.doneFast();
                }
                rejectedBob.doneFast();
            }

            execBob.doneFast();
        }

        generateServerInfo(out);

        return Status::OK();
    }

} // namespace mongo
