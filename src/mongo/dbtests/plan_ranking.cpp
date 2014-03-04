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
#include "mongo/util/fail_point_service.h"

namespace mongo {

    // How we access the external setParameter testing bool.
    extern bool forceIntersectionPlans;

}  // namespace mongo

namespace PlanRankingTests {

    static const char* ns = "unittests.PlanRankingTests";

    // The name of the "fetch always required" failpoint.
    static const char* kFetchFpName = "fetchInMemoryFail";

    class PlanRankingTestBase {
    public:
        PlanRankingTestBase() : _forceIntersectionPlans(forceIntersectionPlans) {
            Client::WriteContext ctx(ns);
            _client.dropCollection(ns);
        }

        virtual ~PlanRankingTestBase() {
            // Restore external setParameter testing bool.
            forceIntersectionPlans = _forceIntersectionPlans;
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

        /**
         * Was a backup plan picked during the ranking process?
         */
        bool hasBackupPlan() const {
            ASSERT(NULL != _mpr.get());
            return _mpr->hasBackupPlan();
        }

        void turnOnAlwaysFetch() {
            FailPointRegistry* registry = getGlobalFailPointRegistry();
            FailPoint* failPoint = registry->getFailPoint(kFetchFpName);
            ASSERT(NULL != failPoint);
            failPoint->setMode(FailPoint::alwaysOn);
        }

        void turnOffAlwaysFetch() {
            FailPointRegistry* registry = getGlobalFailPointRegistry();
            FailPoint* failPoint = registry->getFailPoint(kFetchFpName);
            ASSERT(NULL != failPoint);
            failPoint->setMode(FailPoint::off);
        }

    private:
        static DBDirectClient _client;
        scoped_ptr<MultiPlanRunner> _mpr;
        // Holds the value of global "forceIntersectionPlans" setParameter flag.
        // Restored at end of test invocation regardless of test result.
        bool _forceIntersectionPlans;
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
            // This will be reverted by PlanRankingTestBase's destructor when the test completes.
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
        }
    };

    /**
     * Test that a hashed AND solution plan is picked along with a non-blocking backup solution.
     */
    class PlanRankingIntersectWithBackup : public PlanRankingTestBase {
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

            // Run the query {a:1, b:{$gt:1}.
            CanonicalQuery* cq;
            verify(CanonicalQuery::canonicalize(ns, BSON("a" << 1 << "b" << BSON("$gt" << 1)),
                                                &cq).isOK());
            ASSERT(NULL != cq);

            // Turn on the "force intersect" option.
            // This will be reverted by PlanRankingTestBase's destructor when the test completes.
            forceIntersectionPlans = true;

            // Takes ownership of cq.
            QuerySolution* soln = pickBestPlan(cq);
            ASSERT(QueryPlannerTestLib::solutionMatches(
                             "{fetch: {filter: null, node: {andHash: {nodes: ["
                                     "{ixscan: {filter: null, pattern: {a:1}}},"
                                     "{ixscan: {filter: null, pattern: {b:1}}}]}}}}",
                             soln->root.get()));

            // Confirm that a backup plan is available.
            ASSERT(hasBackupPlan());
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

    /**
     * Index intersection solutions can be covered when single-index solutions
     * are not. If the single-index solutions need to do a lot of fetching,
     * then ixisect should win.
     */
    class PlanRankingIxisectCovered : public PlanRankingTestBase {
    public:
        void run() {
            // Simulate needing lots of FETCH's.
            turnOnAlwaysFetch();

            static const int N = 10000;

            // Neither 'a' nor 'b' is selective.
            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << 1 << "b" << 1));
            }

            // Add indices on 'a' and 'b'.
            addIndex(BSON("a" << 1));
            addIndex(BSON("b" << 1));

            // Query {a:1, b:1}, and project out all fields other than 'a' and 'b'.
            CanonicalQuery* cq;
            ASSERT(CanonicalQuery::canonicalize(ns,
                                                BSON("a" << 1 << "b" << 1),
                                                BSONObj(),
                                                BSON("_id" << 0 << "a" << 1 << "b" << 1),
                                                &cq).isOK());
            ASSERT(NULL != cq);

            // We should choose an ixisect plan because it requires fewer fetches.
            // Takes ownership of cq.
            QuerySolution* soln = pickBestPlan(cq);
            ASSERT(QueryPlannerTestLib::solutionMatches(
                "{proj: {spec: {_id:0,a:1,b:1}, node: {andSorted: {nodes: ["
                    "{ixscan: {filter: null, pattern: {a:1}}},"
                    "{ixscan: {filter: null, pattern: {b:1}}}]}}}}",
                soln->root.get()));

