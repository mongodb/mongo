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

#include <cstddef>
#include <fmt/format.h>
#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/plan_stage.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/exec/working_set.h"
#include "mongo/db/matcher/expression_parser.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/mock_yield_policies.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_planner_params.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/service_context.h"
#include "mongo/dbtests/dbtests.h"  // IWYU pragma: keep
#include "mongo/platform/atomic_word.h"
#include "mongo/rpc/op_msg.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/intrusive_counter.h"

namespace mongo {
namespace {

static const NamespaceString nss =
    NamespaceString::createNamespaceString_forTest("unittests.QueryStageSubplan");

class QueryStageSubplanTest : public unittest::Test {
public:
    QueryStageSubplanTest() : _client(_opCtx.get()) {}

    virtual ~QueryStageSubplanTest() {
        dbtests::WriteContextForTests ctx(opCtx(), nss.ns_forTest());
        _client.dropCollection(nss);
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(opCtx(), nss.ns_forTest(), obj));
    }

    void addIndexWithWildcardProjection(BSONObj keys, BSONObj wildcardProjection) {
        auto indexSpec = BSON(
            IndexDescriptor::kIndexNameFieldName
            << DBClientBase::genIndexName(keys) << IndexDescriptor::kKeyPatternFieldName << keys
            << IndexDescriptor::kUniqueFieldName << false << IndexDescriptor::kIndexVersionFieldName
            << static_cast<int>(IndexDescriptor::IndexVersion::kV2)
            << IndexDescriptor::kWildcardProjectionFieldName << wildcardProjection);
        ASSERT_OK(dbtests::createIndexFromSpec(opCtx(), nss.ns_forTest(), indexSpec));
    }

    void dropIndex(BSONObj keyPattern) {
        _client.dropIndex(nss, std::move(keyPattern));
    }

    void insert(const BSONObj& doc) {
        _client.insert(nss, doc);
    }

    OperationContext* opCtx() {
        return _opCtx.get();
    }

    ExpressionContext* expCtx() {
        return _expCtx.get();
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

        // If there is no '$db', append it.
        auto cmd = OpMsgRequestBuilder::create(
                       auth::ValidatedTenancyScope::get(_opCtx.get()),
                       DatabaseName::createDatabaseName_forTest(boost::none, "test"),
                       cmdObj)
                       .body;
        auto findCommand =
            query_request_helper::makeFromFindCommandForTests(cmd, NamespaceString());

        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = expCtx(),
            .parsedFind = ParsedFindCommandParams{
                .findCommand = std::move(findCommand),
                .allowedFeatures = MatchExpressionParser::kAllowAllSpecialFeatures}});
    }

    QueryPlannerParams makePlannerParams(const CollectionPtr& collection,
                                         const CanonicalQuery& canonicalQuery) {
        const MultipleCollectionAccessor collections{collection};
        return QueryPlannerParams{
            QueryPlannerParams::ArgsForSingleCollectionQuery{
                .opCtx = opCtx(),
                .canonicalQuery = canonicalQuery,
                .collections = collections,
                .plannerOptions = QueryPlannerParams::DEFAULT,
                .ignoreQuerySettings = true,
                .traversalPreference = boost::none,
            },
        };
    }

    void assertSubplanFromCache(QueryStageSubplanTest* test,
                                const dbtests::WriteContextForTests& ctx);

    const ServiceContext::UniqueOperationContext _opCtx = cc().makeOperationContext();
    ClockSource* _clock = _opCtx->getServiceContext()->getFastClockSource();
    boost::intrusive_ptr<ExpressionContext> _expCtx =
        make_intrusive<ExpressionContext>(_opCtx.get(), nullptr, nss);

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
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns_forTest());
    addIndex(BSON("a"
                  << "2d"
                  << "b" << 1));
    addIndex(BSON("a"
                  << "2d"));

    BSONObj query = fromjson(
        "{$or: [{a: {$geoWithin: {$centerSphere: [[0,0],10]}}},"
        "{a: {$geoWithin: {$centerSphere: [[1,1],10]}}}]}");

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(query);
    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

    CollectionPtr collection = ctx.getCollection();

    // Get planner params.
    auto plannerParams = makePlannerParams(ctx.getCollection(), *cq);

    WorkingSet ws;
    std::unique_ptr<SubplanStage> subplan(
        new SubplanStage(_expCtx.get(), &collection, &ws, cq.get()));

    // Plan selection should succeed due to falling back on regular planning.
    NoopYieldPolicy yieldPolicy(_expCtx->opCtx, _clock);
    ASSERT_OK(subplan->pickBestPlan(plannerParams, &yieldPolicy));
}

