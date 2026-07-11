// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/topology/vector_clock/topology_time_ticker.h"

#include "mongo/db/logical_time.h"
#include "mongo/db/service_context.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/topology/vector_clock/vector_clock.h"
#include "mongo/db/topology/vector_clock/vector_clock_mutable.h"
#include "mongo/unittest/unittest.h"

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

TEST_F(TopologyTimeTickerConfigServer,
       GossipingNewTopologyTimesWhenMajorityCommitted_OutOfOrderTopologyTimes) {
    auto sc = getServiceContext();
    auto& topologyTimeTicker = TopologyTimeTicker::get(sc);
    auto vc = VectorClockMutable::get(sc);

    topologyTimeTicker.onNewLocallyCommittedTopologyTimeAvailable(Timestamp(11, 0),
                                                                  Timestamp(3, 0));
    topologyTimeTicker.onNewLocallyCommittedTopologyTimeAvailable(Timestamp(12, 0),
                                                                  Timestamp(2, 0));

    topologyTimeTicker.onMajorityCommitPointUpdate(sc, repl::OpTime(Timestamp(13, 0), -1));
    const auto time = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 0)), time.topologyTime());
}

TEST_F(TopologyTimeTickerConfigServer, MultipleTicksAtSameCommitTime_HighestTopologyTimePrevails) {
    auto sc = getServiceContext();
    auto& topologyTimeTicker = TopologyTimeTicker::get(sc);
    auto vc = VectorClockMutable::get(sc);

    topologyTimeTicker.onNewLocallyCommittedTopologyTimeAvailable(Timestamp(10, 0),
                                                                  Timestamp(3, 0));
    topologyTimeTicker.onNewLocallyCommittedTopologyTimeAvailable(Timestamp(10, 0),
                                                                  Timestamp(4, 0));
    topologyTimeTicker.onNewLocallyCommittedTopologyTimeAvailable(Timestamp(10, 0),
                                                                  Timestamp(2, 0));

    topologyTimeTicker.onMajorityCommitPointUpdate(sc, repl::OpTime(Timestamp(11, 0), -1));
    const auto time = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(4, 0)), time.topologyTime());
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
