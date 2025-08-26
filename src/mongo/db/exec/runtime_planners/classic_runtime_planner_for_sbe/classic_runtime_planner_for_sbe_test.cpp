/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#include "mongo/bson/json.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/classic_runtime_planner_for_sbe_test_util.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner_for_sbe/planner_interface.h"
#include "mongo/db/local_catalog/catalog_test_fixture.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/index_descriptor.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role.h"
#include "mongo/db/pipeline/document_source_internal_projection.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/logv2/log.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::classic_runtime_planner_for_sbe {
namespace {

const NamespaceString kNss = NamespaceString::createNamespaceString_forTest("test.collection");
const BSONObj kFindFilter = fromjson("{a: {$gte: 0}, b: {$gte: 0}}");
const BSONObj kAddFieldsSpec = fromjson(R"({sum: {$add: ["$a", "$b"]}})");

const BSONObj kRootedOrFilter =
    fromjson("{$or: [{a: {$lte: 10}, b: {$gte: 10}}, {c: {$lte: 90}, d: {$gte: 90}}]}");

/**
 * Fixture for classic_runtime_planner_for_sbe::PlannerInterface implementations. As a test query,
 * it uses an aggregation pipeline [{$match: <match statement>}, {$addFields: <addFields statement>]
 * where $match is pushed down to the find query and $addFields is left to be a single-stage agg
 * pipeline.
 */
class ClassicRuntimePlannerForSbeTest : public CatalogTestFixture {
protected:
    void setUp() final {
        CatalogTestFixture::setUp();
        OperationContext* opCtx = operationContext();
        expCtx = make_intrusive<ExpressionContextForTest>(opCtx, kNss);

        ASSERT_OK(storageInterface()->createCollection(opCtx, kNss, CollectionOptions{}));
    }

    void tearDown() final {
        _collections.reset();
        expCtx.reset();
        CatalogTestFixture::tearDown();
    }

    /**
     * Creates a pair of CanonicalQuery & PlannerDataForSBE for the following pipeline: [
     *   {$match: 'findFilter'}},
     *   {$addFields: 'addFieldsSpec'}}
     * ]
     * Defaults to kFindFilter and kAddFieldsSpec for 'findFilter' and 'addFieldsSpec' respectively
     * if arguments are not supplied. If addFieldsSpec is empty, $addFields stage won't be added.
     */
    auto createPlannerData(BSONObj findFilter = kFindFilter,
                           BSONObj addFieldsSpec = kAddFieldsSpec) {
        auto findCommand = std::make_unique<FindCommandRequest>(kNss);
        findCommand->setFilter(findFilter);

        std::vector<boost::intrusive_ptr<DocumentSource>> pipeline;
        if (!addFieldsSpec.isEmpty()) {
            pipeline.emplace_back(make_intrusive<DocumentSourceInternalProjection>(
                expCtx, kAddFieldsSpec, InternalProjectionPolicyEnum::kAddFields));
        }
        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx,
            .parsedFind = ParsedFindCommandParams{.findCommand = std::move(findCommand)},
            .pipeline = std::move(pipeline)});
        cq->setSbeCompatible(true);

        // Whether or not to use the SBE plan cache depends on "featureFlagSbeFull".
        const bool useSbePlanCache = feature_flags::gFeatureFlagSbeFull.isEnabled();

