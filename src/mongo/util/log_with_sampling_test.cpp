/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kDefault

#include "mongo/platform/basic.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/util/log_global_settings.h"
#include "mongo/util/log_with_sampling.h"

namespace mongo {
namespace {


TEST(LogWithSamplingTest, ShouldLogCorrectlyWhenSampleRateIsSet) {
    const auto serviceContext = ServiceContext::make();
    const auto client = serviceContext->makeClient("log_with_sampling_test");
    const auto opCtx = client->makeOperationContext();

    const auto slowOpThresholdMS = Milliseconds(10);

    const auto originalSampleRate = serverGlobalParams.sampleRate;
    // Set sample rate to always profile a slow operation.
    serverGlobalParams.sampleRate = 1;

    // Reset sample rate to the original value after the test exits.
    ON_BLOCK_EXIT([originalSampleRate] { serverGlobalParams.sampleRate = originalSampleRate; });

    // Set the op duration to be greater than slowOpThreshold so that the op is considered slow.
    const auto slowOpDurationMS = Milliseconds(11);
    // Set verbosity level of operation component to info so that it doesn't log due to the log
    // level.
    setMinimumLoggedSeverity(MONGO_LOG_DEFAULT_COMPONENT, logger::LogSeverity::Info());

    bool shouldLogSlowOp, shouldSample;
    std::tie(shouldLogSlowOp, shouldSample) = shouldLogSlowOpWithSampling(
        opCtx.get(), MONGO_LOG_DEFAULT_COMPONENT, slowOpDurationMS, slowOpThresholdMS);

    // Verify that shouldLogSlowOp is true when the sampleRate is 1.
    ASSERT_TRUE(shouldLogSlowOp);

    // Verify that shouldSample is true when the sampleRate is 1.
    ASSERT_TRUE(shouldSample);

    // Set sample rate to never profile a slow operation.
    serverGlobalParams.sampleRate = 0;

    std::tie(shouldLogSlowOp, shouldSample) = shouldLogSlowOpWithSampling(
        opCtx.get(), MONGO_LOG_DEFAULT_COMPONENT, slowOpDurationMS, slowOpThresholdMS);

    // Verify that shouldLogSlowOp is false when the sampleRate is 0.
    ASSERT_FALSE(shouldLogSlowOp);

    // Verify that shouldSample is false when the sampleRate is 0.
    ASSERT_FALSE(shouldSample);
}

TEST(LogWithSamplingTest, ShouldAlwaysLogsWithVerbosityLevelDebug) {
    const auto serviceContext = ServiceContext::make();
    const auto client = serviceContext->makeClient("log_with_sampling_test");
    const auto opCtx = client->makeOperationContext();

    const auto slowOpThresholdMS = Milliseconds(10);

    const auto originalSampleRate = serverGlobalParams.sampleRate;
    // Set sample rate to always profile a slow operation.
    serverGlobalParams.sampleRate = 1;

    // Reset sample rate to the original value after the test exits.
    ON_BLOCK_EXIT([originalSampleRate] { serverGlobalParams.sampleRate = originalSampleRate; });

    // Set the op duration to be greater than slowMS so that the op is considered slow.
    const auto slowOpDurationMS = Milliseconds(11);
    // Set verbosity level of operation component to debug so that it should always log.
    setMinimumLoggedSeverity(MONGO_LOG_DEFAULT_COMPONENT, logger::LogSeverity::Debug(1));

    bool shouldLogSlowOp, shouldSample;
    std::tie(shouldLogSlowOp, shouldSample) = shouldLogSlowOpWithSampling(
        opCtx.get(), MONGO_LOG_DEFAULT_COMPONENT, slowOpDurationMS, slowOpThresholdMS);

    // Verify that shouldLogSlowOp is true when the op is slow.
    ASSERT_TRUE(shouldLogSlowOp);

    // Verify that shouldSample should be true when the sampleRate is 1.
    ASSERT_TRUE(shouldSample);

    // Set sample rate to never profile a slow operation.
    serverGlobalParams.sampleRate = 0;

    std::tie(shouldLogSlowOp, shouldSample) = shouldLogSlowOpWithSampling(
        opCtx.get(), MONGO_LOG_DEFAULT_COMPONENT, slowOpDurationMS, slowOpThresholdMS);

    // Verify that we should still log even when the sampleRate is 0.
    ASSERT_TRUE(shouldLogSlowOp);

    // Verify that shouldSample should be false even if shouldLogSlowOp is true.
    ASSERT_FALSE(shouldSample);
}

TEST(LogWithSamplingTest, ShouldNotLogFastOp) {
    const auto serviceContext = ServiceContext::make();
    const auto client = serviceContext->makeClient("log_with_sampling_test");
    const auto opCtx = client->makeOperationContext();

    const auto slowOpThresholdMS = Milliseconds(10);

    const auto originalSampleRate = serverGlobalParams.sampleRate;
    // Set sample rate to always profile a slow operation.
    serverGlobalParams.sampleRate = 1;

    // Reset sample rate to the original value after the test exits.
    ON_BLOCK_EXIT([originalSampleRate] { serverGlobalParams.sampleRate = originalSampleRate; });

    // Set the op duration to be less than than slowOpThreshold so that the op is considered fast.
    const auto fastOpDurationMS = Milliseconds(9);
    // Set verbosity level of operation component to info so that it doesn't log due to the log
    // level.
    setMinimumLoggedSeverity(MONGO_LOG_DEFAULT_COMPONENT, logger::LogSeverity::Info());

    bool shouldLogSlowOp, shouldSample;
    std::tie(shouldLogSlowOp, shouldSample) = shouldLogSlowOpWithSampling(
        opCtx.get(), MONGO_LOG_DEFAULT_COMPONENT, fastOpDurationMS, slowOpThresholdMS);

    // Verify that shouldLogSlowOp is false when the op is fast.
    ASSERT_FALSE(shouldLogSlowOp);

    // Verify that shouldSample is true when the sampleRate is 1.
    ASSERT_TRUE(shouldSample);

    // Set sample rate to never profile a slow operation.
    serverGlobalParams.sampleRate = 0;

    std::tie(shouldLogSlowOp, shouldSample) = shouldLogSlowOpWithSampling(
        opCtx.get(), MONGO_LOG_DEFAULT_COMPONENT, fastOpDurationMS, slowOpThresholdMS);

    // Verify that we should still not log when the sampleRate is 0.
    ASSERT_FALSE(shouldLogSlowOp);

    // Verify that shouldSample is false when the sampleRate is 0.
    ASSERT_FALSE(shouldSample);
}

}  // namespace
}  // namespace mongo
