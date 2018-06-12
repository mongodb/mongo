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
#include "mongo/db/catalog/index_catalog.h"
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
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/mock_yield_policies.h"
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
extern AtomicBool internalQueryForceIntersectionPlans;

namespace {

using std::unique_ptr;
using std::vector;
using stdx::make_unique;

static const NamespaceString nss("unittests.QueryStageMultiPlan");

std::unique_ptr<QuerySolution> createQuerySolution() {
    auto soln = stdx::make_unique<QuerySolution>();
    soln->cacheData = stdx::make_unique<SolutionCacheData>();
    soln->cacheData->solnType = SolutionCacheData::COLLSCAN_SOLN;
    soln->cacheData->tree = stdx::make_unique<PlanCacheIndexTree>();
    return soln;
}

class QueryStageMultiPlanTest : public unittest::Test {
public:
    QueryStageMultiPlanTest() : _client(_opCtx.get()) {
        dbtests::WriteContextForTests ctx(_opCtx.get(), nss.ns());
        _client.dropCollection(nss.ns());
    }

    virtual ~QueryStageMultiPlanTest() {
        dbtests::WriteContextForTests ctx(_opCtx.get(), nss.ns());
        _client.dropCollection(nss.ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(_opCtx.get(), nss.ns(), obj));
    }

    void insert(const BSONObj& obj) {
        dbtests::WriteContextForTests ctx(_opCtx.get(), nss.ns());
        _client.insert(nss.ns(), obj);
    }