        auto params = std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForTest{});
        params->mainCollectionInfo.indexes = _indices;
        PlannerDataForSBE plannerData{operationContext(),
                                      cq.get(),
                                      std::make_unique<WorkingSet>(),
                                      *_collections,
                                      std::move(params),
                                      PlanYieldPolicy::YieldPolicy::INTERRUPT_ONLY,
                                      /* cachedPlanHash */ boost::none,
                                      makeYieldPolicy(),
                                      useSbePlanCache};
        return std::make_pair(std::move(cq), std::move(plannerData));
    }

    std::vector<BSONObj> getResultDocumentsAndAssertExecState(size_t expectedDocCount,
                                                              PlanExecutor* exec) {
        std::vector<BSONObj> result;
        result.reserve(expectedDocCount);
        for (size_t i = 0; i < expectedDocCount; ++i) {
            BSONObj out;
            auto state = exec->getNext(&out, nullptr);
            ASSERT_EQ(PlanExecutor::ExecState::ADVANCED, state);
            result.push_back(out.getOwned());
        }
        Document doc;
        ASSERT_EQ(PlanExecutor::ExecState::IS_EOF, exec->getNextDocument(doc));
        return result;
    }

    /**
     * For a given PlanExecutor, asserts the following:
     * 1. PlanExecutor returns EOF after exactly expectedSums.size() documents.
     * 2. Each returned document have a field "sum" with an integer value, equal to corresponding
     *    element in expectedSums.
     */
    void assertPlanExecutorReturnsCorrectSums(std::vector<int> expectedSums, PlanExecutor* exec) {
        size_t docCount = expectedSums.size();
        auto result = getResultDocumentsAndAssertExecState(docCount, exec);
        for (size_t i = 0; i < docCount; ++i) {
            ASSERT_EQ(expectedSums[i], result[i].getField("sum").Int());
        }
    }

    /**
     * Create an 'index' on an empty collection with name 'indexName' and add the index to
     * QueryPlannerParams.
     */
    void createIndexOnEmptyCollection(OperationContext* opCtx,
                                      BSONObj index,
                                      std::string indexName) {
        auto acquisition = acquireCollection(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kWrite),
            MODE_X);
        CollectionWriter coll(opCtx, &acquisition);

        WriteUnitOfWork wunit(opCtx);
        auto indexCatalog = coll.getWritableCollection(opCtx)->getIndexCatalog();
        ASSERT(indexCatalog);
        auto indexesBefore = indexCatalog->numIndexesReady();
        ASSERT_OK(indexCatalog
                      ->createIndexOnEmptyCollection(
                          opCtx, coll.getWritableCollection(opCtx), makeIndexSpec(index, indexName))
                      .getStatus());
        wunit.commit();
        ASSERT_EQ(indexesBefore + 1, indexCatalog->numIndexesReady());

        // The QueryPlannerParams should also have information about the index to consider it when
        // actually doing the planning.
        _indices.push_back(IndexEntry(index,
                                      IndexNames::nameToType(IndexNames::findPluginName(index)),
                                      IndexConfig::kLatestIndexVersion,
                                      false,
                                      {},
                                      {},
                                      false,
                                      false,
                                      IndexEntry::Identifier{indexName},
                                      nullptr,
                                      BSONObj(),
                                      nullptr,
                                      nullptr));
    }

    BSONObj makeIndexSpec(BSONObj index, StringData indexName) {
        return BSON("v" << IndexConfig::kLatestIndexVersion << "key" << index << "name"
                        << indexName);
    }

    void acquireCollectionForRead() {
        auto opCtx = operationContext();
        _collections.emplace(acquireCollectionMaybeLockFree(
            opCtx,
            CollectionAcquisitionRequest::fromOpCtx(opCtx, kNss, AcquisitionPrerequisites::kRead)));
    }

    /**
     * Creates indexes {a: 1}, {b: 1}, {c: 1}, {d: 1} and inserts 100 docs with {a: i, b: i, c: i,
     * d: i}.
     *
     * Acquires the collection in read mode and clears both the SBE and classic plan caches.
     */
    void setUpSubPlannerTest() {
        auto opCtx = operationContext();

        createIndexOnEmptyCollection(opCtx, BSON("a" << 1), "a_1");
        createIndexOnEmptyCollection(opCtx, BSON("b" << 1), "b_1");
        createIndexOnEmptyCollection(opCtx, BSON("c" << 1), "c_1");
        createIndexOnEmptyCollection(opCtx, BSON("d" << 1), "d_1");

        std::vector<InsertStatement> docs;
        for (int i = 0; i < 100; ++i) {
            docs.emplace_back(InsertStatement(
                BSON("_id" << OID::gen() << "a" << i << "b" << i << "c" << i << "d" << i)));
        }

        ASSERT_OK(storageInterface()->insertDocuments(opCtx, kNss, docs));

        acquireCollectionForRead();
        // Clear both plan caches in order to make sure that we don't see cache entries from
        // previous runs.
        sbe::getPlanCache(operationContext()).clear();
        getClassicPlanCache().clear();
    }

    /**
     * Creates the subplanner with a rooted $or filter and returns the resulting PlanExecutor.
     * Asserts that the subplanner reports the given number of per-$or branch multi-planner
     * invocations.
     */
    std::unique_ptr<PlanExecutor, PlanExecutor::Deleter> getExecutorWithSubPlanning(
        int expectedPerBranchMultiplans) {
        // createPlannerDataForSBE adds a cqPipeline with an $addFields stage. Based on the data, we
        // expect the plan for the first branch to use the "a" index and the plan for the second
        // branch to use the "d" index.
        auto [cq, plannerData] = createPlannerData(kRootedOrFilter);

        SubPlanner planner{std::move(plannerData)};
        ASSERT_EQ(planner.numPerBranchMultiplans(), expectedPerBranchMultiplans);
        auto exec = planner.makeExecutor(std::move(cq));
        assertPlanExecutorReturnsCorrectSums({20, 180}, exec.get());
        return exec;
    }

    std::pair<std::vector<std::unique_ptr<QuerySolution>>, std::vector<int>>
    createVirtualScanQuerySolutionsForDefaultFilter(int resultDocCount, const CanonicalQuery* cq) {
        std::vector<std::unique_ptr<QuerySolution>> solutions;
        std::vector<int> expectedSums;
        expectedSums.reserve(resultDocCount);

        auto addVirtualScanSolution = [&](std::vector<BSONArray> docs) {
            auto virtScan = std::make_unique<VirtualScanNode>(
                std::move(docs), VirtualScanNode::ScanType::kCollScan, false /*hasRecordId*/);
            virtScan->filter = cq->getPrimaryMatchExpression()->clone();
            auto solution = std::make_unique<QuerySolution>();
            solution->cacheData = std::make_unique<SolutionCacheData>();
            solution->cacheData->virtualScanData = std::make_unique<VirtualScanCacheData>(
                virtScan->docs, false /* hasRecordId */, BSONObj() /* indexKeyPattern */);
            solution->setRoot(std::move(virtScan));
            solution->cacheData->solnType = SolutionCacheData::VIRTSCAN_SOLN;
            solutions.push_back(std::move(solution));
        };

        {  // Generate a plan with every second document matching the filter. We
           // expect the multi-planner to choose this plan.
            std::vector<BSONArray> docs;
            docs.reserve(2 * resultDocCount);
            for (int i = 1; i <= resultDocCount; ++i) {
                docs.push_back(BSON_ARRAY(BSON("a" << i << "b" << 1)));
                docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 1)));
                expectedSums.push_back(i + 1);
            }
            addVirtualScanSolution(std::move(docs));
        }
        {  // Generate a plan with every third document matching the filter.
            std::vector<BSONArray> docs;
            docs.reserve(3 * resultDocCount);
            for (int i = 1; i <= resultDocCount; ++i) {
                docs.push_back(BSON_ARRAY(BSON("a" << i << "b" << 2)));
                docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 2)));
                docs.push_back(BSON_ARRAY(BSON("a" << -i << "b" << 2)));
            }
            addVirtualScanSolution(std::move(docs));
        }
        return {std::move(solutions), std::move(expectedSums)};
    }

    /**
     * Returns a PlannerGenerator for a classic cache entry, using the given canonical query and
     * planner data. The PlannerGenerator can then be used to make a runtime planner. The caller
     * may also swap the SBE plan created by the PlannerGenerator, in order to simulate a plan that
     * has other behavior.
     *
     * The 'numReads' argument is used to override the 'numReads' value stored in the cache entry,
     * as this will always be 0 when a VirtualScan plan is used.
     */
    std::pair<PlanCacheKey, std::unique_ptr<PlannerGeneratorFromClassicCacheEntry>>
    makePlannerGeneratorFromClassicCache(const CanonicalQuery* cq,
                                         PlannerDataForSBE plannerData,
                                         NumReads numReads) {
        auto planCacheKey = plan_cache_key_factory::make<PlanCacheKey>(
            *cq, plannerData.collections.getMainCollection());

        auto cacheEntry = getClassicPlanCache().getCacheEntryIfActive(planCacheKey);
        ASSERT_TRUE(static_cast<bool>(cacheEntry));

        invariant(cacheEntry->cachedPlan->virtualScanData);
        auto statusWithQs =
            QueryPlanner::planFromCache(*cq, *plannerData.plannerParams, *cacheEntry->cachedPlan);
        ASSERT(statusWithQs.isOK());
        auto querySolution = std::move(statusWithQs.getValue());

        querySolution = QueryPlanner::extendWithAggPipeline(
            *plannerData.cq,
            std::move(querySolution),
            plannerData.plannerParams->secondaryCollectionsInfo);

        return {planCacheKey,
                std::make_unique<PlannerGeneratorFromClassicCacheEntry>(
                    std::move(plannerData), std::move(querySolution), numReads.value)};
    }


    /**
     * Inserts 200 documents with {a: i, b: 1} which is the same as the outputs from the
     * VirtualScanNode from createVirtualScanQuerySolutionsForDefaultFilter(). Also responsible for
     * acquiring the test collection.
     */
    void setUpCachedPlannerTest() {
        auto opCtx = operationContext();
        std::vector<InsertStatement> docs;
        for (int i = 1; i <= 200; ++i) {
            docs.emplace_back(InsertStatement(BSON("_id" << OID::gen() << "a" << i << "b" << 1)));
        }

        ASSERT_OK(storageInterface()->insertDocuments(opCtx, kNss, docs));

        acquireCollectionForRead();
    }

    /**
     * Helper to run unit tests in two configurations: once with featureFlagSbeFull disabled, and
     * then once again with featureFlagSbeFull enabled.
     *
     * TODO SERVER-90496: Ideally, this can be done by calling the base 'run' function twice in the
     * test fixture's implementation of 'run()'. This is non-trivial because it requires making it
     * so that all setup and teardown code is executed exactly once (as opposed to once for each
     * time 'run' is called) as some set up/tear down code runs when constructing/destructing the
     * test fixture itself. Clean up the setUp()/tearDown() functions to allow for this.
     */
    void testSbeFullOnAndOffFn(std::function<void(bool)> testFn) {
        try {
            for (auto value : {false, true}) {
                RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", value);
                LOGV2(9049201,
                      "Running test with 'featureFlagSbeFull' set to {value}",
                      "value"_attr = value);
                testFn(value);
            }
        } catch (...) {
            LOGV2(9049203, "Exception while testing classic runtime planner for SBE");
            throw;
        }
    }

    PlanCache& getClassicPlanCache() {
        auto&& collQueryInfo = CollectionQueryInfo::get(_collections->getMainCollection());
        auto cache = collQueryInfo.getPlanCache();
        ASSERT(cache);
        return *cache;
    }

    /**
     * The plan cache size functions exposed by the plan caches themselves may express the size in
     * terms of number of bytes rather than number of entries, depending on whether the plan cache
     * maximum size is set in units of bytes or entry count.
     *
     * This template function ensures that there is always a an easy way to count the number of
     * entries in either the SBE or classic plan caches.
     */
    int numEntriesInCache(auto&& planCache) {
        int count = 0;
        planCache.forEach([&](const auto&, const auto&) { ++count; });
        return count;
    }

    boost::intrusive_ptr<ExpressionContext> expCtx;

