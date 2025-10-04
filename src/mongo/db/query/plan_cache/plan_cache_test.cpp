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
 * This file contains tests for mongo/db/query/plan_cache/plan_cache.h
 */

#include "mongo/db/query/plan_cache/plan_cache.h"

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/exec/plan_cache_util.h"
#include "mongo/db/index/index_constants.h"
#include "mongo/db/index/wildcard_key_generator.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/local_catalog/shard_role_api/shard_role_mock.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/canonical_query_test_util.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collation/collator_interface_mock.h"
#include "mongo/db/query/compiler/metadata/index_entry.h"
#include "mongo/db/query/compiler/parsers/matcher/expression_parser.h"
#include "mongo/db/query/compiler/physical_model/query_solution/query_solution.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/plan_cache/classic_plan_cache.h"
#include "mongo/db/query/plan_cache/plan_cache_indexability.h"
#include "mongo/db/query/plan_cache/plan_cache_key_factory.h"
#include "mongo/db/query/plan_cache/plan_cache_key_info.h"
#include "mongo/db/query/plan_cache/plan_cache_test_util.h"
#include "mongo/db/query/plan_cache/sbe_plan_cache.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_planner_test_lib.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/db/service_context.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"

#include <memory>

#include <boost/cstdint.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
#include <fmt/format.h>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using unittest::assertGet;

class PlanCacheTest : public CanonicalQueryTest {
protected:
    /**
     * Test functions for shouldCacheQuery.
     *
     * Use these functions to assert which categories of canonicalized queries are suitable for
     * inclusion in the plan cache.
     */
    void assertShouldCacheQuery(const CanonicalQuery& query) {
        if (shouldCacheQuery(query)) {
            return;
        }
        str::stream ss;
        ss << "Canonical query should be cacheable: " << query.toString();
        FAIL(std::string(ss));
    }

    void assertShouldNotCacheQuery(const CanonicalQuery& query) {
        if (!shouldCacheQuery(query)) {
            return;
        }
        str::stream ss;
        ss << "Canonical query should not be cacheable: " << query.toString();
        FAIL(std::string(ss));
    }

    void assertShouldNotCacheQuery(const BSONObj& query) {
        std::unique_ptr<CanonicalQuery> cq(canonicalize(query));
        assertShouldNotCacheQuery(*cq);
    }

    void assertShouldNotCacheQuery(const char* queryStr) {
        std::unique_ptr<CanonicalQuery> cq(canonicalize(queryStr));
        assertShouldNotCacheQuery(*cq);
    }
};

PlanCacheKey makeKey(const CanonicalQuery& cq, const std::vector<CoreIndexInfo>& indexCores = {}) {
    PlanCacheIndexabilityState indexabilityState;
    indexabilityState.updateDiscriminators(indexCores);

    StringBuilder indexabilityKeyBuilder;
    plan_cache_detail::encodeIndexability(
        cq.getPrimaryMatchExpression(), indexabilityState, &indexabilityKeyBuilder);

    return {PlanCacheKeyInfo{
        encodeKey(cq), indexabilityKeyBuilder.str(), query_settings::QuerySettings()}};
}

/**
 * Helper which constructs a $** IndexEntry and returns it along with an owned ProjectionExecutor.
 * The latter simulates the ProjectionExecutor which, during normal operation, is owned and
 * maintained by the $** index's IndexAccessMethod, and is required because the plan cache will
 * obtain unowned pointers to it.
 */
std::pair<IndexEntry, std::unique_ptr<WildcardProjection>> makeWildcardEntry(BSONObj keyPattern) {
    auto wcProj = std::make_unique<WildcardProjection>(
        WildcardKeyGenerator::createProjectionExecutor(keyPattern, {}));
    return {IndexEntry(keyPattern,
                       IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                       IndexConfig::kLatestIndexVersion,
                       false,  // multikey
                       {},
                       {},
                       false,  // sparse
                       false,  // unique
                       IndexEntry::Identifier{"indexName"},
                       nullptr,
                       BSONObj(),
                       nullptr,
                       wcProj.get()),
            std::move(wcProj)};
}

//
// Tests for CachedSolution.
//

std::unique_ptr<QuerySolution> getQuerySolutionForCaching() {
    std::unique_ptr<QuerySolution> qs = std::make_unique<QuerySolution>();
    qs->cacheData = std::make_unique<SolutionCacheData>();
    qs->cacheData->tree = std::make_unique<PlanCacheIndexTree>();
    return qs;
}

/**
 * Cacheable queries.
 *
 * These queries will be added to the cache with run-time statistics and can be managed with the
 * cache DB commands.
 */

TEST_F(PlanCacheTest, ShouldCacheQueryBasic) {
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    assertShouldCacheQuery(*cq);
}

TEST_F(PlanCacheTest, ShouldCacheQuerySort) {
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{}", "{a: -1}", "{_id: 0, a: 1}", "{}"));
    assertShouldCacheQuery(*cq);
}

/**
 * Non-cacheable queries.
 *
 * These queries will be sent through the planning process everytime.
 */

/**
 * Collection scan.
 *
 * This should normally be handled by the IDHack runner.
 */
TEST_F(PlanCacheTest, ShouldNotCacheQueryCollectionScan) {
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{}"));
    assertShouldNotCacheQuery(*cq);
}

/**
 * Hint.
 *
 * A hinted query implies strong user preference for a particular index.
 * Therefore, not much point in caching.
 */
TEST_F(PlanCacheTest, ShouldNotCacheQueryWithHint) {
    std::unique_ptr<CanonicalQuery> cq(
        canonicalize("{a: 1}", "{}", "{}", 0, 0, "{a: 1, b: 1}", "{}", "{}"));
    assertShouldNotCacheQuery(*cq);
}

/**
 * Min queries are a specialized case of hinted queries.
 */
TEST_F(PlanCacheTest, ShouldNotCacheQueryWithMin) {
    std::unique_ptr<CanonicalQuery> cq(
        canonicalize("{a: 1}", "{}", "{}", 0, 0, "{a: 1}", "{a: 100}", "{}"));
    assertShouldNotCacheQuery(*cq);
}

/**
 * Max queries are non-cacheable for the same reasons as min queries.
 */
TEST_F(PlanCacheTest, ShouldNotCacheQueryWithMax) {
    std::unique_ptr<CanonicalQuery> cq(
        canonicalize("{a: 1}", "{}", "{}", 0, 0, "{a: 1}", "{}", "{a: 100}"));
    assertShouldNotCacheQuery(*cq);
}

/**
 * $geoWithin queries with legacy coordinates are cacheable as long as the planner is able to come
 * up with a cacheable solution.
 */
TEST_F(PlanCacheTest, ShouldCacheQueryWithGeoWithinLegacyCoordinates) {
    std::unique_ptr<CanonicalQuery> cq(
        canonicalize("{a: {$geoWithin: "
                     "{$box: [[-180, -90], [180, 90]]}}}"));
    assertShouldCacheQuery(*cq);
}

/**
 * $geoWithin queries with GeoJSON coordinates are supported by the index bounds builder.
 */
TEST_F(PlanCacheTest, ShouldCacheQueryWithGeoWithinJSONCoordinates) {
    std::unique_ptr<CanonicalQuery> cq(
        canonicalize("{a: {$geoWithin: "
                     "{$geometry: {type: 'Polygon', coordinates: "
                     "[[[0, 0], [0, 90], [90, 0], [0, 0]]]}}}}"));
    assertShouldCacheQuery(*cq);
}

/**
 * $geoWithin queries with both legacy and GeoJSON coordinates are cacheable.
 */
TEST_F(PlanCacheTest, ShouldCacheQueryWithGeoWithinLegacyAndJSONCoordinates) {
    std::unique_ptr<CanonicalQuery> cq(
        canonicalize("{$or: [{a: {$geoWithin: {$geometry: {type: 'Polygon', "
                     "coordinates: [[[0, 0], [0, 90], "
                     "[90, 0], [0, 0]]]}}}},"
                     "{a: {$geoWithin: {$box: [[-180, -90], [180, 90]]}}}]}"));
    assertShouldCacheQuery(*cq);
}

/**
 * $geoIntersects queries are always cacheable because they support GeoJSON coordinates only.
 */
TEST_F(PlanCacheTest, ShouldCacheQueryWithGeoIntersects) {
    std::unique_ptr<CanonicalQuery> cq(
        canonicalize("{a: {$geoIntersects: "
                     "{$geometry: {type: 'Point', coordinates: "
                     "[10.0, 10.0]}}}}"));
    assertShouldCacheQuery(*cq);
}

/**
 * $geoNear queries are cacheable because we are able to distinguish between flat and spherical
 * queries.
 */
TEST_F(PlanCacheTest, ShouldNotCacheQueryWithGeoNear) {
    std::unique_ptr<CanonicalQuery> cq(
        canonicalize("{a: {$geoNear: {$geometry: {type: 'Point',"
                     "coordinates: [0,0]}, $maxDistance:100}}}"));
    assertShouldCacheQuery(*cq);
}

/**
 * Explain queries are not-cacheable because of allPlans cannot be accurately generated from stale
 * cached stats in the plan cache for non-winning plans.
 */
TEST_F(PlanCacheTest, ShouldNotCacheQueryExplain) {
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}",
                                                    "{}",
                                                    "{}",
                                                    0,
                                                    0,
                                                    "{}",
                                                    "{}",
                                                    "{}",  // min, max
                                                    true   // explain
                                                    ));
    ASSERT_TRUE(cq->getExplain());
    assertShouldNotCacheQuery(*cq);
}

/**
 * Trivially false queries are not cacheable.
 */
TEST_F(PlanCacheTest, ShouldNotCacheQueryTriviallyFalse) {
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{$alwaysFalse: 1}"));
    ASSERT_TRUE(cq->getPrimaryMatchExpression()->isTriviallyFalse());
    assertShouldNotCacheQuery(*cq);
}

TEST_F(PlanCacheTest, ShouldNotCacheIfCachingDisabled) {
    bool oldDisablePlanCache = internalQueryDisablePlanCache.load();
    ON_BLOCK_EXIT(
        [oldDisablePlanCache] { internalQueryDisablePlanCache.store(oldDisablePlanCache); });
    internalQueryDisablePlanCache.store(true);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    assertShouldNotCacheQuery(*cq);
}

