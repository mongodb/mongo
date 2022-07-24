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

/**
 * This file contains tests for mongo/db/query/get_executor.h
 */

#include "mongo/db/query/get_executor.h"

#include <boost/optional.hpp>
#include <string>

#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/exec/projection_executor.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/exec/wildcard_projection.h"
#include "mongo/db/json.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/projection_parser.h"
#include "mongo/db/query/projection_policies.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/str.h"

using namespace mongo;

namespace {
auto createProjectionExecutor(const BSONObj& spec, const ProjectionPolicies& policies) {
    const boost::intrusive_ptr<ExpressionContextForTest> expCtx(new ExpressionContextForTest());
    auto projection = projection_ast::parseAndAnalyze(expCtx, spec, policies);
    auto executor = projection_executor::buildProjectionExecutor(
        expCtx, &projection, policies, projection_executor::kDefaultBuilderParams);
    return WildcardProjection{std::move(executor)};
}

using std::unique_ptr;

static const NamespaceString nss("test.collection");

/**
 * Utility functions to create a CanonicalQuery
 */
unique_ptr<CanonicalQuery> canonicalize(const char* queryStr,
                                        const char* sortStr,
                                        const char* projStr) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();

    auto findCommand = std::make_unique<FindCommandRequest>(nss);
    findCommand->setFilter(fromjson(queryStr));
    findCommand->setSort(fromjson(sortStr));
    findCommand->setProjection(fromjson(projStr));
    auto statusWithCQ = CanonicalQuery::canonicalize(opCtx.get(), std::move(findCommand));
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
                        stdx::unordered_set<std::string> indexNames,
                        stdx::unordered_set<std::string> expectedFilteredNames) {
    PlanCache planCache(5000);
    QuerySettings querySettings;

    // getAllowedIndices should return false when query shape is not yet in query settings.
    unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}", "{}", "{}"));
    const auto key = cq->encodeKeyForPlanCacheCommand();
    ASSERT_FALSE(querySettings.getAllowedIndicesFilter(key));

    querySettings.setAllowedIndices(*cq, keyPatterns, indexNames);
    // Index entry vector should contain 1 entry after filtering.
    boost::optional<AllowedIndicesFilter> hasFilter = querySettings.getAllowedIndicesFilter(key);
    ASSERT_TRUE(hasFilter);
    ASSERT_FALSE(key.empty());
    auto& filter = *hasFilter;

    // Apply filter in allowed indices.
    filterAllowedIndexEntries(filter, &indexes);
    ASSERT_EQ(std::max<size_t>(expectedFilteredNames.size(), indexNames.size()), indexes.size());
    for (const auto& indexEntry : indexes) {
        ASSERT_TRUE(expectedFilteredNames.find(indexEntry.identifier.catalogName) !=
                    expectedFilteredNames.end());
    }
}

/**
 * Make a minimal IndexEntry from just a key pattern and a name.
 */
IndexEntry buildSimpleIndexEntry(const BSONObj& kp, const std::string& indexName) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexDescriptor::kLatestIndexVersion,
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier(indexName),
            nullptr,
            {},
            nullptr,
            nullptr};
}

/**
 * Make a minimal IndexEntry from just a key pattern and a name. Include a wildcardProjection which
 * is neccesary for wildcard indicies.
 */
IndexEntry buildWildcardIndexEntry(const BSONObj& kp,
                                   const WildcardProjection& wcProj,
                                   const std::string& indexName) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexDescriptor::kLatestIndexVersion,
            false,
            {},
            {},
            false,
            false,
            CoreIndexInfo::Identifier(indexName),
            nullptr,
            {},
            nullptr,
            &wcProj};
}

