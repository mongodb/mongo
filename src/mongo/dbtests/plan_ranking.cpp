/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This file tests db/query/plan_ranker.cpp and db/query/multi_plan_runner.cpp.
 */

#include "mongo/client/dbclient_cursor.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/trial_period_utils.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/dbtests/dbtests.h"

namespace mongo {

// How we access the external setParameter testing bool.
extern AtomicWord<bool> internalQueryForceIntersectionPlans;

extern AtomicWord<bool> internalQueryPlannerEnableHashIntersection;

extern AtomicWord<int> internalQueryMaxBlockingSortMemoryUsageBytes;

extern AtomicWord<int> internalQueryPlanEvaluationMaxResults;

namespace PlanRankingTests {

static const NamespaceString nss("unittests.PlanRankingTests");

class PlanRankingTestBase {
public:
    PlanRankingTestBase()
        : _internalQueryForceIntersectionPlans(internalQueryForceIntersectionPlans.load()),
          _enableHashIntersection(internalQueryPlannerEnableHashIntersection.load()),
          _client(&_opCtx) {
        // Run all tests with hash-based intersection enabled.
        internalQueryPlannerEnableHashIntersection.store(true);

        // Ensure N is significantly larger then internalQueryPlanEvaluationWorks.
        ASSERT_GTE(N, internalQueryPlanEvaluationWorks.load() + 1000);

        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
        _client.dropCollection(nss);
    }

    virtual ~PlanRankingTestBase() {
        // Restore external setParameter testing bools.
        internalQueryForceIntersectionPlans.store(_internalQueryForceIntersectionPlans);
        internalQueryPlannerEnableHashIntersection.store(_enableHashIntersection);
    }

    void insert(const BSONObj& obj) {
        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
        _client.insert(nss, obj);
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), obj));
    }

    /**
     * Use the MultiPlanRunner to pick the best plan for the query 'cq'.  Goes through
     * normal planning to generate solutions and feeds them to the MPR.
     *
     * Does NOT take ownership of 'cq'.  Caller DOES NOT own the returned QuerySolution*.
     */
    const QuerySolution* pickBestPlan(CanonicalQuery* cq) {
        AutoGetCollectionForReadCommand collection(&_opCtx, nss);

        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_opCtx, collection.getCollection(), cq, &plannerParams);

        // Plan.
        auto statusWithMultiPlanSolns = QueryPlanner::plan(*cq, plannerParams);
        ASSERT_OK(statusWithMultiPlanSolns.getStatus());
        auto solutions = std::move(statusWithMultiPlanSolns.getValue());

        ASSERT_GREATER_THAN_OR_EQUALS(solutions.size(), 1U);

        // Fill out the MPR.
        _mps.reset(new MultiPlanStage(_expCtx.get(), collection.getCollection(), cq));
        std::unique_ptr<WorkingSet> ws(new WorkingSet());
        // Put each solution from the planner into the MPR.
        for (size_t i = 0; i < solutions.size(); ++i) {
            auto&& root = stage_builder::buildClassicExecutableTree(
                &_opCtx, collection.getCollection(), *cq, *solutions[i], ws.get());
            _mps->addPlan(std::move(solutions[i]), std::move(root), ws.get());
        }
        // This is what sets a backup plan, should we test for it.
        NoopYieldPolicy yieldPolicy(&_opCtx, _opCtx.getServiceContext()->getFastClockSource());
        _mps->pickBestPlan(&yieldPolicy).transitional_ignore();
        ASSERT(_mps->bestPlanChosen());

        auto bestPlanIdx = _mps->bestPlanIdx();
        ASSERT(bestPlanIdx.has_value());
        ASSERT_LESS_THAN(*bestPlanIdx, solutions.size());

        // And return a pointer to the best solution.
        return static_cast<const MultiPlanStage*>(_mps.get())->bestSolution();
    }

    /**
     * Was a backup plan picked during the ranking process?
     */
    bool hasBackupPlan() const {
        ASSERT(nullptr != _mps.get());
        return _mps->hasBackupPlan();
    }

    OperationContext* opCtx() {
        return &_opCtx;
    }

