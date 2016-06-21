/**
 *    Copyright (C) 2013 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This file contains tests for mongo/db/commands/plan_cache_commands.h
 */

#include "mongo/db/commands/plan_cache_commands.h"

#include <algorithm>

#include "mongo/db/json.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongo;

namespace {

using std::string;
using std::unique_ptr;
using std::vector;

static const NamespaceString nss("test.collection");

/**
 * Tests for planCacheListQueryShapes
 */

/**
 * Utility function to get list of keys in the cache.
 */
std::vector<BSONObj> getShapes(const PlanCache& planCache) {
    BSONObjBuilder bob;
    ASSERT_OK(PlanCacheListQueryShapes::list(planCache, &bob));
    BSONObj resultObj = bob.obj();
    BSONElement shapesElt = resultObj.getField("shapes");
    ASSERT_EQUALS(shapesElt.type(), mongo::Array);
    vector<BSONElement> shapesEltArray = shapesElt.Array();
    vector<BSONObj> shapes;
    for (vector<BSONElement>::const_iterator i = shapesEltArray.begin(); i != shapesEltArray.end();
         ++i) {
        const BSONElement& elt = *i;

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

        // collation
        BSONElement collationElt = obj.getField("collation");
        if (!collationElt.eoo()) {
            ASSERT_TRUE(collationElt.isABSONObj());
        }

        // All fields OK. Append to vector.
        shapes.push_back(obj.getOwned());
    }
    return shapes;
}

/**
 * Utility function to create a SolutionCacheData
 */
SolutionCacheData* createSolutionCacheData() {
    unique_ptr<SolutionCacheData> scd(new SolutionCacheData());
    scd->tree.reset(new PlanCacheIndexTree());
    return scd.release();
}

/**
 * Utility function to create a PlanRankingDecision
 */
PlanRankingDecision* createDecision(size_t numPlans) {
    unique_ptr<PlanRankingDecision> why(new PlanRankingDecision());
    for (size_t i = 0; i < numPlans; ++i) {
        CommonStats common("COLLSCAN");
        unique_ptr<PlanStageStats> stats(new PlanStageStats(common, STAGE_COLLSCAN));
        stats->specific.reset(new CollectionScanStats());
        why->stats.mutableVector().push_back(stats.release());
        why->scores.push_back(0U);
        why->candidateOrder.push_back(i);
    }
    return why.release();
}

TEST(PlanCacheCommandsTest, planCacheListQueryShapesEmpty) {
    PlanCache empty;
    vector<BSONObj> shapes = getShapes(empty);
    ASSERT_TRUE(shapes.empty());
}

TEST(PlanCacheCommandsTest, planCacheListQueryShapesOneKey) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    // Create a canonical query
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson("{a: 1}"));
    qr->setSort(fromjson("{a: -1}"));
    qr->setProj(fromjson("{_id: 0}"));
    qr->setCollation(fromjson("{locale: 'mock_reverse_string'}"));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // Plan cache with one entry
    PlanCache planCache;
    QuerySolution qs;
    qs.cacheData.reset(createSolutionCacheData());
    std::vector<QuerySolution*> solns;
    solns.push_back(&qs);
    planCache.add(*cq, solns, createDecision(1U));

    vector<BSONObj> shapes = getShapes(planCache);
    ASSERT_EQUALS(shapes.size(), 1U);
    ASSERT_EQUALS(shapes[0].getObjectField("query"), cq->getQueryObj());
    ASSERT_EQUALS(shapes[0].getObjectField("sort"), cq->getQueryRequest().getSort());
    ASSERT_EQUALS(shapes[0].getObjectField("projection"), cq->getQueryRequest().getProj());
    ASSERT_EQUALS(shapes[0].getObjectField("collation"), cq->getCollator()->getSpec().toBSON());
}

/**
 * Tests for planCacheClear
 */

TEST(PlanCacheCommandsTest, planCacheClearAllShapes) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    // Create a canonical query
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson("{a: 1}"));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // Plan cache with one entry
    PlanCache planCache;
    QuerySolution qs;

    qs.cacheData.reset(createSolutionCacheData());
    std::vector<QuerySolution*> solns;
    solns.push_back(&qs);
    planCache.add(*cq, solns, createDecision(1U));
    ASSERT_EQUALS(getShapes(planCache).size(), 1U);

    // Clear cache and confirm number of keys afterwards.
    ASSERT_OK(PlanCacheClear::clear(txn.get(), &planCache, nss.ns(), BSONObj()));
    ASSERT_EQUALS(getShapes(planCache).size(), 0U);
}

