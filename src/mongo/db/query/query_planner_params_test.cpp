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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj_comparator_interface.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/json.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/db/exec/index_path_projection.h"
#include "mongo/db/exec/projection_executor_builder.h"
#include "mongo/db/index_names.h"
#include "mongo/db/local_catalog/collection_mock.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_parser.h"
#include "mongo/db/query/compiler/logical_model/projection/projection_policies.h"
#include "mongo/db/query/distinct_access.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/query_settings.h"
#include "mongo/db/query/query_settings_decoration.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/unittest/unittest.h"

#include <string>
#include <utility>

#include <absl/container/node_hash_map.h>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

namespace mongo {
namespace {

const NamespaceString nss = NamespaceString::createNamespaceString_forTest("test.collection");

class QueryPlannerParamsTest : public ServiceContextTest {
protected:
    /**
     * Utility functions to create a CanonicalQuery
     */
    std::unique_ptr<CanonicalQuery> canonicalize(const char* queryStr,
                                                 const char* sortStr,
                                                 const char* projStr) {
        auto findCommand = std::make_unique<FindCommandRequest>(nss);
        findCommand->setFilter(fromjson(queryStr));
        findCommand->setSort(fromjson(sortStr));
        findCommand->setProjection(fromjson(projStr));
        return std::make_unique<CanonicalQuery>(CanonicalQueryParams{
            .expCtx = ExpressionContextBuilder{}.fromRequest(_opCtx.get(), *findCommand).build(),
            .parsedFind = ParsedFindCommandParams{std::move(findCommand)}});
    }

    auto createProjectionExecutor(const BSONObj& spec, const ProjectionPolicies& policies) {
        const boost::intrusive_ptr<ExpressionContextForTest> expCtx(
            new ExpressionContextForTest(_opCtx.get()));
        auto projection = projection_ast::parseAndAnalyze(expCtx, spec, policies);
        auto executor = projection_executor::buildProjectionExecutor(
            expCtx, &projection, policies, projection_executor::kDefaultBuilderParams);
        return WildcardProjection{std::move(executor)};
    }

    /**
     * Asserts index filters are applied correctly and the list of available indexes after
     * application equals to 'expectedFilteredNames'.
     */
    void assertIndexFiltersApplication(std::vector<IndexEntry> indexes,
                                       BSONObjSet keyPatterns,
                                       stdx::unordered_set<std::string> indexNames,
                                       stdx::unordered_set<std::string> expectedFilteredNames) {
        // Create collection.
        std::shared_ptr<Collection> collection = std::make_shared<CollectionMock>(nss);
        auto catalog = CollectionCatalog::get(_opCtx.get());
        catalog->onCreateCollection(_opCtx.get(), collection);
        auto collectionPtr = CollectionPtr(catalog->establishConsistentCollection(
            _opCtx.get(), nss, boost::none /* readTimestamp */));

        // Retrieve query settings decoration.
        auto& querySettings = *QuerySettingsDecoration::get(collectionPtr->getSharedDecorations());

        // Ensure getAllowedIndices() returns boost::none if no index filters are specified for the
        // query shape.
        std::unique_ptr<CanonicalQuery> cq(canonicalize("{a: 1}", "{}", "{}"));
        ASSERT_FALSE(querySettings.getAllowedIndicesFilter(*cq));

        // Set index filter on the given query shape.
        querySettings.setAllowedIndices(*cq, keyPatterns, indexNames);

        // "Fill out" planner params and apply the index filters.
        QueryPlannerParams plannerParams(QueryPlannerParams::ArgsForTest{});
        plannerParams.mainCollectionInfo.indexes = indexes;
        plannerParams.applyIndexFilters(*cq.get(), collectionPtr);
        ASSERT_EQ(expectedFilteredNames.size(), plannerParams.mainCollectionInfo.indexes.size());
        for (const auto& indexEntry : plannerParams.mainCollectionInfo.indexes) {
            ASSERT_TRUE(expectedFilteredNames.find(indexEntry.identifier.catalogName) !=
                        expectedFilteredNames.end());
        }
    }

