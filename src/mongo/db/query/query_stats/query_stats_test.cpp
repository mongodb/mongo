/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include "mongo/db/query/query_stats/query_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/local_catalog/collection_type.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/idl/server_parameter_test_controller.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQueryStats

namespace mongo::query_stats {
class QueryStatsTest : public ServiceContextTest {};

TEST_F(QueryStatsTest, TwoRegisterRequestsWithSameOpCtxRateLimitedFirstCall) {
    // This test simulates what happens with queries over views where two calls to registerRequest()
    // can be made with the same opCtx.

    // Make query for query stats.
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.testColl");
    FindCommandRequest fcr((NamespaceStringOrUUID(nss)));
    fcr.setFilter(BSONObj());

    auto fcrCopy = std::make_unique<FindCommandRequest>(fcr);
    auto opCtx = makeOperationContext();
    auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *fcrCopy).build();
    auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcrCopy)}));
    query_shape::FindCmdShape findShape(*parsedFind, expCtx);

    RAIIServerParameterControllerForTest controller("featureFlagQueryStats", true);
    auto& opDebug = CurOp::get(*opCtx)->debug();
    ASSERT_EQ(opDebug.queryStatsInfo.disableForSubqueryExecution, false);

    // First call to registerRequest() should be rate limited.
    auto& limiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
    limiter.configureWindowBased(0);
    ASSERT_DOES_NOT_THROW(query_stats::registerRequest(opCtx.get(), nss, [&]() {
        return std::make_unique<query_stats::FindKey>(
            expCtx,
            *parsedFind->findCommandRequest,
            std::make_unique<query_shape::FindCmdShape>(findShape),
            query_shape::CollectionType::kCollection);
    }));

    // Since the query was rate limited, no key should have been created.
    ASSERT(opDebug.queryStatsInfo.key == nullptr);
    ASSERT_EQ(opDebug.queryStatsInfo.disableForSubqueryExecution, true);

    // Second call should not be rate limited.
    QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext())
        .configureWindowBased(INT_MAX);

    ASSERT_DOES_NOT_THROW(query_stats::registerRequest(opCtx.get(), nss, [&]() {
        return std::make_unique<query_stats::FindKey>(
            expCtx,
            *parsedFind->findCommandRequest,
            std::make_unique<query_shape::FindCmdShape>(findShape),
            query_shape::CollectionType::kCollection);
    }));

    // queryStatsKey should not be created for previously rate limited query.
    ASSERT(opDebug.queryStatsInfo.key == nullptr);
    ASSERT_EQ(opDebug.queryStatsInfo.disableForSubqueryExecution, true);
    ASSERT_FALSE(opDebug.queryStatsInfo.keyHash.has_value());
}