private:
    std::unique_ptr<PlanYieldPolicySBE> makeYieldPolicy() {
        return PlanYieldPolicySBE::make(
            operationContext(),
            PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
            operationContext()->getServiceContext()->getFastClockSource(),
            0,
            Milliseconds::zero(),
            PlanYieldPolicy::YieldThroughAcquisitions{});
    }

    boost::optional<MultipleCollectionAccessor> _collections;
    std::vector<IndexEntry> _indices;
};

TEST_F(ClassicRuntimePlannerForSbeTest, SingleSolutionPassthroughPlannerCreatesCacheEntry) {
    acquireCollectionForRead();

    testSbeFullOnAndOffFn([&](bool sbeFullEnabled) {
        static const std::vector<BSONArray> kDocs = {
            BSON_ARRAY(int64_t{0} << BSON("a" << 1 << "b" << 2)),
            BSON_ARRAY(int64_t{1} << BSON("a" << 2 << "b" << 2)),
            BSON_ARRAY(int64_t{2} << BSON("a" << 3 << "b" << 2))};
        static const bool kHasRecordId = true;
        static const BSONObj kIndexKeyPattern = BSON("a" << 1);
        {  // Run SingleSolutionPassthroughPlanner to create pinned cached entry.

            auto root = std::make_unique<VirtualScanNode>(kDocs,
                                                          VirtualScanNode::ScanType::kIxscan,
                                                          kHasRecordId /*hasRecordId*/,
                                                          kIndexKeyPattern);
            auto solution = std::make_unique<QuerySolution>();

            solution->cacheData = std::make_unique<SolutionCacheData>();
            solution->cacheData->virtualScanData =
                std::make_unique<VirtualScanCacheData>(kDocs, kHasRecordId, kIndexKeyPattern);
            solution->cacheData->solnType = SolutionCacheData::VIRTSCAN_SOLN;
            solution->setRoot(std::move(root));

            auto [cq, plannerData] = createPlannerData();
            SingleSolutionPassthroughPlanner planner{std::move(plannerData), std::move(solution)};
            auto exec = planner.makeExecutor(std::move(cq));
            assertPlanExecutorReturnsCorrectSums({3, 4, 5}, exec.get());
        }

        if (sbeFullEnabled) {  // Run CachedPlanner to execute the cached plan.
            auto [cq, plannerData] = createPlannerData();
            auto planCacheKey =
                plan_cache_key_factory::make(*plannerData.cq, plannerData.collections);
            auto&& planCache = sbe::getPlanCache(operationContext());
            auto cacheEntry = planCache.getCacheEntryIfActive(planCacheKey);
            ASSERT_TRUE(cacheEntry);
            ASSERT_TRUE(cacheEntry->isPinned()) << "Expects single solution to be pinned in cache.";
            auto cachedPlanner =
                makePlannerForSbeCacheEntry(std::move(plannerData), std::move(cacheEntry), {});
            auto cachedExec = cachedPlanner->makeExecutor(std::move(cq));
            PlanSummaryStats stats;
            cachedExec->getPlanExplainer().getSummaryStats(&stats);
            ASSERT_FALSE(stats.replanReason) << "Single solution does not need to be replanned.";
            assertPlanExecutorReturnsCorrectSums({3, 4, 5}, cachedExec.get());
        } else {
            // TODO: SERVER-90880 Cache single-solution plans in classic.
            // No cache entry is created when using the classic cache.
            auto [cq, plannerData] = createPlannerData();
            auto planCacheKey = plan_cache_key_factory::make<PlanCacheKey>(
                *cq, plannerData.collections.getMainCollection());

            auto cacheEntry = getClassicPlanCache().getCacheEntryIfActive(planCacheKey);
            ASSERT_FALSE(static_cast<bool>(cacheEntry));
        }
    });
}

