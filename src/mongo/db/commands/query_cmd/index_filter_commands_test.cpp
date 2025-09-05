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
 * This file contains tests for mongo/db/commands/query_cmd/index_filter_commands.h
 */

#include "mongo/db/commands/query_cmd/index_filter_commands.h"

#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/plan_cache_callbacks_impl.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/exec/plan_stats.h"
#include "mongo/db/exec/sbe/expressions/runtime_environment.h"
#include "mongo/db/exec/sbe/stages/co_scan.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/compiler/physical_model/query_solution/stage_types.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_cache/plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_debug_info.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/query/plan_ranking_decision.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/query/stage_builder/sbe/builder.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/clock_source.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/none.hpp>
#include <fmt/format.h>

namespace mongo {
namespace {

class IndexFilterCommandsTest : public unittest::Test {
protected:
    void setUp() override {
        _queryTestServiceContext = std::make_unique<QueryTestServiceContext>();
        _operationContext = _queryTestServiceContext->makeOperationContext();
        _collection = std::make_unique<CollectionMock>(_nss);

        // The collection holder is guaranteed to be valid for the lifetime of the test. This
        // initialization is safe.
        CollectionPtr collptr = CollectionPtr::CollectionPtr_UNSAFE(_collection.get());
        _collectionAcq =
            std::make_unique<CollectionAcquisition>(shard_role_mock::acquireCollectionMocked(
                _operationContext.get(), _nss, std::move(collptr)));
        _classicPlanCache = std::make_unique<PlanCache>(5000);
        _sbePlanCache = std::make_unique<sbe::PlanCache>(5000);
    }

    void tearDown() override {
        _sbePlanCache.reset();
        _classicPlanCache.reset();

        _collectionAcq.reset();
        _collection.reset();
        _operationContext.reset();
        _queryTestServiceContext.reset();
    }

    PlanCacheKey makeClassicKey(const CanonicalQuery& cq) {
        return plan_cache_key_factory::make<PlanCacheKey>(cq, _collectionAcq->getCollectionPtr());
    }

    sbe::PlanCacheKey makeSbeKey(const CanonicalQuery& cq) {
        ASSERT_TRUE(cq.isSbeCompatible());
        return plan_cache_key_factory::make<sbe::PlanCacheKey>(cq,
                                                               _collectionAcq->getCollectionPtr());
    }

    Status clearIndexFilter(const std::string& cmdJson) {
        return ClearFilters::clear(_operationContext.get(),
                                   _collectionAcq->getCollectionPtr(),
                                   fromjson(cmdJson),
                                   &_querySettings,
                                   _classicPlanCache.get(),
                                   _sbePlanCache.get());
    }

    /**
     * Given a string of the "planCacheClear" command, clear corresponding index filters. Assert
     * that the command works.
     */
    void clearIndexFilterAndAssert(const std::string& cmdJson) {
        ASSERT_OK(ClearFilters::clear(_operationContext.get(),
                                      _collectionAcq->getCollectionPtr(),
                                      fromjson(cmdJson),
                                      &_querySettings,
                                      _classicPlanCache.get(),
                                      _sbePlanCache.get()));
    }

    Status setIndexFilter(const std::string& cmdJson) {
        return SetFilter::set(_operationContext.get(),
                              _collectionAcq->getCollectionPtr(),
                              fromjson(cmdJson),
                              &_querySettings,
                              _classicPlanCache.get(),
                              _sbePlanCache.get());
    }

    /**
     * Given a string of the "planCacheSet" command, set corresponding index filters. Assert that
     * the command works.
     */
    void setIndexFilterAndAssert(const std::string& cmdJson) {
        ASSERT_OK(setIndexFilter(cmdJson));
    }

