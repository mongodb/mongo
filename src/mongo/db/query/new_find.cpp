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
 */

#include "mongo/db/query/new_find.h"

#include "mongo/db/query/cached_plan_runner.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multi_plan_runner.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/simple_plan_runner.h"
#include "mongo/db/query/stage_builder.h"

namespace mongo {

    Runner* getRunner(Message& m, QueryMessage& q, CurOp& curop, Message &result) {
        // Turn the query into something clean we can work with.
        auto_ptr<CanonicalQuery> canonicalQuery(CanonicalQuery::canonicalize(q));

        if (NULL == canonicalQuery.get()) { return NULL; }

        PlanCache* localCache = PlanCache::get(canonicalQuery->ns());
        CachedSolution* cs = localCache->get(*canonicalQuery);
        if (NULL != cs) {
            // Hand the canonical query and cached solution off to the cached plan runner, which
            // takes ownership of both.
            WorkingSet* ws;
            PlanStage* root;
            verify(StageBuilder::build(*cs->solution, &root, &ws));
            return new CachedPlanRunner(canonicalQuery.release(), cs, root, ws);
        }

        // No entry in cache.  We have to pick a best plan.
        // TODO: Can the cache have negative data?
        vector<QuerySolution*> solutions;
        QueryPlanner::plan(*canonicalQuery, &solutions);

        if (1 == solutions.size()) {
            // Only one possible plan.  Run it.  Cache it as well.  If we only found one solution
            // now, we're only going to find one solution later.
            auto_ptr<PlanRankingDecision> why(new PlanRankingDecision());
            why->onlyOneSolution = true;

            // Build the stages from the solution.
            WorkingSet* ws;
            PlanStage* root;
            verify(StageBuilder::build(*solutions[0], &root, &ws));

            // Cache the solution.  Takes ownership of all arguments.
            localCache->add(canonicalQuery.release(), solutions[0], why.release());

            // And, run the plan.
            return new SimplePlanRunner(ws, root);
        }
        else {
            // Many solutions.  Let the MultiPlanRunner pick the best, update the cache, and so on.
            auto_ptr<MultiPlanRunner> mpr(new MultiPlanRunner(canonicalQuery.release()));
            for (size_t i = 0; i < solutions.size(); ++i) {
                WorkingSet* ws;
                PlanStage* root;
                verify(StageBuilder::build(*solutions[i], &root, &ws));
                // Takes ownership of all arguments.
                mpr->addPlan(solutions[i], root, ws);
            }
            return mpr.release();
        }
    }

    string newRunQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result) {
        auto_ptr<Runner> runner(getRunner(m, q, curop, result));

        if (NULL == runner.get()) {
            // TODO: Complain coherently to the user.
        }

        BSONObj obj;
        while (runner->getNext(&obj)) {
            // TODO: append result to output.
        }

        // TODO: what's this?
        return "";
    }


}  // namespace mongo
