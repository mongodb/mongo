// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/query/query_stats/query_stats.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/db/query/find_command.h"
#include "mongo/db/query/parsed_find_command.h"
#include "mongo/db/query/query_stats/find_key.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/shard_role/shard_catalog/collection_type.h"
#include "mongo/unittest/server_parameter_guard.h"
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

    unittest::ServerParameterGuard controller("featureFlagQueryStats", true);
    auto& opDebug = CurOp::get(*opCtx)->debug();
    ASSERT_EQ(opDebug.getQueryStatsInfo().disableForSubqueryExecution, false);

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
    ASSERT(opDebug.getQueryStatsInfo().key == nullptr);
    ASSERT_EQ(opDebug.getQueryStatsInfo().disableForSubqueryExecution, true);

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
    ASSERT(opDebug.getQueryStatsInfo().key == nullptr);
    ASSERT_EQ(opDebug.getQueryStatsInfo().disableForSubqueryExecution, true);
    ASSERT_FALSE(opDebug.getQueryStatsInfo().keyHash.has_value());
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
    ASSERT(opDebug.getQueryStatsInfo().key == nullptr);
    ASSERT_FALSE(opDebug.getQueryStatsInfo().keyHash.has_value());
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

        ASSERT(opDebug.getQueryStatsInfo().key != nullptr);
        ASSERT(opDebug.getQueryStatsInfo().keyHash.has_value());

        ASSERT_DOES_NOT_THROW(
            query_stats::writeQueryStats(opCtx.get(),
                                         opDebug.getQueryStatsInfo().keyHash,
                                         std::move(opDebug.getQueryStatsInfo().key),
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
        ASSERT(opDebug.getQueryStatsInfo().key == nullptr);
        // Query stats are disabled by a lack of space, not by being a on a subquery path.
        ASSERT_EQ(opDebug.getQueryStatsInfo().disableForSubqueryExecution, false);

        // Interestingly, we purposefully leave the hash value around on the OperationContext after
        // the previous operation finishes. This is because we think it may have value in being
        // logged in the future, even after query stats have been written. Excepting obscure
        // internal use-cases, most OperationContexts will die shortly after the query stats are
        // written, so this isn't expected to be a large issue.
        ASSERT(opDebug.getQueryStatsInfo().keyHash.has_value());

        QueryStatsStoreManager::get(serviceCtx)->resetSize(16 * 1024 * 1024);
        // SERVER-84730 this assertion used to throw since there is no key, but there is a hash.
        ASSERT_DOES_NOT_THROW(
            query_stats::writeQueryStats(opCtx.get(),
                                         opDebug.getQueryStatsInfo().keyHash,
                                         std::move(opDebug.getQueryStatsInfo().key),
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
        opDebug.getQueryStatsInfo() = OpDebug::QueryStatsInfo{};
        ASSERT_DOES_NOT_THROW(query_stats::registerRequest(opCtx.get(), nss, [&]() {
            uasserted(ErrorCodes::BadValue, "fake error");
            return nullptr;
        }));
    }

    // Now make sure that errors are propagated when the knob is set.
    internalQueryStatsErrorsAreCommandFatal.store(true);

    // We shouldn't propagate 'BSONObjectTooLarge' errors under any circumstances.
    opDebug.getQueryStatsInfo() = OpDebug::QueryStatsInfo{};
    ASSERT_DOES_NOT_THROW(query_stats::registerRequest(opCtx.get(), nss, [&]() {
        uasserted(ErrorCodes::BSONObjectTooLarge, "size error");
        return nullptr;
    }));

    // This should hit our tripwire assertion.
    opDebug.getQueryStatsInfo() = OpDebug::QueryStatsInfo{};
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
        unittest::ServerParameterGuard flagCtrl("featureFlagQueryStats", true);
        unittest::ServerParameterGuard sampleRateCtrl("internalQueryStatsSampleRate", 0.042);
        auto& rateLimiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
        ASSERT_EQ(rateLimiter.getPolicyType(), RateLimiter::kSampleBasedPolicy);
        ASSERT_EQ(rateLimiter.getSamplingRate(), 42);
    }

    {  // Test that window-based rate limiting will be elected when sampling rate is set to 0.0
        unittest::ServerParameterGuard flagCtrl("featureFlagQueryStats", true);
        unittest::ServerParameterGuard rateLimitCtrl("internalQueryStatsRateLimit", 10);
        unittest::ServerParameterGuard sampleRateCtrl("internalQueryStatsSampleRate", 0.0);

        auto& rateLimiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
        ASSERT_EQ(rateLimiter.getPolicyType(), RateLimiter::kWindowBasedPolicy);
        ASSERT_EQ(rateLimiter.getSamplingRate(), 10);
    }

    {  // Test that sampling-based rate limiting takes precedence over window-based policy when both
       // are enabled.
        unittest::ServerParameterGuard flagCtrl("featureFlagQueryStats", true);
        unittest::ServerParameterGuard rateLimitCtrl("internalQueryStatsRateLimit", 10);
        unittest::ServerParameterGuard sampleRateCtrl("internalQueryStatsSampleRate", 0.042);

        auto& rateLimiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
        ASSERT_EQ(rateLimiter.getPolicyType(), RateLimiter::kSampleBasedPolicy);
        ASSERT_EQ(rateLimiter.getSamplingRate(), 42);
    }

    {  // Test idempotency when both parameters are set but being set in different order.
        unittest::ServerParameterGuard flagCtrl("featureFlagQueryStats", true);
        unittest::ServerParameterGuard sampleRateCtrl("internalQueryStatsSampleRate", 0.042);
        unittest::ServerParameterGuard rateLimitCtrl("internalQueryStatsRateLimit", 10);

        auto& rateLimiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
        ASSERT_EQ(rateLimiter.getPolicyType(), RateLimiter::kSampleBasedPolicy);
        ASSERT_EQ(rateLimiter.getSamplingRate(), 42);
    }

    {  // Test that query stats is disabled when both rate limit and sample rate are set to 0.
        unittest::ServerParameterGuard flagCtrl("featureFlagQueryStats", true);
        unittest::ServerParameterGuard rateLimitCtrl("internalQueryStatsRateLimit", 0.0);
        unittest::ServerParameterGuard sampleRateCtrl("internalQueryStatsSampleRate", 0.0);

        auto& rateLimiter = QueryStatsStoreManager::getRateLimiter(opCtx->getServiceContext());
        ASSERT_EQ(rateLimiter.getPolicyType(), RateLimiter::kWindowBasedPolicy);
        ASSERT_EQ(rateLimiter.getSamplingRate(), 0);
        ASSERT_FALSE(rateLimiter.handle());
    }
}

TEST_F(QueryStatsTest, TestConfiguringWriteCmdRateLimiterViaServerParameters) {
    auto opCtx = makeOperationContext();
    auto serviceCtx = opCtx->getServiceContext();

    {
        unittest::ServerParameterGuard flagCtrl("featureFlagQueryStats", true);
        unittest::ServerParameterGuard sampleRateCtrl("internalQueryStatsWriteCmdSampleRate",
                                                      0.042);

        auto& limiter = QueryStatsStoreManager::getWriteCmdRateLimiter(serviceCtx);
        ASSERT_EQ(limiter.getPolicyType(), RateLimiter::kSampleBasedPolicy);
        ASSERT_EQ(limiter.getSamplingRate(), 42);
    }

    {  // Full sampling rate of 1.0 should yield a per-thousand rate of 1000.
        unittest::ServerParameterGuard flagCtrl("featureFlagQueryStats", true);
        unittest::ServerParameterGuard sampleRateCtrl("internalQueryStatsWriteCmdSampleRate", 1.0);

        auto& limiter = QueryStatsStoreManager::getWriteCmdRateLimiter(serviceCtx);
        ASSERT_EQ(limiter.getPolicyType(), RateLimiter::kSampleBasedPolicy);
        ASSERT_EQ(limiter.getSamplingRate(), 1000);
    }

    {  // A rate of 0.0 should disable write command sampling.
        unittest::ServerParameterGuard flagCtrl("featureFlagQueryStats", true);
        unittest::ServerParameterGuard sampleRateCtrl("internalQueryStatsWriteCmdSampleRate", 0.0);

        auto& limiter = QueryStatsStoreManager::getWriteCmdRateLimiter(serviceCtx);
        ASSERT_EQ(limiter.getPolicyType(), RateLimiter::kSampleBasedPolicy);
        ASSERT_EQ(limiter.getSamplingRate(), 0);
    }
}

}  // namespace mongo::query_stats
