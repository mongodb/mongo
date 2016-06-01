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
#include "mongo/db/client.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/exec/subplan.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/matcher/extensions_callback_noop.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/dbtests/dbtests.h"

namespace QueryStageSubplan {

static const NamespaceString nss("unittests.QueryStageSubplan");

class QueryStageSubplanBase {
public:
    QueryStageSubplanBase() : _client(&_txn) {}

    virtual ~QueryStageSubplanBase() {
        OldClientWriteContext ctx(&_txn, nss.ns());
        _client.dropCollection(nss.ns());
    }

    void addIndex(const BSONObj& obj) {
        ASSERT_OK(dbtests::createIndex(&_txn, nss.ns(), obj));
    }

    void insert(const BSONObj& doc) {
        _client.insert(nss.ns(), doc);
    }

    OperationContext* txn() {
        return &_txn;
    }

protected:
    /**
     * Parses the json string 'findCmd', specifying a find command, to a CanonicalQuery.
     */
    std::unique_ptr<CanonicalQuery> cqFromFindCommand(const std::string& findCmd) {
        BSONObj cmdObj = fromjson(findCmd);

        bool isExplain = false;
        auto qr = unittest::assertGet(QueryRequest::makeFromFindCommand(nss, cmdObj, isExplain));

        auto cq = unittest::assertGet(
            CanonicalQuery::canonicalize(txn(), std::move(qr), ExtensionsCallbackNoop()));
        return cq;
    }

    const ServiceContext::UniqueOperationContext _txnPtr = cc().makeOperationContext();
    OperationContext& _txn = *_txnPtr;
    ClockSource* _clock = _txn.getServiceContext()->getFastClockSource();

private:
    DBDirectClient _client;
};

/**
 * SERVER-15012: test that the subplan stage does not crash when the winning solution
 * for an $or clause uses a '2d' index. We don't produce cache data for '2d'. The subplanner
 * should gracefully fail after finding that no cache data is available, allowing us to fall
 * back to regular planning.
 */
class QueryStageSubplanGeo2dOr : public QueryStageSubplanBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, nss.ns());
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
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        Collection* collection = ctx.getCollection();

        // Get planner params.
        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq.get(), &plannerParams);

        WorkingSet ws;
        std::unique_ptr<SubplanStage> subplan(
            new SubplanStage(&_txn, collection, &ws, plannerParams, cq.get()));

        // Plan selection should succeed due to falling back on regular planning.
        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL, _clock);
        ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));
    }
};

/**
 * Test the SubplanStage's ability to plan an individual branch using the plan cache.
 */
class QueryStageSubplanPlanFromCache : public QueryStageSubplanBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, nss.ns());

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
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // Get planner params.
        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq.get(), &plannerParams);

        WorkingSet ws;
        std::unique_ptr<SubplanStage> subplan(
            new SubplanStage(&_txn, collection, &ws, plannerParams, cq.get()));

        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL, _clock);
        ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

        // Nothing is in the cache yet, so neither branch should have been planned from
        // the plan cache.
        ASSERT_FALSE(subplan->branchPlannedFromCache(0));
        ASSERT_FALSE(subplan->branchPlannedFromCache(1));

        // If we repeat the same query, the plan for the first branch should have come from
        // the cache.
        ws.clear();
        subplan.reset(new SubplanStage(&_txn, collection, &ws, plannerParams, cq.get()));

        ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

        ASSERT_TRUE(subplan->branchPlannedFromCache(0));
        ASSERT_FALSE(subplan->branchPlannedFromCache(1));
    }
};

/**
 * Ensure that the subplan stage doesn't create a plan cache entry if there are no query results.
 */
class QueryStageSubplanDontCacheZeroResults : public QueryStageSubplanBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, nss.ns());

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
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // Get planner params.
        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq.get(), &plannerParams);

        WorkingSet ws;
        std::unique_ptr<SubplanStage> subplan(
            new SubplanStage(&_txn, collection, &ws, plannerParams, cq.get()));

        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL, _clock);
        ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

        // Nothing is in the cache yet, so neither branch should have been planned from
        // the plan cache.
        ASSERT_FALSE(subplan->branchPlannedFromCache(0));
        ASSERT_FALSE(subplan->branchPlannedFromCache(1));

        // If we run the query again, it should again be the case that neither branch gets planned
        // from the cache (because the first call to pickBestPlan() refrained from creating any
        // cache entries).
        ws.clear();
        subplan.reset(new SubplanStage(&_txn, collection, &ws, plannerParams, cq.get()));

        ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

        ASSERT_FALSE(subplan->branchPlannedFromCache(0));
        ASSERT_FALSE(subplan->branchPlannedFromCache(1));
    }
};

