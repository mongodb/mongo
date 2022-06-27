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

#include "mongo/platform/basic.h"

#include <memory>

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/mock_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_cache_key_factory.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageCachedPlan {

static const NamespaceString nss("unittests.QueryStageCachedPlan");

namespace {
std::unique_ptr<CanonicalQuery> canonicalQueryFromFilterObj(OperationContext* opCtx,
                                                            const NamespaceString& nss,
                                                            BSONObj filter) {
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(filter);
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx, std::move(findCommand));
    uassertStatusOK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
}
}  // namespace

class QueryStageCachedPlan : public unittest::Test {
public:
    QueryStageCachedPlan() : _client(&_opCtx) {}

    void setUp() {
        // If collection exists already, we need to drop it.
        dropCollection();

        // Add indices.
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));

        dbtests::WriteContextForTests ctx(&_opCtx, nss.ns());
        CollectionPtr collection = ctx.getCollection();
        ASSERT(collection);

        // Add data.
        for (int i = 0; i < 10; i++) {
            insertDocument(collection, BSON("_id" << i << "a" << i << "b" << 1));
        }
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_opCtx, nss.ns(), obj));
    }

    void dropIndex(BSONObj keyPattern) {
        _client.dropIndex(nss.ns(), std::move(keyPattern));
    }

    void dropCollection() {
        Lock::DBLock dbLock(&_opCtx, nss.dbName(), MODE_X);
        auto databaseHolder = DatabaseHolder::get(&_opCtx);
        auto database = databaseHolder->getDb(&_opCtx, nss.dbName());
        if (!database) {
            return;
        }

        WriteUnitOfWork wuow(&_opCtx);
        database->dropCollection(&_opCtx, nss).transitional_ignore();
        wuow.commit();
    }

    void insertDocument(const CollectionPtr& collection, BSONObj obj) {
        WriteUnitOfWork wuow(&_opCtx);

        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection->insertDocument(&_opCtx, InsertStatement(obj), nullOpDebug));
        wuow.commit();
    }

    OperationContext* opCtx() {
        return &_opCtx;
    }

    static size_t getNumResultsForStage(const WorkingSet& ws,
                                        CachedPlanStage* cachedPlanStage,
                                        CanonicalQuery* cq) {
        size_t numResults = 0;
        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (state != PlanStage::IS_EOF) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            state = cachedPlanStage->work(&id);

            if (state == PlanStage::ADVANCED) {
                auto member = ws.get(id);
                ASSERT(cq->root()->matchesBSON(member->doc.value().toBson()));
                numResults++;
            }
        }

        return numResults;
    }

    void forceReplanning(const CollectionPtr& collection, CanonicalQuery* cq) {
        // Get planner params.
        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_opCtx, collection, cq, &plannerParams);

        const size_t decisionWorks = 10;
        const size_t mockWorks =
            1U + static_cast<size_t>(internalQueryCacheEvictionRatio * decisionWorks);
        auto mockChild = std::make_unique<MockStage>(_expCtx.get(), &_ws);
        for (size_t i = 0; i < mockWorks; i++) {
            mockChild->enqueueStateCode(PlanStage::NEED_TIME);
        }

        CachedPlanStage cachedPlanStage(_expCtx.get(),
                                        collection,
                                        &_ws,
                                        cq,
                                        plannerParams,
                                        decisionWorks,
                                        std::move(mockChild));

        // This should succeed after triggering a replan.
        NoopYieldPolicy yieldPolicy(_opCtx.getServiceContext()->getFastClockSource());
        ASSERT_OK(cachedPlanStage.pickBestPlan(&yieldPolicy));
    }

protected:
    const ServiceContext::UniqueOperationContext _opCtxPtr = cc().makeOperationContext();
    OperationContext& _opCtx = *_opCtxPtr;
    WorkingSet _ws;
    DBDirectClient _client{&_opCtx};

    boost::intrusive_ptr<ExpressionContext> _expCtx =
        make_intrusive<ExpressionContext>(&_opCtx, nullptr, nss);
};

/**
 * Test that on a memory limit exceeded failure, the cached plan stage replans the query but does
 * not create a new cache entry.
 */