/**
 * Helper function to verify that branches of an $or can be subplanned from the cache. Assumes
 * that the indexes to be tested have already been created for the given WriteContextForTest.
 */
void QueryStageSubplanTest::assertSubplanFromCache(QueryStageSubplanTest* test,
                                                   const dbtests::WriteContextForTests& ctx) {
    // This query should result in a plan cache entry for the first $or branch, because
    // there are two competing indices. The second branch has only one relevant index, so
    // its winning plan should not be cached.
    BSONObj query = fromjson("{$or: [{a: 1, b: 3}, {c: 1}]}");

    for (int i = 0; i < 10; i++) {
        test->insert(BSON("a" << 1 << "b" << i << "c" << i));
    }

    CollectionPtr collection = ctx.getCollection();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(query);
    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(test->opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

    // Get planner params.
    auto plannerParams = makePlannerParams(collection, *cq);

    // For the remainder of this test, ensure that cache entries are available immediately, and
    // don't need go through an 'inactive' state before being usable.
    internalQueryCacheDisableInactiveEntries.store(true);

    WorkingSet ws;
    std::unique_ptr<SubplanStage> subplan(
        new SubplanStage(test->expCtx(), &collection, &ws, cq.get()));

    NoopYieldPolicy yieldPolicy(test->opCtx(), test->serviceContext()->getFastClockSource());
    ASSERT_OK(subplan->pickBestPlan(plannerParams, &yieldPolicy));

    // Nothing is in the cache yet, so neither branch should have been planned from
    // the plan cache.
    ASSERT_FALSE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));

    // If we repeat the same query, the plan for the first branch should have come from
    // the cache.
    ws.clear();
    subplan.reset(new SubplanStage(test->expCtx(), &collection, &ws, cq.get()));

    ASSERT_OK(subplan->pickBestPlan(plannerParams, &yieldPolicy));

    ASSERT_TRUE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));
}

/**
 * Test the SubplanStage's ability to plan an individual branch using the plan cache.
 */
TEST_F(QueryStageSubplanTest, QueryStageSubplanPlanFromCache) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns_forTest());

    addIndex(BSON("a" << 1));
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("c" << 1));

    assertSubplanFromCache(this, ctx);
}

/**
 * Test that the SubplanStage can plan an individual branch from the cache using a $** index.
 */
TEST_F(QueryStageSubplanTest, QueryStageSubplanPlanFromCacheWithWildcardIndex) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns_forTest());

    addIndex(BSON("$**" << 1));
    addIndex(BSON("a.$**" << 1));

    assertSubplanFromCache(this, ctx);
}

/**
 * Ensure that the subplan stage doesn't create a plan cache entry if there are no query
 * results.
 */
