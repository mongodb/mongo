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
#include "mongo/base/status.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/classic/working_set.h"
#include "mongo/db/exec/runtime_planners/classic_runtime_planner/planner_interface.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/local_catalog/index_catalog_mock.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role_mock.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_cache/plan_cache_test_util.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_fixture.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/unittest/unittest.h"

#include <benchmark/benchmark.h>

namespace mongo {

const NamespaceString kNss =
    NamespaceString::createNamespaceString_forTest("plan_cache_classic_test.coll");

// The benchmarks in this test allow for the presence of an index. This enum lists the types of
// indexes available to be used by the test cases.
enum AvailableIndexType {
    kNoIndex,
    kSingleFieldIndex,       // {a: 1}
    kTwoFieldCompoundIndex,  // {a: 1, b: 1}
    kTwoIndexes,             // {a: 1} and {b: 1}
    kManyFieldCompoundIndex  // {a: 1, a<long string>0: 1, a<long string>1: 1, ... /* repeated */}
};

// The type of filter that will be part of the generated find command for the test case.
enum FilterType {
    kFieldEqFilter,          // {a: 5}
    kIdEqFilter,             // {_id: 5}
    kOrFilter,               // {$or: [{a: 5}, {b: 5}]}
    kAndFilter,              // {a: 5, b: 5}
    kManyFieldFilter,        // {a: 5, b: 5, aa: 5, ab: 5, ac: 5, ... , zy: 5, zz: 5}
    kFilterWithTwoLargeIns,  // {{a: {$in: [<large in list>]}}, {b: {$in: [<large in list>]}}}
    kDeeplyNested            // {a: {$elemMatch: {a: {$elemMatch: {... {a: 5}}}}}}
};

// The sort that will be part of the generated find command for the test case.
enum SortType {
    kNoSort,
    kSort,  // {a: 1}
};

// The type of projection that will be part of the generated find command for the test case.
enum ProjType {
    kNoProj,
    kFieldInclusion,
    kFieldExclusion,
    kIdExclusionFieldInclusion,
    kIdExclusionFieldExclusion,
};

// The skip value that will be part of the generated find command for the test case.
enum SkipType {
    kNoSkip,
    kSkip,
};

// The limit value that will be part of the generated find command for the test case.
enum LimitType {
    kNoLimit,
    kLimit,
};

struct PlanCacheClassicBenchmarkParameters {
    AvailableIndexType index;
    FilterType filter;
    ProjType proj;
    SortType sort;
    SkipType skip;
    LimitType limit;