void addCacheEntryForShape(const CanonicalQuery& cq, PlanCache* planCache) {
    invariant(planCache);
    auto qs = getQuerySolutionForCaching();

    const size_t nWorks = 123;
    auto decision = createDecision(1U, nWorks);
    auto callbacks = createCallback(cq, *decision);
    ASSERT_OK(planCache->set(makeKey(cq),
                             qs->cacheData->clone(),
                             NumWorks{nWorks},  // Doesn't matter.
                             Date_t{},
                             &callbacks,
                             PlanSecurityLevel::kNotSensitive));
}

TEST_F(PlanCacheTest, InactiveEntriesDisabled) {
    // Set the global flag for disabling active entries.
    internalQueryCacheDisableInactiveEntries.store(true);
    ON_BLOCK_EXIT([] { internalQueryCacheDisableInactiveEntries.store(false); });

    PlanCache planCache(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    auto qs = getQuerySolutionForCaching();
    auto key = makeKey(*cq);
    auto decision = createDecision(1U);
    auto callbacks = createCallback(*cq, *decision);

    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
    ASSERT_OK(planCache.set(makeKey(*cq),
                            qs->cacheData->clone(),
                            NumWorks{123},
                            Date_t{},
                            &callbacks,
                            PlanSecurityLevel::kNotSensitive));

    // After add, the planCache should have an _active_ entry.
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);

    // Call deactivate(). It should be a noop.
    planCache.deactivate(key);

    // The entry should still be active.
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);

    // remove() the entry.
    planCache.remove(key);
    ASSERT_EQ(planCache.size(), 0U);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
}

TEST_F(PlanCacheTest, PlanCacheLRUPolicyRemovesInactiveEntries) {
    // Use a tiny cache size.
    const size_t kCacheSize = 2;
    PlanCache planCache(kCacheSize);

    std::unique_ptr<CanonicalQuery> cqA(canonicalize("{a: 1}"));
    auto keyA = makeKey(*cqA);
    ASSERT_EQ(planCache.get(keyA).state, PlanCache::CacheEntryState::kNotPresent);
    addCacheEntryForShape(*cqA.get(), &planCache);
    // After add, the planCache should have an inactive entry.
    ASSERT_EQ(planCache.get(keyA).state, PlanCache::CacheEntryState::kPresentInactive);

    // Add a cache entry for another shape.
    std::unique_ptr<CanonicalQuery> cqB(canonicalize("{b: 1}"));
    auto keyB = makeKey(*cqB);
    ASSERT_EQ(planCache.get(keyB).state, PlanCache::CacheEntryState::kNotPresent);
    addCacheEntryForShape(*cqB.get(), &planCache);
    ASSERT_EQ(planCache.get(keyB).state, PlanCache::CacheEntryState::kPresentInactive);

    // Access the cached solution for the {a: 1} shape. Now the entry for {b: 1} will be the least
    // recently used.
    ASSERT_EQ(planCache.get(keyA).state, PlanCache::CacheEntryState::kPresentInactive);

    // Insert another entry. Since the cache size is 2, we expect the {b: 1} entry to be ejected.
    std::unique_ptr<CanonicalQuery> cqC(canonicalize("{c: 1}"));
    auto keyC = makeKey(*cqC);
    ASSERT_EQ(planCache.get(keyC).state, PlanCache::CacheEntryState::kNotPresent);
    addCacheEntryForShape(*cqC.get(), &planCache);

    // Check that {b: 1} is gone, but {a: 1} and {c: 1} both still have entries.
    ASSERT_EQ(planCache.get(keyB).state, PlanCache::CacheEntryState::kNotPresent);
    ASSERT_EQ(planCache.get(keyA).state, PlanCache::CacheEntryState::kPresentInactive);
    ASSERT_EQ(planCache.get(keyC).state, PlanCache::CacheEntryState::kPresentInactive);
}

TEST_F(PlanCacheTest, PlanCacheRemoveDeletesInactiveEntries) {
    PlanCache planCache(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    auto qs = getQuerySolutionForCaching();
    auto key = makeKey(*cq);

    auto decision = createDecision(1U);
    auto callbacks = createCallback(*cq, *decision);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
    ASSERT_OK(planCache.set(makeKey(*cq),
                            qs->cacheData->clone(),
                            NumWorks{123},  // Doesn't matter.
                            Date_t{},
                            &callbacks,
                            PlanSecurityLevel::kNotSensitive));

    // After add, the planCache should have an inactive entry.
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentInactive);

    // remove() the entry.
    planCache.remove(key);
    ASSERT_EQ(planCache.size(), 0U);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
}

TEST_F(PlanCacheTest, PlanCacheFlushDeletesInactiveEntries) {
    PlanCache planCache(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    auto qs = getQuerySolutionForCaching();
    auto key = makeKey(*cq);

    auto decision = createDecision(1U);
    auto callbacks = createCallback(*cq, *decision);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
    ASSERT_OK(planCache.set(makeKey(*cq),
                            qs->cacheData->clone(),
                            NumWorks{123},  // Doesn't matter.
                            Date_t{},
                            &callbacks,
                            PlanSecurityLevel::kNotSensitive));

    // After add, the planCache should have an inactive entry.
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentInactive);

    // Clear the plan cache. The inactive entry should now be removed.
    planCache.clear();
    ASSERT_EQ(planCache.size(), 0U);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
}

TEST_F(PlanCacheTest, AddActiveCacheEntry) {
    PlanCache planCache(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    auto qs = getQuerySolutionForCaching();
    auto key = makeKey(*cq);

    auto decision = createDecision(1U);
    auto callbacks = createCallback(*cq, *decision);
    // Check if key is in cache before and after set().
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
    ASSERT_OK(planCache.set(makeKey(*cq),
                            qs->cacheData->clone(),
                            NumWorks{123},
                            Date_t{},
                            &callbacks,
                            PlanSecurityLevel::kNotSensitive));

    // After add, the planCache should have an inactive entry.
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentInactive);

    decision = createDecision(1U, 10);
    auto callbacks1 = createCallback(*cq, *decision);
    // Calling set() again, with a solution that had a lower works value should create an active
    // entry.
    ASSERT_OK(planCache.set(makeKey(*cq),
                            qs->cacheData->clone(),
                            NumWorks{15},
                            Date_t{},
                            &callbacks1,
                            PlanSecurityLevel::kNotSensitive));
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);
    ASSERT_EQUALS(planCache.size(), 1U);

    // Clear the plan cache. The active entry should now be removed.
    planCache.clear();
    ASSERT_EQ(planCache.size(), 0U);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
}

TEST_F(PlanCacheTest, WorksValueIncreases) {
    PlanCache planCache(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    auto qs = getQuerySolutionForCaching();
    auto key = makeKey(*cq);

    {
        const size_t nWorks = 10;
        auto decision = createDecision(1U, nWorks);
        auto callbacks = createCallback(*cq, *decision);

        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
        ASSERT_OK(planCache.set(key,
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));

        // After add, the planCache should have an inactive entry.
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentInactive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), nWorks);
        ASSERT_FALSE(entry->isActive);
    }

    {
        const auto newNWorks = 50;
        auto decision = createDecision(1U, newNWorks);
        auto callbacks1 = createCallback(*cq, *decision);
        // Calling set() again, with a solution that had a higher works value. This should cause the
        // works on the original entry to be increased.
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{newNWorks},
                                Date_t{},
                                &callbacks1,
                                PlanSecurityLevel::kNotSensitive));

        // The entry should still be inactive. Its works should double though.
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentInactive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_FALSE(entry->isActive);
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), 20U);
    }

    {
        const auto newNWorks = 30;
        auto decision = createDecision(1U, newNWorks);
        auto callbacks2 = createCallback(*cq, *decision);
        // Calling set() again, with a solution that had a higher works value. This should cause the
        // works on the original entry to be increased.
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{newNWorks},
                                Date_t{},
                                &callbacks2,
                                PlanSecurityLevel::kNotSensitive));

        // The entry should still be inactive. Its works should have doubled again.
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentInactive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_FALSE(entry->isActive);
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), 40U);
    }

    {
        const size_t newNWorks = 25;
        auto decision = createDecision(1U, newNWorks);
        auto callbacks3 = createCallback(*cq, *decision);
        // Calling set() again, with a solution that has a lower works value than what's currently
        // in the cache.
        ASSERT_OK(planCache.set(key,
                                qs->cacheData->clone(),
                                NumWorks{newNWorks},
                                Date_t{},
                                &callbacks3,
                                PlanSecurityLevel::kNotSensitive));

        // The solution just run should now be in an active cache entry, with a works
        // equal to the number of works the solution took.
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->isActive);

        ASSERT(entry->debugInfo);
        ASSERT(entry->debugInfo->decision);
        auto&& decision1 = entry->debugInfo->decision;
        ASSERT_EQ(decision1->stats.candidatePlanStats[0]->common.works, 25U);
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), newNWorks);
    }

    ASSERT_EQUALS(planCache.size(), 1U);

    // Clear the plan cache. The active entry should now be removed.
    planCache.clear();
    ASSERT_EQ(planCache.size(), 0U);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
}

TEST_F(PlanCacheTest, WorksValueIncreasesByAtLeastOne) {
    // Will use a very small growth coefficient.
    const double kWorksCoeff = 1.10;

    PlanCache planCache(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    auto qs = getQuerySolutionForCaching();
    auto key = makeKey(*cq);

    {
        const size_t nWorks = 3;
        auto decision = createDecision(1U, nWorks);
        auto callbacks = createCallback(*cq, *decision);

        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));

        // After add, the planCache should have an inactive entry.
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentInactive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), nWorks);
        ASSERT_FALSE(entry->isActive);
    }

    {
        const size_t higherWorks = 50;
        auto decision = createDecision(1U, higherWorks);
        auto callbacks1 = createCallback(*cq, *decision);
        // Calling set() again, with a solution that had a higher works value. This should cause the
        // works on the original entry to be increased. In this case, since nWorks is 3,
        // multiplying by the value 1.10 will give a value of 3 (static_cast<size_t>(1.1 * 3) == 3).
        // We check that the works value is increased 1 instead.
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{higherWorks},
                                Date_t{},
                                &callbacks1,
                                PlanSecurityLevel::kNotSensitive,
                                kWorksCoeff));
    }

    {
        // The entry should still be inactive. Its works should increase by 1.
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentInactive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_FALSE(entry->isActive);
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), 4);
    }

    // Clear the plan cache. The inactive entry should now be removed.
    planCache.clear();
    ASSERT_EQ(planCache.size(), 0U);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
}

