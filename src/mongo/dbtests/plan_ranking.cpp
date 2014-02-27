/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

/**
 * This file tests db/query/plan_ranker.cpp and db/query/multi_plan_runner.cpp.
 */

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/instance.h"
#include "mongo/db/json.h"
#include "mongo/db/query/multi_plan_runner.h"
#include "mongo/db/query/get_runner.h"
#include "mongo/db/query/qlog.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/dbtests/dbtests.h"

namespace mongo {

    // How we access the external setParameter testing bool.
    extern bool forceIntersectionPlans;

}  // namespace mongo

namespace PlanRankingTests {

    static const char* ns = "unittests.PlanRankingTests";

    class PlanRankingTestBase {
    public:
        PlanRankingTestBase() {
            Client::WriteContext ctx(ns);
            _client.dropCollection(ns);
        }

        void insert(const BSONObj& obj) {
            Client::WriteContext ctx(ns);
            _client.insert(ns, obj);
        }

        void addIndex(const BSONObj& obj) {
            Client::WriteContext ctx(ns);
            _client.ensureIndex(ns, obj);
        }

        /**
         * Use the MultiPlanRunner to pick the best plan for the query 'cq'.  Goes through
         * normal planning to generate solutions and feeds them to the MPR.
         *
         * Takes ownership of 'cq'.  Caller DOES NOT own the returned QuerySolution*.
         */
        QuerySolution* pickBestPlan(CanonicalQuery* cq) {
            Client::ReadContext ctx(ns);
            Collection* collection = ctx.ctx().db()->getCollection(ns);

            QueryPlannerParams plannerParams;
            fillOutPlannerParams(collection, cq, &plannerParams);
            // Turn this off otherwise it pops up in some plans.
            plannerParams.options &= ~QueryPlannerParams::KEEP_MUTATIONS;

            // Fill out the available indices.
            IndexCatalog::IndexIterator ii = collection->getIndexCatalog()->getIndexIterator(false);
            while (ii.more()) {
                const IndexDescriptor* desc = ii.next();
                plannerParams.indices.push_back(IndexEntry(desc->keyPattern(),
                                                           desc->getAccessMethodName(),
                                                           desc->isMultikey(),
                                                           desc->isSparse(),
                                                           desc->indexName(),
                                                           desc->infoObj()));
            }

            // Plan.
            vector<QuerySolution*> solutions;
            Status status = QueryPlanner::plan(*cq, plannerParams, &solutions);
            ASSERT(status.isOK());

            ASSERT_GREATER_THAN_OR_EQUALS(solutions.size(), 1U);

            // Fill out the MPR.
            _mpr.reset(new MultiPlanRunner(collection, cq));

            // Put each solution from the planner into the MPR.
            for (size_t i = 0; i < solutions.size(); ++i) {
                WorkingSet* ws;
                PlanStage* root;
                ASSERT(StageBuilder::build(*solutions[i], &root, &ws));
                // Takes ownership of all arguments.
                _mpr->addPlan(solutions[i], root, ws);
            }

            // And return a pointer to the best solution.  The MPR owns the pointer.
            size_t bestPlan = numeric_limits<size_t>::max();
            BSONObj unused;
            ASSERT(_mpr->pickBestPlan(&bestPlan, &unused));
            ASSERT_LESS_THAN(bestPlan, solutions.size());
            return solutions[bestPlan];
        }

    private:
        static DBDirectClient _client;
        scoped_ptr<MultiPlanRunner> _mpr;
    };

    DBDirectClient PlanRankingTestBase::_client;