// Use of index filters to select compound index over single key index.
TEST(GetExecutorTest, GetAllowedIndices) {
    testAllowedIndices(
        {buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
         buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
         buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{a: 1, b: 1}")}),
        stdx::unordered_set<std::string>{},
        {"a_1_b_1"});
}

// Setting index filter referring to non-existent indexes
// will effectively disregard the index catalog and
// result in the planner generating a collection scan.
TEST(GetExecutorTest, GetAllowedIndicesNonExistentIndexKeyPatterns) {
    testAllowedIndices(
        {buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
         buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
         buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{nosuchfield: 1}")}),
        stdx::unordered_set<std::string>{},
        stdx::unordered_set<std::string>{});
}

// This test case shows how to force query execution to use
// an index that orders items in descending order.
TEST(GetExecutorTest, GetAllowedIndicesDescendingOrder) {
    testAllowedIndices({buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
                        buildSimpleIndexEntry(fromjson("{a: -1}"), "a_-1")},
                       SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{a: -1}")}),
                       stdx::unordered_set<std::string>{},
                       {"a_-1"});
}

TEST(GetExecutorTest, GetAllowedIndicesMatchesByName) {
    testAllowedIndices({buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1:en")},
                       // BSONObjSet default constructor is explicit, so we cannot
                       // copy-list-initialize until C++14.
                       SimpleBSONObjComparator::kInstance.makeBSONObjSet(),
                       {"a_1"},
                       {"a_1"});
}

TEST(GetExecutorTest, GetAllowedIndicesMatchesMultipleIndexesByKey) {
    testAllowedIndices({buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1:en")},
                       SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{a: 1}")}),
                       stdx::unordered_set<std::string>{},
                       {"a_1", "a_1:en"});
}

TEST(GetExecutorTest, GetAllowedWildcardIndicesByKey) {
    auto wcProj = createProjectionExecutor(
        fromjson("{_id: 0}"),
        {ProjectionPolicies::DefaultIdPolicy::kExcludeId,
         ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays});
    testAllowedIndices({buildWildcardIndexEntry(BSON("$**" << 1), wcProj, "$**_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
                       SimpleBSONObjComparator::kInstance.makeBSONObjSet({BSON("$**" << 1)}),
                       stdx::unordered_set<std::string>{},
                       {"$**_1"});
}

TEST(GetExecutorTest, GetAllowedWildcardIndicesByName) {
    auto wcProj = createProjectionExecutor(
        fromjson("{_id: 0}"),
        {ProjectionPolicies::DefaultIdPolicy::kExcludeId,
         ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays});
    testAllowedIndices({buildWildcardIndexEntry(BSON("$**" << 1), wcProj, "$**_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
                       SimpleBSONObjComparator::kInstance.makeBSONObjSet(),
                       {"$**_1"},
                       {"$**_1"});
}

TEST(GetExecutorTest, GetAllowedPathSpecifiedWildcardIndicesByKey) {
    auto wcProj = createProjectionExecutor(
        fromjson("{_id: 0}"),
        {ProjectionPolicies::DefaultIdPolicy::kExcludeId,
         ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays});
    testAllowedIndices({buildWildcardIndexEntry(BSON("a.$**" << 1), wcProj, "a.$**_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
                       SimpleBSONObjComparator::kInstance.makeBSONObjSet({BSON("a.$**" << 1)}),
                       stdx::unordered_set<std::string>{},
                       {"a.$**_1"});
}

TEST(GetExecutorTest, GetAllowedPathSpecifiedWildcardIndicesByName) {
    auto wcProj = createProjectionExecutor(
        fromjson("{_id: 0}"),
        {ProjectionPolicies::DefaultIdPolicy::kExcludeId,
         ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays});
    testAllowedIndices({buildWildcardIndexEntry(BSON("a.$**" << 1), wcProj, "a.$**_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
                        buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
                       SimpleBSONObjComparator::kInstance.makeBSONObjSet(),
                       {"a.$**_1"},
                       {"a.$**_1"});
}

TEST(GetExecutorTest, isComponentOfPathMultikeyNoMetadata) {
    BSONObj indexKey = BSON("a" << 1 << "b.c" << -1);
    MultikeyPaths multikeyInfo = {};

    ASSERT_TRUE(isAnyComponentOfPathMultikey(indexKey, true, multikeyInfo, "a"));
    ASSERT_TRUE(isAnyComponentOfPathMultikey(indexKey, true, multikeyInfo, "b.c"));

    ASSERT_FALSE(isAnyComponentOfPathMultikey(indexKey, false, multikeyInfo, "a"));
    ASSERT_FALSE(isAnyComponentOfPathMultikey(indexKey, false, multikeyInfo, "b.c"));
}

TEST(GetExecutorTest, isComponentOfPathMultikeyWithMetadata) {
    BSONObj indexKey = BSON("a" << 1 << "b.c" << -1);
    MultikeyPaths multikeyInfo = {{}, {1}};

    ASSERT_FALSE(isAnyComponentOfPathMultikey(indexKey, true, multikeyInfo, "a"));
    ASSERT_TRUE(isAnyComponentOfPathMultikey(indexKey, true, multikeyInfo, "b.c"));
}

TEST(GetExecutorTest, isComponentOfPathMultikeyWithEmptyMetadata) {
    BSONObj indexKey = BSON("a" << 1 << "b.c" << -1);


    MultikeyPaths multikeyInfoAllPathsScalar = {{}, {}};
    ASSERT_FALSE(isAnyComponentOfPathMultikey(indexKey, false, multikeyInfoAllPathsScalar, "a"));
    ASSERT_FALSE(isAnyComponentOfPathMultikey(indexKey, false, multikeyInfoAllPathsScalar, "b.c"));
}

}  // namespace
