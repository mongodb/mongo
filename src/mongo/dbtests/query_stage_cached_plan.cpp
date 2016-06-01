/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/cached_plan.h"
#include "mongo/db/exec/queued_data_stage.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/plan_yield_policy.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/stdx/memory.h"

namespace QueryStageCachedPlan {

static const NamespaceString nss("unittests.QueryStageCachedPlan");

class QueryStageCachedPlanBase {
public:
    QueryStageCachedPlanBase() {
        // If collection exists already, we need to drop it.
        dropCollection();

        // Add indices.
        addIndex(BSON("a" << 1));
        addIndex(BSON("b" << 1));

        OldClientWriteContext ctx(&_txn, nss.ns());
        Collection* collection = ctx.getCollection();
        ASSERT(collection);

        // Add data.
        for (int i = 0; i < 10; i++) {
            insertDocument(collection, BSON("_id" << i << "a" << i << "b" << 1));
        }
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_txn, nss.ns(), obj));
    }

    void dropCollection() {
        ScopedTransaction transaction(&_txn, MODE_X);
        Lock::DBLock dbLock(_txn.lockState(), nss.db(), MODE_X);
        Database* database = dbHolder().get(&_txn, nss.db());
        if (!database) {
            return;
        }

        WriteUnitOfWork wuow(&_txn);
        database->dropCollection(&_txn, nss.ns());
        wuow.commit();
    }

    void insertDocument(Collection* collection, BSONObj obj) {
        WriteUnitOfWork wuow(&_txn);

        const bool enforceQuota = false;
        OpDebug* const nullOpDebug = nullptr;
        ASSERT_OK(collection->insertDocument(&_txn, obj, nullOpDebug, enforceQuota));
        wuow.commit();
    }

    OperationContext* txn() {
        return &_txn;
    }

protected:
    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    WorkingSet _ws;
};

/**
 * Test that on failure, the cached plan stage replans the query but does not create a new cache
 * entry.
 */
class QueryStageCachedPlanFailure : public QueryStageCachedPlanBase {
public:
    void run() {
        AutoGetCollectionForRead ctx(&_txn, nss.ns());
        Collection* collection = ctx.getCollection();
        ASSERT(collection);

        // Query can be answered by either index on "a" or index on "b".
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(fromjson("{a: {$gte: 8}, b: 1}"));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        const std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // We shouldn't have anything in the plan cache for this shape yet.
        PlanCache* cache = collection->infoCache()->getPlanCache();
        ASSERT(cache);
        CachedSolution* rawCachedSolution;
        ASSERT_NOT_OK(cache->get(*cq, &rawCachedSolution));

        // Get planner params.
        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq.get(), &plannerParams);

        // Queued data stage will return a failure during the cached plan trial period.
        auto mockChild = stdx::make_unique<QueuedDataStage>(&_txn, &_ws);
        mockChild->pushBack(PlanStage::FAILURE);

        // High enough so that we shouldn't trigger a replan based on works.
        const size_t decisionWorks = 50;
        CachedPlanStage cachedPlanStage(
            &_txn, collection, &_ws, cq.get(), plannerParams, decisionWorks, mockChild.release());

        // This should succeed after triggering a replan.
        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL,
                                    _txn.getServiceContext()->getFastClockSource());
        ASSERT_OK(cachedPlanStage.pickBestPlan(&yieldPolicy));

        // Make sure that we get 2 legit results back.
        size_t numResults = 0;
        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (state != PlanStage::IS_EOF) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            state = cachedPlanStage.work(&id);

            ASSERT_NE(state, PlanStage::FAILURE);
            ASSERT_NE(state, PlanStage::DEAD);

            if (state == PlanStage::ADVANCED) {
                WorkingSetMember* member = _ws.get(id);
                ASSERT(cq->root()->matchesBSON(member->obj.value()));
                numResults++;
            }
        }

        ASSERT_EQ(numResults, 2U);

        // Plan cache should still be empty, as we don't write to it when we replan a failed
        // query.
        ASSERT_NOT_OK(cache->get(*cq, &rawCachedSolution));
    }
};

/**
 * Test that hitting the cached plan stage trial period's threshold for work cycles causes the
 * query to be replanned. Also verify that the replanning results in a new plan cache entry.
 */
class QueryStageCachedPlanHitMaxWorks : public QueryStageCachedPlanBase {
public:
    void run() {
        AutoGetCollectionForRead ctx(&_txn, nss.ns());
        Collection* collection = ctx.getCollection();
        ASSERT(collection);

        // Query can be answered by either index on "a" or index on "b".
        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(fromjson("{a: {$gte: 8}, b: 1}"));
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        const std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // We shouldn't have anything in the plan cache for this shape yet.
        PlanCache* cache = collection->infoCache()->getPlanCache();
        ASSERT(cache);
        CachedSolution* rawCachedSolution;
        ASSERT_NOT_OK(cache->get(*cq, &rawCachedSolution));

        // Get planner params.
        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq.get(), &plannerParams);

        // Set up queued data stage to take a long time before returning EOF. Should be long
        // enough to trigger a replan.
        const size_t decisionWorks = 10;
        const size_t mockWorks =
            1U + static_cast<size_t>(internalQueryCacheEvictionRatio * decisionWorks);
        auto mockChild = stdx::make_unique<QueuedDataStage>(&_txn, &_ws);
        for (size_t i = 0; i < mockWorks; i++) {
            mockChild->pushBack(PlanStage::NEED_TIME);
        }

        CachedPlanStage cachedPlanStage(
            &_txn, collection, &_ws, cq.get(), plannerParams, decisionWorks, mockChild.release());

        // This should succeed after triggering a replan.
        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL,
                                    _txn.getServiceContext()->getFastClockSource());
        ASSERT_OK(cachedPlanStage.pickBestPlan(&yieldPolicy));

        // Make sure that we get 2 legit results back.
        size_t numResults = 0;
        PlanStage::StageState state = PlanStage::NEED_TIME;
        while (state != PlanStage::IS_EOF) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            state = cachedPlanStage.work(&id);

            ASSERT_NE(state, PlanStage::FAILURE);
            ASSERT_NE(state, PlanStage::DEAD);

            if (state == PlanStage::ADVANCED) {
                WorkingSetMember* member = _ws.get(id);
                ASSERT(cq->root()->matchesBSON(member->obj.value()));
                numResults++;
            }
        }

        ASSERT_EQ(numResults, 2U);

        // This time we expect to find something in the plan cache. Replans after hitting the
        // works threshold result in a cache entry.
        ASSERT_OK(cache->get(*cq, &rawCachedSolution));
        const std::unique_ptr<CachedSolution> cachedSolution(rawCachedSolution);
    }
};

class All : public Suite {
public:
    All() : Suite("query_stage_cached_plan") {}

    void setupTests() {
        add<QueryStageCachedPlanFailure>();
        add<QueryStageCachedPlanHitMaxWorks>();
    }
};

SuiteInstance<All> all;

}  // namespace QueryStageCachedPlan
