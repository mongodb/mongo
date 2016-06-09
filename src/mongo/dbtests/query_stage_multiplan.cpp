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
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/query/stage_builder.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {

const std::unique_ptr<ClockSource> clockSource = stdx::make_unique<ClockSourceMock>();

// How we access the external setParameter testing bool.
extern std::atomic<bool> internalQueryForceIntersectionPlans;  // NOLINT

}  // namespace mongo

namespace QueryStageMultiPlan {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

static const NamespaceString nss("unittests.QueryStageMultiPlan");

/**
 * Create query solution.
 */
QuerySolution* createQuerySolution() {
    unique_ptr<QuerySolution> soln(new QuerySolution());
    soln->cacheData.reset(new SolutionCacheData());
    soln->cacheData->solnType = SolutionCacheData::COLLSCAN_SOLN;
    soln->cacheData->tree.reset(new PlanCacheIndexTree());
    return soln.release();
}

class QueryStageMultiPlanBase {
public:
    QueryStageMultiPlanBase() : _client(&_txn) {
        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.dropCollection(nss.ns());
    }

    virtual ~QueryStageMultiPlanBase() {
        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.dropCollection(nss.ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_txn, nss.ns(), obj));
    }

    void insert(const BSONObj& obj) {
        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.insert(nss.ns(), obj);
    }

    void remove(const BSONObj& obj) {
        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.remove(nss.ns(), obj);
    }

    OperationContext* txn() {
        return &_txn;
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    ClockSource* const _clock = _txn.getServiceContext()->getFastClockSource();

    DBDirectClient _client;
};


// Basic ranking test: collection scan vs. highly selective index scan.  Make sure we also get
// all expected results out as well.
class MPSCollectionScanVsHighlySelectiveIXScan : public QueryStageMultiPlanBase {
public:
    void run() {
        const int N = 5000;
        for (int i = 0; i < N; ++i) {
            insert(BSON("foo" << (i % 10)));
        }

        addIndex(BSON("foo" << 1));

        AutoGetCollectionForRead ctx(&_txn, nss.ns());
        const Collection* coll = ctx.getCollection();

        // Plan 0: IXScan over foo == 7
        // Every call to work() returns something so this should clearly win (by current scoring
        // at least).
        IndexScanParams ixparams;
        ixparams.descriptor =
            coll->getIndexCatalog()->findIndexByKeyPattern(&_txn, BSON("foo" << 1));
        ixparams.bounds.isSimpleRange = true;
        ixparams.bounds.startKey = BSON("" << 7);
        ixparams.bounds.endKey = BSON("" << 7);
        ixparams.bounds.endKeyInclusive = true;
        ixparams.direction = 1;

        unique_ptr<WorkingSet> sharedWs(new WorkingSet());
        IndexScan* ix = new IndexScan(&_txn, ixparams, sharedWs.get(), NULL);
        unique_ptr<PlanStage> firstRoot(new FetchStage(&_txn, sharedWs.get(), ix, NULL, coll));

        // Plan 1: CollScan with matcher.
        CollectionScanParams csparams;
        csparams.collection = coll;
        csparams.direction = CollectionScanParams::FORWARD;

        // Make the filter.
        BSONObj filterObj = BSON("foo" << 7);
        const CollatorInterface* collator = nullptr;
        StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(
            filterObj, ExtensionsCallbackDisallowExtensions(), collator);
        verify(statusWithMatcher.isOK());
        unique_ptr<MatchExpression> filter = std::move(statusWithMatcher.getValue());
        // Make the stage.
        unique_ptr<PlanStage> secondRoot(
            new CollectionScan(&_txn, csparams, sharedWs.get(), filter.get()));

        // Hand the plans off to the MPS.
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("foo" << 7));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        verify(statusWithCQ.isOK());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        verify(NULL != cq.get());

        unique_ptr<MultiPlanStage> mps =
            make_unique<MultiPlanStage>(&_txn, ctx.getCollection(), cq.get());
        mps->addPlan(createQuerySolution(), firstRoot.release(), sharedWs.get());
        mps->addPlan(createQuerySolution(), secondRoot.release(), sharedWs.get());

        // Plan 0 aka the first plan aka the index scan should be the best.
        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL, _clock);
        mps->pickBestPlan(&yieldPolicy);
        ASSERT(mps->bestPlanChosen());
        ASSERT_EQUALS(0, mps->bestPlanIdx());

        // Takes ownership of arguments other than 'collection'.
        auto statusWithPlanExecutor = PlanExecutor::make(&_txn,
                                                         std::move(sharedWs),
                                                         std::move(mps),
                                                         std::move(cq),
                                                         coll,
                                                         PlanExecutor::YIELD_MANUAL);
        ASSERT_OK(statusWithPlanExecutor.getStatus());
        std::unique_ptr<PlanExecutor> exec = std::move(statusWithPlanExecutor.getValue());

