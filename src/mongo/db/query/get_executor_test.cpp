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
 * This file contains tests for mongo/db/query/get_executor.h
 */

#include "mongo/db/query/get_executor.h"

#include <boost/optional.hpp>
#include <string>
#include <unordered_set>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/json.h"
#include "mongo/db/matcher/extensions_callback_disallow_extensions.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/mongoutils/str.h"

using namespace mongo;

namespace {

using std::unique_ptr;

static const NamespaceString nss("test.collection");

/**
 * Utility functions to create a CanonicalQuery
 */
unique_ptr<CanonicalQuery> canonicalize(const char* queryStr,
                                        const char* sortStr,
                                        const char* projStr) {
    QueryTestServiceContext serviceContext;
    auto txn = serviceContext.makeOperationContext();

    auto qr = stdx::make_unique<QueryRequest>(nss);
    qr->setFilter(fromjson(queryStr));
    qr->setSort(fromjson(sortStr));
    qr->setProj(fromjson(projStr));
    auto statusWithCQ = CanonicalQuery::canonicalize(
        txn.get(), std::move(qr), ExtensionsCallbackDisallowExtensions());
    ASSERT_OK(statusWithCQ.getStatus());
    return std::move(statusWithCQ.getValue());
}

//
// get_executor tests
//

//
// filterAllowedIndexEntries
//

/**
 * Test function to check filterAllowedIndexEntries.
 *
 * indexes: A vector of index entries to filter against.
 * keyPatterns: A set of index key patterns to use in the filter.
 * indexNames: A set of index names to use for the filter.
 *
 * expectedFilteredNames: The names of indexes that are expected to pass through the filter.
 */
void testAllowedIndices(std::vector<IndexEntry> indexes,
                        BSONObjSet keyPatterns,
                        std::unordered_set<std::string> indexNames,
                        std::unordered_set<std::string> expectedFilteredNames) {
    PlanCache planCache;
    QuerySettings querySettings;

    // getAllowedIndices should return false when query shape is not yet in query settings.
    unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}", "{}", "{}"));
    PlanCacheKey key = planCache.computeKey(*cq);
    ASSERT_FALSE(querySettings.getAllowedIndicesFilter(key));

    querySettings.setAllowedIndices(*cq, key, keyPatterns, indexNames);
    // Index entry vector should contain 1 entry after filtering.
    boost::optional<AllowedIndicesFilter> hasFilter = querySettings.getAllowedIndicesFilter(key);
    ASSERT_TRUE(hasFilter);
    ASSERT_FALSE(key.empty());
    auto& filter = *hasFilter;

    // Apply filter in allowed indices.
    filterAllowedIndexEntries(filter, &indexes);
    size_t matchedIndexes = 0;
    for (const auto& indexEntry : indexes) {
        ASSERT_TRUE(expectedFilteredNames.find(indexEntry.name) != expectedFilteredNames.end());
        matchedIndexes++;
    }
    ASSERT_EQ(matchedIndexes, indexes.size());
}

// Use of index filters to select compound index over single key index.
TEST(GetExecutorTest, GetAllowedIndices) {
    testAllowedIndices(
        {IndexEntry(fromjson("{a: 1}"), "a_1"),
         IndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
         IndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{a: 1, b: 1}")}),
        {},
        {"a_1_b_1"});
}

// Setting index filter referring to non-existent indexes
// will effectively disregard the index catalog and
// result in the planner generating a collection scan.
TEST(GetExecutorTest, GetAllowedIndicesNonExistentIndexKeyPatterns) {
    testAllowedIndices(
        {IndexEntry(fromjson("{a: 1}"), "a_1"),
         IndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
         IndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{nosuchfield: 1}")}),
        {},
        {});
}

// This test case shows how to force query execution to use
// an index that orders items in descending order.
TEST(GetExecutorTest, GetAllowedIndicesDescendingOrder) {
    testAllowedIndices(
        {IndexEntry(fromjson("{a: 1}"), "a_1"), IndexEntry(fromjson("{a: -1}"), "a_-1")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{a: -1}")}),
        {},
        {"a_-1"});
}

TEST(GetExecutorTest, GetAllowedIndicesMatchesByName) {
    testAllowedIndices(
        {IndexEntry(fromjson("{a: 1}"), "a_1"), IndexEntry(fromjson("{a: 1}"), "a_1:en")},
        // BSONObjSet default constructor is explicit, so we cannot copy-list-initialize until
        // C++14.
        SimpleBSONObjComparator::kInstance.makeBSONObjSet(),
        {"a_1"},
        {"a_1"});
}

TEST(GetExecutorTest, GetAllowedIndicesMatchesMultipleIndexesByKey) {
    testAllowedIndices(
        {IndexEntry(fromjson("{a: 1}"), "a_1"), IndexEntry(fromjson("{a: 1}"), "a_1:en")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{a: 1}")}),
        {},
        {"a_1", "a_1:en"});
}

}  // namespace
