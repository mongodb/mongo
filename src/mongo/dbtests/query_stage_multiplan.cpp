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

#include <boost/container/small_vector.hpp>
#include <fmt/format.h>
// IWYU pragma: no_include "boost/intrusive/detail/iterator.hpp"
#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/collection_scan.h"
#include "mongo/db/exec/collection_scan_common.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/fetch.h"
#include "mongo/db/exec/index_scan.h"
#include "mongo/db/exec/mock_stage.h"
#include "mongo/db/exec/multi_plan.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/matcher/expression.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/classic_plan_cache.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/index_bounds.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_executor_impl.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/plan_yield_policy_impl.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/query/query_settings/query_settings_gen.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/stage_builder_util.h"
#include "mongo/db/query/stage_types.h"
#include "mongo/db/service_context.h"
#include "mongo/db/storage/snapshot.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/bson_test_util.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/clock_source_mock.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/serialization_context.h"

namespace mongo {

using unittest::assertGet;

const std::unique_ptr<ClockSource> clockSource = std::make_unique<ClockSourceMock>();

// How we access the external setParameter testing bool.
extern AtomicWord<bool> internalQueryForceIntersectionPlans;

namespace {

using std::unique_ptr;
using std::vector;

static const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest("unittests.QueryStageMultiPlan");

std::unique_ptr<QuerySolution> createQuerySolution() {
    auto soln = std::make_unique<QuerySolution>();
    soln->setRoot(std::make_unique<CollectionScanNode>());
    soln->cacheData = std::make_unique<SolutionCacheData>();
    soln->cacheData->solnType = SolutionCacheData::COLLSCAN_SOLN;
    soln->cacheData->tree = std::make_unique<PlanCacheIndexTree>();
    return soln;
}

class QueryStageMultiPlanTest : public unittest::Test {
public:
    QueryStageMultiPlanTest() : _client(_opCtx.get()) {
        dbtests::WriteContextForTests ctx(_opCtx.get(), nss.ns_forTest());
        _client.dropCollection(nss);
    }

    virtual ~QueryStageMultiPlanTest() {
        dbtests::WriteContextForTests ctx(_opCtx.get(), nss.ns_forTest());
        _client.dropCollection(nss);
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(_opCtx.get(), nss.ns_forTest(), obj));
    }

    void insert(const BSONObj& obj) {
        dbtests::WriteContextForTests ctx(_opCtx.get(), nss.ns_forTest());
        _client.insert(nss, obj);
    }