    static std::unique_ptr<plan_ranker::PlanRankingDecision> createDecision(size_t numPlans) {
        auto why = std::make_unique<plan_ranker::PlanRankingDecision>();
        std::vector<std::unique_ptr<PlanStageStats>> stats;
        for (size_t i = 0; i < numPlans; ++i) {
            CommonStats common("COLLSCAN");
            auto stat = std::make_unique<PlanStageStats>(common, STAGE_COLLSCAN);
            stat->specific.reset(new CollectionScanStats());
            stats.push_back(std::move(stat));
            why->scores.push_back(0U);
            why->candidateOrder.push_back(i);
        }
        why->stats.candidatePlanStats = std::move(stats);
        return why;
    }

    /**
     * Injects an entry into classic and SBE plan caches for query shape. And asserts that the
     * function calls succeeds.
     */
    void addQueryShapeToPlanCacheAndAssert(const char* queryStr,
                                           const char* sortStr,
                                           const char* projectionStr,
                                           const char* collationStr) {
        addQueryShapeToClassicPlanCache(queryStr, sortStr, projectionStr, collationStr);
        addQueryShapeToSbePlanCache(queryStr, sortStr, projectionStr, collationStr);

        assertPlanCacheContains(queryStr, sortStr, projectionStr, collationStr);
    }

    void assertPlanCacheContains(const char* queryStr,
                                 const char* sortStr,
                                 const char* projectionStr,
                                 const char* collationStr) {
        ASSERT_TRUE(classicPlanCacheContains(queryStr, sortStr, projectionStr, collationStr));
        ASSERT_TRUE(sbePlanCacheContains(queryStr, sortStr, projectionStr, collationStr));
    }

    void assertPlanCacheDoesNotContain(const char* queryStr,
                                       const char* sortStr,
                                       const char* projectionStr,
                                       const char* collationStr) {
        ASSERT_FALSE(classicPlanCacheContains(queryStr, sortStr, projectionStr, collationStr));
        ASSERT_FALSE(sbePlanCacheContains(queryStr, sortStr, projectionStr, collationStr));
    }

    /**
     * Checks if plan cache contains query shape.
     */
    bool classicPlanCacheContains(const char* queryStr,
                                  const char* sortStr,
                                  const char* projectionStr,
                                  const char* collationStr) {
        // Create canonical query.
        auto inputQuery = makeCQ(queryStr, sortStr, projectionStr, collationStr);
        auto inputKey = makeClassicKey(*inputQuery);

        return _classicPlanCache->getEntry(inputKey).isOK();
    }

    /**
     * Checks if SBE plan cache contains query shape.
     */
    bool sbePlanCacheContains(const char* queryStr,
                              const char* sortStr,
                              const char* projectionStr,
                              const char* collationStr) {
        // Create canonical query.
        auto inputQuery = makeCQ(queryStr, sortStr, projectionStr, collationStr);
        inputQuery->setSbeCompatible(true);
        auto inputKey = makeSbeKey(*inputQuery);

        return _sbePlanCache->getEntry(inputKey).isOK();
    }

    /**
     * Utility function to get list of index filters from the query settings.
     */
    std::vector<BSONObj> getFilters() {
        BSONObjBuilder bob;
        ASSERT_OK(ListFilters::list(_querySettings, &bob));
        BSONObj resultObj = bob.obj();
        BSONElement filtersElt = resultObj.getField("filters");
        ASSERT_EQUALS(filtersElt.type(), mongo::BSONType::array);
        std::vector<BSONElement> filtersEltArray = filtersElt.Array();
        std::vector<BSONObj> filters;
        for (auto&& elt : filtersEltArray) {
            ASSERT_TRUE(elt.isABSONObj());
            BSONObj obj = elt.Obj();

            // Check required fields.
            // query
            BSONElement queryElt = obj.getField("query");
            ASSERT_TRUE(queryElt.isABSONObj());

            // sort
            BSONElement sortElt = obj.getField("sort");
            ASSERT_TRUE(sortElt.isABSONObj());

            // projection
            BSONElement projectionElt = obj.getField("projection");
            ASSERT_TRUE(projectionElt.isABSONObj());

            // collation (optional)
            BSONElement collationElt = obj.getField("collation");
            if (!collationElt.eoo()) {
                ASSERT_TRUE(collationElt.isABSONObj());
            }

            // indexes
            BSONElement indexesElt = obj.getField("indexes");
            ASSERT_EQUALS(indexesElt.type(), mongo::BSONType::array);

            // All fields OK. Append to vector.
            filters.push_back(obj.getOwned());
        }

        return filters;
    }

private:
    static const NamespaceString _nss;