TEST_F(ClassicRuntimePlannerForSbeTest, MultiPlannerPicksMoreEfficientPlan) {
    acquireCollectionForRead();

    testSbeFullOnAndOffFn([&](bool sbeFullEnabled) {
        // Ensures that cache entries are available immediately.
        bool previousQueryKnobValue = internalQueryCacheDisableInactiveEntries.swap(true);
        ON_BLOCK_EXIT(
            [&] { internalQueryCacheDisableInactiveEntries.store(previousQueryKnobValue); });

        auto [cq, plannerData] = createPlannerData();
        auto [solutions, expectedSums] =
            createVirtualScanQuerySolutionsForDefaultFilter(200 /*resultDocCount*/, plannerData.cq);
        {
            MultiPlanner planner{
                std::move(plannerData), std::move(solutions), true /*shouldWriteToCache*/};
            auto exec = planner.makeExecutor(std::move(cq));
            assertPlanExecutorReturnsCorrectSums(expectedSums, exec.get());
        }

        {  // Run CachedPlanner to execute the cached plan.
            std::unique_ptr<PlannerInterface> cachedPlanner;
            auto [cq, plannerData] = createPlannerData();
            if (sbeFullEnabled) {
                auto planCacheKey =
                    plan_cache_key_factory::make(*plannerData.cq, plannerData.collections);
                auto&& planCache = sbe::getPlanCache(operationContext());
                auto cacheEntry = planCache.getCacheEntryIfActive(planCacheKey);
                ASSERT_TRUE(cacheEntry);

                cachedPlanner =
                    makePlannerForSbeCacheEntry(std::move(plannerData), std::move(cacheEntry), {});
            } else {
                auto [classicCacheKey, plannerGenerator] = makePlannerGeneratorFromClassicCache(
                    cq.get(), std::move(plannerData), NumReads{10000});
                cachedPlanner = plannerGenerator->makePlanner();
            }

            auto cachedExec = cachedPlanner->makeExecutor(std::move(cq));
            assertPlanExecutorReturnsCorrectSums(std::move(expectedSums), cachedExec.get());
        }
    });
}