TEST_F(QueryStageSubplanTest, QueryStageSubplanDontCacheZeroResults) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns_forTest());

    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1));
    addIndex(BSON("c" << 1));
    // Exclude field 'c' from the wildcard index to make sure that the field has only one
    // relevant index.
    addIndexWithWildcardProjection(BSON("$**" << 1), BSON("c" << 0));


    for (int i = 0; i < 10; i++) {
        insert(BSON("a" << 1 << "b" << i << "c" << i));
    }

    // Running this query should not create any cache entries. For the first branch, it's
    // because there are no matching results. For the second branch it's because there is only
    // one relevant index.
    BSONObj query = fromjson("{$or: [{a: 1, b: 15}, {c: 1}]}");

    CollectionPtr collection = ctx.getCollection();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(query);
    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

    // Get planner params.
    auto plannerParams = makePlannerParams(ctx.getCollection(), *cq);

    WorkingSet ws;
    std::unique_ptr<SubplanStage> subplan(
        new SubplanStage(_expCtx.get(), &collection, &ws, cq.get()));

    NoopYieldPolicy yieldPolicy(_expCtx->opCtx, _clock);
    ASSERT_OK(subplan->pickBestPlan(plannerParams, &yieldPolicy));

    // Nothing is in the cache yet, so neither branch should have been planned from
    // the plan cache.
    ASSERT_FALSE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));

    // If we run the query again, it should again be the case that neither branch gets planned
    // from the cache (because the first call to pickBestPlan() refrained from creating any
    // cache entries).
    ws.clear();
    subplan.reset(new SubplanStage(_expCtx.get(), &collection, &ws, cq.get()));

    ASSERT_OK(subplan->pickBestPlan(plannerParams, &yieldPolicy));

    ASSERT_FALSE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));
}

/**
 * Ensure that the subplan stage doesn't create a plan cache entry if the candidate plans tie.
 */
TEST_F(QueryStageSubplanTest, QueryStageSubplanDontCacheTies) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns_forTest());

    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1 << "c" << 1));
    addIndex(BSON("d" << 1));
    // Exclude field 'd' from the wildcard index to make sure that the field has only one
    // relevant index.
    addIndexWithWildcardProjection(BSON("$**" << 1), BSON("d" << 0));

    for (int i = 0; i < 10; i++) {
        insert(BSON("a" << 1 << "e" << 1 << "d" << 1));
    }

    // Running this query should not create any cache entries. For the first branch, it's
    // because plans using the {a: 1, b: 1} and {a: 1, c: 1} indices should tie during plan
    // ranking. For the second branch it's because there is only one relevant index.
    BSONObj query = fromjson("{$or: [{a: 1, e: 1}, {d: 1}]}");

    CollectionPtr collection = ctx.getCollection();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(query);
    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

    // Get planner params.
    auto plannerParams = makePlannerParams(collection, *cq);

    WorkingSet ws;
    std::unique_ptr<SubplanStage> subplan(
        new SubplanStage(_expCtx.get(), &collection, &ws, cq.get()));

    NoopYieldPolicy yieldPolicy(_expCtx->opCtx, _clock);
    ASSERT_OK(subplan->pickBestPlan(plannerParams, &yieldPolicy));

    // Nothing is in the cache yet, so neither branch should have been planned from
    // the plan cache.
    ASSERT_FALSE(subplan->branchPlannedFromCache(0));
    ASSERT_FALSE(subplan->branchPlannedFromCache(1));

    // If we run the query again, it should again be the case that neither branch gets planned
    // from the cache (because the first call to pickBestPlan() refrained from creating any
    // cache entries).
    ws.clear();
    subplan.reset(new SubplanStage(_expCtx.get(), &collection, &ws, cq.get()));

    ASSERT_OK(subplan->pickBestPlan(plannerParams, &yieldPolicy));

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
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns_forTest());
    addIndex(BSON("a" << 1 << "b" << 1));
    addIndex(BSON("a" << 1 << "c" << 1));

    // Every doc matches.
    insert(BSON("_id" << 1 << "a" << 1));
    insert(BSON("_id" << 2 << "a" << 2));
    insert(BSON("_id" << 3 << "a" << 3));
    insert(BSON("_id" << 4));

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson("{$or: [{a: 1}, {a: {$ne:5}}]}"));
    findCommand->setSort(BSON("d" << 1));
    auto cq = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

    CollectionPtr collection = ctx.getCollection();
    auto plannerParams = makePlannerParams(collection, *cq);

    WorkingSet ws;
    std::unique_ptr<SubplanStage> subplan(
        new SubplanStage(_expCtx.get(), &collection, &ws, cq.get()));

    NoopYieldPolicy yieldPolicy(_expCtx->opCtx, _clock);
    ASSERT_OK(subplan->pickBestPlan(plannerParams, &yieldPolicy));

    size_t numResults = 0;
    PlanStage::StageState stageState = PlanStage::NEED_TIME;
    while (stageState != PlanStage::IS_EOF) {
        WorkingSetID id = WorkingSet::INVALID_ID;
        stageState = subplan->work(&id);
        if (stageState == PlanStage::ADVANCED) {
            ++numResults;
        }
    }

    ASSERT_EQ(numResults, 4U);
}