    PlanCacheClassicBenchmarkParameters(benchmark::State& state)
        : index(static_cast<AvailableIndexType>(state.range(0))),
          filter(static_cast<FilterType>(state.range(1))),
          proj(static_cast<ProjType>(state.range(2))),
          sort(static_cast<SortType>(state.range(3))),
          skip(static_cast<SkipType>(state.range(4))),
          limit(static_cast<LimitType>(state.range(5))) {}
};

void populateIn(BSONObjBuilder& bob) {
    BSONArrayBuilder inList(bob.subarrayStart("$in"));
    for (int i = 0; i < 20000; ++i) {
        inList.append(i);
    }
    inList.done();
    bob.done();
}

// Produces a filter of the shape {{a: {$in: [<large in list>]}}, {b: {$in: [<large in list>]}}}.
BSONObj createTwoLargeInObj() {
    BSONObjBuilder bob;
    BSONObjBuilder subA(bob.subobjStart("a"));
    populateIn(subA);
    BSONObjBuilder subB(bob.subobjStart("b"));
    populateIn(subB);
    return bob.obj();
}

// Produces a filter of the shape {a: {$elemMatch: {a: {$elemMatch: {... {a: 5}}}}}}.
BSONObj createDeeplyNestedObj() {
    BSONObj deepQuery = BSON("a" << 5);
    for (int i = 0; i < 100; i++) {
        deepQuery = BSON("a" << BSON("$elemMatch" << deepQuery));
    }

    return deepQuery;
}

// Produces a filter of the shape {a: 5, b: 5, aa: 5, ab: 5, ac: 5, ... , zy: 5, zz: 5}.
BSONObj createManyFieldFilter() {
    BSONObjBuilder result;
    // Append {a: 5} and {b: 5} so that if one of the two indexes is used, we could do an index
    // scan.
    result.append("a", 5);
    result.append("b", 5);

    std::string fields = "abcdefghijklmnopqrstuvwxyz";
    for (const auto& f1 : fields) {
        for (const auto& f2 : fields) {
            std::string field;
            field.push_back(f1);
            field.push_back(f2);
            result.append(field, 5);
        }
    }

    return result.obj();
}

// Builds index key {a: 1, a<long string>0: 1, a<long string>1: 1, ... a<long string>29: 1}.
BSONObj createManyFieldIndexKey() {
    BSONObjBuilder bob;
    bob << "a" << 1;
    for (int i = 0; i < 30; i++) {
        bob << (std::string(500, 'a') + std::to_string(i)) << 1;
    }
    return bob.obj();
}

std::unique_ptr<FindCommandRequest> makeFindFromBmParams(
    PlanCacheClassicBenchmarkParameters params) {
    auto findCommand = std::make_unique<FindCommandRequest>(kNss);

    // Filter
    switch (params.filter) {
        case kFieldEqFilter:
            findCommand->setFilter(BSON("a" << 5));
            break;
        case kIdEqFilter:
            findCommand->setFilter(BSON("_id" << 5));
            break;
        case kOrFilter: {
            BSONArrayBuilder bab;
            bab.append(BSON("a" << 5));
            bab.append(BSON("b" << 5));
            findCommand->setFilter(BSON("$or" << bab.arr()));
            break;
        }
        case kAndFilter:
            findCommand->setFilter(BSON("a" << 5 << "b" << 5));
            break;
        case kManyFieldFilter:
            findCommand->setFilter(createManyFieldFilter());
            break;
        case kFilterWithTwoLargeIns:
            findCommand->setFilter(createTwoLargeInObj());
            break;
        case kDeeplyNested:
            findCommand->setFilter(createDeeplyNestedObj());
            break;
    }

    // Sort
    switch (params.sort) {
        case kNoSort:
            break;
        case kSort:
            findCommand->setSort(BSON("a" << 1));
            break;
    }

    // Projection
    switch (params.proj) {
        case kNoProj:
            break;
        case kFieldInclusion:
            findCommand->setProjection(BSON("a" << 1));
            break;
        case kFieldExclusion:
            findCommand->setProjection(BSON("a" << 0));
            break;
        case kIdExclusionFieldInclusion:
            findCommand->setProjection(BSON("_id" << 0 << "a" << 1));
            break;
        case kIdExclusionFieldExclusion:
            findCommand->setProjection(BSON("_id" << 0 << "a" << 0));
            break;
    }

    // Skip
    switch (params.skip) {
        case kNoSkip:
            break;
        case kSkip:
            findCommand->setSkip(10);
            break;
    }

    // Limit
    switch (params.limit) {
        case kNoLimit:
            break;
        case kLimit:
            findCommand->setLimit(10);
            break;
    }

    return findCommand;
}

IndexEntry createIndexEntry(BSONObj keyPattern, const std::string& indexName) {
    return IndexEntry(keyPattern,
                      IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
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
                      nullptr);
}

std::unique_ptr<QueryPlannerParams> extractFromBmParams(
    PlanCacheClassicBenchmarkParameters params) {
    auto qpp = std::make_unique<QueryPlannerParams>(QueryPlannerParams::ArgsForTest{});
    // Add the index to the QueryPlannerParams and to the IndexSpec, if specified.
    switch (params.index) {
        case kNoIndex:
            break;
        case kSingleFieldIndex:
            qpp->mainCollectionInfo.indexes.push_back(createIndexEntry(BSON("a" << 1), "a_1"));
            break;
        case kTwoFieldCompoundIndex:
            qpp->mainCollectionInfo.indexes.push_back(
                createIndexEntry(BSON("a" << 1 << "b" << 1), "a_1_b_1"));
            break;
        case kTwoIndexes:
            // {a: 1}
            qpp->mainCollectionInfo.indexes.push_back(createIndexEntry(BSON("a" << 1), "a_1"));

            // {b: 1}
            qpp->mainCollectionInfo.indexes.push_back(createIndexEntry(BSON("b" << 1), "b_1"));

            // If we are in the test case for the AND of two indexes, we want to turn on index
            // intersection.
            if (params.filter == kAndFilter) {
                qpp->mainCollectionInfo.options = QueryPlannerParams::INDEX_INTERSECTION;
            }
            break;
        case kManyFieldCompoundIndex: {
            qpp->mainCollectionInfo.indexes.push_back(
                createIndexEntry(createManyFieldIndexKey(), "a_1_many_fields"));
            break;
        }
    }
    return qpp;
}

/**
 * Benchmarks for classic plan cache retrieval and parameter binding. Templated so that the stdout
 * for the benchmark is descriptive (i.e. we see metrics for each template instantiation of the
 * Query type) and so that flamegraphs have distinct stacks for each test case.
 */
template <typename Query>
void BM_PlanCacheClassic(benchmark::State& state) {
    PlanCacheClassicBenchmarkParameters bmParams(state);
    RAIIServerParameterControllerForTest truncateFeatureFlag{
        "internalQueryPlannerEnableSortIndexIntersection", true};
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    const auto yieldPolicy = PlanYieldPolicy::YieldPolicy::YIELD_AUTO;

    auto findCommand = makeFindFromBmParams(bmParams);
    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build();
    auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
        .expCtx = expCtx,
        .parsedFind = ParsedFindCommandParams{
            .findCommand = std::move(findCommand),
            .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});
    const size_t nWorks = 1;
    auto decision = createDecision(nWorks);
    auto callbacks = createCallback(*cq, *decision);