    void remove(const BSONObj& obj) {
        dbtests::WriteContextForTests ctx(_opCtx.get(), nss.ns());
        _client.remove(nss.ns(), obj);
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    ServiceContext* serviceContext() {
        return _opCtx->getServiceContext();
    }

protected:
    const ServiceContext::UniqueOperationContext _opCtx = cc().makeOperationContext();
    ClockSource* const _clock = _opCtx->getServiceContext()->getFastClockSource();

    DBDirectClient _client;
};

std::unique_ptr<CanonicalQuery> makeCanonicalQuery(OperationContext* opCtx,
                                                   NamespaceString nss,
                                                   BSONObj filter) {
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(filter);
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, std::move(qr));
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
    ASSERT(cq);
    return cq;
}

unique_ptr<PlanStage> getIxScanPlan(OperationContext* opCtx,
                                    const Collection* coll,
                                    WorkingSet* sharedWs,
                                    int desiredFooValue) {
    std::vector<IndexDescriptor*> indexes;
    coll->getIndexCatalog()->findIndexesByKeyPattern(opCtx, BSON("foo" << 1), false, &indexes);
    ASSERT_EQ(indexes.size(), 1U);

    IndexScanParams ixparams;
    ixparams.descriptor = indexes[0];
    ixparams.bounds.isSimpleRange = true;
    ixparams.bounds.startKey = BSON("" << desiredFooValue);
    ixparams.bounds.endKey = BSON("" << desiredFooValue);
    ixparams.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
    ixparams.direction = 1;

    IndexScan* ix = new IndexScan(opCtx, ixparams, sharedWs, nullptr);
    unique_ptr<PlanStage> root(new FetchStage(opCtx, sharedWs, ix, nullptr, coll));

    return root;
}

unique_ptr<MatchExpression> makeMatchExpressionFromFilter(OperationContext* opCtx,
                                                          BSONObj filterObj) {
    const CollatorInterface* collator = nullptr;
    const boost::intrusive_ptr<ExpressionContext> expCtx(new ExpressionContext(opCtx, collator));
    StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(filterObj, expCtx);
    ASSERT_OK(statusWithMatcher.getStatus());
    unique_ptr<MatchExpression> filter = std::move(statusWithMatcher.getValue());
    ASSERT(filter);
    return filter;
}


unique_ptr<PlanStage> getCollScanPlan(OperationContext* opCtx,
                                      const Collection* coll,
                                      WorkingSet* sharedWs,
                                      MatchExpression* matchExpr) {
    CollectionScanParams csparams;
    csparams.collection = coll;
    csparams.direction = CollectionScanParams::FORWARD;

    unique_ptr<PlanStage> root(new CollectionScan(opCtx, csparams, sharedWs, matchExpr));

    return root;
}

std::unique_ptr<MultiPlanStage> runMultiPlanner(OperationContext* opCtx,
                                                const NamespaceString& nss,
                                                const Collection* coll,
                                                int desiredFooValue) {
    // Plan 0: IXScan over foo == desiredFooValue
    // Every call to work() returns something so this should clearly win (by current scoring
    // at least).
    unique_ptr<WorkingSet> sharedWs(new WorkingSet());
    unique_ptr<PlanStage> ixScanRoot = getIxScanPlan(opCtx, coll, sharedWs.get(), desiredFooValue);

    // Plan 1: CollScan.
    BSONObj filterObj = BSON("foo" << desiredFooValue);
    unique_ptr<MatchExpression> filter = makeMatchExpressionFromFilter(opCtx, filterObj);
    unique_ptr<PlanStage> collScanRoot = getCollScanPlan(opCtx, coll, sharedWs.get(), filter.get());

    // Hand the plans off to the MPS.
    auto cq = makeCanonicalQuery(opCtx, nss, BSON("foo" << desiredFooValue));

    unique_ptr<MultiPlanStage> mps = make_unique<MultiPlanStage>(opCtx, coll, cq.get());
    mps->addPlan(createQuerySolution(), ixScanRoot.release(), sharedWs.get());
    mps->addPlan(createQuerySolution(), collScanRoot.release(), sharedWs.get());

    // Plan 0 aka the first plan aka the index scan should be the best.
    PlanYieldPolicy yieldPolicy(PlanExecutor::NO_YIELD,
                                opCtx->getServiceContext()->getFastClockSource());
    ASSERT_OK(mps->pickBestPlan(&yieldPolicy));
    ASSERT(mps->bestPlanChosen());
    ASSERT_EQUALS(0, mps->bestPlanIdx());

    return mps;
}

size_t getBestPlanWorks(MultiPlanStage* mps) {
    return mps->getChildren()[mps->bestPlanIdx()]->getStats()->common.works;
}


// Basic ranking test: collection scan vs. highly selective index scan.  Make sure we also get
// all expected results out as well.
TEST_F(QueryStageMultiPlanTest, MPSCollectionScanVsHighlySelectiveIXScan) {
    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        insert(BSON("foo" << (i % 10)));
    }

    addIndex(BSON("foo" << 1));

    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);
    const Collection* coll = ctx.getCollection();

    // Plan 0: IXScan over foo == 7
    // Every call to work() returns something so this should clearly win (by current scoring
    // at least).
    unique_ptr<WorkingSet> sharedWs(new WorkingSet());
    unique_ptr<PlanStage> ixScanRoot = getIxScanPlan(_opCtx.get(), coll, sharedWs.get(), 7);

    // Plan 1: CollScan with matcher.
    BSONObj filterObj = BSON("foo" << 7);
    unique_ptr<MatchExpression> filter = makeMatchExpressionFromFilter(_opCtx.get(), filterObj);
    unique_ptr<PlanStage> collScanRoot =
        getCollScanPlan(_opCtx.get(), coll, sharedWs.get(), filter.get());

    // Hand the plans off to the MPS.
    auto cq = makeCanonicalQuery(_opCtx.get(), nss, filterObj);

    unique_ptr<MultiPlanStage> mps =
        make_unique<MultiPlanStage>(_opCtx.get(), ctx.getCollection(), cq.get());
    mps->addPlan(createQuerySolution(), ixScanRoot.release(), sharedWs.get());
    mps->addPlan(createQuerySolution(), collScanRoot.release(), sharedWs.get());

    // Plan 0 aka the first plan aka the index scan should be the best.
    PlanYieldPolicy yieldPolicy(PlanExecutor::NO_YIELD, _clock);
    ASSERT_OK(mps->pickBestPlan(&yieldPolicy));
    ASSERT(mps->bestPlanChosen());
    ASSERT_EQUALS(0, mps->bestPlanIdx());

    // Takes ownership of arguments other than 'collection'.
    auto statusWithPlanExecutor = PlanExecutor::make(_opCtx.get(),
                                                     std::move(sharedWs),
                                                     std::move(mps),
                                                     std::move(cq),
                                                     coll,
                                                     PlanExecutor::NO_YIELD);
    ASSERT_OK(statusWithPlanExecutor.getStatus());
    auto exec = std::move(statusWithPlanExecutor.getValue());

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