protected:
    // A large number, which must be larger than the number of times
    // candidate plans are worked by the multi plan runner. Used for
    // determining the number of documents in the tests below.
    const int N = 12000;

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_txnPtr;

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        make_intrusive<ExpressionContext>(&_opCtx, nullptr, nss);

private:
    // Holds the value of global "internalQueryForceIntersectionPlans" setParameter flag.
    // Restored at end of test invocation regardless of test result.
    bool _internalQueryForceIntersectionPlans;

    // Holds the value of the global set parameter so it can be restored at the end
    // of the test.
    bool _enableHashIntersection;

    std::unique_ptr<MultiPlanStage> _mps;

    DBDirectClient _client;
};

/**
 * Ensures that if a plan fails, but scores higher than a succeeding plan, then the plan which
 * doesn't fail is chosen.
 */
class PlanRankingPreferNonFailed : public PlanRankingTestBase {
public:
    PlanRankingPreferNonFailed()
        : PlanRankingTestBase(),
          _internalQueryMaxBlockingSortMemoryUsageBytes(
              internalQueryMaxBlockingSortMemoryUsageBytes.load()),
          // We set the max results to decrease the amount of work that is done during the trial
          // period. We want it to do less work than there are docs to ensure that no plan reaches
          // EOF.
          _internalQueryPlanEvaluationMaxResults(internalQueryPlanEvaluationMaxResults.load()) {
        internalQueryMaxBlockingSortMemoryUsageBytes.store(10);
        internalQueryPlanEvaluationMaxResults.store(100);
    }

    ~PlanRankingPreferNonFailed() {
        internalQueryMaxBlockingSortMemoryUsageBytes.store(
            _internalQueryMaxBlockingSortMemoryUsageBytes);
        internalQueryPlanEvaluationMaxResults.store(_internalQueryPlanEvaluationMaxResults);
    }

    void run() {
        const size_t numDocs = 1000;
        const size_t smallNumber = 10;

        // Insert 'numDocs' documents. A number of documents given by 'smallNumber' should have
        // a==1, while all other docs have a==0.
        for (size_t i = 0; i < numDocs; ++i) {
            insert(BSON("a" << static_cast<int>(i >= (numDocs - smallNumber)) << "d"
                            << static_cast<int>(i)));
        }

        // The index {a: 1} is what we expect to be used. The index {d: 1} is just to produce a
        // competing plan.
        addIndex(BSON("a" << 1));
        addIndex(BSON("d" << 1));

        // Run a query where we expect the most efficient plan to fail due to exhausting the
        // blocking sort memory limit during multi-planning. We expect this error to be swallowed
        // and the less efficient plan using index {d: 1} to be chosen instead.
        //
        // Query: find({a: 1}).sort({d: 1})
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(BSON("a" << 1));
        findCommand->setSort(BSON("d" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(cq);

        auto soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches("{fetch: {filter: {a:1}, node: "
                                                    "{ixscan: {filter: null, pattern: {d:1}}}}}",
                                                    soln->root())
                   .isOK());

        AutoGetCollectionForReadCommand collection(&_opCtx, nss);

        StatusWith<std::unique_ptr<PlanCacheEntry>> planCacheEntryWithStatus =
            CollectionQueryInfo::get(collection.getCollection())
                .getPlanCache()
                ->getEntry(
                    plan_cache_key_factory::make<PlanCacheKey>(*cq, collection.getCollection()));
        ASSERT_OK(planCacheEntryWithStatus.getStatus());
        auto debugInfo = planCacheEntryWithStatus.getValue()->debugInfo;
        ASSERT(debugInfo);

        // We assert that there was only one plan scored, implying that there was only one
        // non-failing plan.
        ASSERT(debugInfo->decision->scores.size() == 1);
        // We assert that there was one failing plan.
        ASSERT(debugInfo->decision->failedCandidates.size() == 1);
    }

private:
    // Holds the value of global "internalQueryMaxBlockingSortMemoryUsageBytes" setParameter flag.
    // Restored at end of test invocation regardless of test result.
    int _internalQueryMaxBlockingSortMemoryUsageBytes;
    int _internalQueryPlanEvaluationMaxResults;
};

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

