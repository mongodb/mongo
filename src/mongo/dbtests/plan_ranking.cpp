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

#include "mongo/platform/basic.h"

#include <iostream>

#include "mongo/client/dbclientcursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"

namespace mongo {

// How we access the external setParameter testing bool.
extern std::atomic<bool> internalQueryForceIntersectionPlans;  // NOLINT

extern std::atomic<bool> internalQueryPlannerEnableHashIntersection;  // NOLINT

}  // namespace mongo

namespace PlanRankingTests {

using std::unique_ptr;
using std::vector;

static const NamespaceString nss("unittests.PlanRankingTests");

class PlanRankingTestBase {
public:
    PlanRankingTestBase()
        : _internalQueryForceIntersectionPlans(internalQueryForceIntersectionPlans),
          _enableHashIntersection(internalQueryPlannerEnableHashIntersection),
          _client(&_txn) {
        // Run all tests with hash-based intersection enabled.
        internalQueryPlannerEnableHashIntersection = true;

        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.dropCollection(nss.ns());
    }

    virtual ~PlanRankingTestBase() {
        // Restore external setParameter testing bools.
        internalQueryForceIntersectionPlans = _internalQueryForceIntersectionPlans;
        internalQueryPlannerEnableHashIntersection = _enableHashIntersection;
    }

    void insert(const BSONObj& obj) {
        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.insert(nss.ns(), obj);
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_txn, nss.ns(), obj));
    }

    /**
     * Use the MultiPlanRunner to pick the best plan for the query 'cq'.  Goes through
     * normal planning to generate solutions and feeds them to the MPR.
     *
     * Does NOT take ownership of 'cq'.  Caller DOES NOT own the returned QuerySolution*.
     */
    QuerySolution* pickBestPlan(CanonicalQuery* cq) {
        AutoGetCollectionForRead ctx(&_txn, nss.ns());
        Collection* collection = ctx.getCollection();

        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq, &plannerParams);
        // Turn this off otherwise it pops up in some plans.
        plannerParams.options &= ~QueryPlannerParams::KEEP_MUTATIONS;

        // Plan.
        vector<QuerySolution*> solutions;
        Status status = QueryPlanner::plan(*cq, plannerParams, &solutions);
        ASSERT(status.isOK());

        ASSERT_GREATER_THAN_OR_EQUALS(solutions.size(), 1U);

        // Fill out the MPR.
        _mps.reset(new MultiPlanStage(&_txn, collection, cq));
        unique_ptr<WorkingSet> ws(new WorkingSet());
        // Put each solution from the planner into the MPR.
        for (size_t i = 0; i < solutions.size(); ++i) {
            PlanStage* root;
            ASSERT(StageBuilder::build(&_txn, collection, *cq, *solutions[i], ws.get(), &root));
            // Takes ownership of all (actually some) arguments.
            _mps->addPlan(solutions[i], root, ws.get());
        }
        // This is what sets a backup plan, should we test for it.
        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL,
                                    _txn.getServiceContext()->getFastClockSource());
        _mps->pickBestPlan(&yieldPolicy);
        ASSERT(_mps->bestPlanChosen());

        size_t bestPlanIdx = _mps->bestPlanIdx();
        ASSERT_LESS_THAN(bestPlanIdx, solutions.size());

        // And return a pointer to the best solution.
        return _mps->bestSolution();
    }

    /**
     * Was a backup plan picked during the ranking process?
     */
    bool hasBackupPlan() const {
        ASSERT(NULL != _mps.get());
        return _mps->hasBackupPlan();
    }

    OperationContext* txn() {
        return &_txn;
    }

protected:
    // A large number, which must be larger than the number of times
    // candidate plans are worked by the multi plan runner. Used for
    // determining the number of documents in the tests below.
    static const int N;

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;

private:
    // Holds the value of global "internalQueryForceIntersectionPlans" setParameter flag.
    // Restored at end of test invocation regardless of test result.
    bool _internalQueryForceIntersectionPlans;

    // Holds the value of the global set parameter so it can be restored at the end
    // of the test.
    bool _enableHashIntersection;

    unique_ptr<MultiPlanStage> _mps;

    DBDirectClient _client;
};

// static
const int PlanRankingTestBase::N = internalQueryPlanEvaluationWorks + 1000;

/**
 * Test that the "prefer ixisect" parameter works.
 */
