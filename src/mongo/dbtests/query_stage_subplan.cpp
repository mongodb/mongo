/**
 *    Copyright (C) 2014 MongoDB Inc.
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

#include <memory>

#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/dbtests/dbtests.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace {

static const NamespaceString nss("unittests.QueryStageSubplan");

class QueryStageSubplanTest : public unittest::Test {
public:
    QueryStageSubplanTest() : _client(_opCtx.get()) {}

    virtual ~QueryStageSubplanTest() {
        dbtests::WriteContextForTests ctx(opCtx(), nss.ns());
        _client.dropCollection(nss.ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(opCtx(), nss.ns(), obj));
    }

    void insert(const BSONObj& doc) {
        _client.insert(nss.ns(), doc);
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    ServiceContext* serviceContext() {
        return _opCtx->getServiceContext();
    }

protected:
    /**
     * Parses the json string 'findCmd', specifying a find command, to a CanonicalQuery.
     */
    std::unique_ptr<CanonicalQuery> cqFromFindCommand(const std::string& findCmd) {
        BSONObj cmdObj = fromjson(findCmd);

        bool isExplain = false;
        auto qr = unittest::assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain));

        boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
        auto cq = unittest::assertGet(
            CanonicalQuery::canonicalize(opCtx(),
                                         std::move(qr),
                                         expCtx,
                                         ExtensionsCallbackNoop(),
                                         MatchExpressionParser::kAllowAllSpecialFeatures));
        return cq;
    }

    const ServiceContext::UniqueOperationContext _opCtx = cc().makeOperationContext();
    ClockSource* _clock = _opCtx->getServiceContext()->getFastClockSource();

private:
    DBDirectClient _client;
};

/**
 * SERVER-15012: test that the subplan stage does not crash when the winning solution
 * for an $or clause uses a '2d' index. We don't produce cache data for '2d'. The subplanner
 * should gracefully fail after finding that no cache data is available, allowing us to fall
 * back to regular planning.
 */
TEST_F(QueryStageSubplanTest, QueryStageSubplanGeo2dOr) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns());
    addIndex(BSON("a"
                  << "2d"
                  << "b"
                  << 1));
    addIndex(BSON("a"
                  << "2d"));

    BSONObj query = fromjson(
        "{$or: [{a: {$geoWithin: {$centerSphere: [[0,0],10]}}},"
        "{a: {$geoWithin: {$centerSphere: [[1,1],10]}}}]}");

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(query);
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(qr));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    Collection* collection = ctx.getCollection();

    // Get planner params.
    QueryPlannerParams plannerParams;
    fillOutPlannerParams(opCtx(), collection, cq.get(), &plannerParams);

    WorkingSet ws;
    std::unique_ptr<SubplanStage> subplan(
        new SubplanStage(opCtx(), collection, &ws, plannerParams, cq.get()));

    // Plan selection should succeed due to falling back on regular planning.
    PlanYieldPolicy yieldPolicy(PlanExecutor::NO_YIELD, _clock);
    ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));
}

/**
 * Test the SubplanStage's ability to plan an individual branch using the plan cache.
 */
TEST_F(QueryStageSubplanTest, QueryStageSubplanPlanFromCache) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns());

    addIndex(BSON("a" << 1));
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1));

    for (int i = 0; i < 10; i++) {
        insert(BSON("a" << 1 << "b" << i << "c" << i));
    }

    // This query should result in a plan cache entry for the first $or branch, because
    // there are two competing indices. The second branch has only one relevant index, so
    // its winning plan should not be cached.
    BSONObj query = fromjson("{$or: [{a: 1, b: 3}, {c: 1}]}");

    Collection* collection = ctx.getCollection();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(query);
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(qr));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // Get planner params.
    QueryPlannerParams plannerParams;
    fillOutPlannerParams(opCtx(), collection, cq.get(), &plannerParams);

    // For the remainder of this test, ensure that cache entries are available immediately, and
    // don't need go through an 'inactive' state before being usable.
    internalQueryCacheDisableInactiveEntries.store(true);

    WorkingSet ws;
    std::unique_ptr<SubplanStage> subplan(
        new SubplanStage(opCtx(), collection, &ws, plannerParams, cq.get()));

    PlanYieldPolicy yieldPolicy(PlanExecutor::NO_YIELD, _clock);
    ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

    // Nothing is in the cache yet, so neither branch should have been planned from
    // the plan cache.
    ASSERT_FALSE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));

    // If we repeat the same query, the plan for the first branch should have come from
    // the cache.
    ws.clear();
    subplan.reset(new SubplanStage(opCtx(), collection, &ws, plannerParams, cq.get()));

    ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

    ASSERT_TRUE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));
}