/**
 * Tests for PlanCacheCommand::makeCacheKey
 * Mostly validation on the input parameters
 */

TEST(PlanCacheCommandsTest, Canonicalize) {
    // Invalid parameters
    PlanCache planCache;
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    // Missing query field
    ASSERT_NOT_OK(PlanCacheCommand::canonicalize(txn.get(), nss.ns(), fromjson("{}")).getStatus());
    // Query needs to be an object
    ASSERT_NOT_OK(
        PlanCacheCommand::canonicalize(txn.get(), nss.ns(), fromjson("{query: 1}")).getStatus());
    // Sort needs to be an object
    ASSERT_NOT_OK(
        PlanCacheCommand::canonicalize(txn.get(), nss.ns(), fromjson("{query: {}, sort: 1}"))
            .getStatus());
    // Projection needs to be an object.
    ASSERT_NOT_OK(
        PlanCacheCommand::canonicalize(txn.get(), nss.ns(), fromjson("{query: {}, projection: 1}"))
            .getStatus());
    // Collation needs to be an object.
    ASSERT_NOT_OK(
        PlanCacheCommand::canonicalize(txn.get(), nss.ns(), fromjson("{query: {}, collation: 1}"))
            .getStatus());
    // Bad query (invalid sort order)
    ASSERT_NOT_OK(
        PlanCacheCommand::canonicalize(txn.get(), nss.ns(), fromjson("{query: {}, sort: {a: 0}}"))
            .getStatus());

    // Valid parameters
    auto statusWithCQ =
        PlanCacheCommand::canonicalize(txn.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> query = std::move(statusWithCQ.getValue());


    // Equivalent query should generate same key.
    statusWithCQ =
        PlanCacheCommand::canonicalize(txn.get(), nss.ns(), fromjson("{query: {b: 1, a: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> equivQuery = std::move(statusWithCQ.getValue());
    ASSERT_EQUALS(planCache.computeKey(*query), planCache.computeKey(*equivQuery));

    // Sort query should generate different key from unsorted query.
    statusWithCQ = PlanCacheCommand::canonicalize(
        txn.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}, sort: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> sortQuery1 = std::move(statusWithCQ.getValue());
    ASSERT_NOT_EQUALS(planCache.computeKey(*query), planCache.computeKey(*sortQuery1));

    // Confirm sort arguments are properly delimited (SERVER-17158)
    statusWithCQ = PlanCacheCommand::canonicalize(
        txn.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}, sort: {aab: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> sortQuery2 = std::move(statusWithCQ.getValue());
    ASSERT_NOT_EQUALS(planCache.computeKey(*sortQuery1), planCache.computeKey(*sortQuery2));

    // Changing order and/or value of predicates should not change key
    statusWithCQ = PlanCacheCommand::canonicalize(
        txn.get(), nss.ns(), fromjson("{query: {b: 3, a: 3}, sort: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> sortQuery3 = std::move(statusWithCQ.getValue());
    ASSERT_EQUALS(planCache.computeKey(*sortQuery1), planCache.computeKey(*sortQuery3));

    // Projected query should generate different key from unprojected query.
    statusWithCQ = PlanCacheCommand::canonicalize(
        txn.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}, projection: {_id: 0, a: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> projectionQuery = std::move(statusWithCQ.getValue());
    ASSERT_NOT_EQUALS(planCache.computeKey(*query), planCache.computeKey(*projectionQuery));
}

/**
 * Tests for planCacheClear (single query shape)
 */

TEST(PlanCacheCommandsTest, planCacheClearInvalidParameter) {
    PlanCache planCache;
    OperationContextNoop txn;

    // Query field type must be BSON object.
    ASSERT_NOT_OK(PlanCacheClear::clear(&txn, &planCache, nss.ns(), fromjson("{query: 12345}")));
    ASSERT_NOT_OK(
        PlanCacheClear::clear(&txn, &planCache, nss.ns(), fromjson("{query: /keyisnotregex/}")));
    // Query must pass canonicalization.
    ASSERT_NOT_OK(PlanCacheClear::clear(
        &txn, &planCache, nss.ns(), fromjson("{query: {a: {$no_such_op: 1}}}")));
    // Sort present without query is an error.
    ASSERT_NOT_OK(PlanCacheClear::clear(&txn, &planCache, nss.ns(), fromjson("{sort: {a: 1}}")));
    // Projection present without query is an error.
    ASSERT_NOT_OK(PlanCacheClear::clear(
        &txn, &planCache, nss.ns(), fromjson("{projection: {_id: 0, a: 1}}")));
    // Collation present without query is an error.
    ASSERT_NOT_OK(PlanCacheClear::clear(
        &txn, &planCache, nss.ns(), fromjson("{collation: {locale: 'en_US'}}")));
}

TEST(PlanCacheCommandsTest, planCacheClearUnknownKey) {
    PlanCache planCache;
    OperationContextNoop txn;

    ASSERT_OK(PlanCacheClear::clear(&txn, &planCache, nss.ns(), fromjson("{query: {a: 1}}")));
}

TEST(PlanCacheCommandsTest, planCacheClearOneKey) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    // Create 2 canonical queries.
    auto qrA = stdx::make_unique<QueryRequest>(nss);
    qrA->setFilter(fromjson("{a: 1}"));
    auto statusWithCQA = CanonicalQuery::canonicalize(
        txn.get(), std::move(qrA), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQA.getStatus());
    auto qrB = stdx::make_unique<QueryRequest>(nss);
    qrB->setFilter(fromjson("{b: 1}"));
    unique_ptr<CanonicalQuery> cqA = std::move(statusWithCQA.getValue());
    auto statusWithCQB = CanonicalQuery::canonicalize(
        txn.get(), std::move(qrB), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQB.getStatus());
    unique_ptr<CanonicalQuery> cqB = std::move(statusWithCQB.getValue());

    // Create plan cache with 2 entries.
    PlanCache planCache;
    QuerySolution qs;
    qs.cacheData.reset(createSolutionCacheData());
    std::vector<QuerySolution*> solns;
    solns.push_back(&qs);
    planCache.add(*cqA, solns, createDecision(1U));
    planCache.add(*cqB, solns, createDecision(1U));

    // Check keys in cache before dropping {b: 1}
    vector<BSONObj> shapesBefore = getShapes(planCache);
    ASSERT_EQUALS(shapesBefore.size(), 2U);
    BSONObj shapeA = BSON(
        "query" << cqA->getQueryObj() << "sort" << cqA->getQueryRequest().getSort() << "projection"
                << cqA->getQueryRequest().getProj());
    BSONObj shapeB = BSON(
        "query" << cqB->getQueryObj() << "sort" << cqB->getQueryRequest().getSort() << "projection"
                << cqB->getQueryRequest().getProj());
    ASSERT_TRUE(std::find(shapesBefore.begin(), shapesBefore.end(), shapeA) != shapesBefore.end());
    ASSERT_TRUE(std::find(shapesBefore.begin(), shapesBefore.end(), shapeB) != shapesBefore.end());

    // Drop {b: 1} from cache. Make sure {a: 1} is still in cache afterwards.
    BSONObjBuilder bob;

    ASSERT_OK(PlanCacheClear::clear(
        txn.get(), &planCache, nss.ns(), BSON("query" << cqB->getQueryObj())));
    vector<BSONObj> shapesAfter = getShapes(planCache);
    ASSERT_EQUALS(shapesAfter.size(), 1U);
    ASSERT_EQUALS(shapesAfter[0], shapeA);
}

TEST(PlanCacheCommandsTest, planCacheClearOneKeyCollation) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    // Create 2 canonical queries, one with collation.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson("{a: 'foo'}"));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
    auto qrCollation = stdx::make_unique<QueryRequest>(nss);
    qrCollation->setFilter(fromjson("{a: 'foo'}"));
    qrCollation->setCollation(fromjson("{locale: 'mock_reverse_string'}"));
    auto statusWithCQCollation = CanonicalQuery::canonicalize(
        txn.get(), std::move(qrCollation), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQCollation.getStatus());
    unique_ptr<CanonicalQuery> cqCollation = std::move(statusWithCQCollation.getValue());

    // Create plan cache with 2 entries. Add an index so that indexability is included in the plan
    // cache keys.
    PlanCache planCache;
    planCache.notifyOfIndexEntries(
        {IndexEntry(fromjson("{a: 1}"), false, false, false, "index_name", NULL, BSONObj())});
    QuerySolution qs;
    qs.cacheData.reset(createSolutionCacheData());
    std::vector<QuerySolution*> solns;
    solns.push_back(&qs);
    planCache.add(*cq, solns, createDecision(1U));
    planCache.add(*cqCollation, solns, createDecision(1U));

    // Check keys in cache before dropping the query with collation.
    vector<BSONObj> shapesBefore = getShapes(planCache);
    ASSERT_EQUALS(shapesBefore.size(), 2U);
    BSONObj shape = BSON("query" << cq->getQueryObj() << "sort" << cq->getQueryRequest().getSort()
                                 << "projection"
                                 << cq->getQueryRequest().getProj());
    BSONObj shapeWithCollation = BSON("query" << cqCollation->getQueryObj() << "sort"
                                              << cqCollation->getQueryRequest().getSort()
                                              << "projection"
                                              << cqCollation->getQueryRequest().getProj()
                                              << "collation"
                                              << cqCollation->getCollator()->getSpec().toBSON());
    ASSERT_TRUE(std::find(shapesBefore.begin(), shapesBefore.end(), shape) != shapesBefore.end());
    ASSERT_TRUE(std::find(shapesBefore.begin(), shapesBefore.end(), shapeWithCollation) !=
                shapesBefore.end());

    // Drop query with collation from cache. Make other query is still in cache afterwards.
    BSONObjBuilder bob;

    ASSERT_OK(PlanCacheClear::clear(txn.get(), &planCache, nss.ns(), shapeWithCollation));
    vector<BSONObj> shapesAfter = getShapes(planCache);
    ASSERT_EQUALS(shapesAfter.size(), 1U);
    ASSERT_EQUALS(shapesAfter[0], shape);
}

/**
 * Tests for planCacheListPlans
 */

/**
 * Function to extract plan ID from BSON element.
 * Validates planID during extraction.
 * Each BSON element contains an embedded BSON object with the following layout:
 * {
 *     plan: <plan_id>,
 *     details: <plan_details>,
 *     reason: <ranking_stats>,
 *     feedback: <execution_stats>,
 *     source: <source>
 * }
 * Compilation note: GCC 4.4 has issues with getPlan() declared as a function object.
 */
BSONObj getPlan(const BSONElement& elt) {
    ASSERT_TRUE(elt.isABSONObj());
    BSONObj obj = elt.Obj();

    // Check required fields.
    // details
    BSONElement detailsElt = obj.getField("details");
    ASSERT_TRUE(detailsElt.isABSONObj());

    // reason
    BSONElement reasonElt = obj.getField("reason");
    ASSERT_TRUE(reasonElt.isABSONObj());

    // feedback
    BSONElement feedbackElt = obj.getField("feedback");
    ASSERT_TRUE(feedbackElt.isABSONObj());

    return obj.getOwned();
}

/**
 * Utility function to get list of plan IDs for a query in the cache.
 */
vector<BSONObj> getPlans(const PlanCache& planCache,
                         const BSONObj& query,
                         const BSONObj& sort,
                         const BSONObj& projection,
                         const BSONObj& collation) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    BSONObjBuilder bob;
    BSONObjBuilder cmdObjBuilder;
    cmdObjBuilder.append("query", query);
    cmdObjBuilder.append("sort", sort);
    cmdObjBuilder.append("projection", projection);
    if (!collation.isEmpty()) {
        cmdObjBuilder.append("collation", collation);
    }
    BSONObj cmdObj = cmdObjBuilder.obj();
    ASSERT_OK(PlanCacheListPlans::list(txn.get(), planCache, nss.ns(), cmdObj, &bob));
    BSONObj resultObj = bob.obj();
    BSONElement plansElt = resultObj.getField("plans");
    ASSERT_EQUALS(plansElt.type(), mongo::Array);
    vector<BSONElement> planEltArray = plansElt.Array();
    ASSERT_FALSE(planEltArray.empty());
    vector<BSONObj> plans(planEltArray.size());
    std::transform(planEltArray.begin(), planEltArray.end(), plans.begin(), getPlan);
    return plans;
}

TEST(PlanCacheCommandsTest, planCacheListPlansInvalidParameter) {
    PlanCache planCache;
    BSONObjBuilder ignored;
    OperationContextNoop txn;

    // Missing query field is not ok.
    ASSERT_NOT_OK(PlanCacheListPlans::list(&txn, planCache, nss.ns(), BSONObj(), &ignored));
    // Query field type must be BSON object.
    ASSERT_NOT_OK(
        PlanCacheListPlans::list(&txn, planCache, nss.ns(), fromjson("{query: 12345}"), &ignored));
    ASSERT_NOT_OK(PlanCacheListPlans::list(
        &txn, planCache, nss.ns(), fromjson("{query: /keyisnotregex/}"), &ignored));
}

TEST(PlanCacheCommandsTest, planCacheListPlansUnknownKey) {
    // Leave the plan cache empty.
    PlanCache planCache;
    OperationContextNoop txn;

    BSONObjBuilder ignored;
    ASSERT_OK(
        PlanCacheListPlans::list(&txn, planCache, nss.ns(), fromjson("{query: {a: 1}}"), &ignored));
}

TEST(PlanCacheCommandsTest, planCacheListPlansOnlyOneSolutionTrue) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    // Create a canonical query
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson("{a: 1}"));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // Plan cache with one entry
    PlanCache planCache;
    QuerySolution qs;
    qs.cacheData.reset(createSolutionCacheData());
    std::vector<QuerySolution*> solns;
    solns.push_back(&qs);
    planCache.add(*cq, solns, createDecision(1U));

    vector<BSONObj> plans = getPlans(planCache,
                                     cq->getQueryObj(),
                                     cq->getQueryRequest().getSort(),
                                     cq->getQueryRequest().getProj(),
                                     cq->getQueryRequest().getCollation());
    ASSERT_EQUALS(plans.size(), 1U);
}

TEST(PlanCacheCommandsTest, planCacheListPlansOnlyOneSolutionFalse) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    // Create a canonical query
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson("{a: 1}"));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());

    // Plan cache with one entry
    PlanCache planCache;
    QuerySolution qs;
    qs.cacheData.reset(createSolutionCacheData());
    // Add cache entry with 2 solutions.
    std::vector<QuerySolution*> solns;
    solns.push_back(&qs);
    solns.push_back(&qs);
    planCache.add(*cq, solns, createDecision(2U));

    vector<BSONObj> plans = getPlans(planCache,
                                     cq->getQueryObj(),
                                     cq->getQueryRequest().getSort(),
                                     cq->getQueryRequest().getProj(),
                                     cq->getQueryRequest().getCollation());
    ASSERT_EQUALS(plans.size(), 2U);
}