    std::unique_ptr<CanonicalQuery> makeCQ(const BSONObj& query,
                                           const BSONObj& sort,
                                           const BSONObj& projection,
                                           const BSONObj& collation) {
        auto findCommand = std::make_unique<FindCommandRequest>(_nss);
        findCommand->setFilter(query);
        findCommand->setSort(sort);
        findCommand->setProjection(projection);
        findCommand->setCollation(collation);
        return std::make_unique<CanonicalQuery>(
            CanonicalQueryParams{.expCtx = ExpressionContextBuilder{}
                                               .fromRequest(_operationContext.get(), *findCommand)
                                               .build(),
                                 .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    }

    std::unique_ptr<CanonicalQuery> makeCQ(const char* queryStr,
                                           const char* sortStr,
                                           const char* projectionStr,
                                           const char* collationStr) {
        return makeCQ(
            fromjson(queryStr), fromjson(sortStr), fromjson(projectionStr), fromjson(collationStr));
    }

    /**
     * Injects an entry into classic plan cache for query shape.
     */
    void addQueryShapeToClassicPlanCache(const char* queryStr,
                                         const char* sortStr,
                                         const char* projectionStr,
                                         const char* collationStr) {
        // Create canonical query.
        std::unique_ptr<CanonicalQuery> cq = makeCQ(queryStr, sortStr, projectionStr, collationStr);

        auto cacheData = std::make_unique<SolutionCacheData>();
        cacheData->tree = std::make_unique<PlanCacheIndexTree>();
        const size_t nWorks = 1;
        auto decision = createDecision(nWorks);
        auto buildDebugInfoFn = [&]() -> plan_cache_debug_info::DebugInfo {
            return plan_cache_util::buildDebugInfo(*cq, std::move(decision));
        };
        auto printCachedPlanFn = [](const SolutionCacheData& plan) {
            return plan.toString();
        };
        PlanCacheCallbacksImpl<PlanCacheKey, SolutionCacheData, plan_cache_debug_info::DebugInfo>
            callbacks{*cq, buildDebugInfoFn, printCachedPlanFn, _collectionAcq->getCollectionPtr()};
        ASSERT_OK(_classicPlanCache->set(
            makeClassicKey(*cq),
            std::move(cacheData),
            NumWorks{nWorks},
            _operationContext.get()->getServiceContext()->getPreciseClockSource()->now(),
            &callbacks,
            PlanSecurityLevel::kNotSensitive,
            boost::none /* worksGrowthCoefficient */));
    }

    /**
     * Injects an entry into SBE plan cache for query shape.
     */
    void addQueryShapeToSbePlanCache(const char* queryStr,
                                     const char* sortStr,
                                     const char* projectionStr,
                                     const char* collationStr) {
        // Create canonical query.
        std::unique_ptr<CanonicalQuery> cq = makeCQ(queryStr, sortStr, projectionStr, collationStr);
        cq->setSbeCompatible(true);

        // This "sbe::CachedSbePlan" is working only as a placeholder plan. The contents of it don't
        // matter to the tests.
        auto cacheData = std::make_unique<sbe::CachedSbePlan>(
            std::make_unique<sbe::CoScanStage>(PlanNodeId{}),
            stage_builder::PlanStageData(
                stage_builder::Environment(std::make_unique<sbe::RuntimeEnvironment>()),
                std::make_unique<stage_builder::PlanStageStaticData>()),
            0 /*hash*/);
        const size_t nWorks = 1;
        auto decision = createDecision(nWorks);
        auto querySolution = std::make_unique<QuerySolution>();

        auto buildDebugInfoFn = [soln =
                                     querySolution.get()]() -> plan_cache_debug_info::DebugInfoSBE {
            return plan_cache_util::buildDebugInfo(soln);
        };
        auto printCachedPlanFn = [](const sbe::CachedSbePlan& plan) {
            sbe::DebugPrinter p;
            return p.print(*plan.root.get());
        };
        PlanCacheCallbacksImpl<sbe::PlanCacheKey,
                               sbe::CachedSbePlan,
                               plan_cache_debug_info::DebugInfoSBE>
            callbacks{*cq, buildDebugInfoFn, printCachedPlanFn, _collectionAcq->getCollectionPtr()};

        ASSERT_OK(_sbePlanCache->set(
            makeSbeKey(*cq),
            std::move(cacheData),
            NumWorks{nWorks},
            _operationContext.get()->getServiceContext()->getPreciseClockSource()->now(),
            &callbacks,
            PlanSecurityLevel::kNotSensitive,
            boost::none /* worksGrowthCoefficient */));
    }

    std::unique_ptr<QueryTestServiceContext> _queryTestServiceContext;

    ServiceContext::UniqueOperationContext _operationContext;
    std::unique_ptr<Collection> _collection;
    std::unique_ptr<CollectionAcquisition> _collectionAcq;

    std::unique_ptr<PlanCache> _classicPlanCache;
    std::unique_ptr<sbe::PlanCache> _sbePlanCache;

    QuerySettings _querySettings;
};

const NamespaceString IndexFilterCommandsTest::_nss(
    NamespaceString::createNamespaceString_forTest("test.collection"));

/**
 * Tests for ListFilters
 */

TEST_F(IndexFilterCommandsTest, ListFiltersEmpty) {
    std::vector<BSONObj> filters = getFilters();
    ASSERT_TRUE(filters.empty());
}

/**
 * Tests for ClearFilters
 */

TEST_F(IndexFilterCommandsTest, ClearFiltersInvalidParameter) {
    std::vector<std::pair<std::string, std::string>> testCases{
        {"{query: 1234}", "If present, query has to be an object."},
        {"{query: {a: 1}, sort: 1234}", "If present, sort must be an object."},
        {"{query: {a: 1}, projection: 1234}", "If present, projection must be an object."},
        {"{query: {a: {$no_such_op: 1}}}", "Query must pass canonicalization."},
        {"{sort: {a: 1}}", "Sort present without query is an error."},
        {"{projection: {_id: 0, a: 1}}", "Projection present without query is an error."},
    };

    for (const auto& [cmdJson, message] : testCases) {
        ASSERT_NOT_OK(clearIndexFilter(cmdJson)) << message;
    }
}

TEST_F(IndexFilterCommandsTest, ClearNonexistentIndexFilter) {
    std::string setCmdObject{"{query: {a: 1}, indexes: [{a: 1}]}"};
    std::string clearCmdObject{"{query: {b: 1}}"};

    ASSERT_OK(setIndexFilter(setCmdObject));
    std::vector<BSONObj> filters = getFilters();
    ASSERT_EQUALS(filters.size(), 1U);

    // Clear nonexistent index filter.
    // Command should succeed and cache should remain unchanged.
    ASSERT_OK(clearIndexFilter(clearCmdObject));
    filters = getFilters();
    ASSERT_EQUALS(filters.size(), 1U);
}

/**
 * Tests for SetFilter
 */

TEST_F(IndexFilterCommandsTest, SetFilterInvalidParameter) {
    std::vector<std::pair<std::string, std::string>> testCases{
        {"{}", "Empty command object"},
        {"{indexes: [{a: 1}]}", "Missing required query field."},
        {"{query: {a: 1}}", "Missing required indexes field."},
        {"{query: 1234, indexes: [{a: 1}, {b: 1}]}", "Query has to be an object."},
        {"{query: {a: 1}, indexes: 1234}", "Indexes field has to be an array."},
        {"{query: {a: 1}, indexes: []}", "Array indexes field cannot empty."},
        {"{query: {a: 1}, indexes: [{a: 1}, 99]}", "Elements in indexes have to be objects."},
        {"{query: {a: 1}, indexes: [{a: 1}, {}]}", "Objects in indexes cannot be empty."},
        {"{query: {a: 1}, sort: 1234, indexes: [{a: 1}, {b: 1}]}",
         "If present, sort must be an object."},
        {"{query: {a: 1}, projection: 1234, indexes: [{a: 1}, {b: 1}]}",
         "If present, projection must be an object."},
        {"{query: {a: 1}, collation: 1234, indexes: [{a: 1}, {b: 1}]}",
         "If present, collation must be an object."},
        {"{query: {a: {$no_such_op: 1}}, indexes: [{a: 1}, {b: 1}]}",
         "Query must pass canonicalization."},
    };

    for (const auto& [cmdJson, message] : testCases) {
        ASSERT_NOT_OK(setIndexFilter(cmdJson)) << message;
    }
}

TEST_F(IndexFilterCommandsTest, SetAndClearFilters) {
    // Inject query shape into plan cache.
    addQueryShapeToPlanCacheAndAssert(
        "{a: 1, b: 1}", "{a: -1}", "{_id: 0, a: 1}", "{locale: 'mock_reverse_string'}");

    setIndexFilterAndAssert(
        R"({query: {a: 1, b: 1},
            sort: {a: -1},
            projection: {_id: 0, a: 1},
            collation: {locale: 'mock_reverse_string'},
            indexes: [{a: 1}]})");

    size_t expectedNumFilters = 1;
    std::vector<BSONObj> filters = getFilters();
    ASSERT_EQ(expectedNumFilters, filters.size());

    // Query shape should not exist in plan cache after index filter is updated.
    assertPlanCacheDoesNotContain(
        "{a: 1, b: 1}", "{a: -1}", "{_id: 0, a: 1}", "{locale: 'mock_reverse_string'}");

    // Fields in filter should match criteria in most recent query settings update.
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("query"), fromjson("{a: 1, b: 1}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("sort"), fromjson("{a: -1}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("projection"), fromjson("{_id: 0, a: 1}"));
    ASSERT_EQUALS(StringData(filters[0].getObjectField("collation").getStringField("locale")),
                  "mock_reverse_string");

    // Replacing the index filter for the same query shape ({a: 1, b: 1} and {b: 2, a: 3} share same
    // shape) should not change the query settings size.
    setIndexFilterAndAssert(
        R"({query: {b: 2, a: 3},
            sort: {a: -1},
            projection: {_id: 0, a: 1},
            collation: {locale: 'mock_reverse_string'},
            indexes: [{a: 1, b: 1}]})");
    filters = getFilters();
    ASSERT_EQ(expectedNumFilters, filters.size());