    auto params = extractFromBmParams(bmParams);
    auto statusWithMultiPlanSolns = QueryPlanner::plan(*cq, *params);
    ASSERT_OK(statusWithMultiPlanSolns.getStatus());

    auto solns = std::move(statusWithMultiPlanSolns.getValue());
    size_t indexToGet = -1;
    if (bmParams.index == kTwoIndexes && bmParams.filter == kAndFilter) {
        // In the test case for an AND stage of two indexes, there will be 3 plans that come out of
        // QueryPlanner::plan(): one that uses just the index on {a: 1}, one that uses just
        // the index on {b: 1}, and one that is {a: 1} AND {b: 1}. For this test case, we want to
        // explicitly use the third case, so we extract that plan from the result.
        ASSERT(solns.size() == 3);
        for (size_t i = 0; i < solns.size(); i++) {
            if (solns[i]->hasNode(STAGE_AND_SORTED)) {
                indexToGet = i;
                break;
            }
        }
    } else {
        // For the rest of the test cases in this benchmark there should only ever be one solution
        // that comes out of QueryPlanner::plan(). This is because in the case where there are no
        // specified indexes, we will always generate just a collection scan plan. If there is an
        // index, we will only generate the relevant index scan plan but not the collection scan
        // plan.
        ASSERT(solns.size() == 1);
        indexToGet = 0;
    }
    auto soln = std::move(solns[indexToGet]);

    auto collection =
        std::make_shared<CollectionMock>(UUID::gen(), kNss, std::make_unique<IndexCatalogMock>());
    auto catalog = CollectionCatalog::get(opCtx.get());
    catalog->onCreateCollection(opCtx.get(), collection);
    // The initialization of the CollectionPtr is SAFE. The lifetime of the Mocked Collection
    // instance is managed by the test and guaranteed to be valid for the entire duration of the
    // test.
    const auto collectionAcquisition = shard_role_mock::acquireCollectionMocked(
        opCtx.get(), kNss, CollectionPtr::CollectionPtr_UNSAFE(collection.get()));
    auto collectionsAccessor = MultipleCollectionAccessor(collectionAcquisition);