/**
 * Ensure that the subplan stage doesn't create a plan cache entry if there are no query results.
 */
class QueryStageSubplanDontCacheTies : public QueryStageSubplanBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, nss.ns());

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
        auto statusWithCQ = CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions());
        ASSERT_OK(statusWithCQ.getStatus());
        std::unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

        // Get planner params.
        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq.get(), &plannerParams);

        WorkingSet ws;
        std::unique_ptr<SubplanStage> subplan(
            new SubplanStage(&_txn, collection, &ws, plannerParams, cq.get()));

        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL, _clock);
        ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

        // Nothing is in the cache yet, so neither branch should have been planned from
        // the plan cache.
        ASSERT_FALSE(subplan->branchPlannedFromCache(0));
        ASSERT_FALSE(subplan->branchPlannedFromCache(1));

        // If we run the query again, it should again be the case that neither branch gets planned
        // from the cache (because the first call to pickBestPlan() refrained from creating any
        // cache entries).
        ws.clear();
        subplan.reset(new SubplanStage(&_txn, collection, &ws, plannerParams, cq.get()));

        ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

        ASSERT_FALSE(subplan->branchPlannedFromCache(0));
        ASSERT_FALSE(subplan->branchPlannedFromCache(1));
    }
};

/**
 * Unit test the subplan stage's canUseSubplanning() method.
 */
class QueryStageSubplanCanUseSubplanning : public QueryStageSubplanBase {
public:
    void run() {
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

        // Can't use subplanning with snapshot.
        {
            std::string findCmd =
                "{find: 'testns',"
                "filter: {$or: [{a:1, b:1}, {c:1, d:1}]},"
                "snapshot: true}";
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
        //
        // TODO: Consider allowing this to use subplanning (see SERVER-13732).
        {
            std::string findCmd =
                "{find: 'testns',"
                "filter: {e: 1, $or: [{a:1, b:1}, {c:1, d:1}]}}";
            std::unique_ptr<CanonicalQuery> cq = cqFromFindCommand(findCmd);
            ASSERT_FALSE(SubplanStage::canUseSubplanning(*cq));
        }

        // Can't use subplanning if the contained $or query has a geo predicate.
        //
        // TODO: Consider allowing this to use subplanning (see SERVER-13732).
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
};

/**
 * Unit test the subplan stage's rewriteToRootedOr() method.
 */
class QueryStageSubplanRewriteToRootedOr : public QueryStageSubplanBase {
public:
    void run() {
        // Rewrite (AND (OR a b) e) => (OR (AND a e) (AND b e))
        {
            BSONObj queryObj = fromjson("{$or:[{a:1}, {b:1}], e:1}");
            const CollatorInterface* collator = nullptr;
            StatusWithMatchExpression expr = MatchExpressionParser::parse(
                queryObj, ExtensionsCallbackDisallowExtensions(), collator);
            ASSERT_OK(expr.getStatus());
            std::unique_ptr<MatchExpression> rewrittenExpr =
                SubplanStage::rewriteToRootedOr(std::move(expr.getValue()));

            std::string findCmdRewritten =
                "{find: 'testns',"
                "filter: {$or:[{a:1,e:1}, {b:1,e:1}]}}";
            std::unique_ptr<CanonicalQuery> cqRewritten = cqFromFindCommand(findCmdRewritten);

            ASSERT(rewrittenExpr->equivalent(cqRewritten->root()));
        }

        // Rewrite (AND (OR a b) e f) => (OR (AND a e f) (AND b e f))
        {
            BSONObj queryObj = fromjson("{$or:[{a:1}, {b:1}], e:1, f:1}");
            const CollatorInterface* collator = nullptr;
            StatusWithMatchExpression expr = MatchExpressionParser::parse(
                queryObj, ExtensionsCallbackDisallowExtensions(), collator);
            ASSERT_OK(expr.getStatus());
            std::unique_ptr<MatchExpression> rewrittenExpr =
                SubplanStage::rewriteToRootedOr(std::move(expr.getValue()));

            std::string findCmdRewritten =
                "{find: 'testns',"
                "filter: {$or:[{a:1,e:1,f:1}, {b:1,e:1,f:1}]}}";
            std::unique_ptr<CanonicalQuery> cqRewritten = cqFromFindCommand(findCmdRewritten);

            ASSERT(rewrittenExpr->equivalent(cqRewritten->root()));
        }

        // Rewrite (AND (OR (AND a b) (AND c d) e f) => (OR (AND a b e f) (AND c d e f))
        {
            BSONObj queryObj = fromjson("{$or:[{a:1,b:1}, {c:1,d:1}], e:1,f:1}");
            const CollatorInterface* collator = nullptr;
            StatusWithMatchExpression expr = MatchExpressionParser::parse(
                queryObj, ExtensionsCallbackDisallowExtensions(), collator);
            ASSERT_OK(expr.getStatus());
            std::unique_ptr<MatchExpression> rewrittenExpr =
                SubplanStage::rewriteToRootedOr(std::move(expr.getValue()));

            std::string findCmdRewritten =
                "{find: 'testns',"
                "filter: {$or:[{a:1,b:1,e:1,f:1},"
                "{c:1,d:1,e:1,f:1}]}}";
            std::unique_ptr<CanonicalQuery> cqRewritten = cqFromFindCommand(findCmdRewritten);

            ASSERT(rewrittenExpr->equivalent(cqRewritten->root()));
        }
    }
};

/**
 * Test the subplan stage's ability to answer a contained $or query.
 */
class QueryStageSubplanPlanContainedOr : public QueryStageSubplanBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, nss.ns());
        addIndex(BSON("b" << 1 << "a" << 1));
        addIndex(BSON("c" << 1 << "a" << 1));

