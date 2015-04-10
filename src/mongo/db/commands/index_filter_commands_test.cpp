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
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

/**
 * This file contains tests for mongo/db/commands/index_filter_commands.h
 */

#include "mongo/db/commands/index_filter_commands.h"

#include <boost/scoped_ptr.hpp>

#include "mongo/db/json.h"
#include "mongo/db/operation_context_noop.h"
#include "mongo/db/query/plan_ranker.h"
#include "mongo/db/query/query_solution.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;

namespace {

    using boost::scoped_ptr;
    using std::auto_ptr;
    using std::string;
    using std::vector;

    static const char* ns = "somebogusns";

    /**
     * Utility function to get list of index filters from the query settings.
     */
    vector<BSONObj> getFilters(const QuerySettings& querySettings) {
        BSONObjBuilder bob;
        ASSERT_OK(ListFilters::list(querySettings, &bob));
        BSONObj resultObj = bob.obj();
        BSONElement filtersElt = resultObj.getField("filters");
        ASSERT_EQUALS(filtersElt.type(), mongo::Array);
        vector<BSONElement> filtersEltArray = filtersElt.Array();
        vector<BSONObj> filters;
        for (vector<BSONElement>::const_iterator i = filtersEltArray.begin();
             i != filtersEltArray.end(); ++i) {
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

            // indexes
            BSONElement indexesElt = obj.getField("indexes");
            ASSERT_EQUALS(indexesElt.type(), mongo::Array);

            // All fields OK. Append to vector.
            filters.push_back(obj.getOwned());
        }

        return filters;
    }

    /**
     * Utility function to create a PlanRankingDecision
     */
    PlanRankingDecision* createDecision(size_t numPlans) {
        auto_ptr<PlanRankingDecision> why(new PlanRankingDecision());
        for (size_t i = 0; i < numPlans; ++i) {
            CommonStats common("COLLSCAN");
            auto_ptr<PlanStageStats> stats(new PlanStageStats(common, STAGE_COLLSCAN));
            stats->specific.reset(new CollectionScanStats());
            why->stats.mutableVector().push_back(stats.release());
            why->scores.push_back(0U);
            why->candidateOrder.push_back(i);
        }
        return why.release();
    }