    // If there is an index specified, add it to the IndexCatalogMock.
    if (bmParams.index == kSingleFieldIndex || bmParams.index == kTwoFieldCompoundIndex ||
        bmParams.index == kManyFieldCompoundIndex) {
        IndexSpec ixSpec;
        if (bmParams.index == kSingleFieldIndex) {
            ixSpec.version(1).name("a_1").addKeys(BSON("a" << 1));
        } else if (bmParams.index == kTwoFieldCompoundIndex) {
            ixSpec.version(1).name("a_1_b_1").addKeys(BSON("a" << 1 << "b" << 1));
        } else {
            ixSpec.version(1).name("a_1_many_fields").addKeys(createManyFieldIndexKey());
        }

        IndexDescriptor ixDescriptor(IndexNames::BTREE, ixSpec.toBSON());
        collection->getIndexCatalog()->createIndexEntry(
            opCtx.get(), collection.get(), std::move(ixDescriptor), {});
    } else if (bmParams.index == kTwoIndexes) {
        // {a: 1}
        IndexSpec ixSpec1;
        ixSpec1.version(1).name("a_1").addKeys(BSON("a" << 1));
        IndexDescriptor ixDescriptor1(IndexNames::BTREE, ixSpec1.toBSON());
        collection->getIndexCatalog()->createIndexEntry(
            opCtx.get(), collection.get(), std::move(ixDescriptor1), {});

        // {b: 1}
        IndexSpec ixSpec2;
        ixSpec2.version(1).name("b_1").addKeys(BSON("b" << 1));
        IndexDescriptor ixDescriptor2(IndexNames::BTREE, ixSpec2.toBSON());
        collection->getIndexCatalog()->createIndexEntry(
            opCtx.get(), collection.get(), std::move(ixDescriptor2), {});
    }

    auto planCache =
        CollectionQueryInfo::get(collectionsAccessor.getMainCollection()).getPlanCache();
    auto planCacheKey = plan_cache_key_factory::make<PlanCacheKey>(
        *cq.get(), collectionsAccessor.getMainCollection());

    // At first, there is no entry for the plan cache key.
    ASSERT_EQ(planCache->get(planCacheKey).state, PlanCache::CacheEntryState::kNotPresent);

    // Add the entry into the cache, which at first sets it to be "inactive".
    ASSERT_OK(planCache->set(planCacheKey,
                             soln->cacheData->clone(),
                             NumWorks{nWorks},
                             Date_t{},
                             &callbacks,
                             PlanSecurityLevel::kNotSensitive));
    ASSERT_EQ(planCache->get(planCacheKey).state, PlanCache::CacheEntryState::kPresentInactive);

    // We need to add the entry once more to activate it so that it would be used.
    ASSERT_OK(planCache->set(planCacheKey,
                             soln->cacheData->clone(),
                             NumWorks{nWorks},
                             Date_t{},
                             &callbacks,
                             PlanSecurityLevel::kNotSensitive));
    ASSERT_EQ(planCache->get(planCacheKey).state, PlanCache::CacheEntryState::kPresentActive);

    for (auto curState : state) {
        auto cs = planCache->getCacheEntryIfActive(planCacheKey);
        auto statusWithQs = QueryPlanner::planFromCache(*cq.get(), *params, *cs->cachedPlan.get());

        state.PauseTiming();
        // Assert state is as expected before std::moving it away.
        ASSERT(cs != nullptr);
        ASSERT_OK(statusWithQs);

        // Recreate a QueryPlannerParams to std::move into the PlannerData.
        auto params = extractFromBmParams(bmParams);
        auto ws = std::make_unique<WorkingSet>();
        PlannerData plannerData{
            opCtx.get(),
            cq.get(),
            std::move(ws),
            collectionsAccessor,
            std::move(params),
            yieldPolicy,
            0 /* cachedPlanHash */
        };
        state.ResumeTiming();

        std::unique_ptr<QuerySolution> querySolution = std::move(statusWithQs.getValue());

        auto finalPlan = std::make_unique<classic_runtime_planner::CachedPlanner>(
            std::move(plannerData), std::move(cs), std::move(querySolution));

        state.PauseTiming();

        // Ensure that we actually did retrieve the plan from the cache.
        ASSERT(finalPlan != nullptr);

        state.ResumeTiming();
    }
}

