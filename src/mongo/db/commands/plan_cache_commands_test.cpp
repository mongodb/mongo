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

#include "mongo/db/commands/plan_cache_commands.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/query/plan_cache.h"
#include "mongo/db/query/query_test_service_context.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

static const NamespaceString nss{"test.collection"_sd};

TEST(PlanCacheCommandsTest, CannotCanonicalizeWithMissingQueryField) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    ASSERT_NOT_OK(
        plan_cache_commands::canonicalize(opCtx.get(), nss.ns(), fromjson("{}")).getStatus());
}

TEST(PlanCacheCommandsTest, CannotCanonicalizeWhenQueryFieldIsNotObject) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    ASSERT_NOT_OK(plan_cache_commands::canonicalize(opCtx.get(), nss.ns(), fromjson("{query: 1}"))
                      .getStatus());
}

TEST(PlanCacheCommandsTest, CannotCanonicalizeWhenSortFieldIsNotObject) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    ASSERT_NOT_OK(
        plan_cache_commands::canonicalize(opCtx.get(), nss.ns(), fromjson("{query: {}, sort: 1}"))
            .getStatus());
}

TEST(PlanCacheCommandsTest, CannotCanonicalizeWhenProjectionFieldIsNotObject) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    ASSERT_NOT_OK(plan_cache_commands::canonicalize(
                      opCtx.get(), nss.ns(), fromjson("{query: {}, projection: 1}"))
                      .getStatus());
}

TEST(PlanCacheCommandsTest, CannotCanonicalizeWhenCollationFieldIsNotObject) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    ASSERT_NOT_OK(plan_cache_commands::canonicalize(
                      opCtx.get(), nss.ns(), fromjson("{query: {}, collation: 1}"))
                      .getStatus());
}

TEST(PlanCacheCommandsTest, CannotCanonicalizeWhenSortObjectIsMalformed) {
    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    ASSERT_NOT_OK(plan_cache_commands::canonicalize(
                      opCtx.get(), nss.ns(), fromjson("{query: {}, sort: {a: 0}}"))
                      .getStatus());
}

TEST(PlanCacheCommandsTest, CanCanonicalizeWithValidQuery) {
    PlanCache planCache;

    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    auto statusWithCQ =
        plan_cache_commands::canonicalize(opCtx.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> query = std::move(statusWithCQ.getValue());

    // Equivalent query should generate same key.
    statusWithCQ =
        plan_cache_commands::canonicalize(opCtx.get(), nss.ns(), fromjson("{query: {b: 3, a: 4}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> equivQuery = std::move(statusWithCQ.getValue());
    ASSERT_EQUALS(planCache.computeKey(*query), planCache.computeKey(*equivQuery));
}

TEST(PlanCacheCommandsTest, SortQueryResultsInDifferentPlanCacheKeyFromUnsorted) {
    PlanCache planCache;

    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    auto statusWithCQ =
        plan_cache_commands::canonicalize(opCtx.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> query = std::move(statusWithCQ.getValue());

    // Sort query should generate different key from unsorted query.
    statusWithCQ = plan_cache_commands::canonicalize(
        opCtx.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}, sort: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> sortQuery = std::move(statusWithCQ.getValue());
    ASSERT_NOT_EQUALS(planCache.computeKey(*query), planCache.computeKey(*sortQuery));
}

// Regression test for SERVER-17158.
TEST(PlanCacheCommandsTest, SortsAreProperlyDelimitedInPlanCacheKey) {
    PlanCache planCache;

    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    auto statusWithCQ = plan_cache_commands::canonicalize(
        opCtx.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}, sort: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> sortQuery1 = std::move(statusWithCQ.getValue());

    // Confirm sort arguments are properly delimited (SERVER-17158)
    statusWithCQ = plan_cache_commands::canonicalize(
        opCtx.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}, sort: {aab: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> sortQuery2 = std::move(statusWithCQ.getValue());
    ASSERT_NOT_EQUALS(planCache.computeKey(*sortQuery1), planCache.computeKey(*sortQuery2));
}

TEST(PlanCacheCommandsTest, ProjectQueryResultsInDifferentPlanCacheKeyFromUnprojected) {
    PlanCache planCache;

    QueryTestServiceContext serviceContext;
    auto opCtx = serviceContext.makeOperationContext();
    auto statusWithCQ =
        plan_cache_commands::canonicalize(opCtx.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> query = std::move(statusWithCQ.getValue());

    statusWithCQ = plan_cache_commands::canonicalize(
        opCtx.get(), nss.ns(), fromjson("{query: {a: 1, b: 1}, projection: {_id: 0, a: 1}}"));
    ASSERT_OK(statusWithCQ.getStatus());
    std::unique_ptr<CanonicalQuery> projectionQuery = std::move(statusWithCQ.getValue());
    ASSERT_NOT_EQUALS(planCache.computeKey(*query), planCache.computeKey(*projectionQuery));
}

}  // namespace
}  // namespace mongo
