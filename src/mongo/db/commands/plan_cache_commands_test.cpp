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

using namespace mongo;

namespace {

    using std::string;
    using std::vector;

    static const char* ns = "somebogusns";

    /**
     * Tests for planCacheListKeys
     */

    /**
     * Functor to extract cache key from BSON element.
     * Validates cache key during extraction.
     */
    struct GetCacheKey {
        PlanCacheKey operator()(const BSONElement& elt) {
            string keyStr = elt.String();
            ASSERT_FALSE(keyStr.empty());
            return PlanCacheKey(keyStr);
        }
    };

    /**
     * Utility function to get list of keys in the cache.
     */
    vector<PlanCacheKey> getKeys(const PlanCache& planCache) {
        BSONObjBuilder bob;
        ASSERT_OK(PlanCacheListKeys::listKeys(planCache, &bob));
        BSONObj resultObj = bob.obj();
        BSONElement queriesElt = resultObj.getField("queries");
        ASSERT_EQUALS(queriesElt.type(), mongo::Array);
        vector<BSONElement> keyEltArray = queriesElt.Array();
        vector<PlanCacheKey> keys(keyEltArray.size());
        std::transform(keyEltArray.begin(), keyEltArray.end(), keys.begin(), GetCacheKey());
        return keys;
    }

    /**
     * Utility function to create a SolutionCacheData
     */
    SolutionCacheData* createSolutionCacheData() {
        auto_ptr<SolutionCacheData> scd(new SolutionCacheData());
        scd->tree.reset(new PlanCacheIndexTree());
        return scd.release();
    }

    TEST(PlanCacheCommandsTest, planCacheListKeysEmpty) {
        PlanCache empty;
        vector<PlanCacheKey> keys = getKeys(empty);
        ASSERT_TRUE(keys.empty());
    }

    TEST(PlanCacheCommandsTest, planCacheListKeysOneKey) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Generate a cache key
        PlanCacheKey queryKey = PlanCache::getPlanCacheKey(*cq);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        planCache.add(*cq, solns, new PlanRankingDecision());