        std::unique_ptr<CanonicalQuery> cq;

        // Run the query {a:4, b:1}.
        {
            auto findCommand = std::make_unique<FindCommandRequest>(nss);
            findCommand->setFilter(BSON("a" << 100 << "b" << 1));
            auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
            verify(statusWithCQ.isOK());
            cq = std::move(statusWithCQ.getValue());
            ASSERT(cq.get());
        }

        // {a:100} is super selective so choose that.
        auto soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches(
                   "{fetch: {filter: {b:1}, node: {ixscan: {pattern: {a: 1}}}}}", soln->root())
                   .isOK());

        // Turn on the "force intersect" option.
        // This will be reverted by PlanRankingTestBase's destructor when the test completes.
        internalQueryForceIntersectionPlans.store(true);

        // And run the same query again.
        {
            auto findCommand = std::make_unique<FindCommandRequest>(nss);
            findCommand->setFilter(BSON("a" << 100 << "b" << 1));
            auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
            verify(statusWithCQ.isOK());
            cq = std::move(statusWithCQ.getValue());
        }

        // With the "ranking picks ixisect always" option we pick an intersection plan that uses
        // both the {a:1} and {b:1} indices even though it performs poorly.

        soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches("{fetch: {node: {andSorted: {nodes: ["
                                                    "{ixscan: {filter: null, pattern: {a:1}}},"
                                                    "{ixscan: {filter: null, pattern: {b:1}}}]}}}}",
                                                    soln->root())
                   .isOK());
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
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(BSON("a" << 1 << "b" << BSON("$gt" << 1)));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        verify(statusWithCQ.isOK());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(nullptr != cq.get());

        // Turn on the "force intersect" option.
        // This will be reverted by PlanRankingTestBase's destructor when the test completes.
        internalQueryForceIntersectionPlans.store(true);

        auto soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches("{fetch: {node: {andHash: {nodes: ["
                                                    "{ixscan: {filter: null, pattern: {a:1}}},"
                                                    "{ixscan: {filter: null, pattern: {b:1}}}]}}}}",
                                                    soln->root())
                   .isOK());

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
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(BSON("a" << 27));
        findCommand->setProjection(BSON("_id" << 0 << "a" << 1 << "b" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(nullptr != cq.get());

        auto soln = pickBestPlan(cq.get());

        // Prefer the fully covered plan.
        ASSERT(QueryPlannerTestLib::solutionMatches(
                   "{proj: {spec: {_id:0, a:1, b:1}, node: {ixscan: {pattern: {a: 1, b:1}}}}}",
                   soln->root())
                   .isOK());
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
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(BSON("a" << 1 << "b" << 1 << "c" << 99));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(nullptr != cq.get());

        auto soln = pickBestPlan(cq.get());

        // Anti-prefer the intersection plan.
        auto bestIsScanOverA = QueryPlannerTestLib::solutionMatches(
            "{fetch: {node: {ixscan: {pattern: {a: 1}}}}}", soln->root());
        auto bestIsScanOverB = QueryPlannerTestLib::solutionMatches(
            "{fetch: {node: {ixscan: {pattern: {b: 1}}}}}", soln->root());
        ASSERT(bestIsScanOverA.isOK() || bestIsScanOverB.isOK());
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
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(BSON("a" << 2));
        findCommand->setProjection(BSON("_id" << 0 << "a" << 1 << "b" << 1));

        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(nullptr != cq.get());

        auto soln = pickBestPlan(cq.get());
        // Prefer the fully covered plan.
        ASSERT(QueryPlannerTestLib::solutionMatches(
                   "{proj: {spec: {_id:0, a:1, b:1}, node: {ixscan: {pattern: {a: 1, b:1}}}}}",
                   soln->root())
                   .isOK());
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
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(BSON("a" << N + 1 << "b" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        verify(statusWithCQ.isOK());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(nullptr != cq.get());

        // {a: 100} is super selective so choose that.
        auto soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches(
                   "{fetch: {filter: {b:1}, node: {ixscan: {pattern: {a: 1}}}}}", soln->root())
                   .isOK());
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
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(BSON("a" << BSON("$gte" << N + 1) << "b" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        verify(statusWithCQ.isOK());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(nullptr != cq.get());

        // {a: 100} is super selective so choose that.
        auto soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches(
                   "{fetch: {filter: {b:1}, node: {ixscan: {pattern: {a: 1}}}}}", soln->root())
                   .isOK());
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
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(BSON("_id" << BSON("$gte" << 20 << "$lte" << 200)));
        findCommand->setSort(BSON("c" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        auto soln = pickBestPlan(cq.get());

        // The best must not be a collscan.
        ASSERT(QueryPlannerTestLib::solutionMatches(
                   "{sort: {pattern: {c: 1}, limit: 0, type:'simple', node:"
                   "{fetch: {filter: null, node: "
                   "{ixscan: {filter: null, pattern: {_id: 1}}}}}}}",
                   soln->root())
                   .isOK());
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
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(BSON("foo" << 2001));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        verify(statusWithCQ.isOK());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(nullptr != cq.get());

        auto soln = pickBestPlan(cq.get());

        // The best must be a collscan.
        ASSERT(QueryPlannerTestLib::solutionMatches("{cscan: {dir: 1, filter: {foo: 2001}}}",
                                                    soln->root())
                   .isOK());
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
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(BSON("a" << 1));
        findCommand->setSort(BSON("d" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(nullptr != cq.get());

        // No results will be returned during the trial period,
        // so we expect to choose {d: 1, e: 1}, as it allows us
        // to avoid the sort stage.
        auto soln = pickBestPlan(cq.get());
        ASSERT(
            QueryPlannerTestLib::solutionMatches("{fetch: {filter: {a:1}, node: "
                                                 "{ixscan: {filter: null, pattern: {d:1,e:1}}}}}",
                                                 soln->root())
                .isOK());
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
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(fromjson("{a: 1, b: 1, c: {$gte: 5000}}"));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(nullptr != cq.get());

        // Use index on 'b'.
        auto soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches("{fetch: {node: {ixscan: {pattern: {b: 1}}}}}",
                                                    soln->root())
                   .isOK());
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

        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(fromjson("{a: 9, b: {$ne: 10}, c: 9}"));
        auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(nullptr != cq.get());

        // Expect to use index {a: 1, b: 1}.
        auto soln = pickBestPlan(cq.get());
        ASSERT(QueryPlannerTestLib::solutionMatches("{fetch: {node: {ixscan: {pattern: {a: 1}}}}}",
                                                    soln->root())
                   .isOK());
    }
};

class All : public OldStyleSuiteSpecification {
public:
    All() : OldStyleSuiteSpecification("query_plan_ranking") {}

    void setupTests() {
        add<PlanRankingIntersectOverride>();
        add<PlanRankingIntersectWithBackup>();
        add<PlanRankingPreferCovered>();
        add<PlanRankingAvoidIntersectIfNoResults>();
        add<PlanRankingPreferCoveredEvenIfNoResults>();
        add<PlanRankingPreferImmediateEOF>();
        add<PlanRankingPreferImmediateEOFAgainstHashed>();
        add<PlanRankingPreferNonFailed>();
        add<PlanRankingNoCollscan>();
        add<PlanRankingCollscan>();
        add<PlanRankingAvoidBlockingSort>();
        add<PlanRankingWorkPlansLongEnough>();
        add<PlanRankingAccountForKeySkips>();
    }
};

OldStyleSuiteInitializer<All> planRankingAll;

}  // namespace PlanRankingTests
}  // namespace mongo