    /**
     * Injects an entry into plan cache for query shape.
     */
    void addQueryShapeToPlanCache(PlanCache* planCache, const char* queryStr, const char* sortStr,
                                  const char* projectionStr) {
        BSONObj queryObj = fromjson(queryStr);
        BSONObj sortObj = fromjson(sortStr);
        BSONObj projectionObj = fromjson(projectionStr);

        // Create canonical query.
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, queryObj, sortObj, projectionObj, &cqRaw));
        scoped_ptr<CanonicalQuery> cq(cqRaw);

        QuerySolution qs;
        qs.cacheData.reset(new SolutionCacheData());
        qs.cacheData->tree.reset(new PlanCacheIndexTree());
        std::vector<QuerySolution*> solns;
        solns.push_back(&qs);
        ASSERT_OK(planCache->add(*cq, solns, createDecision(1U)));
    }

    /**
     * Checks if plan cache contains query shape.
     */
    bool planCacheContains(const PlanCache& planCache, const char* queryStr, const char* sortStr,
                           const char* projectionStr) {
        BSONObj queryObj = fromjson(queryStr);
        BSONObj sortObj = fromjson(sortStr);
        BSONObj projectionObj = fromjson(projectionStr);

        // Create canonical query.
        CanonicalQuery* cqRaw;
        ASSERT_OK(CanonicalQuery::canonicalize(ns, queryObj, sortObj, projectionObj, &cqRaw));
        scoped_ptr<CanonicalQuery> cq(cqRaw);

        // Retrieve cache entries from plan cache.
        vector<PlanCacheEntry*> entries = planCache.getAllEntries();

        // Search keys.
        bool found = false;
        for (vector<PlanCacheEntry*>::const_iterator i = entries.begin(); i != entries.end(); i++) {
            PlanCacheEntry* entry = *i;

            // Canonicalizing query shape in cache entry to get cache key.
            // Alternatively, we could add key to PlanCacheEntry but that would be used in one place only.
            ASSERT_OK(CanonicalQuery::canonicalize(ns, entry->query, entry->sort,
                                                   entry->projection, &cqRaw));
            scoped_ptr<CanonicalQuery> currentQuery(cqRaw);

            if (planCache.computeKey(*currentQuery) == planCache.computeKey(*cq)) {
                found = true;
            }
            // Release resources for cache entry after extracting key.
            delete entry;
        }
        return found;
    }

    /**
     * Tests for ListFilters
     */

    TEST(IndexFilterCommandsTest, ListFiltersEmpty) {
        QuerySettings empty;
        vector<BSONObj> filters = getFilters(empty);
        ASSERT_TRUE(filters.empty());
    }

    /**
     * Tests for ClearFilters
     */

    TEST(IndexFilterCommandsTest, ClearFiltersInvalidParameter) {
        QuerySettings empty;
        PlanCache planCache;
        OperationContextNoop txn;

        // If present, query has to be an object.
        ASSERT_NOT_OK(ClearFilters::clear(&txn, &empty, &planCache, ns, fromjson("{query: 1234}")));
        // If present, sort must be an object.
        ASSERT_NOT_OK(ClearFilters::clear(&txn, &empty, &planCache, ns,
                                        fromjson("{query: {a: 1}, sort: 1234}")));
        // If present, projection must be an object.
        ASSERT_NOT_OK(ClearFilters::clear(&txn, &empty, &planCache, ns,
                                        fromjson("{query: {a: 1}, projection: 1234}")));
        // Query must pass canonicalization.
        ASSERT_NOT_OK(ClearFilters::clear(&txn, &empty, &planCache, ns,
                                        fromjson("{query: {a: {$no_such_op: 1}}}")));
        // Sort present without query is an error.
        ASSERT_NOT_OK(ClearFilters::clear(&txn, &empty, &planCache, ns, fromjson("{sort: {a: 1}}")));
        // Projection present without query is an error.
        ASSERT_NOT_OK(ClearFilters::clear(&txn, &empty, &planCache, ns,
                                        fromjson("{projection: {_id: 0, a: 1}}")));
    }

    TEST(IndexFilterCommandsTest, ClearNonexistentHint) {
        QuerySettings querySettings;
        PlanCache planCache;
        OperationContextNoop txn;

        ASSERT_OK(SetFilter::set(&txn, &querySettings, &planCache, ns,
            fromjson("{query: {a: 1}, indexes: [{a: 1}]}")));
        vector<BSONObj> filters = getFilters(querySettings);
        ASSERT_EQUALS(filters.size(), 1U);

        // Clear nonexistent hint.
        // Command should succeed and cache should remain unchanged.
        ASSERT_OK(ClearFilters::clear(&txn, &querySettings, &planCache, ns, fromjson("{query: {b: 1}}")));
        filters = getFilters(querySettings);
        ASSERT_EQUALS(filters.size(), 1U);
    }

    /**
     * Tests for SetFilter
     */

    TEST(IndexFilterCommandsTest, SetFilterInvalidParameter) {
        QuerySettings empty;
        PlanCache planCache;
        OperationContextNoop txn;

        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns, fromjson("{}")));
        // Missing required query field.
        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns, fromjson("{indexes: [{a: 1}]}")));
        // Missing required indexes field.
        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns, fromjson("{query: {a: 1}}")));
        // Query has to be an object.
        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns,
            fromjson("{query: 1234, indexes: [{a: 1}, {b: 1}]}")));
        // Indexes field has to be an array.
        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns,
                                   fromjson("{query: {a: 1}, indexes: 1234}")));
        // Array indexes field cannot empty.
        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns,
                                   fromjson("{query: {a: 1}, indexes: []}")));
        // Elements in indexes have to be objects.
        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns,
                                   fromjson("{query: {a: 1}, indexes: [{a: 1}, 99]}")));
        // Objects in indexes cannot be empty.
        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns,
                                   fromjson("{query: {a: 1}, indexes: [{a: 1}, {}]}")));
        // If present, sort must be an object.
        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns,
            fromjson("{query: {a: 1}, sort: 1234, indexes: [{a: 1}, {b: 1}]}")));
        // If present, projection must be an object.
        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns,
            fromjson("{query: {a: 1}, projection: 1234, indexes: [{a: 1}, {b: 1}]}")));
        // Query must pass canonicalization.
        ASSERT_NOT_OK(SetFilter::set(&txn, &empty, &planCache, ns,
            fromjson("{query: {a: {$no_such_op: 1}}, indexes: [{a: 1}, {b: 1}]}")));
    }

    TEST(IndexFilterCommandsTest, SetAndClearFilters) {
        QuerySettings querySettings;
        PlanCache planCache;
        OperationContextNoop txn;

        // Inject query shape into plan cache.
        addQueryShapeToPlanCache(&planCache, "{a: 1, b: 1}", "{a: -1}", "{_id: 0, a: 1}");
        ASSERT_TRUE(planCacheContains(planCache, "{a: 1, b: 1}", "{a: -1}", "{_id: 0, a: 1}"));

        ASSERT_OK(SetFilter::set(&txn, &querySettings, &planCache, ns,
            fromjson("{query: {a: 1, b: 1}, sort: {a: -1}, projection: {_id: 0, a: 1}, "
                     "indexes: [{a: 1}]}")));
        vector<BSONObj> filters = getFilters(querySettings);
        ASSERT_EQUALS(filters.size(), 1U);

        // Query shape should not exist in plan cache after hint is updated.
        ASSERT_FALSE(planCacheContains(planCache, "{a: 1, b: 1}", "{a: -1}", "{_id: 0, a: 1}"));

        // Fields in filter should match criteria in most recent query settings update.
        ASSERT_EQUALS(filters[0].getObjectField("query"), fromjson("{a: 1, b: 1}"));
        ASSERT_EQUALS(filters[0].getObjectField("sort"), fromjson("{a: -1}"));
        ASSERT_EQUALS(filters[0].getObjectField("projection"), fromjson("{_id: 0, a: 1}"));

        // Replacing the hint for the same query shape ({a: 1, b: 1} and {b: 2, a: 3}
        // share same shape) should not change the query settings size.
        ASSERT_OK(SetFilter::set(&txn, &querySettings, &planCache, ns,
            fromjson("{query: {b: 2, a: 3}, sort: {a: -1}, projection: {_id: 0, a: 1}, "
                     "indexes: [{a: 1, b: 1}]}")));
        filters = getFilters(querySettings);
        ASSERT_EQUALS(filters.size(), 1U);

        // Add hint for different query shape.
        ASSERT_OK(SetFilter::set(&txn, &querySettings, &planCache, ns,
                                 fromjson("{query: {b: 1}, indexes: [{b: 1}]}")));
        filters = getFilters(querySettings);
        ASSERT_EQUALS(filters.size(), 2U);

        // Add hint for 3rd query shape. This is to prepare for ClearHint tests.
        ASSERT_OK(SetFilter::set(&txn, &querySettings, &planCache, ns,
                                 fromjson("{query: {a: 1}, indexes: [{a: 1}]}")));
        filters = getFilters(querySettings);
        ASSERT_EQUALS(filters.size(), 3U);

        // Add 2 entries to plan cache and check plan cache after clearing one/all filters.
        addQueryShapeToPlanCache(&planCache, "{a: 1}", "{}", "{}");
        addQueryShapeToPlanCache(&planCache, "{b: 1}", "{}", "{}");

        // Clear single hint.
        ASSERT_OK(ClearFilters::clear(&txn, &querySettings, &planCache, ns,
                                    fromjson("{query: {a: 1}}")));
        filters = getFilters(querySettings);
        ASSERT_EQUALS(filters.size(), 2U);

        // Query shape should not exist in plan cache after cleaing 1 hint.
        ASSERT_FALSE(planCacheContains(planCache, "{a: 1}", "{}", "{}"));
        ASSERT_TRUE(planCacheContains(planCache, "{b: 1}", "{}", "{}"));

        // Clear all filters
        ASSERT_OK(ClearFilters::clear(&txn, &querySettings, &planCache, ns, fromjson("{}")));
        filters = getFilters(querySettings);
        ASSERT_TRUE(filters.empty());

        // {b: 1} should be gone from plan cache after flushing query settings.
        ASSERT_FALSE(planCacheContains(planCache, "{b: 1}", "{}", "{}"));
    }

}  // namespace