        // Get all our results out.
        int results = 0;
        BSONObj obj;
        PlanExecutor::ExecState state;
        while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, NULL))) {
            ASSERT_EQUALS(obj["foo"].numberInt(), 7);
            ++results;
        }
        ASSERT_EQUALS(PlanExecutor::IS_EOF, state);
        ASSERT_EQUALS(results, N / 10);
    }
};

// Case in which we select a blocking plan as the winner, and a non-blocking plan
// is available as a backup.
class MPSBackupPlan : public QueryStageMultiPlanBase {
public:
    void run() {
        // Data is just a single {_id: 1, a: 1, b: 1} document.
        insert(BSON("_id" << 1 << "a" << 1 << "b" << 1));

        // Indices on 'a' and 'b'.
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));

        AutoGetCollectionForRead ctx(&_txn, nss.ns());
        Collection* collection = ctx.getCollection();

        // Query for both 'a' and 'b' and sort on 'b'.
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("a" << 1 << "b" << 1));
        qr->setSort(BSON("b" << 1));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        verify(statusWithCQ.isOK());
        unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
        ASSERT(NULL != cq.get());

        // Force index intersection.
        bool forceIxisectOldValue = internalQueryForceIntersectionPlans;
        internalQueryForceIntersectionPlans = true;

        // Get planner params.
        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq.get(), &plannerParams);
        // Turn this off otherwise it pops up in some plans.
        plannerParams.options &= ~QueryPlannerParams::KEEP_MUTATIONS;

        // Plan.
        vector<QuerySolution*> solutions;
        Status status = QueryPlanner::plan(*cq, plannerParams, &solutions);
        ASSERT(status.isOK());

        // We expect a plan using index {a: 1} and plan using index {b: 1} and
        // an index intersection plan.
        ASSERT_EQUALS(solutions.size(), 3U);

        // Fill out the MultiPlanStage.
        unique_ptr<MultiPlanStage> mps(new MultiPlanStage(&_txn, collection, cq.get()));
        unique_ptr<WorkingSet> ws(new WorkingSet());
        // Put each solution from the planner into the MPR.
        for (size_t i = 0; i < solutions.size(); ++i) {
            PlanStage* root;
            ASSERT(StageBuilder::build(&_txn, collection, *cq, *solutions[i], ws.get(), &root));
            // Takes ownership of 'solutions[i]' and 'root'.
            mps->addPlan(solutions[i], root, ws.get());
        }

        // This sets a backup plan.
        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL, _clock);
        mps->pickBestPlan(&yieldPolicy);
        ASSERT(mps->bestPlanChosen());
        ASSERT(mps->hasBackupPlan());

        // We should have picked the index intersection plan due to forcing ixisect.
        QuerySolution* soln = mps->bestSolution();
        ASSERT(QueryPlannerTestLib::solutionMatches(
            "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node:"
            "{fetch: {node: {andSorted: {nodes: ["
            "{ixscan: {filter: null, pattern: {a:1}}},"
            "{ixscan: {filter: null, pattern: {b:1}}}]}}}}}}}}",
            soln->root.get()));

        // Get the resulting document.
        PlanStage::StageState state = PlanStage::NEED_TIME;
        WorkingSetID wsid;
        while (state != PlanStage::ADVANCED) {
            state = mps->work(&wsid);
        }
        WorkingSetMember* member = ws->get(wsid);

        // Check the document returned by the query.
        ASSERT(member->hasObj());
        BSONObj expectedDoc = BSON("_id" << 1 << "a" << 1 << "b" << 1);
        ASSERT(expectedDoc.woCompare(member->obj.value()) == 0);

        // The blocking plan became unblocked, so we should no longer have a backup plan,
        // and the winning plan should still be the index intersection one.
        ASSERT(!mps->hasBackupPlan());
        soln = mps->bestSolution();
        ASSERT(QueryPlannerTestLib::solutionMatches(
            "{sort: {pattern: {b: 1}, limit: 0, node: {sortKeyGen: {node:"
            "{fetch: {node: {andSorted: {nodes: ["
            "{ixscan: {filter: null, pattern: {a:1}}},"
            "{ixscan: {filter: null, pattern: {b:1}}}]}}}}}}}}",
            soln->root.get()));

        // Restore index intersection force parameter.
        internalQueryForceIntersectionPlans = forceIxisectOldValue;
    }
};