TEST_F(ClassicRuntimePlannerForSbeTest, MultiPlannerUsesEofOptimization) {
    if (kDebugBuild) {
        // EOF optimization is not used in debug builds.
        return;
    }

    acquireCollectionForRead();

    testSbeFullOnAndOffFn([&](bool sbeFullEnabled) {
        {
            // When the query has 200 result documents, no plan will reach EOF during multi-planner,
            // so we should use SBE.
            auto [cq, plannerData] = createPlannerData(kFindFilter, BSONObj{} /*addFieldsSpec*/);
            auto [solutions, expectedSums] = createVirtualScanQuerySolutionsForDefaultFilter(
                200 /*resultDocCount*/, plannerData.cq);
            MultiPlanner planner{
                std::move(plannerData), std::move(solutions), true /*shouldWriteToCache*/};
            auto exec = planner.makeExecutor(std::move(cq));
            ASSERT_EQ(exec->getPlanExplainer().getVersion(), "2");
        }

        {
            // When the query has only 50 result documents, winning plan will reach EOF during
            // multi-planner, so we should use the Classic Engine.
            auto [cq, plannerData] = createPlannerData(kFindFilter, BSONObj{} /*addFieldsSpec*/);
            auto [solutions, expectedSums] = createVirtualScanQuerySolutionsForDefaultFilter(
                50 /*resultDocCount*/, plannerData.cq);
            MultiPlanner planner{
                std::move(plannerData), std::move(solutions), true /*shouldWriteToCache*/};
            auto exec = planner.makeExecutor(std::move(cq));
            ASSERT_EQ(exec->getPlanExplainer().getVersion(), "1");
        }
    });
}

TEST_F(ClassicRuntimePlannerForSbeTest, SbePlanCacheIsUpdatedDuringEofOptimization) {
    if (kDebugBuild) {
        // EOF optimization is not used in debug builds.
        return;
    }
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);

    acquireCollectionForRead();

    static const int kDocCount = 50;
    // Run the query twice to ensure active cache entry.
    std::vector<BSONObj> queryResult;
    for (int i = 0; i < 2; ++i) {
        auto [cq, plannerData] = createPlannerData(kFindFilter, BSONObj{} /*addFieldsSpec*/);
        auto [solutions, expectedSums] =
            createVirtualScanQuerySolutionsForDefaultFilter(kDocCount, plannerData.cq);
        MultiPlanner planner{
            std::move(plannerData), std::move(solutions), true /*shouldWriteToCache*/};
        auto exec = planner.makeExecutor(std::move(cq));
        ASSERT_EQ(exec->getPlanExplainer().getVersion(), "1");
        queryResult = getResultDocumentsAndAssertExecState(kDocCount, exec.get());
        for (int i = 1; i <= kDocCount; ++i) {
            ASSERT_BSONOBJ_EQ(BSON("a" << i << "b" << 1), queryResult[i - 1]);
        }
    }
    {  // Run CachedPlanner to execute the cached plan.
        auto [cq, plannerData] = createPlannerData(kFindFilter, BSONObj{} /*addFieldsSpec*/);
        auto planCacheKey = plan_cache_key_factory::make(*plannerData.cq, plannerData.collections);
        auto&& planCache = sbe::getPlanCache(operationContext());
        auto cacheEntry = planCache.getCacheEntryIfActive(planCacheKey);
        ASSERT_TRUE(cacheEntry);
        auto cachedPlanner =
            makePlannerForSbeCacheEntry(std::move(plannerData), std::move(cacheEntry), {});
        auto cachedExec = cachedPlanner->makeExecutor(std::move(cq));
        ASSERT_EQ(cachedExec->getPlanExplainer().getVersion(), "2");
        auto cachedResult = getResultDocumentsAndAssertExecState(kDocCount, cachedExec.get());
        for (size_t i = 0; i < kDocCount; ++i) {
            ASSERT_BSONOBJ_EQ(cachedResult[i], queryResult[i]);
        }
    }
}