TEST_F(QueryStageMultiPlanTest, MPSDoesNotCreateActiveCacheEntryImmediately) {
    const int N = 100;
    for (int i = 0; i < N; ++i) {
        // Have a larger proportion of 5's than anything else.
        int toInsert = i % 10 >= 8 ? 5 : i % 10;
        insert(BSON("foo" << toInsert));
    }

    addIndex(BSON("foo" << 1));

    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);
    const Collection* coll = ctx.getCollection();

    const auto cq = makeCanonicalQuery(_opCtx.get(), nss, BSON("foo" << 7));

    // Run an index scan and collection scan, searching for {foo: 7}.
    auto mps = runMultiPlanner(_opCtx.get(), nss, coll, 7);

    // Be sure that an inactive cache entry was added.
    PlanCache* cache = coll->infoCache()->getPlanCache();
    ASSERT_EQ(cache->size(), 1U);
    auto entry = assertGet(cache->getEntry(*cq));
    ASSERT_FALSE(entry->isActive);
    const size_t firstQueryWorks = getBestPlanWorks(mps.get());
    ASSERT_EQ(firstQueryWorks, entry->works);

    // Run the multi-planner again. The index scan will again win, but the number of works
    // will be greater, since {foo: 5} appears more frequently in the collection.
    mps = runMultiPlanner(_opCtx.get(), nss, coll, 5);

    // The last plan run should have required far more works than the previous plan. This means
    // that the 'works' in the cache entry should have doubled.
    ASSERT_EQ(cache->size(), 1U);
    entry = assertGet(cache->getEntry(*cq));
    ASSERT_FALSE(entry->isActive);
    ASSERT_EQ(firstQueryWorks * 2, entry->works);

    // Run the exact same query again. This will still take more works than 'works', and
    // should cause the cache entry's 'works' to be doubled again.
    mps = runMultiPlanner(_opCtx.get(), nss, coll, 5);
    ASSERT_EQ(cache->size(), 1U);
    entry = assertGet(cache->getEntry(*cq));
    ASSERT_FALSE(entry->isActive);
    ASSERT_EQ(firstQueryWorks * 2 * 2, entry->works);

    // Run the query yet again. This time, an active cache entry should be created.
    mps = runMultiPlanner(_opCtx.get(), nss, coll, 5);
    ASSERT_EQ(cache->size(), 1U);
    entry = assertGet(cache->getEntry(*cq));
    ASSERT_TRUE(entry->isActive);
    ASSERT_EQ(getBestPlanWorks(mps.get()), entry->works);
}