/**
 * Ensure that the subplan stage doesn't create a plan cache entry if there are no query results.
 */
TEST_F(QueryStageSubplanTest, QueryStageSubplanDontCacheZeroResults) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns());

    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1));
    addIndex(BSON("c" << 1));

    for (int i = 0; i < 10; i++) {
        insert(BSON("a" << 1 << "b" << i << "c" << i));
    }

    // Running this query should not create any cache entries. For the first branch, it's
    // because there are no matching results. For the second branch it's because there is only
    // one relevant index.
    BSONObj query = fromjson("{$or: [{a: 1, b: 15}, {c: 1}]}");

    Collection* collection = ctx.getCollection();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(query);
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(qr));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // Get planner params.
    QueryPlannerParams plannerParams;
    fillOutPlannerParams(opCtx(), collection, cq.get(), &plannerParams);

    WorkingSet ws;
    std::unique_ptr<SubplanStage> subplan(
        new SubplanStage(opCtx(), collection, &ws, plannerParams, cq.get()));

    PlanYieldPolicy yieldPolicy(PlanExecutor::NO_YIELD, _clock);
    ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

    // Nothing is in the cache yet, so neither branch should have been planned from
    // the plan cache.
    ASSERT_FALSE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));

    // If we run the query again, it should again be the case that neither branch gets planned
    // from the cache (because the first call to pickBestPlan() refrained from creating any
    // cache entries).
    ws.clear();
    subplan.reset(new SubplanStage(opCtx(), collection, &ws, plannerParams, cq.get()));

    ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

    ASSERT_FALSE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));
}

/**
 * Ensure that the subplan stage doesn't create a plan cache entry if there are no query results.
 */
TEST_F(QueryStageSubplanTest, QueryStageSubplanDontCacheTies) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns());

    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1 << "c" << 1));
    addIndex(BSON("d" << 1));

    for (int i = 0; i < 10; i++) {
        insert(BSON("a" << 1 << "e" << 1 << "d" << 1));
    }

    // Running this query should not create any cache entries. For the first branch, it's
    // because plans using the {a: 1, b: 1} and {a: 1, c: 1} indices should tie during plan
    // ranking. For the second branch it's because there is only one relevant index.
    BSONObj query = fromjson("{$or: [{a: 1, e: 1}, {d: 1}]}");

    Collection* collection = ctx.getCollection();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(query);
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx(), std::move(qr));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // Get planner params.
    QueryPlannerParams plannerParams;
    fillOutPlannerParams(opCtx(), collection, cq.get(), &plannerParams);

    WorkingSet ws;
    std::unique_ptr<SubplanStage> subplan(
        new SubplanStage(opCtx(), collection, &ws, plannerParams, cq.get()));

    PlanYieldPolicy yieldPolicy(PlanExecutor::NO_YIELD, _clock);
    ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

    // Nothing is in the cache yet, so neither branch should have been planned from
    // the plan cache.
    ASSERT_FALSE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));

    // If we run the query again, it should again be the case that neither branch gets planned
    // from the cache (because the first call to pickBestPlan() refrained from creating any
    // cache entries).
    ws.clear();
    subplan.reset(new SubplanStage(opCtx(), collection, &ws, plannerParams, cq.get()));

    ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

    ASSERT_FALSE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));
}

/**
 * Unit test the subplan stage's canUseSubplanning() method.
 */