    void remove(const BSONObj& obj) {
        dbtests::WriteContextForTests ctx(_opCtx.get(), nss.ns_forTest());
        _client.remove(nss, obj);
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

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        make_intrusive<ExpressionContext>(_opCtx.get(), nullptr, nss);

    DBDirectClient _client;
};

std::unique_ptr<CanonicalQuery> makeCanonicalQuery(OperationContext* opCtx,
                                                   NamespaceString nss,
                                                   BSONObj filter) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(filter);
    return std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx, *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
}

unique_ptr<PlanStage> getIxScanPlan(ExpressionContext* expCtx,
                                    const CollectionPtr& coll,
                                    WorkingSet* sharedWs,
                                    int desiredFooValue) {
    std::vector<const IndexDescriptor*> indexes;
    coll->getIndexCatalog()->findIndexesByKeyPattern(
        expCtx->opCtx, BSON("foo" << 1), IndexCatalog::InclusionPolicy::kReady, &indexes);
    ASSERT_EQ(indexes.size(), 1U);

    IndexScanParams ixparams(expCtx->opCtx, coll, indexes[0]);
    ixparams.bounds.isSimpleRange = true;
    ixparams.bounds.startKey = BSON("" << desiredFooValue);
    ixparams.bounds.endKey = BSON("" << desiredFooValue);
    ixparams.bounds.boundInclusion = BoundInclusion::kIncludeBothStartAndEndKeys;
    ixparams.direction = 1;

    auto ixscan = std::make_unique<IndexScan>(expCtx, &coll, ixparams, sharedWs, nullptr);
    return std::make_unique<FetchStage>(expCtx, sharedWs, std::move(ixscan), nullptr, &coll);
}

unique_ptr<MatchExpression> makeMatchExpressionFromFilter(ExpressionContext* expCtx,
                                                          BSONObj filterObj) {
    StatusWithMatchExpression statusWithMatcher = MatchExpressionParser::parse(filterObj, expCtx);
    ASSERT_OK(statusWithMatcher.getStatus());
    unique_ptr<MatchExpression> filter = std::move(statusWithMatcher.getValue());
    ASSERT(filter);
    return filter;
}


unique_ptr<PlanStage> getCollScanPlan(ExpressionContext* expCtx,
                                      const CollectionPtr& coll,
                                      WorkingSet* sharedWs,
                                      MatchExpression* matchExpr) {
    CollectionScanParams csparams;
    csparams.direction = CollectionScanParams::FORWARD;

    unique_ptr<PlanStage> root(new CollectionScan(expCtx, &coll, csparams, sharedWs, matchExpr));

    return root;
}

const PlanStage* getBestPlanRoot(const MultiPlanStage* mps) {
    auto bestPlanIdx = mps->bestPlanIdx();
    return bestPlanIdx ? mps->getChildren()[bestPlanIdx.get()].get() : nullptr;
}

std::unique_ptr<MultiPlanStage> runMultiPlanner(ExpressionContext* expCtx,
                                                const NamespaceString& nss,
                                                const CollectionPtr& coll,
                                                int desiredFooValue) {
    // Plan 0: IXScan over foo == desiredFooValue
    // Every call to work() returns something so this should clearly win (by current scoring
    // at least).
    unique_ptr<WorkingSet> sharedWs(new WorkingSet());
    unique_ptr<PlanStage> ixScanRoot = getIxScanPlan(expCtx, coll, sharedWs.get(), desiredFooValue);
    const auto* ixScanRootPtr = ixScanRoot.get();

    // Plan 1: CollScan.
    BSONObj filterObj = BSON("foo" << desiredFooValue);
    unique_ptr<MatchExpression> filter = makeMatchExpressionFromFilter(expCtx, filterObj);
    unique_ptr<PlanStage> collScanRoot =
        getCollScanPlan(expCtx, coll, sharedWs.get(), filter.get());

    // Hand the plans off to the MPS.
    auto cq = makeCanonicalQuery(expCtx->opCtx, nss, BSON("foo" << desiredFooValue));

    unique_ptr<MultiPlanStage> mps = std::make_unique<MultiPlanStage>(expCtx, &coll, cq.get());
    mps->addPlan(createQuerySolution(), std::move(ixScanRoot), sharedWs.get());
    mps->addPlan(createQuerySolution(), std::move(collScanRoot), sharedWs.get());

    // Plan 0 aka the first plan aka the index scan should be the best.
    NoopYieldPolicy yieldPolicy(expCtx->opCtx,
                                expCtx->opCtx->getServiceContext()->getFastClockSource());
    ASSERT_OK(mps->pickBestPlan(&yieldPolicy));
    ASSERT(mps->bestPlanChosen());
    ASSERT_EQUALS(getBestPlanRoot(mps.get()), ixScanRootPtr);

    return mps;
}

size_t getBestPlanWorks(MultiPlanStage* mps) {
    auto bestPlanIdx = mps->bestPlanIdx();
    tassert(3420011,
            "Trying to get stats of a MultiPlanStage without winning plan",
            bestPlanIdx.has_value());
    return mps->getChildren()[*bestPlanIdx]->getStats()->common.works;
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
    const CollectionPtr& coll = ctx.getCollection();

    // Plan 0: IXScan over foo == 7
    // Every call to work() returns something so this should clearly win (by current scoring
    // at least).
    unique_ptr<WorkingSet> sharedWs(new WorkingSet());
    unique_ptr<PlanStage> ixScanRoot = getIxScanPlan(_expCtx.get(), coll, sharedWs.get(), 7);

    const auto* ixScanRootPtr = ixScanRoot.get();

    // Plan 1: CollScan with matcher.
    BSONObj filterObj = BSON("foo" << 7);
    unique_ptr<MatchExpression> filter = makeMatchExpressionFromFilter(_expCtx.get(), filterObj);
    unique_ptr<PlanStage> collScanRoot =
        getCollScanPlan(_expCtx.get(), coll, sharedWs.get(), filter.get());

    // Hand the plans off to the MPS.
    auto cq = makeCanonicalQuery(_opCtx.get(), nss, filterObj);

    unique_ptr<MultiPlanStage> mps =
        std::make_unique<MultiPlanStage>(_expCtx.get(), &ctx.getCollection(), cq.get());
    mps->addPlan(createQuerySolution(), std::move(ixScanRoot), sharedWs.get());
    mps->addPlan(createQuerySolution(), std::move(collScanRoot), sharedWs.get());

    auto* mpsPtr = mps.get();
    auto planYieldPolicy = makeClassicYieldPolicy(opCtx(),
                                                  nss,
                                                  static_cast<PlanStage*>(mpsPtr),
                                                  PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                  &coll);
    ASSERT_OK(mpsPtr->pickBestPlan(planYieldPolicy.get()));
    ASSERT_TRUE(mpsPtr->bestPlanChosen());
    ASSERT_EQUALS(mpsPtr->getChildren()[mpsPtr->bestPlanIdx().get()].get(), ixScanRootPtr);

    // Takes ownership of arguments other than 'collection'.
    auto execResult = plan_executor_factory::make(std::move(cq),
                                                  std::move(sharedWs),
                                                  std::move(mps),
                                                  &coll,
                                                  PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                  QueryPlannerParams::DEFAULT,
                                                  nss);
    ASSERT_OK(execResult);
    auto exec = std::move(execResult.getValue());

    // Get all our results out.
    int results = 0;
    BSONObj obj;
    PlanExecutor::ExecState state;
    while (PlanExecutor::ADVANCED == (state = exec->getNext(&obj, nullptr))) {
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
    const CollectionPtr& coll = ctx.getCollection();

    const auto cq = makeCanonicalQuery(_opCtx.get(), nss, BSON("foo" << 7));
    auto key = plan_cache_key_factory::make<PlanCacheKey>(*cq, coll);

    // Run an index scan and collection scan, searching for {foo: 7}.
    auto mps = runMultiPlanner(_expCtx.get(), nss, coll, 7);

    // Be sure that an inactive cache entry was added.
    PlanCache* cache = CollectionQueryInfo::get(coll).getPlanCache();
    ASSERT_EQ(cache->size(), 1U);
    auto entry = assertGet(cache->getEntry(key));
    ASSERT_FALSE(entry->isActive);
    const size_t firstQueryWorks = getBestPlanWorks(mps.get());
    ASSERT_EQ(firstQueryWorks, entry->works);

    // Run the multi-planner again. The index scan will again win, but the number of works
    // will be greater, since {foo: 5} appears more frequently in the collection.
    mps = runMultiPlanner(_expCtx.get(), nss, coll, 5);

    // The last plan run should have required far more works than the previous plan. This means
    // that the 'works' in the cache entry should have doubled.
    ASSERT_EQ(cache->size(), 1U);
    entry = assertGet(cache->getEntry(key));
    ASSERT_FALSE(entry->isActive);
    ASSERT_EQ(firstQueryWorks * 2, entry->works);

    // Run the exact same query again. This will still take more works than 'works', and
    // should cause the cache entry's 'works' to be doubled again.
    mps = runMultiPlanner(_expCtx.get(), nss, coll, 5);
    ASSERT_EQ(cache->size(), 1U);
    entry = assertGet(cache->getEntry(key));
    ASSERT_FALSE(entry->isActive);
    ASSERT_EQ(firstQueryWorks * 2 * 2, entry->works);

    // Run the query yet again. This time, an active cache entry should be created.
    mps = runMultiPlanner(_expCtx.get(), nss, coll, 5);
    ASSERT_EQ(cache->size(), 1U);
    entry = assertGet(cache->getEntry(key));
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
    const CollectionPtr& coll = ctx.getCollection();

    const auto cq = makeCanonicalQuery(_opCtx.get(), nss, BSON("foo" << 7));
    auto key = plan_cache_key_factory::make<PlanCacheKey>(*cq, coll);

    // Run an index scan and collection scan, searching for {foo: 7}.
    auto mps = runMultiPlanner(_expCtx.get(), nss, coll, 7);

    // Be sure that an _active_ cache entry was added.
    PlanCache* cache = CollectionQueryInfo::get(coll).getPlanCache();
    ASSERT_EQ(cache->get(key).state, PlanCache::CacheEntryState::kPresentActive);

    // Run the multi-planner again. The entry should still be active.
    mps = runMultiPlanner(_expCtx.get(), nss, coll, 5);

    ASSERT_EQ(cache->get(key).state, PlanCache::CacheEntryState::kPresentActive);
}

// Case in which we select a blocking plan as the winner, and a non-blocking plan
// is available as a backup.
TEST_F(QueryStageMultiPlanTest, MPSBackupPlan) {
    // Data is just a single {_id: 1, a: 1, b: 1} document.
    insert(BSON("_id" << 1 << "a" << 1 << "b" << 1));

    // Indices on 'a' and 'b'.
    addIndex(BSON("a" << 1));
    addIndex(BSON("b" << 1));

    AutoGetCollectionForReadCommand collection(_opCtx.get(), nss);

    // Query for both 'a' and 'b' and sort on 'b'.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(BSON("a" << 1 << "b" << 1));
    findCommand->setSort(BSON("b" << 1));
    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    auto key = plan_cache_key_factory::make<PlanCacheKey>(*cq, collection.getCollection());

    // Force index intersection.
    bool forceIxisectOldValue = internalQueryForceIntersectionPlans.load();
    internalQueryForceIntersectionPlans.store(true);

    // Get planner params.
    QueryPlannerParams plannerParams;
    MultipleCollectionAccessor collectionsAccessor(collection.getCollection());
    plannerParams.fillOutPlannerParams(
        _opCtx.get(), *cq.get(), collectionsAccessor, true /* shouldIgnoreQuerySettings */);

    // Plan.
    auto statusWithMultiPlanSolns = QueryPlanner::plan(*cq, plannerParams);
    ASSERT_OK(statusWithMultiPlanSolns.getStatus());
    auto solutions = std::move(statusWithMultiPlanSolns.getValue());

    // We expect a plan using index {a: 1} and plan using index {b: 1} and
    // an index intersection plan.
    ASSERT_EQUALS(solutions.size(), 3U);

    // Fill out the MultiPlanStage.
    unique_ptr<MultiPlanStage> mps(
        new MultiPlanStage(_expCtx.get(), &collection.getCollection(), cq.get()));
    unique_ptr<WorkingSet> ws(new WorkingSet());
    // Put each solution from the planner into the MPR.
    for (size_t i = 0; i < solutions.size(); ++i) {
        auto&& root = stage_builder::buildClassicExecutableTree(
            _opCtx.get(), &collection.getCollection(), *cq, *solutions[i], ws.get());
        mps->addPlan(std::move(solutions[i]), std::move(root), ws.get());
    }

    // This sets a backup plan.
    NoopYieldPolicy yieldPolicy(_expCtx->opCtx, _clock);
    ASSERT_OK(mps->pickBestPlan(&yieldPolicy));
    ASSERT(mps->bestPlanChosen());
    ASSERT(mps->hasBackupPlan());

    // We should have picked the index intersection plan due to forcing ixisect.
    auto soln = static_cast<const MultiPlanStage*>(mps.get())->bestSolution();
    ASSERT(QueryPlannerTestLib::solutionMatches("{sort: {pattern: {b: 1}, limit: 0, node:"
                                                "{fetch: {node: {andSorted: {nodes: ["
                                                "{ixscan: {filter: null, pattern: {a:1}}},"
                                                "{ixscan: {filter: null, pattern: {b:1}}}]}}}}}}",
                                                soln->root())
               .isOK());

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
    ASSERT_BSONOBJ_EQ(expectedDoc, member->doc.value().toBson());

    // The blocking plan became unblocked, so we should no longer have a backup plan,
    // and the winning plan should still be the index intersection one.
    ASSERT(!mps->hasBackupPlan());
    soln = static_cast<const MultiPlanStage*>(mps.get())->bestSolution();
    ASSERT(QueryPlannerTestLib::solutionMatches("{sort: {pattern: {b: 1}, limit: 0, node:"
                                                "{fetch: {node: {andSorted: {nodes: ["
                                                "{ixscan: {filter: null, pattern: {a:1}}},"
                                                "{ixscan: {filter: null, pattern: {b:1}}}]}}}}}}",
                                                soln->root())
               .isOK());

    // Restore index intersection force parameter.
    internalQueryForceIntersectionPlans.store(forceIxisectOldValue);
}

/**
 * Allocates a new WorkingSetMember with data 'dataObj' in 'ws', and adds the WorkingSetMember
 * to 'qds'.
 */
void addMember(MockStage* mockStage, WorkingSet* ws, BSONObj dataObj) {
    WorkingSetID id = ws->allocate();
    WorkingSetMember* wsm = ws->get(id);
    wsm->doc = {SnapshotId(), Document{BSON("x" << 1)}};
    wsm->transitionToOwnedObj();
    mockStage->enqueueAdvanced(id);
}

// Test the structure and values of the explain output.
TEST_F(QueryStageMultiPlanTest, MPSExplainAllPlans) {
    // Insert a document to create the collection.
    insert(BSON("x" << 1));

    const int nDocs = 500;

    auto ws = std::make_unique<WorkingSet>();
    auto firstPlan = std::make_unique<MockStage>(_expCtx.get(), ws.get());
    const auto* firstPlanPtr = firstPlan.get();

    auto secondPlan = std::make_unique<MockStage>(_expCtx.get(), ws.get());

    for (int i = 0; i < nDocs; ++i) {
        addMember(firstPlan.get(), ws.get(), BSON("x" << 1));

        // Make the second plan slower by inserting a NEED_TIME between every result.
        addMember(secondPlan.get(), ws.get(), BSON("x" << 1));
        secondPlan->enqueueStateCode(PlanStage::NEED_TIME);
    }

    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(BSON("x" << 1));
    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    unique_ptr<MultiPlanStage> mps =
        std::make_unique<MultiPlanStage>(_expCtx.get(), &ctx.getCollection(), cq.get());

    // Put each plan into the MultiPlanStage. Takes ownership of 'firstPlan' and 'secondPlan'.
    mps->addPlan(std::make_unique<QuerySolution>(), std::move(firstPlan), ws.get());
    mps->addPlan(std::make_unique<QuerySolution>(), std::move(secondPlan), ws.get());

    // The first candidate plan should have won.
    auto planYieldPolicy = makeClassicYieldPolicy(opCtx(),
                                                  nss,
                                                  static_cast<PlanStage*>(mps.get()),
                                                  PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                  &ctx.getCollection());
    ASSERT_OK(mps->pickBestPlan(planYieldPolicy.get()));
    ASSERT_TRUE(mps->bestPlanChosen());
    ASSERT_EQ(getBestPlanRoot(mps.get()), firstPlanPtr);

    auto execResult = plan_executor_factory::make(_expCtx,
                                                  std::move(ws),
                                                  std::move(mps),
                                                  &ctx.getCollection(),
                                                  PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                                  QueryPlannerParams::DEFAULT,
                                                  nss);
    ASSERT_OK(execResult);
    auto exec = std::move(execResult.getValue());
    BSONObjBuilder bob;
    Explain::explainStages(exec.get(),
                           ctx.getCollection(),
                           ExplainOptions::Verbosity::kExecAllPlans,
                           BSONObj(),
                           SerializationContext::stateCommandReply(),
                           BSONObj(),
                           &bob);
    BSONObj explained = bob.done();
    ASSERT_EQ(explained["executionStats"]["nReturned"].Int(), nDocs);
    ASSERT_EQ(explained["executionStats"]["executionStages"]["needTime"].Int(), 0);
    auto allPlansStats = explained["executionStats"]["allPlansExecution"].Array();
    ASSERT_EQ(allPlansStats.size(), 2UL);
    for (auto&& planStats : allPlansStats) {
        int maxEvaluationResults = internalQueryPlanEvaluationMaxResults.load();
        ASSERT_EQ(planStats["executionStages"]["stage"].String(), "MOCK");
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
    RAIIServerParameterControllerForTest controller("internalQueryFrameworkControl",
                                                    "forceClassicEngine");

    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        insert(BSON("foo" << (i % 10)));
    }

    // Add two indices to give more plans.
    addIndex(BSON("foo" << 1));
    addIndex(BSON("foo" << -1 << "bar" << 1));

    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);
    const CollectionPtr& coll = ctx.getCollection();

    // Create the executor (Matching all documents).
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(BSON("foo" << BSON("$gte" << 0)));
    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    auto exec = uassertStatusOK(getExecutorFind(opCtx(),
                                                MultipleCollectionAccessor{coll},
                                                std::move(cq),
                                                PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY));

    auto execImpl = dynamic_cast<PlanExecutorImpl*>(exec.get());
    ASSERT(execImpl);
    ASSERT_EQ(execImpl->getRootStage()->stageType(), StageType::STAGE_MULTI_PLAN);

    // Execute the plan executor util EOF, discarding the results.
    {
        BSONObj obj;
        while (exec->getNext(&obj, nullptr) == PlanExecutor::ADVANCED) {
            // Do nothing with the documents produced by the executor.
        }
    }

    PlanSummaryStats stats;
    exec->getPlanExplainer().getSummaryStats(&stats);

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

    AutoGetCollectionForReadCommand coll(_opCtx.get(), nss);

    // Plan 0: IXScan over foo == 7
    // Every call to work() returns something so this should clearly win (by current scoring
    // at least).
    unique_ptr<WorkingSet> sharedWs(new WorkingSet());
    unique_ptr<PlanStage> ixScanRoot =
        getIxScanPlan(_expCtx.get(), coll.getCollection(), sharedWs.get(), 7);

    // Make the filter.
    BSONObj filterObj = BSON("foo" << 7);
    unique_ptr<MatchExpression> filter = makeMatchExpressionFromFilter(_expCtx.get(), filterObj);
    unique_ptr<PlanStage> collScanRoot =
        getCollScanPlan(_expCtx.get(), coll.getCollection(), sharedWs.get(), filter.get());


    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(filterObj);
    auto canonicalQuery = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    MultiPlanStage multiPlanStage(
        _expCtx.get(), &coll.getCollection(), canonicalQuery.get(), PlanCachingMode::NeverCache);
    multiPlanStage.addPlan(createQuerySolution(), std::move(ixScanRoot), sharedWs.get());
    multiPlanStage.addPlan(createQuerySolution(), std::move(collScanRoot), sharedWs.get());

    AlwaysTimeOutYieldPolicy alwaysTimeOutPolicy(_expCtx->opCtx,
                                                 serviceContext()->getFastClockSource());
    const auto status = multiPlanStage.pickBestPlan(&alwaysTimeOutPolicy);
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, status);
    ASSERT_STRING_CONTAINS(status.reason(), "error while multiplanner was selecting best plan");
}