TEST_F(QueryStageMultiPlanTest, MPSDoesCreatesActiveEntryWhenInactiveEntriesDisabled) {
    // Set the global flag for disabling active entries.
    internalQueryCacheDisableInactiveEntries.store(true);
    ON_BLOCK_EXIT([] { internalQueryCacheDisableInactiveEntries.store(false); });

    const int N = 100;
    for (int i = 0; i < N; ++i) {
        insert(BSON("foo" << i));
    }

    addIndex(BSON("foo" << 1));

    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);
    const Collection* coll = ctx.getCollection();

    const auto cq = makeCanonicalQuery(_opCtx.get(), nss, BSON("foo" << 7));

    // Run an index scan and collection scan, searching for {foo: 7}.
    auto mps = runMultiPlanner(_opCtx.get(), nss, coll, 7);

    // Be sure that an _active_ cache entry was added.
    PlanCache* cache = coll->infoCache()->getPlanCache();
    ASSERT_EQ(cache->get(*cq).state, PlanCache::CacheEntryState::kPresentActive);

    // Run the multi-planner again. The entry should still be active.
    mps = runMultiPlanner(_opCtx.get(), nss, coll, 5);

    ASSERT_EQ(cache->get(*cq).state, PlanCache::CacheEntryState::kPresentActive);
}

// Case in which we select a blocking plan as the winner, and a non-blocking plan
// is available as a backup.
TEST_F(QueryStageMultiPlanTest, MPSBackupPlan) {
    // Data is just a single {_id: 1, a: 1, b: 1} document.
    insert(BSON("_id" << 1 << "a" << 1 << "b" << 1));

    // Indices on 'a' and 'b'.
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);
    Collection* collection = ctx.getCollection();

    // Query for both 'a' and 'b' and sort on 'b'.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(BSON("a" << 1 << "b" << 1));
    qr->setSort(BSON("b" << 1));
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(qr));
    verify(statusWithCQ.isOK());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
    ASSERT(NULL != cq.get());

    // Force index intersection.
    bool forceIxisectOldValue = internalQueryForceIntersectionPlans.load();
    internalQueryForceIntersectionPlans.store(true);

    // Get planner params.
    QueryPlannerParams plannerParams;
    fillOutPlannerParams(_opCtx.get(), collection, cq.get(), &plannerParams);
    // Turn this off otherwise it pops up in some plans.
    plannerParams.options &= ~QueryPlannerParams::KEEP_MUTATIONS;

    // Plan.
    auto statusWithSolutions = QueryPlanner::plan(*cq, plannerParams);
    ASSERT_OK(statusWithSolutions.getStatus());
    auto solutions = std::move(statusWithSolutions.getValue());

    // We expect a plan using index {a: 1} and plan using index {b: 1} and
    // an index intersection plan.
    ASSERT_EQUALS(solutions.size(), 3U);

    // Fill out the MultiPlanStage.
    unique_ptr<MultiPlanStage> mps(new MultiPlanStage(_opCtx.get(), collection, cq.get()));
    unique_ptr<WorkingSet> ws(new WorkingSet());
    // Put each solution from the planner into the MPR.
    for (size_t i = 0; i < solutions.size(); ++i) {
        PlanStage* root;
        ASSERT(StageBuilder::build(_opCtx.get(), collection, *cq, *solutions[i], ws.get(), &root));
        // Takes ownership of 'root'.
        mps->addPlan(std::move(solutions[i]), root, ws.get());
    }

    // This sets a backup plan.
    PlanYieldPolicy yieldPolicy(PlanExecutor::NO_YIELD, _clock);
    ASSERT_OK(mps->pickBestPlan(&yieldPolicy));
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
    internalQueryForceIntersectionPlans.store(forceIxisectOldValue);
}

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

