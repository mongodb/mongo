/**
 *    Copyright (C) 2022-present MongoDB, Inc.
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

#include "mongo/db/vector_clock/topology_time_ticker.h"

#include "mongo/base/string_data.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

#include <memory>
#include <string>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {

struct TickPoint {
    Timestamp topologyTime;
    Timestamp commitTime;
};

class TopologyTimeTickerConfigServer : public ConfigServerTestFixture {
protected:
    TopologyTimeTickerConfigServer() : ConfigServerTestFixture(Options{}.useMockClock(true)) {}

    void setUp() override {
        ConfigServerTestFixture::setUp();
    }

    void tearDown() override {
        ConfigServerTestFixture::tearDown();
    }

    /**
     * This function registers three tick points into the TopologyTimeTicker. Those three tick
     * points are also stored in the _ticks vector.
     */
    void registerThreeTickPoints(ServiceContext* sc) {
        auto& topologyTimeTicker = TopologyTimeTicker::get(sc);
        for (int i = 0; i < 3; ++i) {
            TickPoint newTick = {Timestamp(1, 2 * i), Timestamp(1, 2 * i + 1)};
            _ticks.push_back(newTick);
            topologyTimeTicker.onNewLocallyCommittedTopologyTimeAvailable(newTick.commitTime,
                                                                          newTick.topologyTime);
        }
    }

    void checkTopologyTimeAfterAdvancingMajorityCommitPoint(ServiceContext* sc,
                                                            const Timestamp& majorityTime,
                                                            const Timestamp& expectedTopologyTime) {
        auto& topologyTimeTicker = TopologyTimeTicker::get(sc);
        auto vc = VectorClockMutable::get(sc);
        topologyTimeTicker.onMajorityCommitPointUpdate(sc, repl::OpTime(majorityTime, /*term*/ -1));
        const auto time = vc->getTime();
        ASSERT_EQ(LogicalTime(expectedTopologyTime), time.topologyTime());
    }


    static const Timestamp kCommitTimePreTicks;
    static const Timestamp kCommitTimePostTicks;
    // The commit time of the different ticks must belong to the range (kCommitTimePreTicks,
    // kCommitTimePostTicks)
    std::vector<TickPoint> _ticks;
};

const Timestamp TopologyTimeTickerConfigServer::kCommitTimePreTicks = Timestamp(0, 1);
const Timestamp TopologyTimeTickerConfigServer::kCommitTimePostTicks = Timestamp(10, 10);

TEST_F(TopologyTimeTickerConfigServer, GossipingNewTopologyTimesWhenMajorityCommitted1) {
    auto sc = getServiceContext();

    registerThreeTickPoints(sc);

    // Checking initial topologyTime value
    checkTopologyTimeAfterAdvancingMajorityCommitPoint(sc, kCommitTimePreTicks, Timestamp(0, 1));

    // Checking that after advancing the commit point we also advance the topologyTime
    for (const auto& tick : _ticks) {
        checkTopologyTimeAfterAdvancingMajorityCommitPoint(sc, tick.commitTime, tick.topologyTime);
    }

    // The majority commit point is advanced a bit more but there are no tick points: the
    // topology time shouldn't change.
    checkTopologyTimeAfterAdvancingMajorityCommitPoint(
        sc, kCommitTimePostTicks, _ticks.back().topologyTime);
}

TEST_F(TopologyTimeTickerConfigServer, GossipingNewTopologyTimesWhenMajorityCommitted2) {
    auto sc = getServiceContext();

    registerThreeTickPoints(sc);

    // Checking initial topologyTime value
    checkTopologyTimeAfterAdvancingMajorityCommitPoint(sc, kCommitTimePreTicks, Timestamp(0, 1));

    // The majority commit point is advanced to a point that includes all the tick points. The
    // topology time should be the greatest one.
    checkTopologyTimeAfterAdvancingMajorityCommitPoint(
        sc, kCommitTimePostTicks, _ticks.back().topologyTime);
}

DEATH_TEST_F(TopologyTimeTickerConfigServer,
             InvalidonNewLocallyCommittedTopologyTimeAvailable,
             "invariant") {
    // This test verifies that the internal elements on the tick point vector are sorted.
    auto sc = getServiceContext();
    Timestamp topologyTime1(0, 8);
    Timestamp commitTime1(0, 10);
    auto& topologyTimeTicker = TopologyTimeTicker::get(sc);
    topologyTimeTicker.onNewLocallyCommittedTopologyTimeAvailable(commitTime1, topologyTime1);

    Timestamp topologyTime2(0, 5);
    Timestamp commitTime2(0, 6);
    // Newer tick points must have a greater commit time than older ones
    topologyTimeTicker.onNewLocallyCommittedTopologyTimeAvailable(commitTime2, topologyTime2);
}

TEST_F(TopologyTimeTickerConfigServer, RollbackingAllTickPoints) {
    auto sc = getServiceContext();

    registerThreeTickPoints(sc);

    // Checking initial topologyTime value
    checkTopologyTimeAfterAdvancingMajorityCommitPoint(sc, kCommitTimePreTicks, Timestamp(0, 1));

    // We rollback all tick points
    auto& topologyTimeTicker = TopologyTimeTicker::get(sc);
    topologyTimeTicker.onReplicationRollback(repl::OpTime(kCommitTimePreTicks, /*term*/ -1));

    // The majority commit point is advanced to a point that would have included all our tick
    // points, but because they were rollbacked the topology time should still be Timestamp(0, 1).
    checkTopologyTimeAfterAdvancingMajorityCommitPoint(sc, kCommitTimePostTicks, Timestamp(0, 1));
}

TEST_F(TopologyTimeTickerConfigServer, PartialRollbackingTickPoints) {
    auto sc = getServiceContext();

    registerThreeTickPoints(sc);

    // We rollback the last two tick points
    auto& topologyTimeTicker = TopologyTimeTicker::get(sc);
    topologyTimeTicker.onReplicationRollback(repl::OpTime(_ticks.front().commitTime, /*term*/ -1));

    // The majority commit point is advanced to a point that includes all the tick points. The
    // topology time should be the one from the first tick since the other two were rollbacked.
    checkTopologyTimeAfterAdvancingMajorityCommitPoint(
        sc, kCommitTimePostTicks, _ticks.front().topologyTime);
}

}  // namespace
}  // namespace mongo