TEST_F(QueryStageSubplanTest, QueryStageSubplanCanUseSubplanning) {
    // We won't try and subplan something that doesn't have an $or.
    {
        std::string findCmd = "{find: 'testns', filter: {$and:[{a:1}, {b:1}]}}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }

    // Don't try and subplan if there is no filter.
    {
        std::string findCmd = "{find: 'testns'}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }

    // We won't try and subplan two contained ORs.
    {
        std::string findCmd =
            "{find: 'testns',"
            "filter: {$or:[{a:1}, {b:1}], $or:[{c:1}, {d:1}], e:1}}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }

    // Can't use subplanning if there is a hint.
    {
        std::string findCmd =
            "{find: 'testns',"
            "filter: {$or: [{a:1, b:1}, {c:1, d:1}]},"
            "hint: {a:1, b:1}}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }

    // Can't use subplanning with min.
    {
        std::string findCmd =
            "{find: 'testns',"
            "filter: {$or: [{a:1, b:1}, {c:1, d:1}]},"
            "min: {a:1, b:1}}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }

    // Can't use subplanning with max.
    {
        std::string findCmd =
            "{find: 'testns',"
            "filter: {$or: [{a:1, b:1}, {c:1, d:1}]},"
            "max: {a:2, b:2}}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }

    // Can't use subplanning with tailable.
    {
        std::string findCmd =
            "{find: 'testns',"
            "filter: {$or: [{a:1, b:1}, {c:1, d:1}]},"
            "tailable: true}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }

    // Can use subplanning for rooted $or.
    {
        std::string findCmd =
            "{find: 'testns',"
            "filter: {$or: [{a:1, b:1}, {c:1, d:1}]}}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_TRUE(SubplanStage::canUseSubplanning(*cq));

        std::string findCmd2 =
            "{find: 'testns',"
            "filter: {$or: [{a:1}, {c:1}]}}";
        std::unique_ptr<CanonicalQuery> cq2 = cqFromFindCommand(findCmd2);
        ASSERT_TRUE(SubplanStage::canUseSubplanning(*cq2));
    }

    // Can't use subplanning for a single contained $or.
    {
        std::string findCmd =
            "{find: 'testns',"
            "filter: {e: 1, $or: [{a:1, b:1}, {c:1, d:1}]}}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }

    // Can't use subplanning if the contained $or query has a geo predicate.
    {
        std::string findCmd =
            "{find: 'testns',"
            "filter: {loc: {$geoWithin: {$centerSphere: [[0,0], 1]}},"
            "e: 1, $or: [{a:1, b:1}, {c:1, d:1}]}}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }

    // Can't use subplanning if the contained $or query also has a $text predicate.
    {
        std::string findCmd =
            "{find: 'testns',"
            "filter: {$text: {$search: 'foo'},"
            "e: 1, $or: [{a:1, b:1}, {c:1, d:1}]}}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }

    // Can't use subplanning if the contained $or query also has a $near predicate.
    {
        std::string findCmd =
            "{find: 'testns',"
            "filter: {loc: {$near: [0, 0]},"
            "e: 1, $or: [{a:1, b:1}, {c:1, d:1}]}}";
        std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
        ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
    }
}

/**
 * Test the subplan stage's ability to answer a rooted $or query with a $ne and a sort.
 *
 * Regression test for SERVER-19388.
 */
TEST_F(QueryStageSubplanTest, QueryStageSubplanPlanRootedOrNE) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns());
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1 << "c" << 1));

    // Every doc matches.
    insert(BSON("_id" << 1 << "a" << 1));
    insert(BSON("_id" << 2 << "a" << 2));
    insert(BSON("_id" << 3 << "a" << 3));
    insert(BSON("_id" << 4));

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson("{$or: [{a: 1}, {a: {$ne:1}}]}"));
    qr->setSort(BSON("d" << 1));
    auto cq = unittest::assertGet(CanonicalQuery::canonicalize(opCtx(), std::move(qr)));

    Collection* collection = ctx.getCollection();

    QueryPlannerParams plannerParams;
    fillOutPlannerParams(opCtx(), collection, cq.get(), &plannerParams);

    WorkingSet ws;
    std::unique_ptr<SubplanStage> subplan(
        new SubplanStage(opCtx(), collection, &ws, plannerParams, cq.get()));

    PlanYieldPolicy yieldPolicy(PlanExecutor::NO_YIELD, _clock);
    ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

    size_t numResults = 0;
    PlanStage::StageState stageState = PlanStage::NEED_TIME;
    while (stageState != PlanStage::IS_EOF) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        stageState = subplan->work(&id);
        ASSERT_NE(stageState, PlanStage::DEAD);
        ASSERT_NE(stageState, PlanStage::FAILURE);
        if (stageState == PlanStage::ADVANCED) {
            ++numResults;
        }
    }

    ASSERT_EQ(numResults, 4U);
}

TEST_F(QueryStageSubplanTest, ShouldReportErrorIfExceedsTimeLimitDuringPlanning) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns());
    // Build a query with a rooted $or.
    auto queryRequest = stdx::make_unique<QueryRequest>(nss);
    queryRequest->setFilter(BSON("$or" << BSON_ARRAY(BSON("p1" << 1) << BSON("p2" << 2))));
    auto canonicalQuery =
        uassertStatusOK(CanonicalQuery::canonicalize(opCtx(), std::move(queryRequest)));

    // Add 4 indices: 2 for each predicate to choose from.
    addIndex(BSON("p1" << 1 << "opt1" << 1));
    addIndex(BSON("p1" << 1 << "opt2" << 1));
    addIndex(BSON("p2" << 1 << "opt1" << 1));
    addIndex(BSON("p2" << 1 << "opt2" << 1));
    QueryPlannerParams params;
    fillOutPlannerParams(opCtx(), ctx.getCollection(), canonicalQuery.get(), &params);

    // Add some data so planning has to do some thinking.
    for (int i = 0; i < 100; ++i) {
        insert(BSON("_id" << i << "p1" << 1 << "p2" << 1));
        insert(BSON("_id" << 2 * i << "p1" << 1 << "p2" << 2));
        insert(BSON("_id" << 3 * i << "p1" << 2 << "p2" << 1));
        insert(BSON("_id" << 4 * i << "p1" << 2 << "p2" << 2));
    }

    // Create the SubplanStage.
    WorkingSet workingSet;
    SubplanStage subplanStage(
        opCtx(), ctx.getCollection(), &workingSet, params, canonicalQuery.get());

    AlwaysTimeOutYieldPolicy alwaysTimeOutPolicy(serviceContext()->getFastClockSource());
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit, subplanStage.pickBestPlan(&alwaysTimeOutPolicy));
}

TEST_F(QueryStageSubplanTest, ShouldReportErrorIfKilledDuringPlanning) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns());
    // Build a query with a rooted $or.
    auto queryRequest = stdx::make_unique<QueryRequest>(nss);
    queryRequest->setFilter(BSON("$or" << BSON_ARRAY(BSON("p1" << 1) << BSON("p2" << 2))));
    auto canonicalQuery =
        uassertStatusOK(CanonicalQuery::canonicalize(opCtx(), std::move(queryRequest)));

    // Add 4 indices: 2 for each predicate to choose from.
    addIndex(BSON("p1" << 1 << "opt1" << 1));
    addIndex(BSON("p1" << 1 << "opt2" << 1));
    addIndex(BSON("p2" << 1 << "opt1" << 1));
    addIndex(BSON("p2" << 1 << "opt2" << 1));
    QueryPlannerParams params;
    fillOutPlannerParams(opCtx(), ctx.getCollection(), canonicalQuery.get(), &params);

    // Create the SubplanStage.
    WorkingSet workingSet;
    SubplanStage subplanStage(
        opCtx(), ctx.getCollection(), &workingSet, params, canonicalQuery.get());

    AlwaysPlanKilledYieldPolicy alwaysPlanKilledYieldPolicy(serviceContext()->getFastClockSource());
    ASSERT_EQ(ErrorCodes::QueryPlanKilled, subplanStage.pickBestPlan(&alwaysPlanKilledYieldPolicy));
}

}  // namespace
}  // namespace mongo