class PlanRankingIntersectOverride : public PlanRankingTestBase {
public:
    void run() {
        // 'a' is very selective, 'b' is not.
        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << i << "b" << 1));
        }

        // Add indices on 'a' and 'b'.
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));

        unique_ptr<CanonicalQuery> cq;

        // Run the query {a:4, b:1}.
        {
            auto qr = stdx::make_unique<QueryRequest>(nss);
            qr->setFilter(BSON("a" << 100 << "b" << 1));
            auto statusWithCQ = CanonicalQuery::canonicalize(
                txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
            verify(statusWithCQ.isOK());
            cq = std::move(statusWithCQ.getValue());
            ASSERT(cq.get());
        }

        // {a:100} is super selective so choose that.
        QuerySolution* soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches(
            "{fetch: {filter: {b:1}, node: {ixscan: {pattern: {a: 1}}}}}", soln->root.get()));

        // Turn on the "force intersect" option.
        // This will be reverted by PlanRankingTestBase's destructor when the test completes.
        internalQueryForceIntersectionPlans = true;

        // And run the same query again.
        {
            auto qr = stdx::make_unique<QueryRequest>(nss);
            qr->setFilter(BSON("a" << 100 << "b" << 1));
            auto statusWithCQ = CanonicalQuery::canonicalize(
                txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
            verify(statusWithCQ.isOK());
            cq = std::move(statusWithCQ.getValue());
        }

        // With the "ranking picks ixisect always" option we pick an intersection plan that uses
        // both the {a:1} and {b:1} indices even though it performs poorly.

        soln = pickBestPlan(cq.get());
        ASSERT(
            QueryPlannerTestLib::solutionMatches("{fetch: {node: {andSorted: {nodes: ["
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
        // 'a' is very selective, 'b' is not.
        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << i << "b" << 1));
        }

        // Add indices on 'a' and 'b'.
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));

        // Run the query {a:1, b:{$gt:1}.
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("a" << 1 << "b" << BSON("$gt" << 1)));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        verify(statusWithCQ.isOK());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        // Turn on the "force intersect" option.
        // This will be reverted by PlanRankingTestBase's destructor when the test completes.
        internalQueryForceIntersectionPlans = true;

        QuerySolution* soln = pickBestPlan(cq.get());
        ASSERT(
            QueryPlannerTestLib::solutionMatches("{fetch: {node: {andHash: {nodes: ["
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
        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << i << "b" << i));
        }

        addIndex(BSON("a" << 1));
        addIndex(BSON("a" << 1 << "b" << 1));

        // Query for a==27 with projection that wants 'a' and 'b'.
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("a" << 27));
        qr->setProj(BSON("_id" << 0 << "a" << 1 << "b" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        QuerySolution* soln = pickBestPlan(cq.get());

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
        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << 1 << "b" << 1 << "c" << 20));
        }

        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));

        // There is no data that matches this query but we don't know that until EOF.
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("a" << 1 << "b" << 1 << "c" << 99));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        QuerySolution* soln = pickBestPlan(cq.get());

        // Anti-prefer the intersection plan.
        bool bestIsScanOverA = QueryPlannerTestLib::solutionMatches(
            "{fetch: {node: {ixscan: {pattern: {a: 1}}}}}", soln->root.get());
        bool bestIsScanOverB = QueryPlannerTestLib::solutionMatches(
            "{fetch: {node: {ixscan: {pattern: {b: 1}}}}}", soln->root.get());
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
        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << 1 << "b" << 1));
        }

        addIndex(BSON("a" << 1));
        addIndex(BSON("a" << 1 << "b" << 1));

        // There is no data that matches this query ({a:2}).  Both scans will hit EOF before
        // returning any data.
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("a" << 2));
        qr->setProj(BSON("_id" << 0 << "a" << 1 << "b" << 1));

        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        QuerySolution* soln = pickBestPlan(cq.get());
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
        // 'a' is very selective, 'b' is not.
        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << i << "b" << 1));
        }

        // Add indices on 'a' and 'b'.
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));

        // Run the query {a:N+1, b:1}.  (No such document.)
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("a" << N + 1 << "b" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        verify(statusWithCQ.isOK());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        // {a: 100} is super selective so choose that.
        QuerySolution* soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches(
            "{fetch: {filter: {b:1}, node: {ixscan: {pattern: {a: 1}}}}}", soln->root.get()));
    }
};

/**
 * Same as PlanRankingPreferImmediateEOF, but substitute a range predicate on "a" for the
 * equality predicate on "a".  The presence of the range predicate has an impact on the
 * intersection plan that is raced against the single-index plans: since "a" no longer generates
 * point interval bounds, the results of the index scan aren't guaranteed to be returned in
 * RecordId order, and so the intersection plan uses the AND_HASHED stage instead of the
 * AND_SORTED stage.  It is still the case that the query should pick the plan that uses index
 * "b", instead of the plan that uses index "a" or the (hashed) intersection plan.
 */
class PlanRankingPreferImmediateEOFAgainstHashed : public PlanRankingTestBase {
public:
    void run() {
        // 'a' is very selective, 'b' is not.
        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << i << "b" << 1));
        }

        // Add indices on 'a' and 'b'.
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));

        // Run the query {a:N+1, b:1}.  (No such document.)
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("a" << BSON("$gte" << N + 1) << "b" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        verify(statusWithCQ.isOK());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        // {a: 100} is super selective so choose that.
        QuerySolution* soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches(
            "{fetch: {filter: {b:1}, node: {ixscan: {pattern: {a: 1}}}}}", soln->root.get()));
    }
};

/**
 * We have an index on _id and a query over _id with a sort.  Ensure that we don't pick a
 * collscan as the best plan even though the _id-scanning solution doesn't produce any results.
 */