    ServiceContext::UniqueOperationContext _opCtx{makeOperationContext()};
};

/**
 * Make a minimal IndexEntry from just a key pattern and a name.
 */
IndexEntry buildSimpleIndexEntry(const BSONObj& kp, const std::string& indexName) {
    return {kp,
            IndexNames::nameToType(IndexNames::findPluginName(kp)),
            IndexConfig::kLatestIndexVersion,
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
            IndexConfig::kLatestIndexVersion,
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
TEST_F(QueryPlannerParamsTest, GetAllowedIndices) {
    assertIndexFiltersApplication(
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
TEST_F(QueryPlannerParamsTest, GetAllowedIndicesNonExistentIndexKeyPatterns) {
    assertIndexFiltersApplication(
        {buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
         buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
         buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{nosuchfield: 1}")}),
        stdx::unordered_set<std::string>{},
        stdx::unordered_set<std::string>{});
}

// This test case shows how to force query execution to use
// an index that orders items in descending order.
TEST_F(QueryPlannerParamsTest, GetAllowedIndicesDescendingOrder) {
    assertIndexFiltersApplication(
        {buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
         buildSimpleIndexEntry(fromjson("{a: -1}"), "a_-1")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{a: -1}")}),
        stdx::unordered_set<std::string>{},
        {"a_-1"});
}

TEST_F(QueryPlannerParamsTest, GetAllowedIndicesMatchesByName) {
    assertIndexFiltersApplication({buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
                                   buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1:en")},
                                  // BSONObjSet default constructor is explicit, so we cannot
                                  // copy-list-initialize until C++14.
                                  SimpleBSONObjComparator::kInstance.makeBSONObjSet(),
                                  {"a_1"},
                                  {"a_1"});
}

TEST_F(QueryPlannerParamsTest, GetAllowedIndicesMatchesMultipleIndexesByKey) {
    assertIndexFiltersApplication(
        {buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
         buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1:en")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({fromjson("{a: 1}")}),
        stdx::unordered_set<std::string>{},
        {"a_1", "a_1:en"});
}

TEST_F(QueryPlannerParamsTest, GetAllowedWildcardIndicesByKey) {
    auto wcProj = createProjectionExecutor(
        fromjson("{_id: 0}"),
        {ProjectionPolicies::DefaultIdPolicy::kExcludeId,
         ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays});
    assertIndexFiltersApplication(
        {buildWildcardIndexEntry(BSON("$**" << 1), wcProj, "$**_1"),
         buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
         buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
         buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({BSON("$**" << 1)}),
        stdx::unordered_set<std::string>{},
        {"$**_1"});
}

TEST_F(QueryPlannerParamsTest, GetAllowedWildcardIndicesByName) {
    auto wcProj = createProjectionExecutor(
        fromjson("{_id: 0}"),
        {ProjectionPolicies::DefaultIdPolicy::kExcludeId,
         ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays});
    assertIndexFiltersApplication({buildWildcardIndexEntry(BSON("$**" << 1), wcProj, "$**_1"),
                                   buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
                                   buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
                                   buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
                                  SimpleBSONObjComparator::kInstance.makeBSONObjSet(),
                                  {"$**_1"},
                                  {"$**_1"});
}

TEST_F(QueryPlannerParamsTest, GetAllowedPathSpecifiedWildcardIndicesByKey) {
    auto wcProj = createProjectionExecutor(
        fromjson("{_id: 0}"),
        {ProjectionPolicies::DefaultIdPolicy::kExcludeId,
         ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays});
    assertIndexFiltersApplication(
        {buildWildcardIndexEntry(BSON("a.$**" << 1), wcProj, "a.$**_1"),
         buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
         buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
         buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
        SimpleBSONObjComparator::kInstance.makeBSONObjSet({BSON("a.$**" << 1)}),
        stdx::unordered_set<std::string>{},
        {"a.$**_1"});
}

TEST_F(QueryPlannerParamsTest, GetAllowedPathSpecifiedWildcardIndicesByName) {
    auto wcProj = createProjectionExecutor(
        fromjson("{_id: 0}"),
        {ProjectionPolicies::DefaultIdPolicy::kExcludeId,
         ProjectionPolicies::ArrayRecursionPolicy::kDoNotRecurseNestedArrays});
    assertIndexFiltersApplication({buildWildcardIndexEntry(BSON("a.$**" << 1), wcProj, "a.$**_1"),
                                   buildSimpleIndexEntry(fromjson("{a: 1}"), "a_1"),
                                   buildSimpleIndexEntry(fromjson("{a: 1, b: 1}"), "a_1_b_1"),
                                   buildSimpleIndexEntry(fromjson("{a: 1, c: 1}"), "a_1_c_1")},
                                  SimpleBSONObjComparator::kInstance.makeBSONObjSet(),
                                  {"a.$**_1"},
                                  {"a.$**_1"});
}

TEST_F(QueryPlannerParamsTest, isComponentOfPathMultikeyNoMetadata) {
    BSONObj indexKey = BSON("a" << 1 << "b.c" << -1);
    MultikeyPaths multikeyInfo = {};

    ASSERT_TRUE(isAnyComponentOfPathOrProjectionMultikey(indexKey, true, multikeyInfo, "a"));
    ASSERT_TRUE(isAnyComponentOfPathOrProjectionMultikey(indexKey, true, multikeyInfo, "b.c"));

    ASSERT_FALSE(isAnyComponentOfPathOrProjectionMultikey(indexKey, false, multikeyInfo, "a"));
    ASSERT_FALSE(isAnyComponentOfPathOrProjectionMultikey(indexKey, false, multikeyInfo, "b.c"));
}

TEST_F(QueryPlannerParamsTest, isComponentOfPathMultikeyWithMetadata) {
    BSONObj indexKey = BSON("a" << 1 << "b.c" << -1);
    MultikeyPaths multikeyInfo = {{}, {1}};

    ASSERT_FALSE(isAnyComponentOfPathOrProjectionMultikey(indexKey, true, multikeyInfo, "a"));
    ASSERT_TRUE(isAnyComponentOfPathOrProjectionMultikey(indexKey, true, multikeyInfo, "b.c"));
}

TEST_F(QueryPlannerParamsTest, isComponentOfPathMultikeyWithEmptyMetadata) {
    BSONObj indexKey = BSON("a" << 1 << "b.c" << -1);

    MultikeyPaths multikeyInfoAllPathsScalar = {{}, {}};
    ASSERT_FALSE(
        isAnyComponentOfPathOrProjectionMultikey(indexKey, false, multikeyInfoAllPathsScalar, "a"));
    ASSERT_FALSE(isAnyComponentOfPathOrProjectionMultikey(
        indexKey, false, multikeyInfoAllPathsScalar, "b.c"));
}

TEST_F(QueryPlannerParamsTest, isComponentOfProjectionMultikeyWithMetadata) {
    BSONObj indexKey = BSON("a" << 1 << "b.c" << -1);
    MultikeyPaths multikeyInfo = {{}, {1}};

    ASSERT_FALSE(isAnyComponentOfPathOrProjectionMultikey(indexKey, true, multikeyInfo, "a"));

    OrderedPathSet projectionFields = {"b.c"};
    ASSERT_TRUE(isAnyComponentOfPathOrProjectionMultikey(
        indexKey, true, multikeyInfo, "a", projectionFields, false));
    ASSERT_FALSE(isAnyComponentOfPathOrProjectionMultikey(
        indexKey, true, multikeyInfo, "a", projectionFields, true));
    ASSERT_TRUE(isAnyComponentOfPathOrProjectionMultikey(
        indexKey, true, multikeyInfo, "b.c", projectionFields, false));
}

}  // namespace
}  // namespace mongo