TEST_F(ClassicRuntimePlannerForSbeTest, SubPlannerPicksMoreEfficientPlanForEachBranch) {
    setUpSubPlannerTest();
    testSbeFullOnAndOffFn([&](bool sbeFullEnabled) {
        auto exec = getExecutorWithSubPlanning(2 /*expectedPerBranchMultiplans*/);
        PlanSummaryStats stats;
        exec->getPlanExplainer().getSummaryStats(&stats);

        // The most efficient solution should use index "a" for first branch, examining 10 keys (a:
        // 1 to a: 10) and index "b" for second branch, examining 11 keys (b: 90 to b: 100).
        ASSERT_EQ(2, stats.indexesUsed.size());
        ASSERT_EQ(21, stats.totalKeysExamined);
        ASSERT_TRUE(std::find(stats.indexesUsed.begin(), stats.indexesUsed.end(), "a_1") !=
                    stats.indexesUsed.end());
        ASSERT_TRUE(std::find(stats.indexesUsed.begin(), stats.indexesUsed.end(), "d_1") !=
                    stats.indexesUsed.end());
    });
}

TEST_F(ClassicRuntimePlannerForSbeTest,
       SubPlannerPicksCachedPlanForWholeQueryWhenSbePlanCacheEnabled) {
    // 'SubPlanner' uses the SBE plan cache when "SBE full" is enabled.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);
    setUpSubPlannerTest();

    getExecutorWithSubPlanning(2 /*expectedPerBranchMultiplans*/);

    // When "featureFlagSbeFull" is enabled, the subplanner should write a single entry to the
    // SBE plan cache and should not write to the classic cache.
    auto&& sbePlanCache = sbe::getPlanCache(operationContext());
    ASSERT_EQ(numEntriesInCache(sbePlanCache), 1ull);
    ASSERT_EQ(numEntriesInCache(getClassicPlanCache()), 0ull);

    // Run CachedPlanner to execute the cached plan.
    auto [cq, plannerData] = createPlannerData(kRootedOrFilter);
    auto planCacheKey = plan_cache_key_factory::make(*plannerData.cq, plannerData.collections);

    auto cacheEntry = sbePlanCache.getCacheEntryIfActive(planCacheKey);
    ASSERT_TRUE(cacheEntry);
    auto cachedPlanner =
        makePlannerForSbeCacheEntry(std::move(plannerData), std::move(cacheEntry), {});
    auto cachedExec = cachedPlanner->makeExecutor(std::move(cq));
    assertPlanExecutorReturnsCorrectSums({20, 180}, cachedExec.get());
}

TEST_F(ClassicRuntimePlannerForSbeTest, SubPlannerCachesEachBranchWhenSbePlanCacheEnabled) {
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", false);
    setUpSubPlannerTest();

    getExecutorWithSubPlanning(2 /*expectedPerBranchMultiplans*/);

    // When "featureFlagSbeFull" is *not* enabled, the subplanner should write two entries to the
    // classic cache -- one for each branch. It should not write to the SBE plan cache.
    ASSERT_EQ(numEntriesInCache(getClassicPlanCache()), 2ull);
    ASSERT_EQ(numEntriesInCache(sbe::getPlanCache(operationContext())), 0ull);

    // If we subplan a second time, we should still see multi-planning since the cache entries are
    // inactive.
    getExecutorWithSubPlanning(2 /*expectedPerBranchMultiplans*/);

    // The third time, the active cache entries will get used and we should not see any
    // multi-planning for either $or branch.
    getExecutorWithSubPlanning(0);

    // If we clear the classic cache, then we should see multi-planning again and the cache should
    // be repopulated afterwards.
    getClassicPlanCache().clear();
    ASSERT_EQ(numEntriesInCache(getClassicPlanCache()), 0ull);
    ASSERT_EQ(numEntriesInCache(sbe::getPlanCache(operationContext())), 0ull);
    getExecutorWithSubPlanning(2);
    ASSERT_EQ(numEntriesInCache(getClassicPlanCache()), 2ull);
    ASSERT_EQ(numEntriesInCache(sbe::getPlanCache(operationContext())), 0ull);
}

TEST_F(ClassicRuntimePlannerForSbeTest, ClassicCachedPlannerReplansOnFailureMemoryLimitExceeded) {
    // Ensures that cache entries are available immediately.
    bool previousQueryKnobValue = internalQueryCacheDisableInactiveEntries.swap(true);
    ON_BLOCK_EXIT([&] { internalQueryCacheDisableInactiveEntries.store(previousQueryKnobValue); });

    setUpCachedPlannerTest();

    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", false);

    auto [cqForCacheWrite, plannerDataForCacheWrite] = createPlannerData();
    auto [solutions, expectedSums] = createVirtualScanQuerySolutionsForDefaultFilter(
        200 /*resultDocCount*/, plannerDataForCacheWrite.cq);
    {
        MultiPlanner planner{
            std::move(plannerDataForCacheWrite), std::move(solutions), true /*shouldWriteToCache*/};

        auto exec = planner.makeExecutor(std::move(cqForCacheWrite));
        assertPlanExecutorReturnsCorrectSums(expectedSums, exec.get());
    }

    {
        auto [cq, plannerData] = createPlannerData();
        auto [classicCacheKey, plannerGenerator] =
            makePlannerGeneratorFromClassicCache(cq.get(), std::move(plannerData), NumReads{10000});

        {
            // Swap in a mock SBE plan.
            auto planStageData = stage_builder::PlanStageData(
                stage_builder::Environment(std::make_unique<sbe::RuntimeEnvironment>()),
                std::make_shared<stage_builder::PlanStageStaticData>());
            plannerGenerator->setSbePlan_forTest(
                std::make_unique<sbe::MockExceededMemoryLimitStage>(0), std::move(planStageData));
        }

        std::unique_ptr<PlannerInterface> cachedPlanner = plannerGenerator->makePlanner();

        auto cachedExec = cachedPlanner->makeExecutor(std::move(cq));
        PlanSummaryStats stats;
        cachedExec->getPlanExplainer().getSummaryStats(&stats);
        ASSERT_TRUE(stats.replanReason)
            << "CachedPlanner should replan upon hitting a memory exceeds error.";
        ASSERT_STRING_SEARCH_REGEX(*stats.replanReason, "cached plan returned: ");

        assertPlanExecutorReturnsCorrectSums(expectedSums, cachedExec.get());

        ASSERT(getClassicPlanCache().getCacheEntryIfActive(classicCacheKey));
    }
}

