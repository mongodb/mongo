// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/sharding_task_executor_pool_controller.h"

#include "mongo/unittest/server_parameter_guard.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"

#include <memory>

namespace mongo {
namespace {

using Controller = ShardingTaskExecutorPoolController;
using unittest::match::Eq;
using unittest::match::Field;

MATCHER_P(HasMaxPendingConnections, expected, "") {
    return ExplainMatchResult(Field("maxPendingConnections",
                                    &Controller::ConnectionControls::maxPendingConnections,
                                    Eq(expected)),
                              arg,
                              result_listener);
}

class ShardingTaskExecutorPoolControllerTest : public unittest::Test {
protected:
    static constexpr Controller::PoolId kPoolId = 1;

    void setUp() override {
        const HostAndPort host{"localhost", 27017};
        _fixed.addHost(kPoolId, host);
        _arbitrary.addHost(kPoolId, host);
    }

    Controller _fixed{std::weak_ptr<ShardRegistry>{}, Controller::PoolType::kFixed};
    Controller _arbitrary{std::weak_ptr<ShardRegistry>{}, Controller::PoolType::kArbitrary};
};

TEST_F(ShardingTaskExecutorPoolControllerTest, FixedMaxConnectingUpdatesDoNotAffectArbitraryPools) {
    unittest::ServerParameterGuard arbitraryMaxConnecting{"ShardingTaskExecutorPoolMaxConnecting",
                                                          3};
    unittest::ServerParameterGuard fixedMaxConnecting{"ShardingFixedTaskExecutorPoolMaxConnecting",
                                                      7};
    EXPECT_THAT(_fixed.getControls(kPoolId), HasMaxPendingConnections(7));
    EXPECT_THAT(_arbitrary.getControls(kPoolId), HasMaxPendingConnections(3));

    unittest::ServerParameterGuard lowerFixedMaxConnecting{
        "ShardingFixedTaskExecutorPoolMaxConnecting", 1};
    EXPECT_THAT(_fixed.getControls(kPoolId), HasMaxPendingConnections(1));
    EXPECT_THAT(_arbitrary.getControls(kPoolId), HasMaxPendingConnections(3));
}

TEST_F(ShardingTaskExecutorPoolControllerTest, ArbitraryMaxConnectingUpdatesDoNotAffectFixedPools) {
    unittest::ServerParameterGuard arbitraryMaxConnecting{"ShardingTaskExecutorPoolMaxConnecting",
                                                          8};
    unittest::ServerParameterGuard fixedMaxConnecting{"ShardingFixedTaskExecutorPoolMaxConnecting",
                                                      5};
    EXPECT_THAT(_fixed.getControls(kPoolId), HasMaxPendingConnections(5));
    EXPECT_THAT(_arbitrary.getControls(kPoolId), HasMaxPendingConnections(8));

    unittest::ServerParameterGuard lowerArbitraryMaxConnecting{
        "ShardingTaskExecutorPoolMaxConnecting", 1};
    EXPECT_THAT(_fixed.getControls(kPoolId), HasMaxPendingConnections(5));
    EXPECT_THAT(_arbitrary.getControls(kPoolId), HasMaxPendingConnections(1));
}

}  // namespace
}  // namespace mongo