TEST_F(QueryStageSubplanTest, ShouldReportErrorIfExceedsTimeLimitDuringPlanning) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns_forTest());
    // Build a query with a rooted $or.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(BSON("$or" << BSON_ARRAY(BSON("p1" << 1) << BSON("p2" << 2))));
    auto canonicalQuery = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

    // Add 4 indices: 2 for each predicate to choose from.
    addIndex(BSON("p1" << 1 << "opt1" << 1));
    addIndex(BSON("p1" << -1 << "opt2" << 1));
    addIndex(BSON("p2" << 1 << "opt1" << 1));
    addIndex(BSON("p2" << -1 << "opt2" << 1));
    auto coll = ctx.getCollection();
    auto plannerParams = makePlannerParams(coll, *canonicalQuery);

    // Add some data so planning has to do some thinking.
    for (int i = 0; i < 100; ++i) {
        insert(BSON("_id" << i << "p1" << 1 << "p2" << 1));
        insert(BSON("_id" << 2 * i << "p1" << 1 << "p2" << 2));
        insert(BSON("_id" << 3 * i << "p1" << 2 << "p2" << 1));
        insert(BSON("_id" << 4 * i << "p1" << 2 << "p2" << 2));
    }

    // Create the SubplanStage.
    WorkingSet workingSet;
    SubplanStage subplanStage(_expCtx.get(), &coll, &workingSet, canonicalQuery.get());

    AlwaysTimeOutYieldPolicy alwaysTimeOutPolicy(_expCtx->opCtx,
                                                 serviceContext()->getFastClockSource());
    ASSERT_EQ(ErrorCodes::ExceededTimeLimit,
              subplanStage.pickBestPlan(plannerParams, &alwaysTimeOutPolicy));
}

TEST_F(QueryStageSubplanTest, ShouldReportErrorIfKilledDuringPlanning) {
    dbtests::WriteContextForTests ctx(opCtx(), nss.ns_forTest());
    // Build a query with a rooted $or.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(BSON("$or" << BSON_ARRAY(BSON("p1" << 1) << BSON("p2" << 2))));
    auto canonicalQuery = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

    // Add 4 indices: 2 for each predicate to choose from.
    addIndex(BSON("p1" << 1 << "opt1" << 1));
    addIndex(BSON("p1" << -1 << "opt2" << 1));
    addIndex(BSON("p2" << 1 << "opt1" << 1));
    addIndex(BSON("p2" << -1 << "opt2" << 1));
    auto coll = ctx.getCollection();
    auto plannerParams = makePlannerParams(coll, *canonicalQuery);

    // Create the SubplanStage.
    WorkingSet workingSet;
    SubplanStage subplanStage(_expCtx.get(), &coll, &workingSet, canonicalQuery.get());

    AlwaysPlanKilledYieldPolicy alwaysPlanKilledYieldPolicy(_expCtx->opCtx,
                                                            serviceContext()->getFastClockSource());
    ASSERT_EQ(ErrorCodes::QueryPlanKilled,
              subplanStage.pickBestPlan(plannerParams, &alwaysPlanKilledYieldPolicy));
}

