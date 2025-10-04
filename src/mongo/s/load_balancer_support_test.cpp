/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/s/load_balancer_support.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
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

class LoadBalancerSupportTest : public ServiceContextTest {
public:
    LoadBalancerSupportTest() = default;

    using ServiceContextTest::ServiceContextTest;

    BSONObj doHello(bool lbOption) {
        BSONObjBuilder bob;
        load_balancer_support::handleHello(&*makeOperationContext(), &bob, lbOption);
        return bob.obj();
    }

    struct HasServiceId : Matcher {
        std::string describe() const {
            return "HasServiceId";
        }
        MatchResult match(BSONObj obj) const {
            bool ok = obj.hasElement("serviceId");
            std::string msg;
            if (!ok)
                msg = tojson(obj);
            return {ok, msg};
        }
    };

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