TEST_F(PlanCacheTest, SetIsNoopWhenNewEntryIsWorse) {
    PlanCache planCache(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    auto qs = getQuerySolutionForCaching();
    auto key = makeKey(*cq);

    {
        const size_t nWorks = 50;
        auto decision = createDecision(1U, nWorks);
        auto callbacks = createCallback(*cq, *decision);

        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));

        // After add, the planCache should have an inactive entry.
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentInactive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), nWorks);
        ASSERT_FALSE(entry->isActive);
    }

    {
        const size_t nWorks = 20;
        auto decision = createDecision(1U, nWorks);
        auto callbacks1 = createCallback(*cq, *decision);
        // Call set() again, with a solution that has a lower works value. This will result in an
        // active entry being created.
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks1,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->isActive);
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), nWorks);
    }

    {
        const size_t higherWorks = 100;
        auto decision = createDecision(1U, higherWorks);
        auto callbacks2 = createCallback(*cq, *decision);
        // Now call set() again, but with a solution that has a higher works value. This should be
        // a noop.
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{higherWorks},
                                Date_t{},
                                &callbacks2,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->isActive);
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), 20U);
    }
}

TEST_F(PlanCacheTest, SetOverwritesWhenNewEntryIsBetter) {
    PlanCache planCache(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    auto qs = getQuerySolutionForCaching();
    auto key = makeKey(*cq);

    {
        const size_t nWorks = 50;
        auto decision = createDecision(1U, nWorks);
        auto callbacks = createCallback(*cq, *decision);
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));
    }

    {
        // After add, the planCache should have an inactive entry.
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), 50U);
        ASSERT_FALSE(entry->isActive);
    }

    {
        const size_t nWorks = 20;
        auto decision = createDecision(1U, nWorks);
        auto callbacks1 = createCallback(*cq, *decision);
        // Call set() again, with a solution that has a lower works value. This will result in an
        // active entry being created.
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks1,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->isActive);
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), nWorks);
    }

    {
        const size_t nWorks = 10;
        auto decision = createDecision(1U, nWorks);
        auto callbacks2 = createCallback(*cq, *decision);
        // Now call set() again, with a solution that has a lower works value. The current active
        // entry should be overwritten.
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks2,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->isActive);
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), nWorks);
    }
}

TEST_F(PlanCacheTest, DeactivateCacheEntry) {
    PlanCache planCache(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
    auto qs = getQuerySolutionForCaching();
    auto key = makeKey(*cq);

    {
        const size_t nWorks = 50;
        auto decision = createDecision(1U, nWorks);
        auto callbacks = createCallback(*cq, *decision);

        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kNotPresent);
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));

        // After add, the planCache should have an inactive entry.
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), nWorks);
        ASSERT_FALSE(entry->isActive);
    }

    {
        const size_t nWorks = 20;
        auto decision = createDecision(1U, nWorks);
        auto callbacks1 = createCallback(*cq, *decision);
        // Call set() again, with a solution that has a lower works value. This will result in an
        // active entry being created.
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks1,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentActive);
        auto entry = assertGet(planCache.getEntry(key));
        ASSERT_TRUE(entry->isActive);
        ASSERT_TRUE(entry->readsOrWorks);
        ASSERT_EQ(entry->readsOrWorks->rawValue(), nWorks);
    }

    planCache.deactivate(key);
    ASSERT_EQ(planCache.get(key).state, PlanCache::CacheEntryState::kPresentInactive);

    // Be sure the entry has the same works value.
    auto entry = assertGet(planCache.getEntry(key));
    ASSERT_FALSE(entry->isActive);
    ASSERT_TRUE(entry->readsOrWorks);
    ASSERT_EQ(entry->readsOrWorks->rawValue(), 20U);
}

TEST_F(PlanCacheTest, GetMatchingStatsMatchesAndSerializesCorrectly) {
    PlanCache planCache(5000);

    // Create a cache entry with 5 works.
    {
        std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}"));
        const size_t nWorks = 5;
        auto qs = getQuerySolutionForCaching();
        auto decision = createDecision(1U, nWorks);
        auto callbacks = createCallback(*cq, *decision);
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));
    }

    // Create a second cache entry with 3 works.
    {
        std::unique_ptr<CanonicalQuery> cq(canonicalize("{b: 1}"));
        auto qs = getQuerySolutionForCaching();
        const size_t nWorks = 3;
        auto decision = createDecision(1U, nWorks);
        auto callbacks = createCallback(*cq, *decision);
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{nWorks},
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));
    }

    // Verify that the cache entries have been created.
    ASSERT_EQ(2U, planCache.size());

    // Define a serialization function which just serializes the number of works.
    const auto serializer = [](const auto key, const PlanCacheEntry& entry) {
        ASSERT_TRUE(entry.readsOrWorks);
        return BSON("works" << static_cast<int>(entry.readsOrWorks->rawValue()));
    };

    // Define a matcher which matches if the number of works exceeds 4.
    const auto matcher = [](const BSONObj& serializedEntry) {
        BSONElement worksElt = serializedEntry["works"];
        return worksElt && worksElt.number() > 4;
    };

    // Verify the output of getMatchingStats().
    auto getStatsResult =
        planCache.getMatchingStats({} /* cacheKeyFilterFunc */, serializer, matcher);
    ASSERT_EQ(1U, getStatsResult.size());
    ASSERT_BSONOBJ_EQ(BSON("works" << 5), getStatsResult[0]);
}

/**
 * Each test in the CachePlanSelectionTest suite goes through
 * the following flow:
 *
 * 1) Run QueryPlanner::plan on the query, with specified indices
 * available. This simulates the case in which we failed to plan from
 * the plan cache, and fell back on selecting a plan ourselves. The
 * enumerator will run, and cache data will be stashed into each solution
 * that it generates.
 *
 * 2) Use firstMatchingSolution to select one of the mult-planned solutions generated by
 * QueryPlanner::plan. This simulates the multi plan runner picking the "best solution".
 *
 * 3) The cache data stashed inside the "best solution" is used to
 * make a CachedSolution which looks exactly like the data structure that
 * would be returned from the cache. This simulates a plan cache hit.
 *
 * 4) Call QueryPlanner::planFromCache, passing it the CachedSolution.
 * This exercises the code which is able to map from a CachedSolution to
 * a full-blown QuerySolution. Finally, assert that the query solution
 * recovered from the cache is identical to the original "best solution".
 */
class CachePlanSelectionTest : public unittest::Test {
protected:
    void setUp() override {
        params.mainCollectionInfo.options = QueryPlannerParams::INCLUDE_COLLSCAN;
        addIndex(BSON("_id" << 1), std::string{IndexConstants::kIdIndexName});
    }

    void addIndex(BSONObj keyPattern, const std::string& indexName, bool multikey = false) {
        params.mainCollectionInfo.indexes.push_back(
            IndexEntry(keyPattern,
                       IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                       IndexConfig::kLatestIndexVersion,
                       multikey,
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

    void addIndex(BSONObj keyPattern, const std::string& indexName, bool multikey, bool sparse) {
        params.mainCollectionInfo.indexes.push_back(
            IndexEntry(keyPattern,
                       IndexNames::nameToType(IndexNames::findPluginName(keyPattern)),
                       IndexConfig::kLatestIndexVersion,
                       multikey,
                       {},
                       {},
                       sparse,
                       false,
                       IndexEntry::Identifier{indexName},
                       nullptr,
                       BSONObj(),
                       nullptr,
                       nullptr));
    }

    void addIndex(BSONObj keyPattern, const std::string& indexName, CollatorInterface* collator) {
        IndexEntry entry(keyPattern,
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
        entry.collator = collator;
        params.mainCollectionInfo.indexes.push_back(entry);
    }

    //
    // Execute planner.
    //

    void runQuery(BSONObj query) {
        runQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), 0, 0);
    }

    void runQuerySortProj(const BSONObj& query, const BSONObj& sort, const BSONObj& proj) {
        runQuerySortProjSkipLimit(query, sort, proj, 0, 0);
    }

    void runQuerySkipLimit(const BSONObj& query, long long skip, long long limit) {
        runQuerySortProjSkipLimit(query, BSONObj(), BSONObj(), skip, limit);
    }

    void runQueryHint(const BSONObj& query, const BSONObj& hint) {
        runQuerySortProjSkipLimitHint(query, BSONObj(), BSONObj(), 0, 0, hint);
    }

    void runQuerySortProjSkipLimit(const BSONObj& query,
                                   const BSONObj& sort,
                                   const BSONObj& proj,
                                   long long skip,
                                   long long limit) {
        runQuerySortProjSkipLimitHint(query, sort, proj, skip, limit, BSONObj());
    }

    void runQuerySortHint(const BSONObj& query, const BSONObj& sort, const BSONObj& hint) {
        runQuerySortProjSkipLimitHint(query, sort, BSONObj(), 0, 0, hint);
    }

    void runQueryHintMinMax(const BSONObj& query,
                            const BSONObj& hint,
                            const BSONObj& minObj,
                            const BSONObj& maxObj) {
        runQueryFull(query, BSONObj(), BSONObj(), 0, 0, hint, minObj, maxObj);
    }

    void runQuerySortProjSkipLimitHint(const BSONObj& query,
                                       const BSONObj& sort,
                                       const BSONObj& proj,
                                       long long skip,
                                       long long limit,
                                       const BSONObj& hint) {
        runQueryFull(query, sort, proj, skip, limit, hint, BSONObj(), BSONObj());
    }

    void runQueryFull(const BSONObj& query,
                      const BSONObj& sort,
                      const BSONObj& proj,
                      long long skip,
                      long long limit,
                      const BSONObj& hint,
                      const BSONObj& minObj,
                      const BSONObj& maxObj) {
        QueryTestServiceContext serviceContext;
        auto opCtx = serviceContext.makeOperationContext();

        // Clean up any previous state from a call to runQueryFull or runQueryAsCommand.
        solns.clear();

        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.collection");
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(query);
        findCommand->setSort(sort);
        findCommand->setProjection(proj);
        if (skip) {
            findCommand->setSkip(skip);
        }
        if (limit) {
            findCommand->setLimit(limit);
        }
        findCommand->setHint(hint);
        findCommand->setMin(minObj);
        findCommand->setMax(maxObj);
        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{
                .findCommand = std::move(findCommand),
                .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});
        auto statusWithMultiPlanSolns = QueryPlanner::plan(*cq, params);
        ASSERT_OK(statusWithMultiPlanSolns.getStatus());
        solns = std::move(statusWithMultiPlanSolns.getValue());
    }

    void runQueryAsCommand(const BSONObj& cmdObj) {
        QueryTestServiceContext serviceContext;
        auto opCtx = serviceContext.makeOperationContext();

        // Clean up any previous state from a call to runQueryFull or runQueryAsCommand.
        solns.clear();

        std::unique_ptr<FindCommandRequest> findCommand(
            query_request_helper::makeFromFindCommandForTests(cmdObj));
        auto cq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{
                .findCommand = std::move(findCommand),
                .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});
        auto statusWithMultiPlanSolns = QueryPlanner::plan(*cq, params);
        ASSERT_OK(statusWithMultiPlanSolns.getStatus());
        solns = std::move(statusWithMultiPlanSolns.getValue());
    }

