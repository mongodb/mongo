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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

#include "mongo/platform/basic.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/log_test.h"
#include "mongo/util/log_with_sampling.h"
#include "mongo/util/scopeguard.h"

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
    const auto client = serviceContext->makeClient("log_with_sampling_test");
    const auto opCtx = client->makeOperationContext();

    auto loggedSeverityGuard = unittest::MinimumLoggedSeverityGuard(
        component, debugLogEnabled ? logv2::LogSeverity::Debug(1) : logv2::LogSeverity::Info());

    auto sampleRateGuard = makeGuard(
        [savedRate = serverGlobalParams.sampleRate] { serverGlobalParams.sampleRate = savedRate; });
    serverGlobalParams.sampleRate = forceSample ? 1.0 : 0.0;

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