        BSONObj query = fromjson("{a: 1, $or: [{b: 2}, {c: 3}]}");

        // Two of these documents match.
        insert(BSON("_id" << 1 << "a" << 1 << "b" << 2));
        insert(BSON("_id" << 2 << "a" << 2 << "b" << 2));
        insert(BSON("_id" << 3 << "a" << 1 << "c" << 3));
        insert(BSON("_id" << 4 << "a" << 1 << "c" << 4));

        auto qr = stdx::make_unique<QueryRequest>(nss);
        qr->setFilter(query);
        auto cq = unittest::assertGet(CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions()));

        Collection* collection = ctx.getCollection();

        // Get planner params.
        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq.get(), &plannerParams);

        WorkingSet ws;
        std::unique_ptr<SubplanStage> subplan(
            new SubplanStage(&_txn, collection, &ws, plannerParams, cq.get()));

        // Plan selection should succeed due to falling back on regular planning.
        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL, _clock);
        ASSERT_OK(subplan->pickBestPlan(&yieldPolicy));

        // Work the stage until it produces all results.
        size_t numResults = 0;
        PlanStage::StageState stageState = PlanStage::NEED_TIME;
        while (stageState != PlanStage::IS_EOF) {
            WorkingSetID id = WorkingSet::INVALID_ID;
            stageState = subplan->work(&id);
            ASSERT_NE(stageState, PlanStage::DEAD);
            ASSERT_NE(stageState, PlanStage::FAILURE);

            if (stageState == PlanStage::ADVANCED) {
                ++numResults;
                WorkingSetMember* member = ws.get(id);
                ASSERT(member->hasObj());
                ASSERT(member->obj.value() == BSON("_id" << 1 << "a" << 1 << "b" << 2) ||
                       member->obj.value() == BSON("_id" << 3 << "a" << 1 << "c" << 3));
            }
        }

        ASSERT_EQ(numResults, 2U);
    }
};

/**
 * Test the subplan stage's ability to answer a rooted $or query with a $ne and a sort.
 *
 * Regression test for SERVER-19388.
 */
class QueryStageSubplanPlanRootedOrNE : public QueryStageSubplanBase {
public:
    void run() {
        OldClientWriteContext ctx(&_txn, nss.ns());
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
        auto cq = unittest::assertGet(CanonicalQuery::canonicalize(
            txn(), std::move(qr), ExtensionsCallbackDisallowExtensions()));

        Collection* collection = ctx.getCollection();

        QueryPlannerParams plannerParams;
        fillOutPlannerParams(&_txn, collection, cq.get(), &plannerParams);

        WorkingSet ws;
        std::unique_ptr<SubplanStage> subplan(
            new SubplanStage(&_txn, collection, &ws, plannerParams, cq.get()));

        PlanYieldPolicy yieldPolicy(PlanExecutor::YIELD_MANUAL, _clock);
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
};

class All : public Suite {
public:
    All() : Suite("query_stage_subplan") {}

    void setupTests() {
        add<QueryStageSubplanGeo2dOr>();
        add<QueryStageSubplanPlanFromCache>();
        add<QueryStageSubplanDontCacheZeroResults>();
        add<QueryStageSubplanDontCacheTies>();
        add<QueryStageSubplanCanUseSubplanning>();
        add<QueryStageSubplanRewriteToRootedOr>();
        add<QueryStageSubplanPlanContainedOr>();
        add<QueryStageSubplanPlanRootedOrNE>();
    }
};

SuiteInstance<All> all;

}  // namespace QueryStageSubplan