// Test the structure and values of the explain output.
TEST_F(QueryStageMultiPlanTest, MPSExplainAllPlans) {
    // Insert a document to create the collection.
    insert(BSON("x" << 1));

    const int nDocs = 500;

    auto ws = stdx::make_unique<WorkingSet>();
    auto firstPlan = stdx::make_unique<QueuedDataStage>(_opCtx.get(), ws.get());
    auto secondPlan = stdx::make_unique<QueuedDataStage>(_opCtx.get(), ws.get());

    for (int i = 0; i < nDocs; ++i) {
        addMember(firstPlan.get(), ws.get(), BSON("x" << 1));

        // Make the second plan slower by inserting a NEED_TIME between every result.
        addMember(secondPlan.get(), ws.get(), BSON("x" << 1));
        secondPlan->pushBack(PlanStage::NEED_TIME);
    }

    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(BSON("x" << 1));
    auto cq = uassertStatusOK(CanonicalQuery::canonicalize(opCtx(), std::move(qr)));
    unique_ptr<MultiPlanStage> mps =
        make_unique<MultiPlanStage>(_opCtx.get(), ctx.getCollection(), cq.get());

    // Put each plan into the MultiPlanStage. Takes ownership of 'firstPlan' and 'secondPlan'.
    mps->addPlan(stdx::make_unique<QuerySolution>(), firstPlan.release(), ws.get());
    mps->addPlan(stdx::make_unique<QuerySolution>(), secondPlan.release(), ws.get());

    // Making a PlanExecutor chooses the best plan.
    auto exec = uassertStatusOK(PlanExecutor::make(
        _opCtx.get(), std::move(ws), std::move(mps), ctx.getCollection(), PlanExecutor::NO_YIELD));

    auto root = static_cast<MultiPlanStage*>(exec->getRootStage());
    ASSERT_TRUE(root->bestPlanChosen());
    // The first QueuedDataStage should have won.
    ASSERT_EQ(root->bestPlanIdx(), 0);

    BSONObjBuilder bob;
    Explain::explainStages(
        exec.get(), ctx.getCollection(), ExplainOptions::Verbosity::kExecAllPlans, &bob);
    BSONObj explained = bob.done();

    ASSERT_EQ(explained["executionStats"]["nReturned"].Int(), nDocs);
    ASSERT_EQ(explained["executionStats"]["executionStages"]["needTime"].Int(), 0);
    auto allPlansStats = explained["executionStats"]["allPlansExecution"].Array();
    ASSERT_EQ(allPlansStats.size(), 2UL);
    for (auto&& planStats : allPlansStats) {
        int maxEvaluationResults = internalQueryPlanEvaluationMaxResults.load();
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

// Test that the plan summary only includes stats from the winning plan.
//
// This is a regression test for SERVER-20111.
TEST_F(QueryStageMultiPlanTest, MPSSummaryStats) {
    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        insert(BSON("foo" << (i % 10)));
    }

    // Add two indices to give more plans.
    addIndex(BSON("foo" << 1));
    addIndex(BSON("foo" << -1 << "bar" << 1));

    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);
    Collection* coll = ctx.getCollection();

    // Create the executor (Matching all documents).
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(BSON("foo" << BSON("$gte" << 0)));
    auto cq = uassertStatusOK(CanonicalQuery::canonicalize(opCtx(), std::move(qr)));
    auto exec =
        uassertStatusOK(getExecutor(opCtx(), coll, std::move(cq), PlanExecutor::NO_YIELD, 0));
    ASSERT_EQ(exec->getRootStage()->stageType(), STAGE_MULTI_PLAN);

    ASSERT_OK(exec->executePlan());

    PlanSummaryStats stats;
    Explain::getSummaryStats(*exec, &stats);

    // If only the winning plan's stats are recorded, we should not have examined more than the
    // total number of documents/index keys.
    ASSERT_LTE(stats.totalDocsExamined, static_cast<size_t>(N));
    ASSERT_LTE(stats.totalKeysExamined, static_cast<size_t>(N));
}