TEST_F(QueryStageCachedPlan, QueryStageCachedPlanFailureMemoryLimitExceeded) {
    AutoGetCollectionForReadCommand collection(&_opCtx, nss);
    ASSERT(collection);

    // Query can be answered by either index on "a" or index on "b".
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson("{a: {$gte: 8}, b: 1}"));
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
    ASSERT_OK(statusWithCQ.getStatus());
    const std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
    auto key = plan_cache_key_factory::make<PlanCacheKey>(*cq, collection.getCollection());

    // We shouldn't have anything in the plan cache for this shape yet.
    PlanCache* cache = CollectionQueryInfo::get(collection.getCollection()).getPlanCache();
    ASSERT(cache);
    ASSERT_EQ(cache->get(key).state, PlanCache::CacheEntryState::kNotPresent);

    // Get planner params.
    QueryPlannerParams plannerParams;
    fillOutPlannerParams(&_opCtx, collection.getCollection(), cq.get(), &plannerParams);

    // Mock stage will return a failure during the cached plan trial period.
    auto mockChild = std::make_unique<MockStage>(_expCtx.get(), &_ws);
    mockChild->enqueueError(
        Status{ErrorCodes::QueryExceededMemoryLimitNoDiskUseAllowed, "mock error"});

    // High enough so that we shouldn't trigger a replan based on works.
    const size_t decisionWorks = 50;
    CachedPlanStage cachedPlanStage(_expCtx.get(),
                                    collection.getCollection(),
                                    &_ws,
                                    cq.get(),
                                    plannerParams,
                                    decisionWorks,
                                    std::move(mockChild));

    // This should succeed after triggering a replan.
    NoopYieldPolicy yieldPolicy(_opCtx.getServiceContext()->getFastClockSource());
    ASSERT_OK(cachedPlanStage.pickBestPlan(&yieldPolicy));

    ASSERT_EQ(getNumResultsForStage(_ws, &cachedPlanStage, cq.get()), 2U);

    // Plan cache should still be empty, as we don't write to it when we replan a failed
    // query.
    ASSERT_EQ(cache->get(key).state, PlanCache::CacheEntryState::kNotPresent);
}

/**
 * Test that hitting the cached plan stage trial period's threshold for work cycles causes the
 * query to be replanned. Also verify that the replanning results in a new plan cache entry.
 */
TEST_F(QueryStageCachedPlan, QueryStageCachedPlanHitMaxWorks) {
    AutoGetCollectionForReadCommand collection(&_opCtx, nss);
    ASSERT(collection);

    // Query can be answered by either index on "a" or index on "b".
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson("{a: {$gte: 8}, b: 1}"));
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
    ASSERT_OK(statusWithCQ.getStatus());
    const std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
    auto key = plan_cache_key_factory::make<PlanCacheKey>(*cq, collection.getCollection());

    // We shouldn't have anything in the plan cache for this shape yet.
    PlanCache* cache = CollectionQueryInfo::get(collection.getCollection()).getPlanCache();
    ASSERT(cache);
    ASSERT_EQ(cache->get(key).state, PlanCache::CacheEntryState::kNotPresent);

    // Get planner params.
    QueryPlannerParams plannerParams;
    fillOutPlannerParams(&_opCtx, collection.getCollection(), cq.get(), &plannerParams);

    // Set up queued data stage to take a long time before returning EOF. Should be long
    // enough to trigger a replan.
    const size_t decisionWorks = 10;
    const size_t mockWorks =
        1U + static_cast<size_t>(internalQueryCacheEvictionRatio * decisionWorks);
    auto mockChild = std::make_unique<MockStage>(_expCtx.get(), &_ws);
    for (size_t i = 0; i < mockWorks; i++) {
        mockChild->enqueueStateCode(PlanStage::NEED_TIME);
    }

    CachedPlanStage cachedPlanStage(_expCtx.get(),
                                    collection.getCollection(),
                                    &_ws,
                                    cq.get(),
                                    plannerParams,
                                    decisionWorks,
                                    std::move(mockChild));

    // This should succeed after triggering a replan.
    NoopYieldPolicy yieldPolicy(_opCtx.getServiceContext()->getFastClockSource());
    ASSERT_OK(cachedPlanStage.pickBestPlan(&yieldPolicy));

    ASSERT_EQ(getNumResultsForStage(_ws, &cachedPlanStage, cq.get()), 2U);

    // This time we expect to find something in the plan cache. Replans after hitting the
    // works threshold result in a cache entry.
    ASSERT_EQ(cache->get(key).state, PlanCache::CacheEntryState::kPresentInactive);
}

