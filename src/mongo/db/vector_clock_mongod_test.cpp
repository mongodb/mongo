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

#include "mongo/platform/basic.h"

#include "mongo/db/service_context_test_fixture.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

using VectorClockMongoDTest = ServiceContextTest;

TEST_F(VectorClockMongoDTest, TickClusterTime) {
    auto sc = getGlobalServiceContext();
    auto vc = VectorClockMutable::get(sc);

    const auto t0 = vc->getTime();
    ASSERT_EQ(LogicalTime(), t0[VectorClock::Component::ClusterTime]);

    const auto r1 = vc->tick(VectorClock::Component::ClusterTime, 1);
    const auto t1 = vc->getTime();
    ASSERT_EQ(r1, t1[VectorClock::Component::ClusterTime]);
    ASSERT_GT(r1, t0[VectorClock::Component::ClusterTime]);

    const auto r2 = vc->tick(VectorClock::Component::ClusterTime, 2);
    const auto t2 = vc->getTime();
    ASSERT_GT(r2, r1);
    ASSERT_GT(t2[VectorClock::Component::ClusterTime], r1);
}

TEST_F(VectorClockMongoDTest, GossipOutTest) {
    // TODO SERVER-47914: after ClusterTime gossiping has been re-enabled: get the gossipOut
    // internal and external, and for each check that $clusterTime is there, with the right format,
    // and right value, and not configTime.

    // auto sc = getGlobalServiceContext();
    // auto vc = VectorClockMutable::get(sc);

    // const auto r1 = vc->tick(VectorClock::Component::ClusterTime, 1);
}

TEST_F(VectorClockMongoDTest, GossipInTest) {
    // TODO SERVER-47914: after ClusterTime gossiping has been re-enabled: for each of gossipIn
    // internal and external, give it BSON in the correct format, and then check that ClusterTime
    // has been advanced (or not), and that ConfigTime has not.

    // auto sc = getGlobalServiceContext();
    // auto vc = VectorClockMutable::get(sc);

    // const auto r1 = vc->tick(VectorClock::Component::ClusterTime, 1);
}

DEATH_TEST_F(VectorClockMongoDTest, CannotTickConfigTime, "Hit a MONGO_UNREACHABLE") {
    auto sc = getGlobalServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tick(VectorClock::Component::ConfigTime, 1);
}

DEATH_TEST_F(VectorClockMongoDTest, CannotTickToClusterTime, "Hit a MONGO_UNREACHABLE") {
    auto sc = getGlobalServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickTo(VectorClock::Component::ClusterTime, LogicalTime());
}

DEATH_TEST_F(VectorClockMongoDTest, CannotTickToConfigTime, "Hit a MONGO_UNREACHABLE") {
    auto sc = getGlobalServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickTo(VectorClock::Component::ConfigTime, LogicalTime());
}

}  // namespace
}  // namespace mongo