TEST_F(QueryStatsTest, TwoRegisterRequestsWithSameOpCtxDisabledBetween) {
    // This test simulates an observed bug where an opCtx is used for two requests, and between the
    // first and the second the query stats store is emptied/disabled.

    // Make query for query stats.
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.testColl");
    FindCommandRequest fcr((NamespaceStringOrUUID(nss)));
    fcr.setFilter(BSONObj());

    auto serviceCtx = getServiceContext();
    auto opCtx = makeOperationContext();

    auto& opDebug = CurOp::get(*opCtx)->debug();
    ASSERT(opDebug.queryStatsInfo.key == nullptr);
    ASSERT_FALSE(opDebug.queryStatsInfo.keyHash.has_value());
    QueryStatsStoreManager::get(serviceCtx) =
        std::make_unique<QueryStatsStoreManager>(16 * 1024 * 1024, 1);

    auto& limiter = QueryStatsStoreManager::getRateLimiter(serviceCtx);
    limiter.configureWindowBased(-1);

    {
        auto fcrCopy = std::make_unique<FindCommandRequest>(fcr);
        auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *fcrCopy).build();
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcrCopy)}));
        auto findShape = std::make_unique<query_shape::FindCmdShape>(*parsedFind, expCtx);
        ASSERT_DOES_NOT_THROW(query_stats::registerRequest(opCtx.get(), nss, [&]() {
            return std::make_unique<query_stats::FindKey>(expCtx,
                                                          *parsedFind->findCommandRequest,
                                                          std::move(findShape),
                                                          query_shape::CollectionType::kCollection);
        }));

        ASSERT(opDebug.queryStatsInfo.key != nullptr);
        ASSERT(opDebug.queryStatsInfo.keyHash.has_value());

        ASSERT_DOES_NOT_THROW(query_stats::writeQueryStats(opCtx.get(),
                                                           opDebug.queryStatsInfo.keyHash,
                                                           std::move(opDebug.queryStatsInfo.key),
                                                           QueryStatsSnapshot{}));
    }

    // Second call should see that query stats are now disabled.
    {
        // To reproduce SERVER-84730 we need to clear out the query stats store so that writing the
        // stats at the end will attempt to insert a new entry.
        QueryStatsStoreManager::get(serviceCtx)->resetSize(0);

        auto fcrCopy = std::make_unique<FindCommandRequest>(fcr);
        fcrCopy->setFilter(BSON("x" << 1));
        fcrCopy->setSort(BSON("x" << 1));
        auto expCtx = ExpressionContextBuilder{}.fromRequest(opCtx.get(), *fcrCopy).build();
        auto parsedFind = uassertStatusOK(parsed_find_command::parse(expCtx, {std::move(fcrCopy)}));
        auto findShape = std::make_unique<query_shape::FindCmdShape>(*parsedFind, expCtx);

        ASSERT_DOES_NOT_THROW(query_stats::registerRequest(opCtx.get(), nss, [&]() {
            return std::make_unique<query_stats::FindKey>(expCtx,
                                                          *parsedFind->findCommandRequest,
                                                          std::move(findShape),
                                                          query_shape::CollectionType::kCollection);
        }));

        // queryStatsKey should not be created since we have a size budget of 0.
        ASSERT(opDebug.queryStatsInfo.key == nullptr);
        // Query stats are disabled by a lack of space, not by being a on a subquery path.
        ASSERT_EQ(opDebug.queryStatsInfo.disableForSubqueryExecution, false);

        // Interestingly, we purposefully leave the hash value around on the OperationContext after
        // the previous operation finishes. This is because we think it may have value in being
        // logged in the future, even after query stats have been written. Excepting obscure
        // internal use-cases, most OperationContexts will die shortly after the query stats are
        // written, so this isn't expected to be a large issue.
        ASSERT(opDebug.queryStatsInfo.keyHash.has_value());

        QueryStatsStoreManager::get(serviceCtx)->resetSize(16 * 1024 * 1024);
        // SERVER-84730 this assertion used to throw since there is no key, but there is a hash.
        ASSERT_DOES_NOT_THROW(query_stats::writeQueryStats(opCtx.get(),
                                                           opDebug.queryStatsInfo.keyHash,
                                                           std::move(opDebug.queryStatsInfo.key),
                                                           QueryStatsSnapshot{}));
    }
}

TEST_F(QueryStatsTest, RegisterRequestAbsorbsErrors) {
    const NamespaceString nss = NamespaceString::createNamespaceString_forTest("testDB.testColl");

    auto opCtx = makeOperationContext();
    auto& opDebug = CurOp::get(*opCtx)->debug();

    auto& limiter = QueryStatsStoreManager::getRateLimiter(getServiceContext());
    limiter.configureWindowBased(-1);

    // First case - don't treat errors as fatal.
    internalQueryStatsErrorsAreCommandFatal.store(false);

    ASSERT_DOES_NOT_THROW(query_stats::registerRequest(opCtx.get(), nss, [&]() {
        uasserted(ErrorCodes::BSONObjectTooLarge, "size error");
        return nullptr;
    }));

    // Skip this check for debug builds because errors are always fatal in that environment.
    if (!kDebugBuild) {
        opDebug.queryStatsInfo = OpDebug::QueryStatsInfo{};
        ASSERT_DOES_NOT_THROW(query_stats::registerRequest(opCtx.get(), nss, [&]() {
            uasserted(ErrorCodes::BadValue, "fake error");
            return nullptr;
        }));
    }

    // Now make sure that errors are propagated when the knob is set.
    internalQueryStatsErrorsAreCommandFatal.store(true);

    // We shouldn't propagate 'BSONObjectTooLarge' errors under any circumstances.
    opDebug.queryStatsInfo = OpDebug::QueryStatsInfo{};
    ASSERT_DOES_NOT_THROW(query_stats::registerRequest(opCtx.get(), nss, [&]() {
        uasserted(ErrorCodes::BSONObjectTooLarge, "size error");
        return nullptr;
    }));

    // This should hit our tripwire assertion.
    opDebug.queryStatsInfo = OpDebug::QueryStatsInfo{};
    ASSERT_THROWS_CODE(query_stats::registerRequest(opCtx.get(),
                                                    nss,
                                                    [&]() {
                                                        uasserted(ErrorCodes::BadValue,
                                                                  "fake error");
                                                        return nullptr;
                                                    }),
                       DBException,
                       ErrorCodes::QueryStatsFailedToRecord);
}