// Eq filter.
class EqFilter;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, EqFilter)
    ->Args({
        AvailableIndexType::kNoIndex, /* index */
        FilterType::kFieldEqFilter,   /* filter */
        ProjType::kNoProj,            /* proj */
        SortType::kNoSort,            /* sort */
        SkipType::kNoSkip,            /* skip */
        LimitType::kNoLimit           /* limit */
    });

// Eq filter, inclusion projection.
class EqFilterInclProj;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, EqFilterInclProj)
    ->Args({
        AvailableIndexType::kNoIndex, /* index */
        FilterType::kFieldEqFilter,   /* filter */
        ProjType::kFieldInclusion,    /* proj */
        SortType::kNoSort,            /* sort */
        SkipType::kNoSkip,            /* skip */
        LimitType::kNoLimit           /* limit */
    });

// Eq filter, exclusion projection.
class EqFilterExclProj;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, EqFilterExclProj)
    ->Args({
        AvailableIndexType::kNoIndex, /* index */
        FilterType::kFieldEqFilter,   /* filter */
        ProjType::kFieldExclusion,    /* proj */
        SortType::kNoSort,            /* sort */
        SkipType::kNoSkip,            /* skip */
        LimitType::kNoLimit           /* limit */
    });

// Eq filter, id exclusion & field inclusion.
class EqFilterIdExclFieldIncl;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, EqFilterIdExclFieldIncl)
    ->Args({
        AvailableIndexType::kNoIndex,         /* index */
        FilterType::kFieldEqFilter,           /* filter */
        ProjType::kIdExclusionFieldInclusion, /* proj */
        SortType::kNoSort,                    /* sort */
        SkipType::kNoSkip,                    /* skip */
        LimitType::kNoLimit                   /* limit */
    });

// Eq filter, id exclusion & field exclusion.
class EqFilterIdExclFieldExcl;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, EqFilterIdExclFieldExcl)
    ->Args({
        AvailableIndexType::kNoIndex,         /* index */
        FilterType::kFieldEqFilter,           /* filter */
        ProjType::kIdExclusionFieldExclusion, /* proj */
        SortType::kNoSort,                    /* sort */
        SkipType::kNoSkip,                    /* skip */
        LimitType::kNoLimit                   /* limit */
    });

// Eq filter, single field index, sort, limit, skip.
class EqFilterSingleFieldIndexSortLimitSkip;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, EqFilterSingleFieldIndexSortLimitSkip)
    ->Args({
        AvailableIndexType::kSingleFieldIndex, /* index */
        FilterType::kFieldEqFilter,            /* filter */
        ProjType::kNoProj,                     /* proj */
        SortType::kSort,                       /* sort */
        SkipType::kSkip,                       /* skip */
        LimitType::kLimit                      /* limit */
    });

// Eq filter, two field index, sort, limit, skip.
class EqFilterTwoFieldIndexSortLimitSkip;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, EqFilterTwoFieldIndexSortLimitSkip)
    ->Args({
        AvailableIndexType::kTwoFieldCompoundIndex, /* index */
        FilterType::kFieldEqFilter,                 /* filter */
        ProjType::kNoProj,                          /* proj */
        SortType::kSort,                            /* sort */
        SkipType::kSkip,                            /* skip */
        LimitType::kLimit                           /* limit */
    });

// Eq filter, many-field index, sort, limit, skip.
class EqFilterSingleManyFieldIndexSortLimitSkip;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, EqFilterSingleManyFieldIndexSortLimitSkip)
    ->Args({
        AvailableIndexType::kManyFieldCompoundIndex, /* index */
        FilterType::kFieldEqFilter,                  /* filter */
        ProjType::kNoProj,                           /* proj */
        SortType::kSort,                             /* sort */
        SkipType::kSkip,                             /* skip */
        LimitType::kLimit                            /* limit */
    });

// Id filter.
class IdFilter;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, IdFilter)
    ->Args({
        AvailableIndexType::kNoIndex, /* index */
        FilterType::kIdEqFilter,      /* filter */
        ProjType::kNoProj,            /* proj */
        SortType::kNoSort,            /* sort */
        SkipType::kNoSkip,            /* skip */
        LimitType::kNoLimit           /* limit */
    });