    //
    // Solution introspection.
    //

    void dumpSolutions(str::stream& ost) const {
        for (auto&& soln : solns) {
            ost << soln->toString() << '\n';
        }
    }

    /**
     * Returns number of generated solutions matching JSON.
     */
    size_t numSolutionMatches(const std::string& solnJson) const {
        BSONObj testSoln = fromjson(solnJson);
        size_t matches = 0;
        for (auto&& soln : solns) {
            auto matchStatus = QueryPlannerTestLib::solutionMatches(testSoln, soln->root());
            if (matchStatus.isOK()) {
                ++matches;
            } else {
                LOGV2_DEBUG(5619204,
                            2,
                            "Mismatching solution: {reason}",
                            "reason"_attr = matchStatus.reason());
            }
        }
        return matches;
    }

    /**
     * Verifies that the solution tree represented in json by 'solnJson' is
     * one of the solutions generated by QueryPlanner.
     *
     * The number of expected matches, 'numMatches', could be greater than
     * 1 if solutions differ only by the pattern of index tags on a filter.
     */
    void assertSolutionExists(const std::string& solnJson, size_t numMatches = 1) const {
        size_t matches = numSolutionMatches(solnJson);
        if (numMatches == matches) {
            return;
        }
        str::stream ss;
        ss << "expected " << numMatches << " matches for solution " << solnJson << " but got "
           << matches << " instead. all solutions generated: " << '\n';
        dumpSolutions(ss);
        FAIL(std::string(ss));
    }

    /**
     * Plan 'query' from the cache with sort order 'sort', projection 'proj', and collation
     * 'collation'. A mock cache entry is created using the cacheData stored inside the
     * QuerySolution 'soln'.
     */
    std::unique_ptr<QuerySolution> planQueryFromCache(const BSONObj& query,
                                                      const BSONObj& sort,
                                                      const BSONObj& proj,
                                                      const BSONObj& collation,
                                                      const QuerySolution& soln) const {
        QueryTestServiceContext serviceContext;
        auto opCtx = serviceContext.makeOperationContext();

        NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.collection");
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(query);
        findCommand->setSort(sort);
        findCommand->setProjection(proj);
        findCommand->setCollation(collation);
        auto scopedCq = std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{
                .findCommand = std::move(findCommand),
                .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});

        // Create a CachedSolution the long way..
        // QuerySolution -> PlanCacheEntry -> CachedSolution
        QuerySolution qs{};
        qs.cacheData = soln.cacheData->clone();

        uint32_t planCacheShapeHash = ck.planCacheShapeHash();
        uint32_t planCacheKey = planCacheShapeHash;
        auto decision = createDecision(1U);
        auto entry =
            PlanCacheEntry::create(qs.cacheData->clone(),
                                   planCacheShapeHash,
                                   planCacheKey,
                                   0, /* planCacheCommandKey */
                                   Date_t(),
                                   false /* isActive  */,
                                   PlanSecurityLevel::kNotSensitive,
                                   ReadsOrWorks{NumWorks{0}},
                                   plan_cache_util::buildDebugInfo(*scopedCq, std::move(decision)));
        CachedSolution cachedSoln(*entry);

        auto statusWithQs =
            QueryPlanner::planFromCache(*scopedCq, params, *cachedSoln.cachedPlan.get());
        ASSERT_OK(statusWithQs.getStatus());
        return std::move(statusWithQs.getValue());
    }

    /**
     * @param solnJson -- a json representation of a query solution.
     *
     * Returns the first solution matching 'solnJson', or fails if
     * no match is found.
     */
    QuerySolution* firstMatchingSolution(const std::string& solnJson) const {
        BSONObj testSoln = fromjson(solnJson);
        for (auto&& soln : solns) {
            auto matchStatus = QueryPlannerTestLib::solutionMatches(testSoln, soln->root());
            if (matchStatus.isOK()) {
                return soln.get();
            } else {
                LOGV2_DEBUG(5619205,
                            2,
                            "Mismatching solution: {reason}",
                            "reason"_attr = matchStatus.reason());
            }
        }

        str::stream ss;
        ss << "Could not find a match for solution " << solnJson
           << " All solutions generated: " << '\n';
        dumpSolutions(ss);
        FAIL(std::string(ss));

        return nullptr;
    }

    /**
     * Assert that the QuerySolution 'trueSoln' matches the JSON-based representation
     * of the solution in 'solnJson'.
     *
     * Relies on solutionMatches() -- see query_planner_test_lib.h
     */
    void assertSolutionMatches(QuerySolution* trueSoln, const std::string& solnJson) const {
        BSONObj testSoln = fromjson(solnJson);
        auto matchStatus = QueryPlannerTestLib::solutionMatches(testSoln, trueSoln->root());
        if (!matchStatus.isOK()) {
            str::stream ss;
            ss << "Expected solution " << solnJson
               << " did not match true solution: " << trueSoln->toString()
               << ". Reason: " << matchStatus.reason() << '\n';
            FAIL(std::string(ss));
        }
    }

    /**
     * Overloaded so that it is not necessary to specificy sort and project.
     */
    void assertPlanCacheRecoversSolution(const BSONObj& query, const std::string& solnJson) {
        assertPlanCacheRecoversSolution(query, BSONObj(), BSONObj(), BSONObj(), solnJson);
    }

    /**
     * First, the solution matching 'solnJson' is retrieved from the vector
     * of solutions generated by QueryPlanner::plan. This solution is
     * then passed into planQueryFromCache(). Asserts that the solution
     * generated by QueryPlanner::planFromCache matches 'solnJson'.
     *
     * Must be called after calling one of the runQuery* methods.
     *
     * Together, 'query', 'sort', 'proj', and 'collation' should specify the query which was
     * previously run using one of the runQuery* methods.
     */
    void assertPlanCacheRecoversSolution(const BSONObj& query,
                                         const BSONObj& sort,
                                         const BSONObj& proj,
                                         const BSONObj& collation,
                                         const std::string& solnJson) {
        auto bestSoln = firstMatchingSolution(solnJson);
        auto planSoln = planQueryFromCache(query, sort, proj, collation, *bestSoln);
        assertSolutionMatches(planSoln.get(), solnJson);
    }

    /**
     * Check that the solution will not be cached. The planner will store
     * cache data inside non-cachable solutions, but will not do so for
     * non-cachable solutions. Therefore, we just have to check that
     * cache data is NULL.
     */
    void assertNotCached(const std::string& solnJson) {
        QuerySolution* bestSoln = firstMatchingSolution(solnJson);
        ASSERT(nullptr != bestSoln);
        ASSERT(nullptr == bestSoln->cacheData.get());
    }

    static const PlanCacheKey ck;

    BSONObj queryObj;
    QueryPlannerParams params{QueryPlannerParams::ArgsForTest{}};
    std::vector<std::unique_ptr<QuerySolution>> solns;
};

const std::string mockKey("mock_cache_key");
const PlanCacheKey CachePlanSelectionTest::ck{
    PlanCacheKeyInfo{mockKey, "", query_settings::QuerySettings()}};

//
// Equality
//

TEST_F(CachePlanSelectionTest, EqualityIndexScan) {
    addIndex(BSON("x" << 1), "x_1");
    runQuery(BSON("x" << 5));

    assertPlanCacheRecoversSolution(BSON("x" << 5),
                                    "{fetch: {filter: null, node: {ixscan: {pattern: {x: 1}}}}}");
}

TEST_F(CachePlanSelectionTest, EqualityIndexScanWithTrailingFields) {
    addIndex(BSON("x" << 1 << "y" << 1), "x_1_y_1");
    runQuery(BSON("x" << 5));

    assertPlanCacheRecoversSolution(
        BSON("x" << 5), "{fetch: {filter: null, node: {ixscan: {pattern: {x: 1, y: 1}}}}}");
}

//
// Geo
//

TEST_F(CachePlanSelectionTest, Basic2DSphereNonNear) {
    addIndex(BSON("a" << "2dsphere"), "a_2dsphere");
    BSONObj query;

    query = fromjson(
        "{a: {$geoIntersects: {$geometry: {type: 'Point',"
        "coordinates: [10.0, 10.0]}}}}");
    runQuery(query);
    assertPlanCacheRecoversSolution(query, "{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");

    query = fromjson("{a : { $geoWithin : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}");
    runQuery(query);
    assertPlanCacheRecoversSolution(query, "{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}}");
}

TEST_F(CachePlanSelectionTest, Basic2DSphereGeoNear) {
    addIndex(BSON("a" << "2dsphere"), "a_2dsphere");
    BSONObj query;

    query = fromjson("{a: {$nearSphere: [0,0], $maxDistance: 0.31 }}");
    runQuery(query);
    assertPlanCacheRecoversSolution(query,
                                    "{geoNear2dsphere: {pattern: {a: '2dsphere'}, "
                                    "bounds: {a: [['MinKey', 'MaxKey', true, true]]}}}");

    query = fromjson(
        "{a: {$geoNear: {$geometry: {type: 'Point', coordinates: [0,0]},"
        "$maxDistance:100}}}");
    runQuery(query);
    assertPlanCacheRecoversSolution(query,
                                    "{geoNear2dsphere: {pattern: {a: '2dsphere'}, "
                                    "bounds: {a: [['MinKey', 'MaxKey', true, true]]}}}");
}