    auto filterIndexes = filters[0]["indexes"];
    ASSERT(filterIndexes.type() == BSONType::array);
    auto filterArray = filterIndexes.Array();
    ASSERT_EQ(filterArray.size(), 1U);
    ASSERT_BSONOBJ_EQ(filterArray[0].Obj(), fromjson("{a: 1, b: 1}"));

    // Add index filter for different query shape.
    setIndexFilterAndAssert("{query: {b: 1}, indexes: [{b: 1}]}");
    expectedNumFilters += 1;
    filters = getFilters();
    ASSERT_EQ(expectedNumFilters, filters.size());

    // Add index filter for 3rd query shape. This is to prepare for ClearIndexFilter tests.
    setIndexFilterAndAssert("{query: {a: 1}, indexes: [{a: 1}]}");
    expectedNumFilters += 1;
    filters = getFilters();
    ASSERT_EQ(expectedNumFilters, filters.size());

    // Add 2 entries to plan cache and check plan cache after clearing one/all filters.
    addQueryShapeToPlanCacheAndAssert("{a: 1}", "{}", "{}", "{}");
    addQueryShapeToPlanCacheAndAssert("{b: 1}", "{}", "{}", "{}");

    // Clear single index filter.
    clearIndexFilterAndAssert("{query: {a: 1}}");
    expectedNumFilters -= 1;
    filters = getFilters();
    ASSERT_EQ(expectedNumFilters, filters.size());