            turnOffAlwaysFetch();
        }
    };

    /**
     * Use the same data, same indices, and same query as the previous
     * test case, except without the projection. The query is not covered
     * by the index in this case, which means that there is no advantage
     * to an index intersection solution.
     */
    class PlanRankingIxisectNonCovered : public PlanRankingTestBase {
    public:
        void run() {
            // Simulate needing lots of FETCH's.
            turnOnAlwaysFetch();

            static const int N = 10000;

            // Neither 'a' nor 'b' is selective.
            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << 1 << "b" << 1));
            }

            // Add indices on 'a' and 'b'.
            addIndex(BSON("a" << 1));
            addIndex(BSON("b" << 1));

            // Query {a:1, b:1}.
            CanonicalQuery* cq;
            ASSERT(CanonicalQuery::canonicalize(ns,
                                                BSON("a" << 1 << "b" << 1),
                                                &cq).isOK());
            ASSERT(NULL != cq);

            // The intersection is large, and ixisect does not make the
            // query covered. We should NOT choose an intersection plan.
            QuerySolution* soln = pickBestPlan(cq);
            bool bestIsScanOverA = QueryPlannerTestLib::solutionMatches(
                        "{fetch: {node: {ixscan: {pattern: {a: 1}}}}}",
                        soln->root.get());
            bool bestIsScanOverB = QueryPlannerTestLib::solutionMatches(
                        "{fetch: {node: {ixscan: {pattern: {b: 1}}}}}",
                        soln->root.get());
            ASSERT(bestIsScanOverA || bestIsScanOverB);

            turnOffAlwaysFetch();
        }
    };

    /**
     * Index intersection solutions may require fewer fetches even if it does not make the
     * query covered. The ixisect plan will scan as many index keys as the union of the two
     * single index plans, but only needs to retrieve full documents for the intersection
     * of the two plans---this could mean fewer fetches!
     */
    class PlanRankingNonCoveredIxisectFetchesLess : public PlanRankingTestBase {
    public:
        void run() {
            // Simulate needing lots of FETCH's.
            turnOnAlwaysFetch();

            static const int N = 10000;

            // Set up data so that the following conditions hold:
            //  1) Documents matching {a: 1} are of high cardinality.
            //  2) Documents matching {b: 1} are of high cardinality.
            //  3) Documents matching {a: 1, b: 1} are of low cardinality---
            //  the intersection is small.
            //  4) At least one of the documents in the intersection is
            //  returned during the trial period.
            insert(BSON("a" << 1 << "b" << 1));
            for (int i = 0; i < N/2; ++i) {
                insert(BSON("a" << 1 << "b" << 2));
            }
            for (int i = 0; i < N/2; ++i) {
                insert(BSON("a" << 2 << "b" << 1));
            }

            // Add indices on 'a' and 'b'.
            addIndex(BSON("a" << 1));
            addIndex(BSON("b" << 1));

            // Neither the predicate on 'b' nor the predicate on 'a' is
            // very selective: both retrieve about half the documents.
            // However, the intersection is very small, which makes
            // the intersection plan desirable.
            CanonicalQuery* cq;
            ASSERT(CanonicalQuery::canonicalize(ns,
                                                fromjson("{a: 1, b: 1}"),
                                                &cq).isOK());
            ASSERT(NULL != cq);

            QuerySolution* soln = pickBestPlan(cq);
            ASSERT(QueryPlannerTestLib::solutionMatches(
                "{fetch: {filter: null, node: {andSorted: {nodes: ["
                    "{ixscan: {filter: null, pattern: {a:1}}},"
                    "{ixscan: {filter: null, pattern: {b:1}}}]}}}}",
                soln->root.get()));

            turnOffAlwaysFetch();
        }
    };

    /**
     * If the intersection is small, an AND_SORTED plan may be able to
     * hit EOF before the single index plans.
     */
    class PlanRankingIxisectHitsEOFFirst : public PlanRankingTestBase {
    public:
        void run() {
            // Simulate needing lots of FETCH's.
            turnOnAlwaysFetch();

            static const int N = 10000;

            // Set up the data so that for the query {a: 1, b: 1}, the
            // intersection is empty. The single index plans have to do
            // more fetching from disk in order to determine that the result
            // set is empty. As a result, the intersection plan hits EOF first.
            for (int i = 0; i < 30; ++i) {
                insert(BSON("a" << 1 << "b" << 2));
            }
            for (int i = 0; i < 30; ++i) {
                insert(BSON("a" << 2 << "b" << 1));
            }
            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << 2 << "b" << 2));
            }

            // Add indices on 'a' and 'b'.
            addIndex(BSON("a" << 1));
            addIndex(BSON("b" << 1));

            CanonicalQuery* cq;
            ASSERT(CanonicalQuery::canonicalize(ns,
                                                fromjson("{a: 1, b: 1}"),
                                                &cq).isOK());
            ASSERT(NULL != cq);

            // Choose the index intersection plan.
            QuerySolution* soln = pickBestPlan(cq);
            ASSERT(QueryPlannerTestLib::solutionMatches(
                "{fetch: {filter: null, node: {andSorted: {nodes: ["
                    "{ixscan: {filter: null, pattern: {a:1}}},"
                    "{ixscan: {filter: null, pattern: {b:1}}}]}}}}",
                soln->root.get()));

            turnOffAlwaysFetch();
        }
    };

    /**
     * If we query on 'a', 'b', and 'c' with indices on all three fields,
     * then there are three possible size-2 index intersections to consider.
     * Make sure we choose the right one.
     */
    class PlanRankingChooseBetweenIxisectPlans : public PlanRankingTestBase {
    public:
        void run() {
            // Simulate needing lots of FETCH's.
            turnOnAlwaysFetch();

            static const int N = 10000;

            // Set up the data so that for the query {a: 1, b: 1, c: 1}, the intersection
            // between 'b' and 'c' is small, and the other intersections are larger.
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << 1 << "b" << 1 << "c" << 1));
            }
            for (int i = 0; i < 10; ++i) {
                insert(BSON("a" << 2 << "b" << 1 << "c" << 1));
            }
            for (int i = 0; i < N/2; ++i) {
                insert(BSON("a" << 1 << "b" << 1 << "c" << 2));
                insert(BSON("a" << 1 << "b" << 2 << "c" << 1));
            }

            // Add indices on 'a', 'b', and 'c'.
            addIndex(BSON("a" << 1));
            addIndex(BSON("b" << 1));
            addIndex(BSON("c" << 1));

            CanonicalQuery* cq;
            ASSERT(CanonicalQuery::canonicalize(ns,
                                                fromjson("{a: 1, b: 1, c: 1}"),
                                                &cq).isOK());
            ASSERT(NULL != cq);

            // Intersection between 'b' and 'c' should hit EOF while the
            // other plans are busy fetching documents.
            QuerySolution* soln = pickBestPlan(cq);
            ASSERT(QueryPlannerTestLib::solutionMatches(
                "{fetch: {filter: {a:1}, node: {andSorted: {nodes: ["
                    "{ixscan: {filter: null, pattern: {b:1}}},"
                    "{ixscan: {filter: null, pattern: {c:1}}}]}}}}",
                soln->root.get()));

            turnOffAlwaysFetch();
        }
    };

    /**
     * When no other information is available, prefer solutions without
     * a blocking sort stage.
     */
    class PlanRankingAvoidBlockingSort : public PlanRankingTestBase {
    public:
        void run() {
            static const int N = 10000;

            for (int i = 0; i < N; ++i) {
                insert(BSON("a" << 1 << "d" << i));
            }

            // The index {d: 1, e: 1} provides the desired sort order,
            // while index {a: 1, b: 1} can be used to answer the
            // query predicate, but does not provide the sort.
            addIndex(BSON("a" << 1 << "b" << 1));
            addIndex(BSON("d" << 1 << "e" << 1));

            // Query: find({a: 1}).sort({d: 1})
            CanonicalQuery* cq;
            ASSERT(CanonicalQuery::canonicalize(ns,
                                                BSON("a" << 1),
                                                BSON("d" << 1), // sort
                                                BSONObj(), // projection
                                                &cq).isOK());
            ASSERT(NULL != cq);

            // No results will be returned during the trial period,
            // so we expect to choose {d: 1, e: 1}, as it allows us
            // to avoid the sort stage.
            QuerySolution* soln = pickBestPlan(cq);
            ASSERT(QueryPlannerTestLib::solutionMatches(
                        "{fetch: {filter: {a:1}, node: "
                            "{ixscan: {filter: null, pattern: {d:1,e:1}}}}}",
                        soln->root.get()));
        }
    };

    class All : public Suite {
    public:
        All() : Suite( "query_plan_ranking" ) {}

        void setupTests() {
            add<PlanRankingIntersectOverride>();
            add<PlanRankingIntersectWithBackup>();
            add<PlanRankingPreferCovered>();
            add<PlanRankingAvoidIntersectIfNoResults>();
            add<PlanRankingPreferCoveredEvenIfNoResults>();
            add<PlanRankingPreferImmediateEOF>();
            add<PlanRankingNoCollscan>();
            add<PlanRankingCollscan>();
            add<PlanRankingIxisectCovered>();
            add<PlanRankingIxisectNonCovered>();
            add<PlanRankingNonCoveredIxisectFetchesLess>();
            add<PlanRankingIxisectHitsEOFFirst>();
            add<PlanRankingChooseBetweenIxisectPlans>();
            add<PlanRankingAvoidBlockingSort>();
        }
    } planRankingAll;

}  // namespace PlanRankingTest