TEST_F(QueryStageMultiPlanTest, ShouldReportErrorIfKilledDuringPlanning) {
    const int N = 5000;
    for (int i = 0; i < N; ++i) {
        insert(BSON("foo" << (i % 10)));
    }

    // Add two indices to give more plans.
    addIndex(BSON("foo" << 1));
    addIndex(BSON("foo" << -1 << "bar" << 1));

    AutoGetCollectionForReadCommand coll(_opCtx.get(), nss);

    // Plan 0: IXScan over foo == 7
    // Every call to work() returns something so this should clearly win (by current scoring
    // at least).
    unique_ptr<WorkingSet> sharedWs(new WorkingSet());
    unique_ptr<PlanStage> ixScanRoot =
        getIxScanPlan(_expCtx.get(), coll.getCollection(), sharedWs.get(), 7);

    // Plan 1: CollScan.
    BSONObj filterObj = BSON("foo" << 7);
    unique_ptr<MatchExpression> filter = makeMatchExpressionFromFilter(_expCtx.get(), filterObj);
    unique_ptr<PlanStage> collScanRoot =
        getCollScanPlan(_expCtx.get(), coll.getCollection(), sharedWs.get(), filter.get());

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(BSON("foo" << BSON("$gte" << 0)));
    auto canonicalQuery = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    MultiPlanStage multiPlanStage(
        _expCtx.get(), &coll.getCollection(), canonicalQuery.get(), PlanCachingMode::NeverCache);
    multiPlanStage.addPlan(createQuerySolution(), std::move(ixScanRoot), sharedWs.get());
    multiPlanStage.addPlan(createQuerySolution(), std::move(collScanRoot), sharedWs.get());

    AlwaysPlanKilledYieldPolicy alwaysPlanKilledYieldPolicy(_expCtx->opCtx,
                                                            serviceContext()->getFastClockSource());
    ASSERT_EQ(ErrorCodes::QueryPlanKilled,
              multiPlanStage.pickBestPlan(&alwaysPlanKilledYieldPolicy));
}