    // Query shape should not exist in plan cache after clearing 1 index filter.
    assertPlanCacheDoesNotContain("{a: 1}", "{}", "{}", "{}");
    assertPlanCacheContains("{b: 1}", "{}", "{}", "{}");

    // Clear all filters
    clearIndexFilterAndAssert("{}");
    filters = getFilters();
    ASSERT_TRUE(filters.empty());

    // {b: 1} should be gone from both of the two plan caches after flushing query settings.
    assertPlanCacheDoesNotContain("{b: 1}", "{}", "{}", "{}");
}

TEST_F(IndexFilterCommandsTest, SetAndClearFiltersCollation) {
    // Inject query shapes with and without collation into plan cache.
    addQueryShapeToPlanCacheAndAssert("{a: 'foo'}", "{}", "{}", "{locale: 'mock_reverse_string'}");
    addQueryShapeToPlanCacheAndAssert("{a: 'foo'}", "{}", "{}", "{}");

    setIndexFilterAndAssert(
        R"({query: {a: 'foo'},
              sort: {},
              projection: {},
              collation: {locale: 'mock_reverse_string'},
              indexes: [{a: 1}]})");

    std::vector<BSONObj> filters = getFilters();
    ASSERT_EQUALS(filters.size(), 1U);
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("query"), fromjson("{a: 'foo'}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("sort"), fromjson("{}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("projection"), fromjson("{}"));
    ASSERT_EQUALS(StringData(filters[0].getObjectField("collation").getStringField("locale")),
                  "mock_reverse_string");

