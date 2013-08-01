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

#include "mongo/db/commands.h"
#include "mongo/db/query/cached_plan_runner.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/multi_plan_runner.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/simple_plan_runner.h"
#include "mongo/db/query/stage_builder.h"

namespace mongo {

    // Used in db/ops/query.cpp.
    bool useNewQuerySystem = false;

    /**
     * For a given query, get a runner.
     * The runner could be a SimplePlanRunner, a CachedQueryRunner, or a MultiPlanRunner, depending
     * on the cache/query solver/etc.
     */
    Status getRunner(QueryMessage& q, Runner** out) {
        CanonicalQuery* rawCanonicalQuery = NULL;

        // Canonicalize the query and wrap it in an auto_ptr so we don't leak it if something goes
        // wrong.
        Status status = CanonicalQuery::canonicalize(q, &rawCanonicalQuery);
        if (!status.isOK()) { return status; }
        verify(rawCanonicalQuery);
        auto_ptr<CanonicalQuery> canonicalQuery(rawCanonicalQuery);

        // Try to look up a cached solution for the query.
        // TODO: Can the cache have negative data about a solution?
        PlanCache* localCache = PlanCache::get(canonicalQuery->ns());
        CachedSolution* cs = localCache->get(*canonicalQuery);
        if (NULL != cs) {
            // We have a cached solution.  Hand the canonical query and cached solution off to the
            // cached plan runner, which takes ownership of both.
            WorkingSet* ws;
            PlanStage* root;
            verify(StageBuilder::build(*cs->solution, &root, &ws));
            *out = new CachedPlanRunner(canonicalQuery.release(), cs, root, ws);
            return Status::OK();
        }

        // No entry in cache for the query.  We have to solve the query ourself.
        vector<QuerySolution*> solutions;
        QueryPlanner::plan(*canonicalQuery, &solutions);

        // We cannot figure out how to answer the query.  Should this ever happen?
        if (0 == solutions.size()) {
            return Status(ErrorCodes::BadValue, "Can't create a plan for the canonical query " +
                                                 canonicalQuery->toString());
        }

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
            *out = new SimplePlanRunner(ws, root);
            return Status::OK();
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
            *out = mpr.release();
            return Status::OK();
        }
    }

    /**
     * This is called by db/ops/query.cpp.  This is the entry point for answering a query.
     */
    string newRunQuery(Message& m, QueryMessage& q, CurOp& curop, Message &result) {
        // This is a read lock.
        Client::ReadContext ctx(q.ns, dbpath);

        // Parse, canonicalize, plan, transcribe, and get a runner.
        Runner* rawRunner;
        Status status = getRunner(q, &rawRunner);
        if (!status.isOK()) {
            uasserted(17007, "Couldn't process query " + q.query.toString()
                         + " why: " + status.reason());
        }
        verify(NULL != rawRunner);
        auto_ptr<Runner> runner(rawRunner);

        // Run the query.
        BufBuilder bb(32768);
        bb.skip(sizeof(QueryResult));
        int numResults = 0;

        // Yielding is handled within the runner, as is page faulting.
        // TODO: pay attention to numWanted.
        BSONObj obj;
        while (runner->getNext(&obj)) {
            bb.appendBuf((void*)obj.objdata(), obj.objsize());
            ++numResults;
        }

        // Add the results from the query into the output buffer.
        result.appendData(bb.buf(), bb.len());
        bb.decouple();

        // Fill out the output buffer's header.
        QueryResult* qr = static_cast<QueryResult*>(result.header());
        // TODO: Figure out how to make this work with a clientcursor
        qr->cursorId = 0;
        qr->setResultFlagsToOk();
        qr->setOperation(opReply);
        qr->startingFrom = 0;
        qr->nReturned = numResults;
        // TODO: set curop.debug() fields.

        // TODO: what's this?  we either return the ns or we return "".
        return "";
    }

}  // namespace mongo