/**
 * Test the way cache entries are added (either "active" or "inactive") to the plan cache.
 */
TEST_F(QueryStageCachedPlan, QueryStageCachedPlanAddsActiveCacheEntries) {
    AutoGetCollectionForReadCommand collection(&_opCtx, nss);
    ASSERT(collection);

    // Never run - just used as a key for the cache's get() functions, since all of the other
    // CanonicalQueries created in this test will have this shape.
    const auto shapeCq =
        canonicalQueryFromFilterObj(opCtx(), nss, fromjson("{a: {$gte: 123}, b: {$gte: 123}}"));
    auto planCacheKey =
        plan_cache_key_factory::make<PlanCacheKey>(*shapeCq, collection.getCollection());

    // Query can be answered by either index on "a" or index on "b".
    const auto noResultsCq =
        canonicalQueryFromFilterObj(opCtx(), nss, fromjson("{a: {$gte: 11}, b: {$gte: 11}}"));

    // We shouldn't have anything in the plan cache for this shape yet.
    PlanCache* cache = CollectionQueryInfo::get(collection.getCollection()).getPlanCache();
    ASSERT(cache);
    ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kNotPresent);

    // Run the CachedPlanStage with a long-running child plan. Replanning should be
    // triggered and an inactive entry will be added.
    forceReplanning(collection.getCollection(), noResultsCq.get());

    // Check for an inactive cache entry.
    ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kPresentInactive);

    // The works should be 1 for the entry since the query we ran should not have any results.
    auto entry = assertGet(cache->getEntry(planCacheKey));
    size_t works = 1U;
    ASSERT_TRUE(entry->works);
    ASSERT_EQ(entry->works.get(), works);

    const size_t kExpectedNumWorks = 10;
    for (int i = 0; i < std::ceil(std::log(kExpectedNumWorks) / std::log(2)); ++i) {
        works *= 2;
        // Run another query of the same shape, which is less selective, and therefore takes
        // longer).
        auto someResultsCq =
            canonicalQueryFromFilterObj(opCtx(), nss, fromjson("{a: {$gte: 1}, b: {$gte: 0}}"));
        forceReplanning(collection.getCollection(), someResultsCq.get());

        ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kPresentInactive);
        // The works on the cache entry should have doubled.
        entry = assertGet(cache->getEntry(planCacheKey));
        ASSERT_TRUE(entry->works);
        ASSERT_EQ(entry->works.get(), works);
    }

    // Run another query which takes less time, and be sure an active entry is created.
    auto fewResultsCq =
        canonicalQueryFromFilterObj(opCtx(), nss, fromjson("{a: {$gte: 6}, b: {$gte: 0}}"));
    forceReplanning(collection.getCollection(), fewResultsCq.get());

    // Now there should be an active cache entry.
    ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kPresentActive);
    entry = assertGet(cache->getEntry(planCacheKey));
    // This will query will match {a: 6} through {a:9} (4 works), plus one for EOF = 5 works.
    ASSERT_TRUE(entry->works);
    ASSERT_EQ(entry->works.get(), 5U);
}