TEST_F(QueryStageMultiPlanTest, ShouldReportErrorIfExceedsTimeLimitDuringPlanning) {
    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        insert(BSON("foo" << (i % 10)));
    }

    // Add two indices to give more plans.
    addIndex(BSON("foo" << 1));
    addIndex(BSON("foo" << -1 << "bar" << 1));

    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);
    const auto coll = ctx.getCollection();

    // Plan 0: IXScan over foo == 7
    // Every call to work() returns something so this should clearly win (by current scoring
    // at least).
    unique_ptr<WorkingSet> sharedWs(new WorkingSet());
    unique_ptr<PlanStage> ixScanRoot = getIxScanPlan(_opCtx.get(), coll, sharedWs.get(), 7);

    // Plan 1: CollScan with matcher.
    CollectionScanParams csparams;
    csparams.collection = coll;
    csparams.direction = CollectionScanParams::FORWARD;

    // Make the filter.
    BSONObj filterObj = BSON("foo" << 7);
    unique_ptr<MatchExpression> filter = makeMatchExpressionFromFilter(_opCtx.get(), filterObj);
    unique_ptr<PlanStage> collScanRoot =
        getCollScanPlan(_opCtx.get(), coll, sharedWs.get(), filter.get());


    auto queryRequest = stdx::make_unique<QueryRequest>(nss);
    queryRequest->setFilter(filterObj);
    auto canonicalQuery =
        uassertStatusOK(CanonicalQuery::canonicalize(opCtx(), std::move(queryRequest)));
    MultiPlanStage multiPlanStage(opCtx(),
                                  ctx.getCollection(),
                                  canonicalQuery.get(),
                                  MultiPlanStage::CachingMode::NeverCache);
    multiPlanStage.addPlan(createQuerySolution(), ixScanRoot.release(), sharedWs.get());
    multiPlanStage.addPlan(createQuerySolution(), collScanRoot.release(), sharedWs.get());

    AlwaysTimeOutYieldPolicy alwaysTimeOutPolicy(serviceContext()->getFastClockSource());
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, multiPlanStage.pickBestPlan(&alwaysTimeOutPolicy));
}

TEST_F(QueryStageMultiPlanTest, ShouldReportErrorIfKilledDuringPlanning) {
    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        insert(BSON("foo" << (i % 10)));
    }

    // Add two indices to give more plans.
    addIndex(BSON("foo" << 1));
    addIndex(BSON("foo" << -1 << "bar" << 1));

    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);
    const auto coll = ctx.getCollection();

    // Plan 0: IXScan over foo == 7
    // Every call to work() returns something so this should clearly win (by current scoring
    // at least).
    unique_ptr<WorkingSet> sharedWs(new WorkingSet());
    unique_ptr<PlanStage> ixScanRoot = getIxScanPlan(_opCtx.get(), coll, sharedWs.get(), 7);

    // Plan 1: CollScan.
    BSONObj filterObj = BSON("foo" << 7);
    unique_ptr<MatchExpression> filter = makeMatchExpressionFromFilter(_opCtx.get(), filterObj);
    unique_ptr<PlanStage> collScanRoot =
        getCollScanPlan(_opCtx.get(), coll, sharedWs.get(), filter.get());

    auto queryRequest = stdx::make_unique<QueryRequest>(nss);
    queryRequest->setFilter(BSON("foo" << BSON("$gte" << 0)));
    auto canonicalQuery =
        uassertStatusOK(CanonicalQuery::canonicalize(opCtx(), std::move(queryRequest)));
    MultiPlanStage multiPlanStage(opCtx(),
                                  ctx.getCollection(),
                                  canonicalQuery.get(),
                                  MultiPlanStage::CachingMode::NeverCache);
    multiPlanStage.addPlan(createQuerySolution(), ixScanRoot.release(), sharedWs.get());
    multiPlanStage.addPlan(createQuerySolution(), collScanRoot.release(), sharedWs.get());

    AlwaysPlanKilledYieldPolicy alwaysPlanKilledYieldPolicy(serviceContext()->getFastClockSource());
    ASSERT_EQ(ErrorCodes::QueryPlanKilled,
              multiPlanStage.pickBestPlan(&alwaysPlanKilledYieldPolicy));
}

}  // namespace
}  // namespace mongo
