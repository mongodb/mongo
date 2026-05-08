/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/db/admission/flow_control_rate_limiter.h"

#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

class FlowControlRateLimiterTest : public ServiceContextTest {
protected:
    void setUp() override {
        ServiceContextTest::setUp();
        _client = getServiceContext()->getService()->makeClient("test");
        _opCtx = _client->makeOperationContext();
    }

    ServiceContext::UniqueClient _client;
    ServiceContext::UniqueOperationContext _opCtx;
};

TEST_F(FlowControlRateLimiterTest, BasicConstruction) {
    FlowControlRateLimiter rl;
    ASSERT_EQUALS(rl.queued(), 0);
}

TEST_F(FlowControlRateLimiterTest, UpdateRateWithVariousValues) {
    FlowControlRateLimiter rl;

    rl.updateRate(0);  // Clamped to 1 internally.
    rl.updateRate(1);
    rl.updateRate(100);
    rl.updateRate(5000);
    rl.updateRate(1000000);
    rl.updateRate(FlowControlRateLimiter::kMaxRate);
}

TEST_F(FlowControlRateLimiterTest, AcquireTicketSucceeds) {
    FlowControlRateLimiter rl;
    rl.updateRate(1000000);

    FlowControlTicketholder::CurOp stats;
    rl.acquireTicket(_opCtx.get(), &stats);

    ASSERT_EQUALS(stats.ticketsAcquired, 1);
    ASSERT_FALSE(stats.waiting);
}

TEST_F(FlowControlRateLimiterTest, StatsIncrementOnMultipleAcquires) {
    FlowControlRateLimiter rl;
    rl.updateRate(1000000);

    FlowControlTicketholder::CurOp stats;
    for (int i = 0; i < 10; i++) {
        rl.acquireTicket(_opCtx.get(), &stats);
    }

    ASSERT_EQUALS(stats.ticketsAcquired, 10);
    ASSERT_GTE(rl.stats().successfulAdmissions.get(), 10);
}

TEST_F(FlowControlRateLimiterTest, ServiceContextDecoration) {
    auto* svc = getServiceContext();
    ASSERT_FALSE(FlowControlRateLimiter::get(svc));

    FlowControlRateLimiter::set(svc, std::make_unique<FlowControlRateLimiter>());
    ASSERT_TRUE(FlowControlRateLimiter::get(svc));

    auto* rl = FlowControlRateLimiter::get(svc);
    rl->updateRate(5000);

    FlowControlTicketholder::CurOp stats;
    rl->acquireTicket(_opCtx.get(), &stats);
    ASSERT_EQUALS(stats.ticketsAcquired, 1);
}

TEST_F(FlowControlRateLimiterTest, ServiceContextDecorationViaOpCtx) {
    auto* svc = getServiceContext();
    FlowControlRateLimiter::set(svc, std::make_unique<FlowControlRateLimiter>());

    auto* rl = FlowControlRateLimiter::get(_opCtx.get());
    ASSERT_TRUE(rl);
}

TEST_F(FlowControlRateLimiterTest, QueuedCountStartsAtZero) {
    FlowControlRateLimiter rl;
    ASSERT_EQUALS(rl.queued(), 0);
}

TEST_F(FlowControlRateLimiterTest, AppendStatsProducesBSON) {
    FlowControlRateLimiter rl;
    rl.updateRate(1000);

    BSONObjBuilder bob;
    rl.appendStats(&bob);
    auto obj = bob.obj();
    ASSERT_FALSE(obj.isEmpty());
}

TEST_F(FlowControlRateLimiterTest, CurOpTimeTrackingOnHighRate) {
    FlowControlRateLimiter rl;
    rl.updateRate(1000000);

    FlowControlTicketholder::CurOp stats;
    rl.acquireTicket(_opCtx.get(), &stats);

    ASSERT_EQUALS(stats.ticketsAcquired, 1);
    ASSERT_GTE(stats.timeAcquiringMicros, 0);
}

TEST_F(FlowControlRateLimiterTest, RateLimiterStatsAccessible) {
    FlowControlRateLimiter rl;
    rl.updateRate(1000000);

    FlowControlTicketholder::CurOp stats;
    rl.acquireTicket(_opCtx.get(), &stats);

    const auto& rlStats = rl.stats();
    ASSERT_GTE(rlStats.successfulAdmissions.get(), 1);
    ASSERT_GTE(rlStats.attemptedAdmissions.get(), 1);
}

TEST_F(FlowControlRateLimiterTest, CustomMaxQueueDepth) {
    FlowControlRateLimiter rl;
    rl.updateRate(5000);

    FlowControlTicketholder::CurOp stats;
    rl.acquireTicket(_opCtx.get(), &stats);
    ASSERT_EQUALS(stats.ticketsAcquired, 1);
}

}  // namespace
}  // namespace mongo