    /** 
     * Test that the "prefer ixisect" parameter works.
     */
    class PlanRankingIntersectOverride : public PlanRankingTestBase {
    public:
        void run() {
            static const int N = 10000;

            // 'a' is very selective, 'b' is not.
            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << i << "b" << 1));
            }

            // Add indices on 'a' and 'b'.
            addIndex(BSON("a" << 1));
            addIndex(BSON("b" << 1));

            // Run the query {a:4, b:1}.
            CanonicalQuery* cq;
            verify(CanonicalQuery::canonicalize(ns, BSON("a" << 100 << "b" << 1), &cq).isOK());
            ASSERT(NULL != cq);

            // {a:100} is super selective so choose that.
            // Takes ownership of cq.
            QuerySolution* soln = pickBestPlan(cq);
            ASSERT(QueryPlannerTestLib::solutionMatches(
                        "{fetch: {filter: {b:1}, node: {ixscan: {pattern: {a: 1}}}}}",
                        soln->root.get()));

            // Turn on the "force intersect" option.
            forceIntersectionPlans = true;

            // And run the same query again.
            ASSERT(CanonicalQuery::canonicalize(ns, BSON("a" << 100 << "b" << 1), &cq).isOK());

            // With the "ranking picks ixisect always" option we pick an intersection plan that uses
            // both the {a:1} and {b:1} indices even though it performs poorly.

            // Takes ownership of cq.
            soln = pickBestPlan(cq);
            ASSERT(QueryPlannerTestLib::solutionMatches(
                             "{fetch: {filter: null, node: {andSorted: {nodes: ["
                                     "{ixscan: {filter: null, pattern: {a:1}}},"
                                     "{ixscan: {filter: null, pattern: {b:1}}}]}}}}",
                             soln->root.get()));

            // Turn off the intersection forcing flag for future tests.
            forceIntersectionPlans = false;
        }
    };

    /**
     * Two plans hit EOF at the same time, but one is covered. Make sure that we prefer the covered
     * plan.
     */
    class PlanRankingPreferCovered : public PlanRankingTestBase {
    public:
        void run() {
            // Insert data {a:i, b:i}.  Index {a:1} and {a:1, b:1}, query on 'a', projection on 'a'
            // and 'b'.  Should prefer the second index as we can pull the 'b' data out.

            static const int N = 10000;
            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << i << "b" << i));
            }

            addIndex(BSON("a" << 1));
            addIndex(BSON("a" << 1 << "b" << 1));

            // Query for a==27 with projection that wants 'a' and 'b'.  BSONObj() is for sort.
            CanonicalQuery* cq;
            ASSERT(CanonicalQuery::canonicalize(ns,
                                                BSON("a" << 27),
                                                BSONObj(),
                                                BSON("_id" << 0 << "a" << 1 << "b" << 1),
                                                &cq).isOK());
            ASSERT(NULL != cq);

            // Takes ownership of cq.
            QuerySolution* soln = pickBestPlan(cq);

            // Prefer the fully covered plan.
            ASSERT(QueryPlannerTestLib::solutionMatches(
                        "{proj: {spec: {_id:0, a:1, b:1}, node: {ixscan: {pattern: {a: 1, b:1}}}}}",
                        soln->root.get()));
        }
    };

    /**
     * No plan produces any results or hits EOF. In this case we should never choose an index
     * intersection solution.
     */
    class PlanRankingAvoidIntersectIfNoResults : public PlanRankingTestBase {
    public:
        void run() {
            // We insert lots of copies of {a:1, b:1, c: 20}.  We have the indices {a:1} and {b:1},
            // and the query is {a:1, b:1, c: 999}.  No data that matches the query but we won't
            // know that during plan ranking.  We don't want to choose an intersection plan here.
            static const int N = 10000;

            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << 1 << "b" << 1 << "c" << 20));
            }

            addIndex(BSON("a" << 1));
            addIndex(BSON("b" << 1));

            // There is no data that matches this query but we don't know that until EOF.
            CanonicalQuery* cq;
            BSONObj queryObj = BSON("a" << 1 << "b" << 1 << "c" << 99);
            ASSERT(CanonicalQuery::canonicalize(ns, queryObj, &cq).isOK());
            ASSERT(NULL != cq);

            // Takes ownership of cq.
            QuerySolution* soln = pickBestPlan(cq);

            // Anti-prefer the intersection plan.
            bool bestIsScanOverA = QueryPlannerTestLib::solutionMatches(
                        "{fetch: {node: {ixscan: {pattern: {a: 1}}}}}",
                        soln->root.get());
            bool bestIsScanOverB = QueryPlannerTestLib::solutionMatches(
                        "{fetch: {node: {ixscan: {pattern: {b: 1}}}}}",
                        soln->root.get());
            ASSERT(bestIsScanOverA || bestIsScanOverB);
        }
    };

    /**
     * No plan produces any results or hits EOF. In this case we should prefer covered solutions to
     * non-covered solutions.
     */
    class PlanRankingPreferCoveredEvenIfNoResults : public PlanRankingTestBase {
    public:
        void run() {
            // We insert lots of copies of {a:1, b:1}.  We have the indices {a:1} and {a:1, b:1},
            // the query is for a doc that doesn't exist, but there is a projection over 'a' and
            // 'b'.  We should prefer the index that provides a covered query.
            static const int N = 10000;

            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << 1 << "b" << 1));
            }

            addIndex(BSON("a" << 1));
            addIndex(BSON("a" << 1 << "b" << 1));

            // There is no data that matches this query ({a:2}).  Both scans will hit EOF before
            // returning any data.

            CanonicalQuery* cq;
            ASSERT(CanonicalQuery::canonicalize(ns,
                                                BSON("a" << 2),
                                                BSONObj(),
                                                BSON("_id" << 0 << "a" << 1 << "b" << 1),
                                                &cq).isOK());
            ASSERT(NULL != cq);

            // Takes ownership of cq.
            QuerySolution* soln = pickBestPlan(cq);
            // Prefer the fully covered plan.
            ASSERT(QueryPlannerTestLib::solutionMatches(
                        "{proj: {spec: {_id:0, a:1, b:1}, node: {ixscan: {pattern: {a: 1, b:1}}}}}",
                        soln->root.get()));
        }
    };

    /**
     * We have an index on "a" which is somewhat selective and an index on "b" which is highly
     * selective (will cause an immediate EOF). Make sure that a query with predicates on both "a"
     * and "b" will use the index on "b".
     */
    class PlanRankingPreferImmediateEOF : public PlanRankingTestBase {
    public:
        void run() {
            static const int N = 10000;

            // 'a' is very selective, 'b' is not.
            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << i << "b" << 1));
            }

            // Add indices on 'a' and 'b'.
            addIndex(BSON("a" << 1));
            addIndex(BSON("b" << 1));

            // Run the query {a:N+1, b:1}.  (No such document.)
            CanonicalQuery* cq;
            verify(CanonicalQuery::canonicalize(ns, BSON("a" << N + 1 << "b" << 1), &cq).isOK());
            ASSERT(NULL != cq);

            // {a: 100} is super selective so choose that.
            // Takes ownership of cq.
            QuerySolution* soln = pickBestPlan(cq);
            ASSERT(QueryPlannerTestLib::solutionMatches(
                        "{fetch: {filter: {b:1}, node: {ixscan: {pattern: {a: 1}}}}}",
                        soln->root.get()));
        }
    };

    /**
     * We have an index on _id and a query over _id with a sort.  Ensure that we don't pick a
     * collscan as the best plan even though the _id-scanning solution doesn't produce any results.
     */
    class PlanRankingNoCollscan : public PlanRankingTestBase {
    public:
        void run() {
            static const int N = 10000;

            for (int i = 0; i < N; ++i) {
                insert(BSON("_id" << i));
            }

            addIndex(BSON("_id" << 1));

            // Run a query with a sort.  The blocking sort won't produce any data during the
            // evaluation period.
            CanonicalQuery* cq;
            BSONObj queryObj = BSON("_id" << BSON("$gte" << 20 << "$lte" << 200));
            BSONObj sortObj = BSON("c" << 1);
            BSONObj projObj = BSONObj();
            ASSERT(CanonicalQuery::canonicalize(ns,
                                                queryObj,
                                                sortObj,
                                                projObj,
                                                &cq).isOK());

            // Takes ownership of cq.
            QuerySolution* soln = pickBestPlan(cq);

            // The best must not be a collscan.
            ASSERT(QueryPlannerTestLib::solutionMatches(
                        "{sort: {pattern: {c: 1}, limit: 0, node: {"
                            "fetch: {filter: null, node: "
                                "{ixscan: {filter: null, pattern: {_id: 1}}}}}}}}",
                        soln->root.get()));
        }
    };

    /**
     * No indices are available, output a collscan.
     */
    class PlanRankingCollscan : public PlanRankingTestBase {
    public:
        void run() {
            static const int N = 10000;

            // Insert data for which we have no index.
            for (int i = 0; i < N; ++i) {
                insert(BSON("foo" << i));
            }

            // Look for A Space Odyssey.
            CanonicalQuery* cq;
            verify(CanonicalQuery::canonicalize(ns, BSON("foo" << 2001), &cq).isOK());
            ASSERT(NULL != cq);

            // Takes ownership of cq.
            QuerySolution* soln = pickBestPlan(cq);

            // The best must be a collscan.
            ASSERT(QueryPlannerTestLib::solutionMatches(
                        "{cscan: {dir: 1, filter: {foo: 2001}}}",
                        soln->root.get()));
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_plan_ranking" ) {}

        void setupTests() {
            add<PlanRankingIntersectOverride>();
            add<PlanRankingPreferCovered>();
            add<PlanRankingAvoidIntersectIfNoResults>();
            add<PlanRankingPreferCoveredEvenIfNoResults>();
            add<PlanRankingPreferImmediateEOF>();
            add<PlanRankingNoCollscan>();
            add<PlanRankingCollscan>();
        }
    } planRankingAll;

}  // namespace PlanRankingTest
