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
 * Test function to check filterAllowedIndexEntries
 */
void testAllowedIndices(const char* hintKeyPatterns[],
                        const char* indexCatalogKeyPatterns[],
                        const char* expectedFilteredKeyPatterns[]) {
    PlanCache planCache;
    QuerySettings querySettings;
    AllowedIndices* allowedIndicesRaw;

    // getAllowedIndices should return false when query shape is not yet in query settings.
    unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}", "{}", "{}"));
    PlanCacheKey key = planCache.computeKey(*cq);
    ASSERT_FALSE(querySettings.getAllowedIndices(key, &allowedIndicesRaw));

    // Add entry to query settings.
    std::vector<BSONObj> indexKeyPatterns;
    for (int i = 0; hintKeyPatterns[i] != NULL; ++i) {
        indexKeyPatterns.push_back(fromjson(hintKeyPatterns[i]));
    }
    querySettings.setAllowedIndices(*cq, key, indexKeyPatterns);

    // Index entry vector should contain 1 entry after filtering.
    ASSERT_TRUE(querySettings.getAllowedIndices(key, &allowedIndicesRaw));
    ASSERT_FALSE(key.empty());
    ASSERT(NULL != allowedIndicesRaw);
    unique_ptr<AllowedIndices> allowedIndices(allowedIndicesRaw);

    // Indexes from index catalog.
    std::vector<IndexEntry> indexEntries;
    for (int i = 0; indexCatalogKeyPatterns[i] != NULL; ++i) {
        indexEntries.push_back(IndexEntry(fromjson(indexCatalogKeyPatterns[i])));
    }

    // Apply filter in allowed indices.
    filterAllowedIndexEntries(*allowedIndices, &indexEntries);
    size_t numExpected = 0;
    while (expectedFilteredKeyPatterns[numExpected] != NULL) {
        ASSERT_LESS_THAN(numExpected, indexEntries.size());
        ASSERT_EQUALS(indexEntries[numExpected].keyPattern,
                      fromjson(expectedFilteredKeyPatterns[numExpected]));
        numExpected++;
    }
    ASSERT_EQUALS(indexEntries.size(), numExpected);
}

// Use of index filters to select compound index over single key index.
TEST(GetExecutorTest, GetAllowedIndices) {
    const char* hintKeyPatterns[] = {"{a: 1, b: 1}", NULL};
    const char* indexCatalogKeyPatterns[] = {"{a: 1}", "{a: 1, b: 1}", "{a: 1, c: 1}", NULL};
    const char* expectedFilteredKeyPatterns[] = {"{a: 1, b: 1}", NULL};
    testAllowedIndices(hintKeyPatterns, indexCatalogKeyPatterns, expectedFilteredKeyPatterns);
}

// Setting index filter referring to non-existent indexes
// will effectively disregard the index catalog and
// result in the planner generating a collection scan.
TEST(GetExecutorTest, GetAllowedIndicesNonExistentIndexKeyPatterns) {
    const char* hintKeyPatterns[] = {"{nosuchfield: 1}", NULL};
    const char* indexCatalogKeyPatterns[] = {"{a: 1}", "{a: 1, b: 1}", "{a: 1, c: 1}", NULL};
    const char* expectedFilteredKeyPatterns[] = {NULL};
    testAllowedIndices(hintKeyPatterns, indexCatalogKeyPatterns, expectedFilteredKeyPatterns);
}

// This test case shows how to force query execution to use
// an index that orders items in descending order.
TEST(GetExecutorTest, GetAllowedIndicesDescendingOrder) {
    const char* hintKeyPatterns[] = {"{a: -1}", NULL};
    const char* indexCatalogKeyPatterns[] = {"{a: 1}", "{a: -1}", NULL};
    const char* expectedFilteredKeyPatterns[] = {"{a: -1}", NULL};
    testAllowedIndices(hintKeyPatterns, indexCatalogKeyPatterns, expectedFilteredKeyPatterns);
}

}  // namespace
