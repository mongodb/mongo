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
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongo;

namespace {

    using std::string;
    using std::vector;

    static const char* ns = "somebogusns";

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
        for (vector<BSONElement>::const_iterator i = shapesEltArray.begin();
             i != shapesEltArray.end(); ++i) {
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

             // All fields OK. Append to vector.
             shapes.push_back(obj.getOwned());
        }
        return shapes;
    }

    /**
     * Utility function to create a SolutionCacheData
     */
    SolutionCacheData* createSolutionCacheData() {
        auto_ptr<SolutionCacheData> scd(new SolutionCacheData());
        scd->tree.reset(new PlanCacheIndexTree());
        return scd.release();
    }

    /**
     * Utility function to create a PlanRankingDecision
     */
    PlanRankingDecision* createDecision(size_t numPlans) {
        auto_ptr<PlanRankingDecision> why(new PlanRankingDecision());
        for (size_t i = 0; i < numPlans; ++i) {
            auto_ptr<PlanStageStats> stats(new PlanStageStats(CommonStats(), STAGE_COLLSCAN));
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
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

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
        ASSERT_EQUALS(shapes[0].getObjectField("sort"), cq->getParsed().getSort());
        ASSERT_EQUALS(shapes[0].getObjectField("projection"), cq->getParsed().getProj());
    }

    /**
     * Tests for planCacheClear
     */

    TEST(PlanCacheCommandsTest, planCacheClearAllShapes) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        planCache.add(*cq, solns, createDecision(1U));
        ASSERT_EQUALS(getShapes(planCache).size(), 1U);

        // Clear cache and confirm number of keys afterwards.
        ASSERT_OK(PlanCacheClear::clear(&planCache, ns, BSONObj()));
        ASSERT_EQUALS(getShapes(planCache).size(), 0U);
    }

    /**
     * Tests for PlanCacheCommand::makeCacheKey
     * Mostly validation on the input parameters
     */

    TEST(PlanCacheCommandsTest, Canonicalize) {
        // Invalid parameters
        CanonicalQuery* cqRaw;
        // Missing query field
        ASSERT_NOT_OK(PlanCacheCommand::canonicalize(ns, fromjson("{}"), &cqRaw));
        // Query needs to be an object
        ASSERT_NOT_OK(PlanCacheCommand::canonicalize(ns, fromjson("{query: 1}"), &cqRaw));
        // Sort needs to be an object
        ASSERT_NOT_OK(PlanCacheCommand::canonicalize(ns, fromjson("{query: {}, sort: 1}"),
                                                     &cqRaw));
        // Bad query (invalid sort order)
        ASSERT_NOT_OK(PlanCacheCommand::canonicalize(ns, fromjson("{query: {}, sort: {a: 0}}"),
                                                     &cqRaw));

        // Valid parameters
        ASSERT_OK(PlanCacheCommand::canonicalize(ns, fromjson("{query: {a: 1, b: 1}}"), &cqRaw));
        scoped_ptr<CanonicalQuery> query(cqRaw);


        // Equivalent query should generate same key.
        ASSERT_OK(PlanCacheCommand::canonicalize(ns, fromjson("{query: {b: 1, a: 1}}"), &cqRaw));
        scoped_ptr<CanonicalQuery> equivQuery(cqRaw);
        ASSERT_EQUALS(query->getPlanCacheKey(), equivQuery->getPlanCacheKey());

        // Sort query should generate different key from unsorted query.
        ASSERT_OK(PlanCacheCommand::canonicalize(ns,
            fromjson("{query: {a: 1, b: 1}, sort: {a: 1}}"), &cqRaw));
        scoped_ptr<CanonicalQuery> sortQuery(cqRaw);
        ASSERT_NOT_EQUALS(query->getPlanCacheKey(), sortQuery->getPlanCacheKey());

        // Projected query should generate different key from unprojected query.
        ASSERT_OK(PlanCacheCommand::canonicalize(ns,
            fromjson("{query: {a: 1, b: 1}, projection: {_id: 0, a: 1}}"), &cqRaw));
        scoped_ptr<CanonicalQuery> projectionQuery(cqRaw);
        ASSERT_NOT_EQUALS(query->getPlanCacheKey(), projectionQuery->getPlanCacheKey());
    }

    /**
     * Tests for planCacheClear (single query shape)
     */

    TEST(PlanCacheCommandsTest, planCacheClearInvalidParameter) {
        PlanCache planCache;
        // Query field type must be BSON object.
        ASSERT_NOT_OK(PlanCacheClear::clear(&planCache, ns, fromjson("{query: 12345}")));
        ASSERT_NOT_OK(PlanCacheClear::clear(&planCache, ns, fromjson("{query: /keyisnotregex/}")));
        // Query must pass canonicalization.
        ASSERT_NOT_OK(PlanCacheClear::clear(&planCache, ns,
                                            fromjson("{query: {a: {$no_such_op: 1}}}")));
        // Sort present without query is an error.
        ASSERT_NOT_OK(PlanCacheClear::clear(&planCache, ns, fromjson("{sort: {a: 1}}")));
        // Projection present without query is an error.
        ASSERT_NOT_OK(PlanCacheClear::clear(&planCache, ns,
                                            fromjson("{projection: {_id: 0, a: 1}}")));
    }

    TEST(PlanCacheCommandsTest, planCacheClearUnknownKey) {
        PlanCache planCache;
        ASSERT_NOT_OK(PlanCacheClear::clear(&planCache, ns, fromjson("{query: {a: 1}}")));
    }

    TEST(PlanCacheCommandsTest, planCacheClearOneKey) {
        // Create 2 canonical queries.
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cqA(cqRaw);
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{b: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cqB(cqRaw);

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
        BSONObj shapeA = BSON("query" << cqA->getQueryObj() << "sort" << cqA->getParsed().getSort()
                           << "projection" << cqA->getParsed().getProj());
        BSONObj shapeB = BSON("query" << cqB->getQueryObj() << "sort" << cqB->getParsed().getSort()
                           << "projection" << cqB->getParsed().getProj());
        ASSERT_TRUE(std::find(shapesBefore.begin(), shapesBefore.end(), shapeA) != shapesBefore.end());
        ASSERT_TRUE(std::find(shapesBefore.begin(), shapesBefore.end(), shapeB) != shapesBefore.end());

        // Drop {b: 1} from cache. Make sure {a: 1} is still in cache afterwards.
        BSONObjBuilder bob;
        ASSERT_OK(PlanCacheClear::clear(&planCache, ns, BSON("query" << cqB->getQueryObj())));
        vector<BSONObj> shapesAfter = getShapes(planCache);
        ASSERT_EQUALS(shapesAfter.size(), 1U);
        ASSERT_EQUALS(shapesAfter[0], shapeA);
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
    vector<BSONObj> getPlans(const PlanCache& planCache, const BSONObj& query,
                             const BSONObj& sort, const BSONObj& projection) {
        BSONObjBuilder bob;
        BSONObj cmdObj = BSON("query" << query << "sort" << sort << "projection" << projection);
        ASSERT_OK(PlanCacheListPlans::list(planCache, ns, cmdObj, &bob));
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
        // Missing query field is not ok.
        ASSERT_NOT_OK(PlanCacheListPlans::list(planCache, ns, BSONObj(), &ignored));
        // Query field type must be BSON object.
        ASSERT_NOT_OK(PlanCacheListPlans::list(planCache, ns, fromjson("{query: 12345}"),
                                               &ignored));
        ASSERT_NOT_OK(PlanCacheListPlans::list(planCache, ns, fromjson("{query: /keyisnotregex/}"),
                                               &ignored));
    }

    TEST(PlanCacheCommandsTest, planCacheListPlansUnknownKey) {
        // Leave the plan cache empty.
        PlanCache planCache;

        BSONObjBuilder ignored;
        ASSERT_NOT_OK(PlanCacheListPlans::list(planCache, ns, fromjson("{query: {a: 1}}"),
                                               &ignored));
    }

    TEST(PlanCacheCommandsTest, planCacheListPlansOnlyOneSolutionTrue) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        planCache.add(*cq, solns, createDecision(1U));

        vector<BSONObj> plans = getPlans(planCache, cq->getQueryObj(),
                                         cq->getParsed().getSort(), cq->getParsed().getProj());
        ASSERT_EQUALS(plans.size(), 1U);
    }

    TEST(PlanCacheCommandsTest, planCacheListPlansOnlyOneSolutionFalse) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        // Add cache entry with 2 solutions.
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        solns.push_back(&qs);
        planCache.add(*cq, solns, createDecision(2U));

        vector<BSONObj> plans = getPlans(planCache, cq->getQueryObj(),
                                         cq->getParsed().getSort(), cq->getParsed().getProj());
        ASSERT_EQUALS(plans.size(), 2U);
    }

}  // namespace
