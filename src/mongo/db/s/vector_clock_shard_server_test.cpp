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

#include "mongo/db/keys_collection_client_direct.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/s/shard_server_test_fixture.h"
#include "mongo/db/vector_clock_mutable.h"
#include "mongo/unittest/death_test.h"
#include "mongo/util/clock_source_mock.h"

namespace mongo {
namespace {

class VectorClockShardServerTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();

        auto clockSource = std::make_unique<ClockSourceMock>();
        getServiceContext()->setFastClockSource(std::move(clockSource));

        auto keysCollectionClient = std::make_unique<KeysCollectionClientDirect>();

        VectorClockMutable::get(getServiceContext())
            ->tickTo(VectorClock::Component::ClusterTime, LogicalTime(Timestamp(1, 0)));

        _keyManager = std::make_shared<KeysCollectionManager>(
            "dummy", std::move(keysCollectionClient), Seconds(1000));
        auto validator = std::make_unique<LogicalTimeValidator>(_keyManager);
        validator->init(getServiceContext());
        LogicalTimeValidator::set(getServiceContext(), std::move(validator));
    }

    void tearDown() override {
        LogicalTimeValidator::get(getServiceContext())->shutDown();

        ShardServerTestFixture::tearDown();
    }

    /**
     * Forces KeyManager to refresh cache and generate new keys.
     */
    void refreshKeyManager() {
        _keyManager->refreshNow(operationContext());
    }

private:
    std::shared_ptr<KeysCollectionManager> _keyManager;
};


TEST_F(VectorClockShardServerTest, TickClusterTime) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    const auto t0 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 0)), t0[VectorClock::Component::ClusterTime]);

    const auto r1 = vc->tick(VectorClock::Component::ClusterTime, 1);
    const auto t1 = vc->getTime();
    ASSERT_EQ(r1, t1[VectorClock::Component::ClusterTime]);
    ASSERT_GT(r1, t0[VectorClock::Component::ClusterTime]);

    const auto r2 = vc->tick(VectorClock::Component::ClusterTime, 2);
    const auto t2 = vc->getTime();
    ASSERT_GT(r2, r1);
    ASSERT_GT(t2[VectorClock::Component::ClusterTime], r1);
}

TEST_F(VectorClockShardServerTest, TickToClusterTime) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    const auto t0 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 0)), t0[VectorClock::Component::ClusterTime]);

    vc->tickTo(VectorClock::Component::ClusterTime, LogicalTime(Timestamp(1, 1)));
    const auto t1 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 1)), t1[VectorClock::Component::ClusterTime]);

    vc->tickTo(VectorClock::Component::ClusterTime, LogicalTime(Timestamp(3, 3)));
    const auto t2 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 3)), t2[VectorClock::Component::ClusterTime]);

    vc->tickTo(VectorClock::Component::ClusterTime, LogicalTime(Timestamp(2, 2)));
    const auto t3 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 3)), t3[VectorClock::Component::ClusterTime]);
}

DEATH_TEST_F(VectorClockShardServerTest, CannotTickConfigTime, "Hit a MONGO_UNREACHABLE") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tick(VectorClock::Component::ConfigTime, 1);
}

DEATH_TEST_F(VectorClockShardServerTest, CannotTickToConfigTime, "Hit a MONGO_UNREACHABLE") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickTo(VectorClock::Component::ConfigTime, LogicalTime());
}

TEST_F(VectorClockShardServerTest, GossipOutInternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    LogicalTimeValidator::get(getServiceContext())->enableKeyGenerator(operationContext(), true);
    refreshKeyManager();

    const auto clusterTime = vc->tick(VectorClock::Component::ClusterTime, 1);

    BSONObjBuilder bob;
    vc->gossipOut(nullptr, &bob, transport::Session::kInternalClient);
    auto obj = bob.obj();

    // On shard servers, gossip out to internal clients should have $clusterTime and $configTime.
    ASSERT_TRUE(obj.hasField("$clusterTime"));
    ASSERT_EQ(obj["$clusterTime"].Obj()["clusterTime"].timestamp(), clusterTime.asTimestamp());
    ASSERT_TRUE(obj.hasField("$configTime"));
    ASSERT_EQ(obj["$configTime"].timestamp(), Timestamp(0, 0));
}