TEST_F(QueryStatsTest, TestConfiguringQueryStatsViaServerParameters) {
    auto opCtx = makeOperationContext();

    {
        RAIIServerParameterControllerForTest flagCtrl("featureFlagQueryStats", true);
        RAIIServerParameterControllerForTest sampleRateCtrl("internalQueryStatsSampleRate", 0.042);
        auto& rateLimiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
        ASSERT_EQ(rateLimiter.getPolicyType(), RateLimiter::kSampleBasedPolicy);
        ASSERT_EQ(rateLimiter.getSamplingRate(), 42);
    }

    {  // Test that window-based rate limiting will be elected when sampling rate is set to 0.0
        RAIIServerParameterControllerForTest flagCtrl("featureFlagQueryStats", true);
        RAIIServerParameterControllerForTest rateLimitCtrl("internalQueryStatsRateLimit", 10);
        RAIIServerParameterControllerForTest sampleRateCtrl("internalQueryStatsSampleRate", 0.0);

        auto& rateLimiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
        ASSERT_EQ(rateLimiter.getPolicyType(), RateLimiter::kWindowBasedPolicy);
        ASSERT_EQ(rateLimiter.getSamplingRate(), 10);
    }

    {  // Test that sampling-based rate limiting takes precedence over window-based policy when both
       // are enabled.
        RAIIServerParameterControllerForTest flagCtrl("featureFlagQueryStats", true);
        RAIIServerParameterControllerForTest rateLimitCtrl("internalQueryStatsRateLimit", 10);
        RAIIServerParameterControllerForTest sampleRateCtrl("internalQueryStatsSampleRate", 0.042);

        auto& rateLimiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
        ASSERT_EQ(rateLimiter.getPolicyType(), RateLimiter::kSampleBasedPolicy);
        ASSERT_EQ(rateLimiter.getSamplingRate(), 42);
    }

    {  // Test idempotency when both parameters are set but being set in different order.
        RAIIServerParameterControllerForTest flagCtrl("featureFlagQueryStats", true);
        RAIIServerParameterControllerForTest sampleRateCtrl("internalQueryStatsSampleRate", 0.042);
        RAIIServerParameterControllerForTest rateLimitCtrl("internalQueryStatsRateLimit", 10);

        auto& rateLimiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
        ASSERT_EQ(rateLimiter.getPolicyType(), RateLimiter::kSampleBasedPolicy);
        ASSERT_EQ(rateLimiter.getSamplingRate(), 42);
    }

    {  // Test that query stats is disabled when both rate limit and sample rate are set to 0.
        RAIIServerParameterControllerForTest flagCtrl("featureFlagQueryStats", true);
        RAIIServerParameterControllerForTest rateLimitCtrl("internalQueryStatsRateLimit", 0.0);
        RAIIServerParameterControllerForTest sampleRateCtrl("internalQueryStatsSampleRate", 0.0);

        auto& rateLimiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
        ASSERT_EQ(rateLimiter.getPolicyType(), RateLimiter::kWindowBasedPolicy);
        ASSERT_EQ(rateLimiter.getSamplingRate(), 0);
        ASSERT_FALSE(rateLimiter.handle());
    }
}

}  // namespace mongo::query_stats