TEST_F(CachePlanSelectionTest, Basic2DSphereGeoNearReverseCompound) {
    addIndex(BSON("x" << 1), "x_1");
    addIndex(BSON("x" << 1 << "a"
                      << "2dsphere"),
             "x_1_a_2dsphere");
    BSONObj query = fromjson("{x:1, a: {$nearSphere: [0,0], $maxDistance: 0.31 }}");
    runQuery(query);
    assertPlanCacheRecoversSolution(
        query,
        "{geoNear2dsphere: {pattern: {x: 1, a: '2dsphere'}, "
        "bounds: {x: [[1, 1, true, true]], a: [['MinKey', 'MaxKey', true, true]]}}}");
}

TEST_F(CachePlanSelectionTest, TwoDSphereNoGeoPred) {
    addIndex(BSON("x" << 1 << "a"
                      << "2dsphere"),
             "x_1_a_2dsphere");
    runQuery(BSON("x" << 1));
    assertPlanCacheRecoversSolution(BSON("x" << 1),
                                    "{fetch: {node: {ixscan: {pattern: {x: 1, a: '2dsphere'}}}}}");
}

TEST_F(CachePlanSelectionTest, Or2DSphereNonNear) {
    addIndex(BSON("a" << "2dsphere"), "a_2dsphere");
    addIndex(BSON("b" << "2dsphere"), "b_2dsphere");
    BSONObj query = fromjson(
        "{$or: [ {a: {$geoIntersects: {$geometry: {type: 'Point', coordinates: [10.0, 10.0]}}}},"
        " {b: {$geoWithin: { $centerSphere: [[ 10, 20 ], 0.01 ] } }} ]}");

    runQuery(query);
    assertPlanCacheRecoversSolution(
        query,
        "{or: {nodes: [{fetch: {node: {ixscan: {pattern: {a: '2dsphere'}}}}},"
        "{fetch: {node: {ixscan: {pattern: {b: '2dsphere'}}}}}]}}");
}

// Regression test for SERVER-24320. Tests that the PlanCacheIndexTree has the same sort order as
// the MatchExpression used to generate the plan cache key.
TEST_F(CachePlanSelectionTest, AndWithinPolygonWithinCenterSphere) {
    addIndex(BSON("a" << "2dsphere"
                      << "b" << 1),
             "a_2dsphere_b_2dsphere");

    BSONObj query = fromjson(
        "{$and: [{b: 1}, {a: {$within: {$polygon: [[0, 0], [0, 0], [0, 0], [0, 0]]}}}, {a: "
        "{$within: {$centerSphere: [[0, 0], 0]}}}]}");

    runQuery(query);
    assertPlanCacheRecoversSolution(query,
                                    "{fetch: {node: {ixscan: {pattern: {a: '2dsphere', b: 1}}}}}");
}

// $** index
TEST_F(CachePlanSelectionTest, WildcardIxScan) {
    auto entryProjExecPair = makeWildcardEntry(BSON("$**" << 1));
    params.mainCollectionInfo.indexes.push_back(entryProjExecPair.first);

    BSONObj query = fromjson("{a: 1, b: 1}");
    runQuery(query);

    const auto kPlanA =
        "{fetch: {node: {ixscan: "
        "{bounds: {$_path: [['a', 'a', true, true]], a: [[1, 1, true, true]]},"
        "pattern: {$_path: 1, a:1}}}}}";

    const auto kPlanB =
        "{fetch: {node: {ixscan: "
        "{bounds: {$_path: [['b', 'b', true, true]], b: [[1, 1, true, true]]},"
        "pattern: {$_path: 1, b:1}}}}}";

    assertPlanCacheRecoversSolution(query, kPlanA);
    assertPlanCacheRecoversSolution(query, kPlanB);

    // Query with fields in a different order, so that index entry expansion results in the list of
    // indexes being in a different order. Should still yield the same plans.
    BSONObj queryOtherDir = fromjson("{b: 1, a: 1}");
    assertPlanCacheRecoversSolution(query, kPlanA);
    assertPlanCacheRecoversSolution(query, kPlanB);
}

//
// tree operations
//

TEST_F(CachePlanSelectionTest, TwoPredicatesAnding) {
    addIndex(BSON("x" << 1), "x_1");
    BSONObj query = fromjson("{$and: [ {x: {$gt: 1}}, {x: {$lt: 3}} ] }");
    runQuery(query);
    assertPlanCacheRecoversSolution(
        query, "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {x: 1}}}}}");
}

TEST_F(CachePlanSelectionTest, SimpleOr) {
    addIndex(BSON("a" << 1), "a_1");
    BSONObj query = fromjson("{$or: [{a: 20}, {a: 21}]}");
    runQuery(query);
    assertPlanCacheRecoversSolution(
        query, "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {a:1}}}}}");
}

TEST_F(CachePlanSelectionTest, OrWithAndChild) {
    addIndex(BSON("a" << 1), "a_1");
    BSONObj query = fromjson("{$or: [{a: 20}, {$and: [{a:1}, {b:7}]}]}");
    runQuery(query);
    assertPlanCacheRecoversSolution(query,
                                    "{fetch: {filter: null, node: {or: {nodes: ["
                                    "{ixscan: {filter: null, pattern: {a: 1}}}, "
                                    "{fetch: {filter: {b: 7}, node: {ixscan: "
                                    "{filter: null, pattern: {a: 1}}}}}]}}}}");
}

TEST_F(CachePlanSelectionTest, AndWithUnindexedOrChild) {
    addIndex(BSON("a" << 1), "a_1");
    BSONObj query = fromjson("{a:20, $or: [{b:1}, {c:7}]}");
    runQuery(query);
    assertPlanCacheRecoversSolution(query,
                                    "{fetch: {filter: {$or: [{b: 1}, {c: 7}]}, node: "
                                    "{ixscan: {filter: null, pattern: {a: 1}}}}}");
}