class PlanRankingNoCollscan : public PlanRankingTestBase {
public:
    void run() {
        for (int i = 0; i < N; ++i) {
            insert(BSON("_id" << i));
        }

        addIndex(BSON("_id" << 1));

        // Run a query with a sort.  The blocking sort won't produce any data during the
        // evaluation period.
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("_id" << BSON("$gte" << 20 << "$lte" << 200)));
        qr->setSort(BSON("c" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        QuerySolution* soln = pickBestPlan(cq.get());

        // The best must not be a collscan.
        ASSERT(QueryPlannerTestLib::solutionMatches(
            "{sort: {pattern: {c: 1}, limit: 0, node: {sortKeyGen: {node:"
            "{fetch: {filter: null, node: "
            "{ixscan: {filter: null, pattern: {_id: 1}}}}}}}}}",
            soln->root.get()));
    }
};

/**
 * No indices are available, output a collscan.
 */
class PlanRankingCollscan : public PlanRankingTestBase {
public:
    void run() {
        // Insert data for which we have no index.
        for (int i = 0; i < N; ++i) {
            insert(BSON("foo" << i));
        }

        // Look for A Space Odyssey.
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("foo" << 2001));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        verify(statusWithCQ.isOK());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        QuerySolution* soln = pickBestPlan(cq.get());

        // The best must be a collscan.
        ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir: 1, filter: {foo: 2001}}}",
                                                    soln->root.get()));
    }
};

/**
 * When no other information is available, prefer solutions without
 * a blocking sort stage.
 */
class PlanRankingAvoidBlockingSort : public PlanRankingTestBase {
public:
    void run() {
        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << 1 << "d" << i));
        }

        // The index {d: 1, e: 1} provides the desired sort order,
        // while index {a: 1, b: 1} can be used to answer the
        // query predicate, but does not provide the sort.
        addIndex(BSON("a" << 1 << "b" << 1));
        addIndex(BSON("d" << 1 << "e" << 1));

        // Query: find({a: 1}).sort({d: 1})
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("a" << 1));
        qr->setSort(BSON("d" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        // No results will be returned during the trial period,
        // so we expect to choose {d: 1, e: 1}, as it allows us
        // to avoid the sort stage.
        QuerySolution* soln = pickBestPlan(cq.get());
        ASSERT(
            QueryPlannerTestLib::solutionMatches("{fetch: {filter: {a:1}, node: "
                                                 "{ixscan: {filter: null, pattern: {d:1,e:1}}}}}",
                                                 soln->root.get()));
    }
};

/**
 * Make sure we run candidate plans for long enough when none of the
 * plans are producing results.
 */
class PlanRankingWorkPlansLongEnough : public PlanRankingTestBase {
public:
    void run() {
        for (int i = 0; i < N; ++i) {
            insert(BSON("a" << 1));
            insert(BSON("a" << 1 << "b" << 1 << "c" << i));
        }

        // Indices on 'a' and 'b'.
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));

        // Solutions using either 'a' or 'b' will take a long time to start producing
        // results. However, an index scan on 'b' will start producing results sooner
        // than an index scan on 'a'.
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(fromjson("{a: 1, b: 1, c: {$gte: 5000}}"));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        // Use index on 'b'.
        QuerySolution* soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches("{fetch: {node: {ixscan: {pattern: {b: 1}}}}}",
                                                    soln->root.get()));
    }
};

/**
 * Suppose we have two plans which are roughly equivalent, other than that
 * one uses an index which involves doing a lot more skipping of index keys.
 * Prefer the plan which does not have to do this index key skipping.
 */
class PlanRankingAccountForKeySkips : public PlanRankingTestBase {
public:
    void run() {
        for (int i = 0; i < 100; ++i) {
            insert(BSON("a" << i << "b" << i << "c" << i));
        }

        // These indices look equivalent to the ranker for the query below unless we account
        // for key skipping. We should pick index {a: 1} if we account for key skipping
        // properly.
        addIndex(BSON("b" << 1 << "c" << 1));
        addIndex(BSON("a" << 1));

        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(fromjson("{a: 9, b: {$ne: 10}, c: 9}"));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        // Expect to use index {a: 1, b: 1}.
        QuerySolution* soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}",
                                                    soln->root.get()));
    }
};

class All : public Suite {
public:
    All() : Suite("query_plan_ranking") {}

    void setupTests() {
        add<PlanRankingIntersectOverride>();
        add<PlanRankingIntersectWithBackup>();
        add<PlanRankingPreferCovered>();
        add<PlanRankingAvoidIntersectIfNoResults>();
        add<PlanRankingPreferCoveredEvenIfNoResults>();
        add<PlanRankingPreferImmediateEOF>();
        add<PlanRankingPreferImmediateEOFAgainstHashed>();
        add<PlanRankingNoCollscan>();
        add<PlanRankingCollscan>();
        add<PlanRankingAvoidBlockingSort>();
        add<PlanRankingWorkPlansLongEnough>();
        add<PlanRankingAccountForKeySkips>();
    }
};

SuiteInstance<All> planRankingAll;

}  // namespace PlanRankingTest