        vector<PlanCacheKey> keys = getKeys(planCache);
        ASSERT_EQUALS(keys.size(), 1U);
        ASSERT_EQUALS(keys[0], queryKey);
    }

    /**
     * Tests for planCacheClear
     */

    TEST(PlanCacheCommandsTest, planCacheClearOneKey) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Generate a cache key
        PlanCacheKey queryKey = PlanCache::getPlanCacheKey(*cq);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        planCache.add(*cq, solns, new PlanRankingDecision());
        ASSERT_EQUALS(getKeys(planCache).size(), 1U);

        // Clear cache and confirm number of keys afterwards.
        ASSERT_OK(PlanCacheClear::clear(&planCache));
        ASSERT_EQUALS(getKeys(planCache).size(), 0U);
    }

    /**
     * Tests for runGetPlanCacheKey
     * Mostly validation on the input parameters
     */

    /**
     * Utility function to generate a cache key from command object string.
     */
    PlanCacheKey generateKey(const char* cmdStr) {
        BSONObj cmdObj = fromjson(cmdStr);
        BSONObjBuilder bob;
        Status status = PlanCacheGenerateKey::generate(ns, cmdObj, &bob);
        if (!status.isOK()) {
            stringstream ss;
            ss << "failed to generate cache key. cmdObj: " << cmdStr;
            FAIL(ss.str());
        }
        BSONObj obj = bob.obj();
        PlanCacheKey key = obj.getStringField("key");
        if (key.empty()) {
            stringstream ss;
            ss << "zero-length cache key generated. cmdObj: " << cmdStr;
            FAIL(ss.str());
        }
        return key;
    }

    TEST(PlanCacheCommandsTest, planCacheGenerateKey) {
        // Invalid parameters
        BSONObjBuilder ignored;
        // Missing query field
        ASSERT_NOT_OK(PlanCacheGenerateKey::generate(ns, fromjson("{}"), &ignored));
        // Query needs to be an object
        ASSERT_NOT_OK(PlanCacheGenerateKey::generate(ns, fromjson("{query: 1}"), &ignored));
        // Sort needs to be an object
        ASSERT_NOT_OK(PlanCacheGenerateKey::generate(ns, fromjson("{query: {}, sort: 1}"),
                                                     &ignored));
        // Bad query (invalid sort order)
        ASSERT_NOT_OK(PlanCacheGenerateKey::generate(ns, fromjson("{query: {}, sort: {a: 0}}"),
                                                     &ignored));

        // Valid parameters
        PlanCacheKey queryKey = generateKey("{query: {a: 1, b: 1}}");

        // Equivalent query should generate same key.
        PlanCacheKey equivQueryKey = generateKey("{query: {b: 1, a: 1}}");
        ASSERT_EQUALS(queryKey, equivQueryKey);

        // Sort query should generate different key from unsorted query.
        PlanCacheKey sortQueryKey = generateKey("{query: {a: 1, b: 1}, sort: {a: 1}}");
        ASSERT_NOT_EQUALS(queryKey, sortQueryKey);

        // Projected query should generate different key from unprojected query.
        PlanCacheKey projectionQueryKey =
            generateKey("{query: {a: 1, b: 1}, projection: {_id: 0, a: 1}}");
        ASSERT_NOT_EQUALS(queryKey, projectionQueryKey);
    }

    /**
     * Tests for planCacheGet
     */

    TEST(PlanCacheCommandsTest, planCacheGetInvalidParameter) {
        PlanCache planCache;
        BSONObjBuilder ignored;
        // Missing key field is not ok.
        ASSERT_NOT_OK(PlanCacheGet::get(planCache, BSONObj(), &ignored));
        // Key field type must be PlanCacheKey.
        ASSERT_NOT_OK(PlanCacheGet::get(planCache, fromjson("{key: 12345}"), &ignored));
        ASSERT_NOT_OK(PlanCacheGet::get(planCache, fromjson("{key: /keyisnotregex/}"),
                                        &ignored));
    }

    TEST(PlanCacheCommandsTest, planCacheGetUnknownKey) {
        // Generate a cache key for lookup.
        PlanCacheKey queryKey = generateKey("{query: {a: 1}}");

        // Leave the plan cache empty.
        PlanCache planCache;

        BSONObjBuilder ignored;
        ASSERT_NOT_OK(PlanCacheGet::get(planCache, BSON("key" << queryKey), &ignored));
    }

    TEST(PlanCacheCommandsTest, planCacheGetOneKey) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), fromjson("{a: -1}"),
                                               fromjson("{_id: 0, a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Generate a cache key
        PlanCacheKey queryKey = PlanCache::getPlanCacheKey(*cq);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        planCache.add(*cq, solns, new PlanRankingDecision());

        BSONObjBuilder bob;
        ASSERT_OK(PlanCacheGet::get(planCache, BSON("key" << queryKey), &bob));
        BSONObj obj = bob.obj();

        // Check required fields in result.
        // query
        const LiteParsedQuery& pq = cq->getParsed();
        BSONElement queryElt = obj.getField("query");
        ASSERT_TRUE(queryElt.isABSONObj());
        ASSERT_EQUALS(queryElt.Obj(), pq.getFilter());
        // sort
        BSONElement sortElt = obj.getField("sort");
        ASSERT_TRUE(sortElt.isABSONObj());
        ASSERT_EQUALS(sortElt.Obj(), pq.getSort());
        // projection
        BSONElement projectionElt = obj.getField("projection");
        ASSERT_TRUE(projectionElt.isABSONObj());
        ASSERT_EQUALS(projectionElt.Obj(), pq.getProj());
    }

    /**
     * Tests for planCacheDrop
     */

    TEST(PlanCacheCommandsTest, planCacheDropInvalidParameter) {
        PlanCache planCache;
        // Missing key field is not ok.
        ASSERT_NOT_OK(PlanCacheDrop::drop(&planCache, BSONObj()));
        // Key field type must be PlanCacheKey.
        ASSERT_NOT_OK(PlanCacheDrop::drop(&planCache, fromjson("{key: 12345}")));
        ASSERT_NOT_OK(PlanCacheDrop::drop(&planCache, fromjson("{key: /keyisnotregex/}")));
    }

    TEST(PlanCacheCommandsTest, planCacheDropUnknownKey) {
        // Generate a cache key for lookup.
        PlanCacheKey queryKey = generateKey("{query: {a: 1}}");

        // Leave the plan cache empty.
        PlanCache planCache;

        ASSERT_NOT_OK(PlanCacheDrop::drop(&planCache, BSON("key" << queryKey)));
    }

    TEST(PlanCacheCommandsTest, planCacheDropOneKey) {
        // Create 2 canonical queries.
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cqA(cqRaw);
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{b: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cqB(cqRaw);

        // Generate 2 cache keys.
        PlanCacheKey keyA = PlanCache::getPlanCacheKey(*cqA);
        PlanCacheKey keyB = PlanCache::getPlanCacheKey(*cqB);

        // Create plan cache with 2 entries.
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        planCache.add(*cqA, solns, new PlanRankingDecision());
        planCache.add(*cqB, solns, new PlanRankingDecision());

        // Check keys in cache before dropping {b: 1}
        vector<PlanCacheKey> keysBefore = getKeys(planCache);
        ASSERT_EQUALS(keysBefore.size(), 2U);
        ASSERT_TRUE(std::find(keysBefore.begin(), keysBefore.end(), keyA) != keysBefore.end());
        ASSERT_TRUE(std::find(keysBefore.begin(), keysBefore.end(), keyB) != keysBefore.end());

        // Drop {b: 1} from cache. Make sure {a: 1} is still in cache afterwards.
        BSONObjBuilder bob;
        ASSERT_OK(PlanCacheDrop::drop(&planCache, BSON("key" << keyB)));
        vector<PlanCacheKey> keysAfter = getKeys(planCache);
        ASSERT_EQUALS(keysAfter.size(), 1U);
        ASSERT_EQUALS(keysAfter[0], keyA);
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
     *     pinned: <pinned>,
     *     shunned: <shunned>,
     *     source: <source>
     * }
     * Compilation note: GCC 4.4 has issues with getPlan() declared as a function object.
     */
    BSONObj getPlan(const BSONElement& elt) {
        ASSERT_TRUE(elt.isABSONObj());
        BSONObj obj = elt.Obj();

        // Check required fields.
        // plan ID
        BSONElement planElt = obj.getField("plan");
        ASSERT_EQUALS(planElt.type(), mongo::String);
        string planStr = planElt.String();
        ASSERT_FALSE(planStr.empty());

        // details
        BSONElement detailsElt = obj.getField("details");
        ASSERT_TRUE(detailsElt.isABSONObj());

        // reason
        BSONElement reasonElt = obj.getField("reason");
        ASSERT_TRUE(reasonElt.isABSONObj());

        // feedback
        BSONElement feedbackElt = obj.getField("feedback");
        ASSERT_TRUE(feedbackElt.isABSONObj());

        // pinned
        BSONElement pinnedElt = obj.getField("pinned");
        ASSERT_TRUE(pinnedElt.isBoolean());

        // shunned
        BSONElement shunnedElt = obj.getField("shunned");
        ASSERT_TRUE(shunnedElt.isBoolean());

        // source
        BSONElement sourceElt = obj.getField("source");
        ASSERT_EQUALS(sourceElt.type(), mongo::String);
        string sourceStr = sourceElt.String();
        ASSERT_TRUE(sourceStr == "planner" || sourceStr == "client");

        return obj.copy();
    }

    /**
     * Utility function to get list of plan IDs for a query in the cache.
     */
    vector<BSONObj> getPlans(const PlanCache& planCache, const PlanCacheKey& key) {
        BSONObjBuilder bob;
        BSONObj cmdObj = BSON("key" << key);
        ASSERT_OK(PlanCacheListPlans::list(planCache, cmdObj, &bob));
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
        // Missing key field is not ok.
        ASSERT_NOT_OK(PlanCacheListPlans::list(planCache, BSONObj(), &ignored));
        // Key field type must be PlanCacheKey.
        ASSERT_NOT_OK(PlanCacheListPlans::list(planCache, fromjson("{key: 12345}"), &ignored));
        ASSERT_NOT_OK(PlanCacheListPlans::list(planCache, fromjson("{key: /keyisnotregex/}"),
                                               &ignored));
    }

    TEST(PlanCacheCommandsTest, planCacheListPlansUnknownKey) {
        // Generate a cache key for lookup.
        PlanCacheKey queryKey = generateKey("{query: {a: 1}}");

        // Leave the plan cache empty.
        PlanCache planCache;

        BSONObjBuilder ignored;
        ASSERT_NOT_OK(PlanCacheListPlans::list(planCache, BSON("key" << queryKey), &ignored));
    }

    TEST(PlanCacheCommandsTest, planCacheListPlansOnlyOneSolutionTrue) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Generate a cache key
        PlanCacheKey queryKey = PlanCache::getPlanCacheKey(*cq);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        planCache.add(*cq, solns, new PlanRankingDecision());

        vector<BSONObj> plans = getPlans(planCache, queryKey);
        ASSERT_EQUALS(plans.size(), 1U);
    }

    TEST(PlanCacheCommandsTest, planCacheListPlansOnlyOneSolutionFalse) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Generate a cache key
        PlanCacheKey queryKey = PlanCache::getPlanCacheKey(*cq);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        // Add cache entry with 2 solutions.
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        solns.push_back(&qs);
        planCache.add(*cq, solns, new PlanRankingDecision());

        vector<BSONObj> plans = getPlans(planCache, queryKey);
        ASSERT_EQUALS(plans.size(), 2U);
    }

    /**
     * Tests for planCachePinPlan
     */

    TEST(PlanCacheCommandsTest, planCachePinPlanInvalidParameter) {
        PlanCache planCache;
        // Missing key or plan field is not ok.
        ASSERT_NOT_OK(PlanCachePinPlan::pin(&planCache, BSONObj()));
        ASSERT_NOT_OK(PlanCachePinPlan::pin(&planCache, fromjson("{key: 'mykey'}")));
        ASSERT_NOT_OK(PlanCachePinPlan::pin(&planCache, fromjson("{plan: 'myplan'}")));
        // Key field type must be PlanCacheKey.
        ASSERT_NOT_OK(PlanCachePinPlan::pin(&planCache, fromjson("{key: 12345, plan: 'p1'}")));
        ASSERT_NOT_OK(PlanCachePinPlan::pin(&planCache,
                                            fromjson("{key: /myregex/, plan: 'p1'}")));
        // Plan field type must be string.
        ASSERT_NOT_OK(PlanCachePinPlan::pin(&planCache, fromjson("{key: 'mykey', plan: 123}")));
        ASSERT_NOT_OK(PlanCachePinPlan::pin(&planCache,
                                            fromjson("{key: 'mykey', plan: /myregex/}")));
    }

    TEST(PlanCacheCommandsTest, planCachePinPlanUnknownKey) {
        // Generate a cache key for lookup.
        PlanCacheKey queryKey = generateKey("{query: {a: 1}}");

        // Leave the plan cache empty.
        PlanCache planCache;

        ASSERT_NOT_OK(PlanCachePinPlan::pin(&planCache,
                                            BSON("key" << queryKey << "plan" << "myplan")));
    }

    TEST(PlanCacheCommandsTest, planCachePinPlanOneKey) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Generate a cache key
        PlanCacheKey key = PlanCache::getPlanCacheKey(*cq);

        // Plan cache with 2 entries
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        solns.push_back(&qs);
        planCache.add(*cq, solns, new PlanRankingDecision());

        // Get first plan ID
        PlanID plan = getPlans(planCache, key).front().getStringField("plan");

        // Command with invalid plan ID should raise an error.
        PlanID badPlan = "BADPLAN_" + plan;
        ASSERT_NOT_OK(PlanCachePinPlan::pin(&planCache, BSON("key" << key << "plan" << badPlan)));

        // Check pin status before pinning.
        vector<BSONObj> plansBefore = getPlans(planCache, key);
        ASSERT_FALSE(plansBefore[0].getBoolField("pinned"));
        ASSERT_FALSE(plansBefore[1].getBoolField("pinned"));

        ASSERT_OK(PlanCachePinPlan::pin(&planCache, BSON("key" << key << "plan" << plan)));

        // Check pin status after pinning.
        vector<BSONObj> plansAfter = getPlans(planCache, key);
        ASSERT_TRUE(plansAfter[0].getBoolField("pinned"));
        ASSERT_FALSE(plansAfter[1].getBoolField("pinned"));

        // Pin second plan
        PlanID plan2 = getPlans(planCache, key).back().getStringField("plan");

        ASSERT_OK(PlanCachePinPlan::pin(&planCache, BSON("key" << key << "plan" << plan2)));

        // Check pin status after pinning.
        vector<BSONObj> plansAfter2 = getPlans(planCache, key);
        ASSERT_FALSE(plansAfter2[0].getBoolField("pinned"));
        ASSERT_TRUE(plansAfter2[1].getBoolField("pinned"));

    }

    /**
     * Tests for planCacheUnpinPlan
     */

    TEST(PlanCacheCommandsTest, planCacheUnpinPlanInvalidParameter) {
        PlanCache planCache;
        // Missing key field is not ok.
        ASSERT_NOT_OK(PlanCacheUnpinPlan::unpin(&planCache, BSONObj()));
        // Key field type must be PlanCacheKey.
        ASSERT_NOT_OK(PlanCacheUnpinPlan::unpin(&planCache, fromjson("{key: 12345}")));
        ASSERT_NOT_OK(PlanCacheUnpinPlan::unpin(&planCache, fromjson("{key: /myregex/}")));
    }

    TEST(PlanCacheCommandsTest, planCacheUnpinPlanUnknownKey) {
        // Generate a cache key for lookup.
        PlanCacheKey queryKey = generateKey("{query: {a: 1}}");

        // Leave the plan cache empty.
        PlanCache planCache;

        ASSERT_NOT_OK(PlanCacheUnpinPlan::unpin(&planCache, BSON("key" << queryKey)));
    }

    TEST(PlanCacheCommandsTest, planCacheUnpinPlanOneKey) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Generate a cache key
        PlanCacheKey key = PlanCache::getPlanCacheKey(*cq);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        planCache.add(*cq, solns, new PlanRankingDecision());

        ASSERT_OK(PlanCacheUnpinPlan::unpin(&planCache, BSON("key" << key)));
    }

    /**
     * Tests for planCacheAddPlan
     */

    TEST(PlanCacheCommandsTest, planCacheAddPlanInvalidParameter) {
        BSONObjBuilder ignored;
        PlanCache planCache;

        // Missing key field is not ok.
        ASSERT_NOT_OK(PlanCacheAddPlan::add(&planCache, BSONObj(), &ignored));
        // Missing details field is not ok.
        ASSERT_NOT_OK(PlanCacheAddPlan::add(&planCache, BSON("key" << "mykey"), &ignored));
        // Key field type must be PlanCacheKey.
        ASSERT_NOT_OK(PlanCacheAddPlan::add(&planCache, fromjson("{key: 12345, details: {}}"),
                                            &ignored));
        ASSERT_NOT_OK(PlanCacheAddPlan::add(&planCache, fromjson("{key: /myregex/, details: {}}"),
                                            &ignored));
        // Details field type must be an object.
        ASSERT_NOT_OK(PlanCacheAddPlan::add(&planCache, fromjson("{key: 'mykey', details: 123}"),
                                            &ignored));
    }

    TEST(PlanCacheCommandsTest, planCacheAddPlanUnknownKey) {
        // Generate a cache key for lookup.
        PlanCacheKey queryKey = generateKey("{query: {a: 1}}");

        // Leave the plan cache empty.
        PlanCache planCache;

        BSONObjBuilder ignored;
        ASSERT_NOT_OK(PlanCacheAddPlan::add(&planCache,
                                            BSON("key" << queryKey << "details" << BSONObj()),
                                            &ignored));
    }

    TEST(PlanCacheCommandsTest, planCacheAddPlanOneKey) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Generate a cache key
        PlanCacheKey key = PlanCache::getPlanCacheKey(*cq);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        planCache.add(*cq, solns, new PlanRankingDecision());

        BSONObjBuilder bob;
        ASSERT_OK(PlanCacheAddPlan::add(&planCache, BSON("key" << key << "details" << BSONObj()),
                                        &bob));
        BSONObj resultObj = bob.obj();
        BSONElement planElt = resultObj.getField("plan");
        ASSERT_EQUALS(planElt.type(), mongo::String);
        PlanID plan(planElt.String());
        ASSERT_FALSE(plan.empty());
    }

    /**
     * Tests for planCacheShunPlan
     */

    TEST(PlanCacheCommandsTest, planCacheShunPlanInvalidParameter) {
        PlanCache planCache;
        // Missing key or plan field is not ok.
        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache, BSONObj()));
        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache, fromjson("{key: 'mykey'}")));
        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache, fromjson("{plan: 'myplan'}")));
        // Key field type must be PlanCacheKey.
        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache, fromjson("{key: 12345, plan: 'p1'}")));
        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache,
                                              fromjson("{key: /myregex/, plan: 'p1'}")));
        // Plan field type must be string.
        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache, fromjson("{key: 'mykey', plan: 123}")));
        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache,
                                              fromjson("{key: 'mykey', plan: /myregex/}")));
    }

    TEST(PlanCacheCommandsTest, planCacheShunPlanUnknownKey) {
        // Generate a cache key for lookup.
        PlanCacheKey queryKey = generateKey("{query: {a: 1}}");

        // Leave the plan cache empty.
        PlanCache planCache;

        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache,
                                              BSON("key" << queryKey << "plan" << "myplan")));
    }

    // Attempting shun the only plan cached for a query should fail.
    TEST(PlanCacheCommandsTest, planCacheShunPlanSinglePlan) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Generate a cache key
        PlanCacheKey key = PlanCache::getPlanCacheKey(*cq);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        planCache.add(*cq, solns, new PlanRankingDecision());

        // Get first plan ID
        PlanID plan = getPlans(planCache, key).front().getStringField("plan");

        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache, BSON("key" << key << "plan" << plan)));
    }

    TEST(PlanCacheCommandsTest, planCacheShunPlanOneKey) {
        // Create a canonical query
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, fromjson("{a: 1}"), &cqRaw));
        auto_ptr<CanonicalQuery> cq(cqRaw);

        // Generate a cache key
        PlanCacheKey key = PlanCache::getPlanCacheKey(*cq);

        // Plan cache with one entry
        PlanCache planCache;
        QuerySolution qs;
        qs.cacheData.reset(createSolutionCacheData());
        // Add cache entry with 2 solutions.
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        solns.push_back(&qs);
        planCache.add(*cq, solns, new PlanRankingDecision());

        // Get first plan ID
        PlanID plan = getPlans(planCache, key).front().getStringField("plan");

        // Command with invalid plan ID should raise an error.
        PlanID badPlan = "BADPLAN_" + plan;
        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache,
                                              BSON("key" << key << "plan" << badPlan)));

        ASSERT_OK(PlanCacheShunPlan::shun(&planCache, BSON("key" << key << "plan" << plan)));

        // Check plan count after shunning. Should be unchanged.
        // shunned field should be set to false.
        vector<BSONObj> plans = getPlans(planCache, key);
        ASSERT_EQUALS(plans.size(), 2U);
        ASSERT_TRUE(plans.front().getBoolField("shunned"));

        // Shunning the same plan more than once has no effect.
        ASSERT_OK(PlanCacheShunPlan::shun(&planCache, BSON("key" << key << "plan" << plan)));

        // Plan entry must have at least one unshunned plan.
        // Shunning remaining plan should fail.
        PlanID plan1 = plans[1].getStringField("plan");
        ASSERT_NOT_OK(PlanCacheShunPlan::shun(&planCache,
                                              BSON("key" << key << "plan" << plan1)));
    }

}  // namespace