TEST_F(CachePlanSelectionTest, AndWithOrWithOneIndex) {
    addIndex(BSON("b" << 1), "b_1");
    addIndex(BSON("a" << 1), "a_1");
    BSONObj query = fromjson("{$or: [{b:1}, {c:7}], a:20}");
    runQuery(query);
    assertPlanCacheRecoversSolution(query,
                                    "{fetch: {filter: {$or: [{b: 1}, {c: 7}]}, "
                                    "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

//
// Sort orders
//

// SERVER-1205.
TEST_F(CachePlanSelectionTest, MergeSort) {
    addIndex(BSON("a" << 1 << "c" << 1), "a_1_c_1");
    addIndex(BSON("b" << 1 << "c" << 1), "b_1_c_1");

    BSONObj query = fromjson("{$or: [{a:1}, {b:1}]}");
    BSONObj sort = BSON("c" << 1);
    runQuerySortProj(query, sort, BSONObj());

    assertPlanCacheRecoversSolution(
        query,
        sort,
        BSONObj(),
        BSONObj(),
        "{fetch: {node: {mergeSort: {nodes: "
        "[{ixscan: {pattern: {a: 1, c: 1}}}, {ixscan: {pattern: {b: 1, c: 1}}}]}}}}");
}

// SERVER-1205 as well.
TEST_F(CachePlanSelectionTest, NoMergeSortIfNoSortWanted) {
    addIndex(BSON("a" << 1 << "c" << 1), "a_1_c_1");
    addIndex(BSON("b" << 1 << "c" << 1), "b_1_c_1");

    BSONObj query = fromjson("{$or: [{a:1}, {b:1}]}");
    runQuerySortProj(query, BSONObj(), BSONObj());

    assertPlanCacheRecoversSolution(query,
                                    BSONObj(),
                                    BSONObj(),
                                    BSONObj(),
                                    "{fetch: {filter: null, node: {or: {nodes: ["
                                    "{ixscan: {filter: null, pattern: {a: 1, c: 1}}}, "
                                    "{ixscan: {filter: null, pattern: {b: 1, c: 1}}}]}}}}");
}

// Disabled: SERVER-10801.
/*
TEST_F(CachePlanSelectionTest, SortOnGeoQuery) {
    addIndex(BSON("timestamp" << -1 << "position" << "2dsphere"), "timestamp_-1_position_2dsphere");
    BSONObj query = fromjson("{position: {$geoWithin: {$geometry: {type: \"Polygon\", "
                             "coordinates: [[[1, 1], [1, 90], [180, 90], "
                             "[180, 1], [1, 1]]]}}}}");
    BSONObj sort = fromjson("{timestamp: -1}");
    runQuerySortProj(query, sort, BSONObj());

    assertPlanCacheRecoversSolution(query, sort, BSONObj(),
        "{fetch: {node: {ixscan: {pattern: {timestamp: -1, position: '2dsphere'}}}}}");
}
*/

// SERVER-9257
TEST_F(CachePlanSelectionTest, CompoundGeoNoGeoPredicate) {
    addIndex(BSON("creationDate" << 1 << "foo.bar"
                                 << "2dsphere"),
             "creationDate_1_foo.bar_2dsphere");
    BSONObj query = fromjson("{creationDate: {$gt: 7}}");
    BSONObj sort = fromjson("{creationDate: 1}");
    runQuerySortProj(query, sort, BSONObj());

    assertPlanCacheRecoversSolution(
        query,
        sort,
        BSONObj(),
        BSONObj(),
        "{fetch: {node: {ixscan: {pattern: {creationDate: 1, 'foo.bar': '2dsphere'}}}}}");
}

TEST_F(CachePlanSelectionTest, ReverseScanForSort) {
    addIndex(BSON("_id" << 1), "_id_1");
    runQuerySortProj(BSONObj(), fromjson("{_id: -1}"), BSONObj());
    assertPlanCacheRecoversSolution(
        BSONObj(),
        fromjson("{_id: -1}"),
        BSONObj(),
        BSONObj(),
        "{fetch: {filter: null, node: {ixscan: {filter: null, pattern: {_id: 1}}}}}");
}

//
// Caching collection scans.
//

TEST_F(CachePlanSelectionTest, CollscanNoUsefulIndices) {
    addIndex(BSON("a" << 1 << "b" << 1), "a_1_b_1");
    addIndex(BSON("c" << 1), "c_1");
    BSONObj query = fromjson("{b: 4}");
    runQuery(query);
    assertPlanCacheRecoversSolution(query, "{cscan: {filter: {b: 4}, dir: 1}}");
}

TEST_F(CachePlanSelectionTest, CollscanOrWithoutEnoughIndices) {
    addIndex(BSON("a" << 1), "a_1");
    BSONObj query = fromjson("{$or: [{a: 20}, {b: 21}]}");
    runQuery(query);
    assertPlanCacheRecoversSolution(query, "{cscan: {filter: {$or:[{a:20},{b:21}]}, dir: 1}}");
}

TEST_F(CachePlanSelectionTest, CollscanMergeSort) {
    addIndex(BSON("a" << 1 << "c" << 1), "a_1_c_1");
    addIndex(BSON("b" << 1 << "c" << 1), "b_1_c_1");

    BSONObj query = fromjson("{$or: [{a:1}, {b:1}]}");
    BSONObj sort = BSON("c" << 1);
    runQuerySortProj(query, sort, BSONObj());

    assertPlanCacheRecoversSolution(query,
                                    sort,
                                    BSONObj(),
                                    BSONObj(),
                                    "{sort: {pattern: {c: 1}, limit: 0, type: 'simple', node: "
                                    "{cscan: {dir: 1}}}}");
}

//
// Caching plans that use multikey indexes.
//

TEST_F(CachePlanSelectionTest, CachedPlanForCompoundMultikeyIndexCanCompoundBounds) {
    params.mainCollectionInfo.options =
        QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;

    const bool multikey = true;
    addIndex(BSON("a" << 1 << "b" << 1), "a_1_b_1", multikey);

    BSONObj query = fromjson("{a: 2, b: 3}");
    runQuery(query);

    assertPlanCacheRecoversSolution(
        query,
        "{fetch: {filter: null, node: {ixscan: {pattern: {a: 1, b: 1}, "
        "bounds: {a: [[2, 2, true, true]], b: [[3, 3, true, true]]}}}}}");
}

TEST_F(CachePlanSelectionTest,
       CachedPlanForSelfIntersectionOfMultikeyIndexPointRangesCannotIntersectBounds) {
    // Enable a merge sort based index intersection plan to be generated.
    RAIIServerParameterControllerForTest truncateFeatureFlag{
        "internalQueryPlannerEnableSortIndexIntersection", true};
    params.mainCollectionInfo.options =
        QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;

    const bool multikey = true;
    addIndex(BSON("a" << 1), "a_1", multikey);

    BSONObj query = fromjson("{$and: [{a: 2}, {a: 3}]}");
    runQuery(query);

    assertPlanCacheRecoversSolution(
        query,
        "{fetch: {filter: {$and: [{a: 2}, {a: 3}]}, node: {andSorted: {nodes: ["
        "{ixscan: {pattern: {a: 1}, bounds: {a: [[2, 2, true, true]]}}}, "
        "{ixscan: {pattern: {a: 1}, bounds: {a: [[3, 3, true, true]]}}}]}}}}");
}

TEST_F(CachePlanSelectionTest,
       CachedPlanForSelfIntersectionOfMultikeyIndexNonPointRangesCannotIntersectBounds) {
    // Enable a hash-based index intersection plan to be generated because we are scanning a
    // non-point range on the "a" field.
    bool oldEnableHashIntersection = internalQueryPlannerEnableHashIntersection.load();
    ON_BLOCK_EXIT([oldEnableHashIntersection] {
        internalQueryPlannerEnableHashIntersection.store(oldEnableHashIntersection);
    });
    internalQueryPlannerEnableHashIntersection.store(true);
    params.mainCollectionInfo.options =
        QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;

    const bool multikey = true;
    addIndex(BSON("a" << 1), "a_1", multikey);

    BSONObj query = fromjson("{$and: [{a: {$gte: 2}}, {a: {$lt: 3}}]}");
    runQuery(query);

    assertPlanCacheRecoversSolution(
        query,
        "{fetch: {filter: {$and:[{a:{$gte:2}},{a:{$lt:3}}]}, node: {andHash: {nodes: ["
        "{ixscan: {pattern: {a: 1}, bounds: {a: [[2, Infinity, true, true]]}}}, "
        "{ixscan: {pattern: {a: 1}, bounds: {a: [[-Infinity, 3, true, false]]}}}]}}}}");
}


TEST_F(CachePlanSelectionTest, CachedPlanForIntersectionOfMultikeyIndexesWhenUsingElemMatch) {
    // Enable a merge sort based index intersection plan to be generated.
    RAIIServerParameterControllerForTest truncateFeatureFlag{
        "internalQueryPlannerEnableSortIndexIntersection", true};
    params.mainCollectionInfo.options =
        QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;

    const bool multikey = true;
    addIndex(BSON("a.b" << 1), "a.b_1", multikey);
    addIndex(BSON("a.c" << 1), "a.c_1", multikey);

    BSONObj query = fromjson("{a: {$elemMatch: {b: 2, c: 3}}}");
    runQuery(query);

    assertPlanCacheRecoversSolution(
        query,
        "{fetch: {filter: {a: {$elemMatch: {b: 2, c: 3}}}, node: {andSorted: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1}, bounds: {'a.b': [[2, 2, true, true]]}}},"
        "{ixscan: {pattern: {'a.c': 1}, bounds: {'a.c': [[3, 3, true, true]]}}}]}}}}");
}

TEST_F(CachePlanSelectionTest, CachedPlanForIntersectionWithNonMultikeyIndexCanIntersectBounds) {
    // Enable a hash-based index intersection plan to be generated because we are scanning a
    // non-point range on the "a.c" field.
    bool oldEnableHashIntersection = internalQueryPlannerEnableHashIntersection.load();
    ON_BLOCK_EXIT([oldEnableHashIntersection] {
        internalQueryPlannerEnableHashIntersection.store(oldEnableHashIntersection);
    });
    internalQueryPlannerEnableHashIntersection.store(true);
    params.mainCollectionInfo.options =
        QueryPlannerParams::NO_TABLE_SCAN | QueryPlannerParams::INDEX_INTERSECTION;

    const bool multikey = true;
    addIndex(BSON("a.b" << 1), "a.b_1", multikey);
    addIndex(BSON("a.c" << 1), "a.c_1", !multikey);

    BSONObj query = fromjson("{'a.b': 2, 'a.c': {$gte: 0, $lt: 10}}");
    runQuery(query);

    assertPlanCacheRecoversSolution(
        query,
        "{fetch: {node: {andHash: {nodes: ["
        "{ixscan: {pattern: {'a.b': 1}, bounds: {'a.b': [[2, 2, true, true]]}}},"
        "{ixscan: {pattern: {'a.c': 1}, bounds: {'a.c': [[0, 10, true, false]]}}}]}}}}");
}

//
// Check queries that, at least for now, are not cached.
//

TEST_F(CachePlanSelectionTest, GeoNear2DNotCached) {
    addIndex(BSON("a" << "2d"), "a_2d");
    runQuery(fromjson("{a: {$near: [0,0], $maxDistance:0.3 }}"));
    assertNotCached("{geoNear2d: {a: '2d'}}");
}

TEST_F(CachePlanSelectionTest, MinNotCached) {
    addIndex(BSON("a" << 1), "a_1");
    runQueryHintMinMax(BSONObj(), fromjson("{a: 1}"), fromjson("{a: 1}"), BSONObj());
    assertNotCached(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(CachePlanSelectionTest, MaxNotCached) {
    addIndex(BSON("a" << 1), "a_1");
    runQueryHintMinMax(BSONObj(), fromjson("{a: 1}"), BSONObj(), fromjson("{a: 1}"));
    assertNotCached(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

TEST_F(CachePlanSelectionTest, NaturalHintNotCached) {
    addIndex(BSON("a" << 1), "a_1");
    addIndex(BSON("b" << 1), "b_1");
    BSONObj query = fromjson("{a: 1}");
    runQuerySortHint(query, BSON("b" << 1), BSON("$natural" << 1));
    assertNotCached(
        "{sort: {pattern: {b: 1}, limit: 0, type: 'simple', node: "
        "{cscan: {filter: {a: 1}, dir: 1}}}}");
}

TEST_F(CachePlanSelectionTest, HintValidNotCached) {
    addIndex(BSON("a" << 1), "a_1");
    runQueryHint(BSONObj(), fromjson("{a: 1}"));
    assertNotCached(
        "{fetch: {filter: null, "
        "node: {ixscan: {filter: null, pattern: {a: 1}}}}}");
}

//
// Queries using '2d' indices are not cached.
//

TEST_F(CachePlanSelectionTest, Basic2DNonNearNotCached) {
    addIndex(BSON("a" << "2d"), "a_2d");
    BSONObj query;

    // Polygon
    query = fromjson("{a : { $within: { $polygon : [[0,0], [2,0], [4,0]] } }}");
    runQuery(query);
    assertNotCached("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");

    // Center
    query = fromjson("{a : { $within : { $center : [[ 5, 5 ], 7 ] } }}");
    runQuery(query);
    assertNotCached("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");

    // Centersphere
    query = fromjson("{a : { $within : { $centerSphere : [[ 10, 20 ], 0.01 ] } }}");
    runQuery(query);
    assertNotCached("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");

    // Within box.
    query = fromjson("{a : {$within: {$box : [[0,0],[9,9]]}}}");
    runQuery(query);
    assertNotCached("{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}}");
}

TEST_F(CachePlanSelectionTest, Or2DNonNearNotCached) {
    addIndex(BSON("a" << "2d"), "a_2d");
    addIndex(BSON("b" << "2d"), "b_2d");
    BSONObj query = fromjson(
        "{$or: [ {a : { $within : { $polygon : [[0,0], [2,0], [4,0]] } }},"
        " {b : { $within : { $center : [[ 5, 5 ], 7 ] } }} ]}");

    runQuery(query);
    assertNotCached(
        "{or: {nodes: [{fetch: {node: {ixscan: {pattern: {a: '2d'}}}}},"
        "{fetch: {node: {ixscan: {pattern: {b: '2d'}}}}}]}}");
}

//
// Collation.
//

TEST_F(CachePlanSelectionTest, MatchingCollation) {
    CollatorInterfaceMock collator(CollatorInterfaceMock::MockType::kReverseString);
    addIndex(BSON("x" << 1), "x_1", &collator);
    runQueryAsCommand(
        fromjson("{find: 'testns', filter: {x: 'foo'}, collation: {locale: 'mock_reverse_string'}, "
                 "'$db': 'test'}"));

    assertPlanCacheRecoversSolution(BSON("x" << "bar"),
                                    BSONObj(),
                                    BSONObj(),
                                    BSON("locale" << "mock_reverse_string"),
                                    "{fetch: {node: {ixscan: {pattern: {x: 1}}}}}");
}

TEST_F(CachePlanSelectionTest, ContainedOr) {
    addIndex(BSON("b" << 1 << "a" << 1), "b_1_a_1");
    addIndex(BSON("c" << 1 << "a" << 1), "c_1_a_1");
    BSONObj query = fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}");
    runQuery(query);
    assertPlanCacheRecoversSolution(
        query,
        "{fetch: {filter: null, node: {or: {nodes: ["
        "{ixscan: {pattern: {b: 1, a: 1}, bounds: {b: [[6, 6, true, true]], a: [[5, 5, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1, a: 1}, bounds: {c: [[7, 7, true, true]], a: [[5, 5, true, "
        "true]]}}}"
        "]}}}}");
}

TEST_F(CachePlanSelectionTest, ContainedOrAndIntersection) {
    bool oldEnableHashIntersection = internalQueryPlannerEnableHashIntersection.load();
    ON_BLOCK_EXIT([oldEnableHashIntersection] {
        internalQueryPlannerEnableHashIntersection.store(oldEnableHashIntersection);
    });
    internalQueryPlannerEnableHashIntersection.store(true);
    params.mainCollectionInfo.options =
        QueryPlannerParams::INCLUDE_COLLSCAN | QueryPlannerParams::INDEX_INTERSECTION;
    addIndex(BSON("a" << 1 << "b" << 1), "a_1_b_1");
    addIndex(BSON("c" << 1), "c_1");
    BSONObj query = fromjson("{$and: [{a: 5}, {$or: [{b: 6}, {c: 7}]}]}");
    runQuery(query);
    assertPlanCacheRecoversSolution(
        query,
        "{fetch: {filter: {$and:[{a:5},{$or:[{a:5,b:6},{c:7}]}]}, node: {andHash: {nodes: ["
        "{or: {nodes: ["
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [[6, 6, true, "
        "true]]}}},"
        "{ixscan: {pattern: {c: 1}, bounds: {c: [[7, 7, true, true]]}}}]}},"
        "{ixscan: {pattern: {a: 1, b: 1}, bounds: {a: [[5, 5, true, true]], b: [['MinKey', "
        "'MaxKey', true, true]]}}}"
        "]}}}}");
}

TEST_F(PlanCacheTest, PlanCacheSizeWithCRUDOperations) {
    PlanCache planCache(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1, b: 1}"));
    auto qs = getQuerySolutionForCaching();
    long long previousSize, originalSize = planCacheTotalSizeEstimateBytes.get();
    auto key = makeKey(*cq);
    auto decision = createDecision(1U);
    auto callbacks = createCallback(*cq, *decision);

    // Verify that the plan cache size increases after adding new entry to cache.
    previousSize = planCacheTotalSizeEstimateBytes.get();
    ASSERT_OK(planCache.set(key,
                            qs->cacheData->clone(),
                            NumWorks{123},  // Doesn't matter.
                            Date_t{},
                            &callbacks,
                            PlanSecurityLevel::kNotSensitive));
    ASSERT_GT(planCacheTotalSizeEstimateBytes.get(), previousSize);

    decision = createDecision(1U);
    auto callbacks1 = createCallback(*cq, *decision);
    // Verify that trying to set the same entry won't change the plan cache size.
    previousSize = planCacheTotalSizeEstimateBytes.get();
    ASSERT_OK(planCache.set(key,
                            qs->cacheData->clone(),
                            NumWorks{123},  // Doesn't matter.
                            Date_t{},
                            &callbacks1,
                            PlanSecurityLevel::kNotSensitive));
    ASSERT_EQ(planCacheTotalSizeEstimateBytes.get(), previousSize);

    decision = createDecision(2U);
    auto callbacks2 = createCallback(*cq, *decision);
    // Verify that the plan cache size increases after updating the same entry with more solutions.
    ASSERT_OK(planCache.set(key,
                            qs->cacheData->clone(),
                            NumWorks{123},  // Doesn't matter.
                            Date_t{},
                            &callbacks2,
                            PlanSecurityLevel::kNotSensitive));
    ASSERT_GT(planCacheTotalSizeEstimateBytes.get(), previousSize);

    decision = createDecision(1U);
    auto callbacks3 = createCallback(*cq, *decision);
    // Verify that the plan cache size decreases after updating the same entry with fewer solutions.
    previousSize = planCacheTotalSizeEstimateBytes.get();
    ASSERT_OK(planCache.set(key,
                            qs->cacheData->clone(),
                            NumWorks{123},  // Doesn't matter.
                            Date_t{},
                            &callbacks3,
                            PlanSecurityLevel::kNotSensitive));
    ASSERT_LT(planCacheTotalSizeEstimateBytes.get(), previousSize);
    ASSERT_GT(planCacheTotalSizeEstimateBytes.get(), originalSize);

    // Verify that adding multiple entries will increasing the cache size.
    long long sizeWithOneEntry = planCacheTotalSizeEstimateBytes.get();
    std::string queryString = "{a: 1, c: 1}";
    for (int i = 0; i < 5; ++i) {
        // Update the field name in the query string so that plan cache creates a new entry.
        queryString[1] = 'b' + i;
        std::unique_ptr<CanonicalQuery> query(canonicalize(queryString));
        decision = createDecision(1U);
        auto callbacks4 = createCallback(*query, *decision);
        previousSize = planCacheTotalSizeEstimateBytes.get();
        ASSERT_OK(planCache.set(makeKey(*query),
                                qs->cacheData->clone(),
                                NumWorks{123},  // Doesn't matter.
                                Date_t{},
                                &callbacks4,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_GT(planCacheTotalSizeEstimateBytes.get(), previousSize);
    }

    // Verify that removing multiple entries will decreasing the cache size.
    for (int i = 0; i < 5; ++i) {
        // Update the field name in the query to match the previously created plan cache entry key.
        queryString[1] = 'b' + i;
        std::unique_ptr<CanonicalQuery> query(canonicalize(queryString));
        previousSize = planCacheTotalSizeEstimateBytes.get();
        planCache.remove(makeKey(*query));
        ASSERT_LT(planCacheTotalSizeEstimateBytes.get(), previousSize);
    }
    // Verify that size is reset to the size when there is only entry.
    ASSERT_EQ(planCacheTotalSizeEstimateBytes.get(), sizeWithOneEntry);

    // Verify that trying to remove a non-existing key won't change the plan cache size.
    previousSize = planCacheTotalSizeEstimateBytes.get();
    std::unique_ptr<CanonicalQuery> newQuery(canonicalize("{a: 1}"));
    planCache.remove(makeKey(*newQuery));
    ASSERT_EQ(planCacheTotalSizeEstimateBytes.get(), previousSize);

    // Verify that the plan cache size goes back to original size when the entry is removed.
    planCache.remove(key);
    ASSERT_EQ(planCache.size(), 0U);
    ASSERT_EQ(planCacheTotalSizeEstimateBytes.get(), originalSize);
}

TEST_F(PlanCacheTest, PlanCacheSizeWithEviction) {
    const size_t kCacheSize = 5;
    PlanCache planCache(kCacheSize);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1, b: 1}"));
    auto qs = getQuerySolutionForCaching();
    long long originalSize = planCacheTotalSizeEstimateBytes.get();
    long long previousSize = planCacheTotalSizeEstimateBytes.get();
    auto key = makeKey(*cq);

    // Add entries until plan cache is full and verify that the size keeps increasing.
    std::string queryString = "{a: 1, c: 1}";
    for (size_t i = 0; i < kCacheSize; ++i) {
        // Update the field name in the query string so that plan cache creates a new entry.
        queryString[1]++;
        std::unique_ptr<CanonicalQuery> query(canonicalize(queryString));
        previousSize = planCacheTotalSizeEstimateBytes.get();
        auto decision = createDecision(2U);
        auto callbacks = createCallback(*query, *decision);
        ASSERT_OK(planCache.set(makeKey(*query),
                                qs->cacheData->clone(),
                                NumWorks{123},  // Doesn't matter.
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_GT(planCacheTotalSizeEstimateBytes.get(), previousSize);
    }

    // Verify that adding entry of same size as evicted entry wouldn't change the plan cache size.
    {
        queryString = "{k: 1, c: 1}";
        cq = std::unique_ptr<CanonicalQuery>(canonicalize(queryString));
        auto decision = createDecision(2U);
        auto callbacks = createCallback(*cq, *decision);
        previousSize = planCacheTotalSizeEstimateBytes.get();
        ASSERT_EQ(planCache.size(), kCacheSize);
        ASSERT_OK(planCache.set(key,
                                qs->cacheData->clone(),
                                NumWorks{123},  // Doesn't matter.
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_EQ(planCache.size(), kCacheSize);
        ASSERT_EQ(planCacheTotalSizeEstimateBytes.get(), previousSize);
    }

    // Verify that adding entry with query bigger than the evicted entry's key should change the
    // plan cache size.
    {
        queryString = "{k: 1, c: 1, extraField: 1}";
        std::unique_ptr<CanonicalQuery> queryBiggerKey(canonicalize(queryString));
        auto decision = createDecision(2U);
        auto callbacks = createCallback(*queryBiggerKey, *decision);
        previousSize = planCacheTotalSizeEstimateBytes.get();
        ASSERT_OK(planCache.set(makeKey(*queryBiggerKey),
                                qs->cacheData->clone(),
                                NumWorks{123},  // Doesn't matter.
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_GT(planCacheTotalSizeEstimateBytes.get(), previousSize);
    }

    // Verify that adding entry with query solutions larger than the evicted entry's query solutions
    // should increase the plan cache size.
    {
        queryString = "{l: 1, c: 1}";
        cq = std::unique_ptr<CanonicalQuery>(canonicalize(queryString));
        auto decision = createDecision(3U);
        auto callbacks = createCallback(*cq, *decision);
        previousSize = planCacheTotalSizeEstimateBytes.get();
        ASSERT_OK(planCache.set(key,
                                qs->cacheData->clone(),
                                NumWorks{123},  // Doesn't matter.
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_GT(planCacheTotalSizeEstimateBytes.get(), previousSize);
    }

    // Verify that adding entry with query solutions smaller than the evicted entry's query
    // solutions should decrease the plan cache size.
    {
        queryString = "{m: 1, c: 1}";
        cq = std::unique_ptr<CanonicalQuery>(canonicalize(queryString));
        auto decision = createDecision(1U);
        auto callbacks = createCallback(*cq, *decision);
        previousSize = planCacheTotalSizeEstimateBytes.get();
        ASSERT_OK(planCache.set(key,
                                qs->cacheData->clone(),
                                NumWorks{123},  // Doesn't matter.
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_LT(planCacheTotalSizeEstimateBytes.get(), previousSize);

        // clear() should reset the size.
        planCache.clear();
        ASSERT_EQ(planCacheTotalSizeEstimateBytes.get(), originalSize);
    }
}

TEST_F(PlanCacheTest, PlanCacheSizeWithMultiplePlanCaches) {
    PlanCache planCache1(5000);
    PlanCache planCache2(5000);
    std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1, b: 1}"));
    auto qs = getQuerySolutionForCaching();
    long long previousSize, originalSize = planCacheTotalSizeEstimateBytes.get();

    // Verify that adding entries to both plan caches will keep increasing the cache size.
    std::string queryString = "{a: 1, c: 1}";
    for (int i = 0; i < 5; ++i) {
        // Update the field name in the query string so that plan cache creates a new entry.
        queryString[1] = 'b' + i;
        std::unique_ptr<CanonicalQuery> query(canonicalize(queryString));
        auto decision = createDecision(1U);
        auto callbacks = createCallback(*query, *decision);
        previousSize = planCacheTotalSizeEstimateBytes.get();
        ASSERT_OK(planCache1.set(makeKey(*query),
                                 qs->cacheData->clone(),
                                 NumWorks{123},  // Doesn't matter.
                                 Date_t{},
                                 &callbacks,
                                 PlanSecurityLevel::kNotSensitive));
        ASSERT_GT(planCacheTotalSizeEstimateBytes.get(), previousSize);

        decision = createDecision(1U);
        auto callbacks1 = createCallback(*query, *decision);
        previousSize = planCacheTotalSizeEstimateBytes.get();
        ASSERT_OK(planCache2.set(makeKey(*query),
                                 qs->cacheData->clone(),
                                 NumWorks{123},  // Doesn't matter.
                                 Date_t{},
                                 &callbacks1,
                                 PlanSecurityLevel::kNotSensitive));
        ASSERT_GT(planCacheTotalSizeEstimateBytes.get(), previousSize);
    }

    // Verify that removing entries from one plan caches will keep decreasing the cache size.
    for (int i = 0; i < 5; ++i) {
        // Update the field name in the query to match the previously created plan cache entry key.
        queryString[1] = 'b' + i;
        std::unique_ptr<CanonicalQuery> query(canonicalize(queryString));
        previousSize = planCacheTotalSizeEstimateBytes.get();
        planCache1.remove(makeKey(*query));
        ASSERT_LT(planCacheTotalSizeEstimateBytes.get(), previousSize);
    }

    // Verify for scoped PlanCache object.
    long long sizeBeforeScopedPlanCache = planCacheTotalSizeEstimateBytes.get();
    {
        auto decision = createDecision(1U);
        auto callbacks = createCallback(*cq, *decision);
        PlanCache planCache(5000);
        previousSize = planCacheTotalSizeEstimateBytes.get();
        ASSERT_OK(planCache.set(makeKey(*cq),
                                qs->cacheData->clone(),
                                NumWorks{123},  // Doesn't matter.
                                Date_t{},
                                &callbacks,
                                PlanSecurityLevel::kNotSensitive));
        ASSERT_GT(planCacheTotalSizeEstimateBytes.get(), previousSize);
    }

    // Verify that size is reset to 'sizeBeforeScopedPlanCache' after the destructor of 'planCache'
    // is called.
    ASSERT_EQ(planCacheTotalSizeEstimateBytes.get(), sizeBeforeScopedPlanCache);

    // Clear 'planCache2' to remove all entries.
    previousSize = planCacheTotalSizeEstimateBytes.get();
    planCache2.clear();
    ASSERT_LT(planCacheTotalSizeEstimateBytes.get(), previousSize);

    // Verify that size is reset to the original size after removing all entries.
    ASSERT_EQ(planCacheTotalSizeEstimateBytes.get(), originalSize);
}

TEST_F(PlanCacheTest, PlanCacheMaxSizeParameterCanBeZero) {
    PlanCache planCache{0U};
    std::unique_ptr<CanonicalQuery> query(canonicalize("{a: 1, c: 1}"));
    auto qs = getQuerySolutionForCaching();
    auto decision = createDecision(1U);
    auto callbacks = createCallback(*query, *decision);

    ASSERT_OK(planCache.set(makeKey(*query),
                            qs->cacheData->clone(),
                            NumWorks{123},  // Doesn't matter.
                            Date_t{},
                            &callbacks,
                            PlanSecurityLevel::kNotSensitive));
    ASSERT_EQ(0U, planCache.size());
}

//
// QS hashing.
//

TEST_F(CachePlanSelectionTest,
       RecoveredSolutionWithMatchExpressionHasTheSameTaggedMatchExpressionHash) {
    addIndex(BSON("x" << 1), "x_1");
    addIndex(BSON("y" << 1), "y_1");

    // Pick any match expression query.
    auto query = BSON("x" << 5 << "y" << 1);
    runQuery(query);

    auto bestSoln =
        firstMatchingSolution("{fetch: {filter: {x: 5}, node: {ixscan: {pattern: {y: 1}}}}}");
    ASSERT_NE(0, bestSoln->taggedMatchExpressionHash);

    auto planSoln = planQueryFromCache(query, {}, {}, {}, *bestSoln);
    ASSERT_EQ(bestSoln->taggedMatchExpressionHash, planSoln->taggedMatchExpressionHash);
}

/**
 * Tests specifically for SBE plan cache.
 */
class SbePlanCacheTest : public unittest::Test {
protected:
    void setUp() override {
        _queryTestServiceContext = std::make_unique<QueryTestServiceContext>();
        _operationContext = _queryTestServiceContext->makeOperationContext();
        auto collection = std::make_shared<CollectionMock>(_nss);
        auto catalog = CollectionCatalog::get(_operationContext.get());
        catalog->onCreateCollection(_operationContext.get(), collection);
        auto consistentColl = catalog->establishConsistentCollection(
            _operationContext.get(), _nss, boost::none /* readTimestamp */);
        _collectionAcq =
            std::make_unique<CollectionAcquisition>(shard_role_mock::acquireCollectionMocked(
                _operationContext.get(), _nss, consistentColl));
    }

    void tearDown() override {
        _collectionAcq.reset();
        _operationContext.reset();
        _queryTestServiceContext.reset();
    }

    sbe::PlanCacheKey makeSbeKey(const CanonicalQuery& cq) {
        ASSERT_TRUE(cq.isSbeCompatible());
        return plan_cache_key_factory::make<sbe::PlanCacheKey>(cq, *_collectionAcq);
    }

    /**
     * Checks if plan cache size calculation returns expected result.
     */
    void assertSbePlanCacheKeySize(const char* queryStr,
                                   const char* sortStr,
                                   const char* projectionStr,
                                   const char* collationStr) {
        // Create canonical query.
        std::unique_ptr<CanonicalQuery> cq = makeCQ(queryStr, sortStr, projectionStr, collationStr);
        cq->setSbeCompatible(true);

        auto sbeKey = makeSbeKey(*cq);

        // The static size of the key structure.
        const size_t staticSize = sizeof(sbeKey);

        // The actual key representation is encoded as a string.
        const size_t keyRepresentationSize = sbeKey.toString().size();

        // The tests are setup for a single collection.
        const size_t additionalCollectionSize = 0;

        ASSERT_TRUE(sbeKey.estimatedKeySizeBytes() ==
                    staticSize + keyRepresentationSize + additionalCollectionSize);
    }


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
        BSONObj query = fromjson(queryStr);
        BSONObj sort = fromjson(sortStr);
        BSONObj projection = fromjson(projectionStr);
        BSONObj collation = fromjson(collationStr);

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

private:
    static const NamespaceString _nss;
    std::unique_ptr<QueryTestServiceContext> _queryTestServiceContext;

    ServiceContext::UniqueOperationContext _operationContext;
    std::unique_ptr<CollectionAcquisition> _collectionAcq;
};

const NamespaceString SbePlanCacheTest::_nss(
    NamespaceString::createNamespaceString_forTest("test.collection"));

TEST_F(SbePlanCacheTest, SBEPlanCacheBudgetTest) {
    assertSbePlanCacheKeySize("{a: 2}", "{}", "{_id: 1, a: 1}", "{}");

    assertSbePlanCacheKeySize(
        "{b: 'foo'}", "{}", "{_id: 1, b: 1}", "{locale: 'mock_reverse_string'}");

    assertSbePlanCacheKeySize(
        "{a: 1, b: 1}", "{a: -1}", "{_id: 0, a: 1}", "{locale: 'mock_reverse_string'}");
}

TEST_F(SbePlanCacheTest, SBEPlanCacheKeyMakeAndCompare) {
    std::unique_ptr<CanonicalQuery> cq1 = makeCQ("{a: 5}", "{}", "{_id: 1, a: 1}", "{}");
    cq1->setSbeCompatible(true);
    auto sbeKey1 = makeSbeKey(*cq1);
    std::unique_ptr<CanonicalQuery> cq2 = makeCQ("{a: 5}", "{}", "{_id: 0, a: 1}", "{}");
    cq2->setSbeCompatible(true);
    auto sbeKey2 = makeSbeKey(*cq2);
    ASSERT_NE(sbeKey1, sbeKey2);
    ASSERT_NE(sbeKey1.planCacheKeyHash(), sbeKey2.planCacheKeyHash());
}

TEST_F(SbePlanCacheTest, SBEPlanCacheUpdateSize) {
    ASSERT_OK(plan_cache_util::onPlanCacheSizeUpdate("10%"));
    ASSERT_OK(plan_cache_util::onPlanCacheSizeUpdate("5MB"));
    ASSERT_NOT_OK(plan_cache_util::onPlanCacheSizeUpdate("10&"));
}
}  // namespace
}  // namespace mongo