TEST_F(QueryStageCachedPlan, DeactivatesEntriesOnReplan) {
    AutoGetCollectionForReadCommand collection(&_opCtx, nss);
    ASSERT(collection);

    // Never run - just used as a key for the cache's get() functions, since all of the other
    // CanonicalQueries created in this test will have this shape.
    const auto shapeCq =
        canonicalQueryFromFilterObj(opCtx(), nss, fromjson("{a: {$gte: 123}, b: {$gte: 123}}"));
    auto planCacheKey =
        plan_cache_key_factory::make<PlanCacheKey>(*shapeCq, collection.getCollection());

    // Query can be answered by either index on "a" or index on "b".
    const auto noResultsCq =
        canonicalQueryFromFilterObj(opCtx(), nss, fromjson("{a: {$gte: 11}, b: {$gte: 11}}"));

    // We shouldn't have anything in the plan cache for this shape yet.
    PlanCache* cache = CollectionQueryInfo::get(collection.getCollection()).getPlanCache();
    ASSERT(cache);
    ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kNotPresent);

    // Run the CachedPlanStage with a long-running child plan. Replanning should be
    // triggered and an inactive entry will be added.
    forceReplanning(collection.getCollection(), noResultsCq.get());

    // Check for an inactive cache entry.
    ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kPresentInactive);

    // Run the plan again, to create an active entry.
    forceReplanning(collection.getCollection(), noResultsCq.get());

    // The works should be 1 for the entry since the query we ran should not have any results.
    ASSERT_EQ(cache
                  ->get(plan_cache_key_factory::make<PlanCacheKey>(*noResultsCq,
                                                                   collection.getCollection()))
                  .state,
              PlanCache::CacheEntryState::kPresentActive);
    auto entry = assertGet(cache->getEntry(planCacheKey));
    size_t works = 1U;
    ASSERT_TRUE(entry->works);
    ASSERT_EQ(entry->works.get(), works);

    // Run another query which takes long enough to evict the active cache entry. The current
    // cache entry's works value is a very low number. When replanning is triggered, the cache
    // entry will be deactivated, but the new plan will not overwrite it, since the new plan will
    // have a higher works. Therefore, we will be left in an inactive entry which has had its works
    // value doubled from 1 to 2.
    auto highWorksCq =
        canonicalQueryFromFilterObj(opCtx(), nss, fromjson("{a: {$gte: 0}, b: {$gte:0}}"));
    forceReplanning(collection.getCollection(), highWorksCq.get());
    ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kPresentInactive);
    entry = assertGet(cache->getEntry(planCacheKey));
    ASSERT_TRUE(entry->works);
    ASSERT_EQ(entry->works.get(), 2U);

    // Again, force replanning. This time run the initial query which finds no results. The multi
    // planner will choose a plan with works value lower than the existing inactive
    // entry. Replanning will thus deactivate the existing entry (it's already
    // inactive so this is a noop), then create a new entry with a works value of 1.
    forceReplanning(collection.getCollection(), noResultsCq.get());
    ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kPresentActive);
    entry = assertGet(cache->getEntry(planCacheKey));
    ASSERT_TRUE(entry->works);
    ASSERT_EQ(entry->works.get(), 1U);
}

TEST_F(QueryStageCachedPlan, EntriesAreNotDeactivatedWhenInactiveEntriesDisabled) {
    // Set the global flag for disabling active entries.
    internalQueryCacheDisableInactiveEntries.store(true);
    ON_BLOCK_EXIT([] { internalQueryCacheDisableInactiveEntries.store(false); });

    AutoGetCollectionForReadCommand collection(&_opCtx, nss);
    ASSERT(collection);

    // Never run - just used as a key for the cache's get() functions, since all of the other
    // CanonicalQueries created in this test will have this shape.
    const auto shapeCq =
        canonicalQueryFromFilterObj(opCtx(), nss, fromjson("{a: {$gte: 123}, b: {$gte: 123}}"));
    auto planCacheKey =
        plan_cache_key_factory::make<PlanCacheKey>(*shapeCq, collection.getCollection());

    // Query can be answered by either index on "a" or index on "b".
    const auto noResultsCq =
        canonicalQueryFromFilterObj(opCtx(), nss, fromjson("{a: {$gte: 11}, b: {$gte: 11}}"));
    auto noResultKey =
        plan_cache_key_factory::make<PlanCacheKey>(*noResultsCq, collection.getCollection());

    // We shouldn't have anything in the plan cache for this shape yet.
    PlanCache* cache = CollectionQueryInfo::get(collection.getCollection()).getPlanCache();
    ASSERT(cache);
    ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kNotPresent);

    // Run the CachedPlanStage with a long-running child plan. Replanning should be
    // triggered and an _active_ entry will be added (since the disableInactiveEntries flag is on).
    forceReplanning(collection.getCollection(), noResultsCq.get());

    // Check for an inactive cache entry.
    ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kPresentActive);

    // Run the plan again. The entry should still be active.
    forceReplanning(collection.getCollection(), noResultsCq.get());
    ASSERT_EQ(cache->get(noResultKey).state, PlanCache::CacheEntryState::kPresentActive);

    // Run another query which takes long enough to evict the active cache entry. After replanning
    // is triggered, be sure that the the cache entry is still active.
    auto highWorksCq =
        canonicalQueryFromFilterObj(opCtx(), nss, fromjson("{a: {$gte: 0}, b: {$gte:0}}"));
    forceReplanning(collection.getCollection(), highWorksCq.get());
    ASSERT_EQ(cache->get(planCacheKey).state, PlanCache::CacheEntryState::kPresentActive);
}