// Plan that produces an OR stage of the two indexes.
class OrOfTwoIndexes;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, OrOfTwoIndexes)
    ->Args({
        AvailableIndexType::kTwoIndexes, /* index */
        FilterType::kOrFilter,           /* filter */
        ProjType::kNoProj,               /* proj */
        SortType::kNoSort,               /* sort */
        SkipType::kNoSkip,               /* skip */
        LimitType::kNoLimit              /* limit */
    });

// Plan that produces an AND_SORTED stage of the two indexes.
class AndOfTwoIndexes;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, AndOfTwoIndexes)
    ->Args({
        AvailableIndexType::kTwoIndexes, /* index */
        FilterType::kAndFilter,          /* filter */
        ProjType::kNoProj,               /* proj */
        SortType::kNoSort,               /* sort */
        SkipType::kNoSkip,               /* skip */
        LimitType::kNoLimit              /* limit */
    });

// Filter with many fields, no index.
class ManyFieldsNoIndex;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, ManyFieldsNoIndex)
    ->Args({
        AvailableIndexType::kNoIndex, /* index */
        FilterType::kManyFieldFilter, /* filter */
        ProjType::kNoProj,            /* proj */
        SortType::kNoSort,            /* sort */
        SkipType::kNoSkip,            /* skip */
        LimitType::kNoLimit           /* limit */
    });

// Filter with many fields, single field index.
class ManyFieldsSingleFieldIndex;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, ManyFieldsSingleFieldIndex)
    ->Args({
        AvailableIndexType::kSingleFieldIndex, /* index */
        FilterType::kManyFieldFilter,          /* filter */
        ProjType::kNoProj,                     /* proj */
        SortType::kNoSort,                     /* sort */
        SkipType::kNoSkip,                     /* skip */
        LimitType::kNoLimit                    /* limit */
    });

// Filter with many fields, two field index.
class ManyFieldsTwoFieldIndex;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, ManyFieldsTwoFieldIndex)
    ->Args({
        AvailableIndexType::kTwoFieldCompoundIndex, /* index */
        FilterType::kManyFieldFilter,               /* filter */
        ProjType::kNoProj,                          /* proj */
        SortType::kNoSort,                          /* sort */
        SkipType::kNoSkip,                          /* skip */
        LimitType::kNoLimit                         /* limit */
    });

// Deeply nested query.
class DeeplyNestedFilter;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, DeeplyNestedFilter)
    ->Args({
        AvailableIndexType::kNoIndex, /* index */
        FilterType::kDeeplyNested,    /* filter */
        ProjType::kNoProj,            /* proj */
        SortType::kNoSort,            /* sort */
        SkipType::kNoSkip,            /* skip */
        LimitType::kNoLimit           /* limit */
    });

// Large in, no index.
class LargeInNoIndex;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, LargeInNoIndex)
    ->Args({
        AvailableIndexType::kNoIndex,       /* index */
        FilterType::kFilterWithTwoLargeIns, /* filter */
        ProjType::kNoProj,                  /* proj */
        SortType::kNoSort,                  /* sort */
        SkipType::kNoSkip,                  /* skip */
        LimitType::kNoLimit                 /* limit */
    });

// Large in, single field index.
class LargeInSingleFieldIndex;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, LargeInSingleFieldIndex)
    ->Args({
        AvailableIndexType::kSingleFieldIndex, /* index */
        FilterType::kFilterWithTwoLargeIns,    /* filter */
        ProjType::kNoProj,                     /* proj */
        SortType::kNoSort,                     /* sort */
        SkipType::kNoSkip,                     /* skip */
        LimitType::kNoLimit                    /* limit */
    });

// Large in, two field index.
class LargeInTwoFieldIndex;
BENCHMARK_TEMPLATE(BM_PlanCacheClassic, LargeInTwoFieldIndex)
    ->Args({
        AvailableIndexType::kTwoFieldCompoundIndex, /* index */
        FilterType::kFilterWithTwoLargeIns,         /* filter */
        ProjType::kNoProj,                          /* proj */
        SortType::kNoSort,                          /* sort */
        SkipType::kNoSkip,                          /* skip */
        LimitType::kNoLimit                         /* limit */
    });

}  // namespace mongo
