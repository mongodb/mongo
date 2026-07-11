// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/process_health/fault_manager.h"

#include "mongo/db/process_health/fault_manager_test_suite.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace process_health {

using test::FaultManagerTest;

namespace {

TEST(SimpleFaultManagerTest, Registration) {
    auto serviceCtx = ServiceContext::make();
    ASSERT_TRUE(FaultManager::get(serviceCtx.get()));
}

// Tests the default health observer intensity of non-critical
TEST_F(FaultManagerTest, GetHealthObserverIntensity) {
    auto& config = manager().getConfig();
    ASSERT(config.getHealthObserverIntensity(FaultFacetType::kLdap) ==
           HealthObserverIntensityEnum::kOff);
    ASSERT(config.getHealthObserverIntensity(FaultFacetType::kDns) ==
           HealthObserverIntensityEnum::kOff);
}

}  // namespace
}  // namespace process_health
}  // namespace mongo