TEST(PlanCacheCommandsTest, planCacheListPlansCollation) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    // Create 2 canonical queries, one with collation.
    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson("{a: 'foo'}"));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    unique_ptr<CanonicalQuery> cq = std::move(statusWithCQ.getValue());
    auto qrCollation = stdx::make_unique<QueryRequest>(nss);
    qrCollation->setFilter(fromjson("{a: 'foo'}"));
    qrCollation->setCollation(fromjson("{locale: 'mock_reverse_string'}"));
    auto statusWithCQCollation = CanonicalQuery::canonicalize(
        txn.get(), std::move(qrCollation), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQCollation.getStatus());
    unique_ptr<CanonicalQuery> cqCollation = std::move(statusWithCQCollation.getValue());

    // Create plan cache with 2 entries. Add an index so that indexability is included in the plan
    // cache keys. Give query with collation two solutions.
    PlanCache planCache;
    planCache.notifyOfIndexEntries(
        {IndexEntry(fromjson("{a: 1}"), false, false, false, "index_name", NULL, BSONObj())});
    QuerySolution qs;
    qs.cacheData.reset(createSolutionCacheData());
    std::vector<QuerySolution*> solns;
    solns.push_back(&qs);
    planCache.add(*cq, solns, createDecision(1U));
    std::vector<QuerySolution*> twoSolns;
    twoSolns.push_back(&qs);
    twoSolns.push_back(&qs);
    planCache.add(*cqCollation, twoSolns, createDecision(2U));

    // Normal query should have one solution.
    vector<BSONObj> plans = getPlans(planCache,
                                     cq->getQueryObj(),
                                     cq->getQueryRequest().getSort(),
                                     cq->getQueryRequest().getProj(),
                                     cq->getQueryRequest().getCollation());
    ASSERT_EQUALS(plans.size(), 1U);

    // Query with collation should have two solutions.
    vector<BSONObj> plansCollation = getPlans(planCache,
                                              cqCollation->getQueryObj(),
                                              cqCollation->getQueryRequest().getSort(),
                                              cqCollation->getQueryRequest().getProj(),
                                              cqCollation->getQueryRequest().getCollation());
    ASSERT_EQUALS(plansCollation.size(), 2U);
}

}  // namespace