TEST_F(QueryStageCachedPlan, ThrowsOnYieldRecoveryWhenIndexIsDroppedBeforePlanSelection) {
    // Create an index which we will drop later on.
    BSONObj keyPattern = BSON("c" << 1);
    addIndex(keyPattern);

    boost::optional<AutoGetCollectionForReadCommand> readLock;
    readLock.emplace(&_opCtx, nss);
    const auto& collection = readLock->getCollection();
    ASSERT(collection);

    // Query can be answered by either index on "a" or index on "b".
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
    ASSERT_OK(statusWithCQ.getStatus());
    const std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // We shouldn't have anything in the plan cache for this shape yet.
    PlanCache* cache = CollectionQueryInfo::get(collection).getPlanCache();
    ASSERT(cache);

    // Get planner params.
    QueryPlannerParams plannerParams;
    fillOutPlannerParams(&_opCtx, collection, cq.get(), &plannerParams);

    const size_t decisionWorks = 10;
    CachedPlanStage cachedPlanStage(_expCtx.get(),
                                    collection,
                                    &_ws,
                                    cq.get(),
                                    plannerParams,
                                    decisionWorks,
                                    std::make_unique<MockStage>(_expCtx.get(), &_ws));

    // Drop an index while the CachedPlanStage is in a saved state. Restoring should fail, since we
    // may still need the dropped index for plan selection.
    cachedPlanStage.saveState();
    readLock.reset();
    dropIndex(keyPattern);
    readLock.emplace(&_opCtx, nss);
    ASSERT_THROWS_CODE(cachedPlanStage.restoreState(&readLock->getCollection()),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

TEST_F(QueryStageCachedPlan, DoesNotThrowOnYieldRecoveryWhenIndexIsDroppedAferPlanSelection) {
    // Create an index which we will drop later on.
    BSONObj keyPattern = BSON("c" << 1);
    addIndex(keyPattern);

    boost::optional<AutoGetCollectionForReadCommand> readLock;
    readLock.emplace(&_opCtx, nss);
    const auto& collection = readLock->getCollection();
    ASSERT(collection);

    // Query can be answered by either index on "a" or index on "b".
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(findCommand));
    ASSERT_OK(statusWithCQ.getStatus());
    const std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // We shouldn't have anything in the plan cache for this shape yet.
    PlanCache* cache = CollectionQueryInfo::get(collection).getPlanCache();
    ASSERT(cache);

    // Get planner params.
    QueryPlannerParams plannerParams;
    fillOutPlannerParams(&_opCtx, collection, cq.get(), &plannerParams);

    const size_t decisionWorks = 10;
    CachedPlanStage cachedPlanStage(_expCtx.get(),
                                    collection,
                                    &_ws,
                                    cq.get(),
                                    plannerParams,
                                    decisionWorks,
                                    std::make_unique<MockStage>(_expCtx.get(), &_ws));

    NoopYieldPolicy yieldPolicy(_opCtx.getServiceContext()->getFastClockSource());
    ASSERT_OK(cachedPlanStage.pickBestPlan(&yieldPolicy));

    // Drop an index while the CachedPlanStage is in a saved state. We should be able to restore
    // successfully.
    cachedPlanStage.saveState();
    readLock.reset();
    dropIndex(keyPattern);
    readLock.emplace(&_opCtx, nss);
    cachedPlanStage.restoreState(&readLock->getCollection());
}

}  // namespace QueryStageCachedPlan