/**
 * A PlanStage for testing which always throws exceptions.
 */
class ThrowyPlanStage : public PlanStage {
protected:
    StageState doWork(WorkingSetID* out) {
        uasserted(ErrorCodes::InternalError, "Mwahahaha! You've fallen into my trap.");
    }

public:
    ThrowyPlanStage(ExpressionContext* expCtx) : PlanStage("throwy", expCtx) {}
    bool isEOF() final {
        return false;
    }
    StageType stageType() const final {
        return STAGE_UNKNOWN;
    }
    virtual std::unique_ptr<PlanStageStats> getStats() final {
        return nullptr;
    }
    virtual const SpecificStats* getSpecificStats() const final {
        return nullptr;
    }
};

TEST_F(QueryStageMultiPlanTest, AddsContextDuringException) {
    insert(BSON("foo" << 10));
    AutoGetCollectionForReadCommand ctx(_opCtx.get(), nss);

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(BSON("fake"
                                << "query"));
    auto canonicalQuery = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    MultiPlanStage multiPlanStage(
        _expCtx.get(), &ctx.getCollection(), canonicalQuery.get(), PlanCachingMode::NeverCache);
    unique_ptr<WorkingSet> sharedWs(new WorkingSet());
    multiPlanStage.addPlan(
        createQuerySolution(), std::make_unique<ThrowyPlanStage>(_expCtx.get()), sharedWs.get());
    multiPlanStage.addPlan(
        createQuerySolution(), std::make_unique<ThrowyPlanStage>(_expCtx.get()), sharedWs.get());

    NoopYieldPolicy yieldPolicy(_expCtx->opCtx, _clock);
    auto status = multiPlanStage.pickBestPlan(&yieldPolicy);
    ASSERT_EQ(ErrorCodes::InternalError, status);
    ASSERT_STRING_CONTAINS(status.reason(), "error while multiplanner was selecting best plan");
}

}  // namespace
}  // namespace mongo
