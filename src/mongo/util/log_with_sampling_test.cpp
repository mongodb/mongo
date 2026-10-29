// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/log_with_sampling.h"

#include "mongo/db/service_context.h"
#include "mongo/unittest/log_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <ostream>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

/**
 *  Get the relevant parts of the environment into a specified state, run
 *  shouldLogSlowOpWithSampling(...) once, then restore the enviroment's state.
 *
 * `debugLogEnabled`: Select whether to log debug messages.
 *     Unless we're logging debug messages, we should only log the op if it's slow.
 *
 * `slowOp`: whether to simulate a slow op or a fast op relative to the test's chosen
 *     threshold of 10 milliseconds.
 *
 * `forceSample`: Set the slow op sampleRate to 0% or 100% to force sampled or not sampled op.
 */
auto scenario(bool debugLogEnabled, bool slowOp, bool forceSample) {
    static const logv2::LogComponent component = logv2::LogComponent::kDefault;
    const auto serviceContext = ServiceContext::make();
    const auto client = serviceContext->getService()->makeClient("log_with_sampling_test");
    const auto opCtx = client->makeOperationContext();

    auto loggedSeverityGuard = unittest::MinimumLoggedSeverityGuard(
        component, debugLogEnabled ? logv2::LogSeverity::Debug(1) : logv2::LogSeverity::Info());

    ScopeGuard sampleRateGuard([savedRate = serverGlobalParams.sampleRate.load()] {
        serverGlobalParams.sampleRate.store(savedRate);
    });
    serverGlobalParams.sampleRate.store(forceSample ? 1.0 : 0.0);

    return shouldLogSlowOpWithSampling(
        opCtx.get(), component, Milliseconds{slowOp ? 11 : 9}, Milliseconds{10});
}

TEST(LogWithSamplingTest, SlowOpWithoutDebugLogging) {
    {
        auto [logSlow, sample] = scenario(false, true, true);
        ASSERT_TRUE(logSlow) << "should log when sampled";
        ASSERT_TRUE(sample) << "should sample when sampled";
    }
    {
        auto [logSlow, sample] = scenario(false, true, false);
        ASSERT_FALSE(logSlow) << "should not log when not sampled";
        ASSERT_FALSE(sample) << "should not sample when not sampled";
    }
}

TEST(LogWithSamplingTest, SlowOpWithDebugLogging) {
    {
        auto [logSlow, sample] = scenario(true, true, true);
        ASSERT_TRUE(logSlow) << "should log when the op is slow.";
        ASSERT_TRUE(sample) << "should sample when sampled";
    }
    {
        auto [logSlow, sample] = scenario(true, true, false);
        ASSERT_TRUE(logSlow) << "should still log even when not sampled";
        ASSERT_FALSE(sample) << "should not sample even if should log";
    }
}

TEST(LogWithSamplingTest, FastOpWithoutDebugLogging) {
    {
        auto [logSlow, sample] = scenario(false, false, true);
        ASSERT_FALSE(logSlow) << "should not log when the op is fast.";
        ASSERT_TRUE(sample) << "should sample when sampled";
    }
    {
        auto [logSlow, sample] = scenario(false, false, false);
        ASSERT_FALSE(logSlow) << "should not log when not sampled";
        ASSERT_FALSE(sample) << "should not sample when not sampled";
    }
}

}  // namespace
}  // namespace mongo