    // Setting a filter will remove the cache entry associated with the query so now the plan cache
    // should only contain the entry for the query without collation.
    assertPlanCacheDoesNotContain("{a: 'foo'}", "{}", "{}", "{locale: 'mock_reverse_string'}");
    assertPlanCacheContains("{a: 'foo'}", "{}", "{}", "{}");

    // Add filter for query shape without collation.
    setIndexFilterAndAssert("{query: {a: 'foo'}, indexes: [{b: 1}]}");
    filters = getFilters();
    ASSERT_EQUALS(filters.size(), 2U);

    // Add plan cache entries for both queries.
    addQueryShapeToPlanCacheAndAssert("{a: 'foo'}", "{}", "{}", "{locale: 'mock_reverse_string'}");
    addQueryShapeToPlanCacheAndAssert("{a: 'foo'}", "{}", "{}", "{}");

    // Clear filter for query with collation.
    clearIndexFilterAndAssert("{query: {a: 'foo'}, collation: {locale: 'mock_reverse_string'}}");
    filters = getFilters();
    ASSERT_EQUALS(filters.size(), 1U);
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("query"), fromjson("{a: 'foo'}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("sort"), fromjson("{}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("projection"), fromjson("{}"));
    ASSERT_BSONOBJ_EQ(filters[0].getObjectField("collation"), fromjson("{}"));

    // Plan cache should only contain entry for query without collation.
    assertPlanCacheDoesNotContain("{a: 'foo'}", "{}", "{}", "{locale: 'mock_reverse_string'}");
    assertPlanCacheContains("{a: 'foo'}", "{}", "{}", "{}");
}

TEST_F(IndexFilterCommandsTest, SetFilterAcceptsIndexNames) {
    addQueryShapeToPlanCacheAndAssert("{a: 2}", "{}", "{}", "{}");

    setIndexFilterAndAssert(
        R"({query: {a: 2},
             sort: {},
             projection: {},
             indexes: [{a: 1}, 'a_1:rev']})");
    assertPlanCacheDoesNotContain("{a: 2}", "{}", "{}", "{}");

    auto filters = getFilters();
    ASSERT_EQUALS(filters.size(), 1U);
    auto indexes = filters[0]["indexes"].Array();

    ASSERT_BSONOBJ_EQ(indexes[0].embeddedObject(), fromjson("{a: 1}"));
    ASSERT_EQUALS(indexes[1].valueStringData(), "a_1:rev");
}

}  // namespace
}  // namespace mongo