TEST_F(VectorClockShardServerTest, GossipOutExternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    LogicalTimeValidator::get(getServiceContext())->enableKeyGenerator(operationContext(), true);
    refreshKeyManager();

    const auto clusterTime = vc->tick(VectorClock::Component::ClusterTime, 1);

    BSONObjBuilder bob;
    vc->gossipOut(nullptr, &bob);
    auto obj = bob.obj();

    // On shard servers, gossip out to external clients should have $clusterTime, but not
    // $configTime.
    ASSERT_TRUE(obj.hasField("$clusterTime"));
    ASSERT_EQ(obj["$clusterTime"].Obj()["clusterTime"].timestamp(), clusterTime.asTimestamp());
    ASSERT_FALSE(obj.hasField("$configTime"));
}

TEST_F(VectorClockShardServerTest, GossipInInternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    vc->tick(VectorClock::Component::ClusterTime, 1);

    auto dummySignature =
        BSON("hash" << BSONBinData("\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1", 20, BinDataGeneral)
                    << "keyId" << 0);
    vc->gossipIn(nullptr,
                 BSON("$clusterTime"
                      << BSON("clusterTime" << Timestamp(2, 2) << "signature" << dummySignature)
                      << "$configTime" << Timestamp(2, 2)),
                 false,
                 transport::Session::kInternalClient);

    // On shard servers, gossip in from internal clients should update $clusterTime and $configTime.
    auto afterTime = vc->getTime();
    ASSERT_EQ(afterTime[VectorClock::Component::ClusterTime].asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime[VectorClock::Component::ConfigTime].asTimestamp(), Timestamp(2, 2));

    vc->gossipIn(nullptr,
                 BSON("$clusterTime"
                      << BSON("clusterTime" << Timestamp(1, 1) << "signature" << dummySignature)
                      << "$configTime" << Timestamp(1, 1)),
                 false,
                 transport::Session::kInternalClient);

    auto afterTime2 = vc->getTime();
    ASSERT_EQ(afterTime2[VectorClock::Component::ClusterTime].asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime2[VectorClock::Component::ConfigTime].asTimestamp(), Timestamp(2, 2));

    vc->gossipIn(nullptr,
                 BSON("$clusterTime"
                      << BSON("clusterTime" << Timestamp(3, 3) << "signature" << dummySignature)
                      << "$configTime" << Timestamp(3, 3)),
                 false,
                 transport::Session::kInternalClient);

    auto afterTime3 = vc->getTime();
    ASSERT_EQ(afterTime3[VectorClock::Component::ClusterTime].asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime3[VectorClock::Component::ConfigTime].asTimestamp(), Timestamp(3, 3));
}

TEST_F(VectorClockShardServerTest, GossipInExternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    vc->tick(VectorClock::Component::ClusterTime, 1);

    auto dummySignature =
        BSON("hash" << BSONBinData("\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1", 20, BinDataGeneral)
                    << "keyId" << 0);
    vc->gossipIn(nullptr,
                 BSON("$clusterTime"
                      << BSON("clusterTime" << Timestamp(2, 2) << "signature" << dummySignature)
                      << "$configTime" << Timestamp(2, 2)),
                 false);

    // On shard servers, gossip in from external clients should update $clusterTime, but not
    // $configTime.
    auto afterTime = vc->getTime();
    ASSERT_EQ(afterTime[VectorClock::Component::ClusterTime].asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime[VectorClock::Component::ConfigTime].asTimestamp(), Timestamp(0, 0));

    vc->gossipIn(nullptr,
                 BSON("$clusterTime"
                      << BSON("clusterTime" << Timestamp(1, 1) << "signature" << dummySignature)
                      << "$configTime" << Timestamp(1, 1)),
                 false);

    auto afterTime2 = vc->getTime();
    ASSERT_EQ(afterTime2[VectorClock::Component::ClusterTime].asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime2[VectorClock::Component::ConfigTime].asTimestamp(), Timestamp(0, 0));

    vc->gossipIn(nullptr,
                 BSON("$clusterTime"
                      << BSON("clusterTime" << Timestamp(3, 3) << "signature" << dummySignature)
                      << "$configTime" << Timestamp(3, 3)),
                 false);

    auto afterTime3 = vc->getTime();
    ASSERT_EQ(afterTime3[VectorClock::Component::ClusterTime].asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime3[VectorClock::Component::ConfigTime].asTimestamp(), Timestamp(0, 0));
}

}  // namespace
}  // namespace mongo