// Test the structure and values of the explain output.
class MPSExplainAllPlans : public QueryStageMultiPlanBase {
public:
    void run() {
        // Insert a document to create the collection.
        insert(BSON("x" << 1));

        const int nDocs = 500;

        auto ws = stdx::make_unique<WorkingSet>();
        auto firstPlan = stdx::make_unique<QueuedDataStage>(&_txn, ws.get());
        auto secondPlan = stdx::make_unique<QueuedDataStage>(&_txn, ws.get());

        for (int i = 0; i < nDocs; ++i) {
            addMember(firstPlan.get(), ws.get(), BSON("x" << 1));

            // Make the second plan slower by inserting a NEED_TIME between every result.
            addMember(secondPlan.get(), ws.get(), BSON("x" << 1));
            secondPlan->pushBack(PlanStage::NEED_TIME);
        }

        AutoGetCollectionForRead ctx(&_txn, nss.ns());

        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("x" << 1));
        auto cq = uassertStatusOK(CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions()));
        unique_ptr<MultiPlanStage> mps =
            make_unique<MultiPlanStage>(&_txn, ctx.getCollection(), cq.get());

        // Put each plan into the MultiPlanStage. Takes ownership of 'firstPlan' and 'secondPlan'.
        auto firstSoln = stdx::make_unique<QuerySolution>();
        auto secondSoln = stdx::make_unique<QuerySolution>();
        mps->addPlan(firstSoln.release(), firstPlan.release(), ws.get());
        mps->addPlan(secondSoln.release(), secondPlan.release(), ws.get());

        // Making a PlanExecutor chooses the best plan.
        auto exec = uassertStatusOK(PlanExecutor::make(
            &_txn, std::move(ws), std::move(mps), ctx.getCollection(), PlanExecutor::YIELD_MANUAL));

        auto root = static_cast<MultiPlanStage*>(exec->getRootStage());
        ASSERT_TRUE(root->bestPlanChosen());
        // The first QueuedDataStage should have won.
        ASSERT_EQ(root->bestPlanIdx(), 0);

        BSONObjBuilder bob;
        Explain::explainStages(
            exec.get(), ctx.getCollection(), ExplainCommon::EXEC_ALL_PLANS, &bob);
        BSONObj explained = bob.done();

        ASSERT_EQ(explained["executionStats"]["nReturned"].Int(), nDocs);
        ASSERT_EQ(explained["executionStats"]["executionStages"]["needTime"].Int(), 0);
        auto allPlansStats = explained["executionStats"]["allPlansExecution"].Array();
        ASSERT_EQ(allPlansStats.size(), 2UL);
        for (auto&& planStats : allPlansStats) {
            int maxEvaluationResults = internalQueryPlanEvaluationMaxResults;
            ASSERT_EQ(planStats["executionStages"]["stage"].String(), "QUEUED_DATA");
            if (planStats["executionStages"]["needTime"].Int() > 0) {
                // This is the losing plan. Should only have advanced about half the time.
                ASSERT_LT(planStats["nReturned"].Int(), maxEvaluationResults);
            } else {
                // This is the winning plan. Stats here should be from the trial period.
                ASSERT_EQ(planStats["nReturned"].Int(), maxEvaluationResults);
            }
        }
    }

private:
    /**
     * Allocates a new WorkingSetMember with data 'dataObj' in 'ws', and adds the WorkingSetMember
     * to 'qds'.
     */
    void addMember(QueuedDataStage* qds, WorkingSet* ws, BSONObj dataObj) {
        WorkingSetID id = ws->allocate();
        WorkingSetMember* wsm = ws->get(id);
        wsm->obj = Snapshotted<BSONObj>(SnapshotId(), BSON("x" << 1));
        wsm->transitionToOwnedObj();
        qds->pushBack(id);
    }
};

// Test that the plan summary only includes stats from the winning plan.
//
// This is a regression test for SERVER-20111.
class MPSSummaryStats : public QueryStageMultiPlanBase {
public:
    void run() {
        const int N = 5000;
        for (int i = 0; i < N; ++i) {
            insert(BSON("foo" << (i % 10)));
        }

        // Add two indices to give more plans.
        addIndex(BSON("foo" << 1));
        addIndex(BSON("foo" << -1 << "bar" << 1));

        AutoGetCollectionForRead ctx(&_txn, nss.ns());
        Collection* coll = ctx.getCollection();

        // Create the executor (Matching all documents).
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(BSON("foo" << BSON("$gte" << 0)));
        auto cq = uassertStatusOK(CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions()));
        auto exec =
            uassertStatusOK(getExecutor(&_txn, coll, std::move(cq), PlanExecutor::YIELD_MANUAL));

        ASSERT_EQ(exec->getRootStage()->stageType(), STAGE_MULTI_PLAN);

        exec->executePlan();

        PlanSummaryStats stats;
        Explain::getSummaryStats(*exec, &stats);

        // If only the winning plan's stats are recorded, we should not have examined more than the
        // total number of documents/index keys.
        ASSERT_LTE(stats.totalDocsExamined, static_cast<size_t>(N));
        ASSERT_LTE(stats.totalKeysExamined, static_cast<size_t>(N));
    }
};

class All : public Suite {
public:
    All() : Suite("query_stage_multiplan") {}

    void setupTests() {
        add<MPSCollectionScanVsHighlySelectiveIXScan>();
        add<MPSBackupPlan>();
        add<MPSExplainAllPlans>();
        add<MPSSummaryStats>();
    }
};

SuiteInstance<All> queryStageMultiPlanAll;

}  // namespace QueryStageMultiPlan