TEST_F(ClassicRuntimePlannerForSbeTest, SbeCachedPlannerReplansOnFailureMemoryLimitExceeded) {
    // Ensures that cache entries are available immediately.
    bool previousQueryKnobValue = internalQueryCacheDisableInactiveEntries.swap(true);
    ON_BLOCK_EXIT([&] { internalQueryCacheDisableInactiveEntries.store(previousQueryKnobValue); });

    setUpCachedPlannerTest();
    auto&& sbePlanCache = sbe::getPlanCache(operationContext());

    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);

    auto [cqForCacheWrite, plannerDataForCacheWrite] = createPlannerData();
    auto [solutions, expectedSums] = createVirtualScanQuerySolutionsForDefaultFilter(
        200 /*resultDocCount*/, plannerDataForCacheWrite.cq);
    {
        MultiPlanner planner{
            std::move(plannerDataForCacheWrite), std::move(solutions), true /*shouldWriteToCache*/};

        auto exec = planner.makeExecutor(std::move(cqForCacheWrite));
        assertPlanExecutorReturnsCorrectSums(expectedSums, exec.get());
    }

    std::unique_ptr<PlannerInterface> cachedPlanner;
    auto [cq, plannerData] = createPlannerData();
    // Run CachedPlanner to execute the cached plan.
    sbe::PlanCacheKey sbePlanCacheKey =
        plan_cache_key_factory::make(*plannerData.cq, plannerData.collections);
    auto cacheEntry = sbePlanCache.getCacheEntryIfActive(sbePlanCacheKey);
    ASSERT_TRUE(cacheEntry);

    // Replace the 'root' with a mock stage which always throws memory exceeds exception.
    cacheEntry->cachedPlan->root = std::make_unique<sbe::MockExceededMemoryLimitStage>(0);
    cacheEntry->cachedPlan->planStageData.staticData =
        std::make_shared<stage_builder::PlanStageStaticData>();

    cachedPlanner = makePlannerForSbeCacheEntry(std::move(plannerData), std::move(cacheEntry), {});

    auto cachedExec = cachedPlanner->makeExecutor(std::move(cq));
    PlanSummaryStats stats;
    cachedExec->getPlanExplainer().getSummaryStats(&stats);
    ASSERT_TRUE(stats.replanReason)
        << "CachedPlanner should replan upon hitting a memory exceeds error.";
    ASSERT_STRING_SEARCH_REGEX(*stats.replanReason, "cached plan returned: ");

    assertPlanExecutorReturnsCorrectSums(expectedSums, cachedExec.get());

    ASSERT_TRUE(sbePlanCache.getCacheEntryIfActive(sbePlanCacheKey));
}