TEST_F(QueryStageSubplanTest, ShouldThrowOnRestoreIfIndexDroppedBeforePlanSelection) {
    CollectionPtr collection;
    {
        dbtests::WriteContextForTests ctx{opCtx(), nss.ns_forTest()};
        addIndex(BSON("p1" << 1 << "opt1" << 1));
        addIndex(BSON("p1" << 1 << "opt2" << 1));
        addIndex(BSON("p2" << 1 << "opt1" << 1));
        addIndex(BSON("p2" << 1 << "opt2" << 1));
        addIndex(BSON("irrelevant" << 1));

        collection = ctx.getCollection();
        ASSERT(collection);
    }

    // Build a query with a rooted $or.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(BSON("$or" << BSON_ARRAY(BSON("p1" << 1) << BSON("p2" << 2))));
    auto canonicalQuery = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

    boost::optional<AutoGetCollectionForReadCommand> collLock;
    collLock.emplace(opCtx(), nss);

    // Create the SubplanStage.
    WorkingSet workingSet;
    SubplanStage subplanStage(_expCtx.get(), &collection, &workingSet, canonicalQuery.get());

    // Mimic a yield by saving the state of the subplan stage. Then, drop an index not being
    // used while yielded.
    subplanStage.saveState();
    collLock.reset();
    dropIndex(BSON("irrelevant" << 1));

    // Attempt to restore state. This should throw due the index drop. As a future improvement,
    // we may wish to make the subplan stage tolerate drops of indices it is not using.
    collLock.emplace(opCtx(), nss);
    ASSERT_THROWS_CODE(subplanStage.restoreState(&collLock->getCollection()),
                       DBException,
                       ErrorCodes::QueryPlanKilled);
}

TEST_F(QueryStageSubplanTest, ShouldNotThrowOnRestoreIfIndexDroppedAfterPlanSelection) {
    CollectionPtr collection;
    {
        dbtests::WriteContextForTests ctx{opCtx(), nss.ns_forTest()};
        addIndex(BSON("p1" << 1 << "opt1" << 1));
        addIndex(BSON("p1" << 1 << "opt2" << 1));
        addIndex(BSON("p2" << 1 << "opt1" << 1));
        addIndex(BSON("p2" << 1 << "opt2" << 1));
        addIndex(BSON("irrelevant" << 1));

        collection = ctx.getCollection();
        ASSERT(collection);
    }

    // Build a query with a rooted $or.
    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(BSON("$or" << BSON_ARRAY(BSON("p1" << 1) << BSON("p2" << 2))));
    auto canonicalQuery = std::make_unique<CanonicalQuery>(
        CanonicalQueryParams{.expCtx = makeExpressionContext(opCtx(), *findCommand),
                             .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});

    boost::optional<AutoGetCollectionForReadCommand> collLock;
    collLock.emplace(opCtx(), nss);


    // Add 4 indices: 2 for each predicate to choose from, and one index which is not relevant
    // to the query.
    auto plannerParams = makePlannerParams(collection, *canonicalQuery);

    // Create the SubplanStage.
    WorkingSet workingSet;
    SubplanStage subplanStage(_expCtx.get(), &collection, &workingSet, canonicalQuery.get());

    NoopYieldPolicy yieldPolicy(_expCtx->opCtx, serviceContext()->getFastClockSource());
    ASSERT_OK(subplanStage.pickBestPlan(plannerParams, &yieldPolicy));

    // Mimic a yield by saving the state of the subplan stage and dropping our lock. Then drop
    // an index not being used while yielded.
    subplanStage.saveState();
    collLock.reset();
    dropIndex(BSON("irrelevant" << 1));

    // Restoring state should succeed, since the plan selected by pickBestPlan() does not use
    // the index {irrelevant: 1}.
    collLock.emplace(opCtx(), nss);
    subplanStage.restoreState(&collLock->getCollection());
}

}  // namespace
}  // namespace mongo
