// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/s/load_balancer_support.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/fail_point.h"

#include <memory>
#include <ostream>
#include <string>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo {
namespace {

using namespace unittest::match;

auto HasServiceId() {
    return Truly([](auto&& arg) { return arg.hasElement("serviceId"); });
}

class LoadBalancerSupportTest : public ServiceContextTest {
public:
    LoadBalancerSupportTest() = default;

    using ServiceContextTest::ServiceContextTest;

    BSONObj doHello(bool lbOption) {
        BSONObjBuilder bob;
        load_balancer_support::handleHello(&*makeOperationContext(), &bob, lbOption);
        return bob.obj();
    }

    FailPointEnableBlock simulateLoadBalancerConnection() const {
        return FailPointEnableBlock("loadBalancerSupportClientIsFromLoadBalancerPort");
    }
};

TEST_F(LoadBalancerSupportTest, HelloNormalClientNoOption) {
    ASSERT_THAT(doHello(false), Not(HasServiceId()));
}

TEST_F(LoadBalancerSupportTest, HelloNormalClientGivesOption) {
    ASSERT_THAT(doHello(true), Not(HasServiceId()));
}

TEST_F(LoadBalancerSupportTest, HelloLoadBalancedClientNoOption) {
    auto simLB = simulateLoadBalancerConnection();
    ASSERT_THAT(doHello(false), Not(HasServiceId()));
}

TEST_F(LoadBalancerSupportTest, HelloLoadBalancedClientGivesOption) {
    auto simLB = simulateLoadBalancerConnection();
    ASSERT_THAT(doHello(true), HasServiceId());
    ASSERT_THAT(doHello(true), Not(HasServiceId())) << "only first hello is special";
}

}  // namespace
}  // namespace mongo