TEST_F(ClassicRuntimePlannerForSbeTest, ClassicCachedPlannerReplansOnHittingMaxNumReads) {
    // Ensures that cache entries are available immediately.
    bool previousQueryKnobValue = internalQueryCacheDisableInactiveEntries.swap(true);
    ON_BLOCK_EXIT([&] { internalQueryCacheDisableInactiveEntries.store(previousQueryKnobValue); });
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", false);

    setUpCachedPlannerTest();

    auto [cq, plannerData] = createPlannerData();
    auto [solutions, expectedSums] =
        createVirtualScanQuerySolutionsForDefaultFilter(200 /*resultDocCount*/, plannerData.cq);
    {
        MultiPlanner planner{
            std::move(plannerData), std::move(solutions), true /*shouldWriteToCache*/};
        auto exec = planner.makeExecutor(std::move(cq));
        assertPlanExecutorReturnsCorrectSums(expectedSums, exec.get());
    }

    {  // Run CachedPlanner to execute the cached plan.
        std::unique_ptr<PlannerInterface> cachedPlanner;
        auto [cq, plannerData] = createPlannerData();
        auto [classicCacheKey, plannerGenerator] =
            makePlannerGeneratorFromClassicCache(cq.get(),
                                                 std::move(plannerData),
                                                 // We give a threshold of 1 read, which will
                                                 // always be exceeded by the mock stage.
                                                 NumReads{1});

        {
            // Swap in a mock SBE plan that always exceeds the number of reads.
            auto staticData = std::make_shared<stage_builder::PlanStageStaticData>();
            staticData->resultSlot = sbe::value::SlotId{0};
            auto planStageData = stage_builder::PlanStageData(
                stage_builder::Environment(std::make_unique<sbe::RuntimeEnvironment>()),
                staticData);
            plannerGenerator->setSbePlan_forTest(
                std::make_unique<sbe::MockExceededMaxReadsStage>(0), std::move(planStageData));
        }


        cachedPlanner = plannerGenerator->makePlanner();

        auto cachedExec = cachedPlanner->makeExecutor(std::move(cq));

        PlanSummaryStats stats;
        cachedExec->getPlanExplainer().getSummaryStats(&stats);
        ASSERT_TRUE(stats.replanReason)
            << "CachedPlanner should replan upon hitting max number of reads allowed.";
        ASSERT_STRING_SEARCH_REGEX(*stats.replanReason,
                                   "cached plan was less efficient than expected");

        assertPlanExecutorReturnsCorrectSums(expectedSums, cachedExec.get());

        // Verify if the cache is not deactivated due to replanning.
        ASSERT_TRUE(getClassicPlanCache().getCacheEntryIfActive(classicCacheKey));
    }
}

TEST_F(ClassicRuntimePlannerForSbeTest, SbeCachedPlannerReplansOnHittingMaxNumReads) {
    // Ensures that cache entries are available immediately.
    bool previousQueryKnobValue = internalQueryCacheDisableInactiveEntries.swap(true);
    ON_BLOCK_EXIT([&] { internalQueryCacheDisableInactiveEntries.store(previousQueryKnobValue); });

    // Enable SBE plan cache.
    RAIIServerParameterControllerForTest sbeFullController("featureFlagSbeFull", true);

    setUpCachedPlannerTest();

    auto [cq, plannerData] = createPlannerData();
    auto [solutions, expectedSums] =
        createVirtualScanQuerySolutionsForDefaultFilter(200 /*resultDocCount*/, plannerData.cq);
    {
        MultiPlanner planner{
            std::move(plannerData), std::move(solutions), true /*shouldWriteToCache*/};
        auto exec = planner.makeExecutor(std::move(cq));
        assertPlanExecutorReturnsCorrectSums(expectedSums, exec.get());
    }

    {  // Run CachedPlanner to execute the cached plan.
        auto [cq, plannerData] = createPlannerData();
        auto planCacheKey = plan_cache_key_factory::make(*plannerData.cq, plannerData.collections);
        auto&& planCache = sbe::getPlanCache(operationContext());
        ASSERT_TRUE(planCache.getCacheEntryIfActive(planCacheKey));

        // Since 'works' is an immutable data member, create a mock cache entry with non-zero
        // 'works' to allow tracking of number reads. Originally the 'works' is always zero due
        // to reading from VirtualScanNode.
        auto entry = std::move(planCache.getEntry(planCacheKey).getValue());
        auto mockCacheEntry =
            PlanCacheEntryBase<sbe::CachedSbePlan, plan_cache_debug_info::DebugInfoSBE>::create(
                entry->cachedPlan->clone(),
                entry->planCacheShapeHash,
                entry->planCacheKey,
                entry->planCacheCommandKey,
                entry->timeOfCreation,
                entry->isActive,
                entry->securityLevel,
                NumReads{1},
                *entry->debugInfo);
        auto mockPlanCacheHolder = std::make_unique<
            CachedPlanHolder<sbe::CachedSbePlan, plan_cache_debug_info::DebugInfoSBE>>(
            *mockCacheEntry);

        // Replace the 'root' with a mock stage which always exceeds max number of reads.
        mockPlanCacheHolder->cachedPlan->root = std::make_unique<sbe::MockExceededMaxReadsStage>(0);
        sbe::value::SlotIdGenerator ids = sbe::value::SlotIdGenerator{};
        auto staticData = std::make_shared<stage_builder::PlanStageStaticData>();
        staticData->resultSlot = 0;
        mockPlanCacheHolder->cachedPlan->planStageData.staticData = staticData;

        auto cachedPlanner =
            makePlannerForSbeCacheEntry(std::move(plannerData), std::move(mockPlanCacheHolder), {});
        auto cachedExec = cachedPlanner->makeExecutor(std::move(cq));
        PlanSummaryStats stats;
        cachedExec->getPlanExplainer().getSummaryStats(&stats);
        ASSERT_TRUE(stats.replanReason)
            << "CachedPlanner should replan upon hitting max number of reads allowed.";
        ASSERT_STRING_SEARCH_REGEX(*stats.replanReason,
                                   "cached plan was less efficient than expected");

        assertPlanExecutorReturnsCorrectSums(expectedSums, cachedExec.get());

        // Verify if the cache is not deactivated due to replanning.
        ASSERT_TRUE(planCache.getCacheEntryIfActive(planCacheKey));
    }
}

}  // namespace
}  // namespace mongo::classic_runtime_planner_for_sbe
