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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/bson/timestamp.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/keys_collection_client_sharded.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sharding_environment/config_server_test_fixture.h"
#include "mongo/db/sharding_environment/grid.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/transport/session.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

#include <memory>
#include <string>
#include <utility>

namespace mongo {
namespace {

class VectorClockConfigServerTest : public ConfigServerTestFixture {
protected:
    VectorClockConfigServerTest()
        : ConfigServerTestFixture(Options{}.useMockClock(true), false /* setUpMajorityReads */) {}

    void createKeysCollection() {
        DBDirectClient client(operationContext());
        client.createCollection(NamespaceString::kKeysCollectionNamespace);
    }

    void setUp() override {
        ConfigServerTestFixture::setUp();

        auto keysCollectionClient = std::make_unique<KeysCollectionClientSharded>(
            Grid::get(operationContext())->catalogClient());

        VectorClockMutable::get(getServiceContext())
            ->tickClusterTimeTo(LogicalTime(Timestamp(1, 0)));

        _keyManager = std::make_shared<KeysCollectionManager>(
            "dummy", std::move(keysCollectionClient), Seconds(1000));
        auto validator = std::make_unique<LogicalTimeValidator>(_keyManager);
        validator->init(getServiceContext());
        LogicalTimeValidator::set(getServiceContext(), std::move(validator));
    }

    void tearDown() override {
        LogicalTimeValidator::get(getServiceContext())->shutDown();

        ConfigServerTestFixture::tearDown();
    }

private:
    std::shared_ptr<KeysCollectionManager> _keyManager;
};


TEST_F(VectorClockConfigServerTest, TickClusterTime) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    const auto t0 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 0)), t0.clusterTime());

    const auto r1 = vc->tickClusterTime(1);
    const auto t1 = vc->getTime();
    ASSERT_EQ(r1, t1.clusterTime());
    ASSERT_GT(r1, t0.clusterTime());

    const auto r2 = vc->tickClusterTime(2);
    const auto t2 = vc->getTime();
    ASSERT_GT(r2, r1);
    ASSERT_GT(t2.clusterTime(), r1);
}

TEST_F(VectorClockConfigServerTest, TickToClusterTime) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    const auto t0 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 0)), t0.clusterTime());

    vc->tickClusterTimeTo(LogicalTime(Timestamp(1, 1)));
    const auto t1 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 1)), t1.clusterTime());

    vc->tickClusterTimeTo(LogicalTime(Timestamp(3, 3)));
    const auto t2 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 3)), t2.clusterTime());

    vc->tickClusterTimeTo(LogicalTime(Timestamp(2, 2)));
    const auto t3 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 3)), t3.clusterTime());
}

DEATH_TEST_F(VectorClockConfigServerTest, CannotTickConfigTime, "invariant") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickConfigTime(1);
}

TEST_F(VectorClockConfigServerTest, TickToConfigTime) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    const auto t0 = vc->getTime();
    ASSERT_EQ(VectorClock::kInitialComponentTime, t0.configTime());

    vc->tickConfigTimeTo(LogicalTime(Timestamp(1, 1)));
    const auto t1 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 1)), t1.configTime());

    vc->tickConfigTimeTo(LogicalTime(Timestamp(3, 3)));
    const auto t2 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 3)), t2.configTime());

    vc->tickConfigTimeTo(LogicalTime(Timestamp(2, 2)));
    const auto t3 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 3)), t3.configTime());
}

DEATH_TEST_F(VectorClockConfigServerTest, CannotTickTopologyTime, "invariant") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickTopologyTime(1);
}

TEST_F(VectorClockConfigServerTest, TickToTopologyTime) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    const auto t0 = vc->getTime();
    ASSERT_EQ(VectorClock::kInitialComponentTime, t0.topologyTime());

    vc->tickTopologyTimeTo(LogicalTime(Timestamp(1, 1)));
    const auto t1 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(1, 1)), t1.topologyTime());

    vc->tickTopologyTimeTo(LogicalTime(Timestamp(3, 3)));
    const auto t2 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 3)), t2.topologyTime());

    vc->tickTopologyTimeTo(LogicalTime(Timestamp(2, 2)));
    const auto t3 = vc->getTime();
    ASSERT_EQ(LogicalTime(Timestamp(3, 3)), t3.topologyTime());
}

TEST_F(VectorClockConfigServerTest, GossipOutInternal) {
    // Create the admin.system.keys collection to prevent a potential race condition with the vector
    // clock on slower machines. Without this step, the admin.system.keys collection creation
    // performed in the following enableKeyGenerator operation, updating the vector clock, might
    // occur before the node has finished gossiping out, resulting in a time mismatch and causing
    // the test to fail.
    createKeysCollection();

    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    LogicalTimeValidator::get(getServiceContext())->enableKeyGenerator(operationContext(), true);

    vc->tickClusterTime(1);                           // (1, 1)
    const auto clusterTime = vc->tickClusterTime(1);  // (1, 2)
    const auto configTime = LogicalTime(Timestamp(1, 1));
    vc->tickConfigTimeTo(configTime);
    const auto topologyTime = LogicalTime(Timestamp(1, 0));
    vc->tickTopologyTimeTo(topologyTime);

    BSONObjBuilder bob;
    vc->gossipOut(operationContext(), &bob, true /*force internal*/);
    auto obj = bob.obj();

    // On config servers, gossip out to internal clients should have $clusterTime, $configTime, and
    // $topologyTime.
    ASSERT_TRUE(obj.hasField("$clusterTime"));
    ASSERT_EQ(obj["$clusterTime"].Obj()["clusterTime"].timestamp(), clusterTime.asTimestamp());
    // No signature is attached for internal clients.
    ASSERT_FALSE(obj["$clusterTime"].Obj().hasField("signature"));
    ASSERT_TRUE(obj.hasField("$configTime"));
    ASSERT_EQ(obj["$configTime"].timestamp(), configTime.asTimestamp());
    ASSERT_TRUE(obj.hasField("$topologyTime"));
    ASSERT_EQ(obj["$topologyTime"].timestamp(), topologyTime.asTimestamp());
}

TEST_F(VectorClockConfigServerTest, GossipOutExternal) {
    // Create the admin.system.keys collection to prevent a potential race condition with the vector
    // clock on slower machines. Without this step, the admin.system.keys collection creation
    // performed in the following enableKeyGenerator operation, updating the vector clock, might
    // occur before the node has finished gossiping out, resulting in a time mismatch and causing
    // the test to fail.
    createKeysCollection();

    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    LogicalTimeValidator::get(getServiceContext())->enableKeyGenerator(operationContext(), true);

    vc->tickClusterTime(1);                           // (1, 1)
    const auto clusterTime = vc->tickClusterTime(1);  // (1, 2)
    const auto configTime = LogicalTime(Timestamp(1, 1));
    vc->tickConfigTimeTo(configTime);
    const auto topologyTime = LogicalTime(Timestamp(1, 0));
    vc->tickTopologyTimeTo(topologyTime);

    BSONObjBuilder bob;
    vc->gossipOut(operationContext(), &bob);
    auto obj = bob.obj();

    // On config servers, gossip out to external clients should have $clusterTime, but not
    // $configTime or $topologyTime.
    ASSERT_TRUE(obj.hasField("$clusterTime"));
    ASSERT_EQ(obj["$clusterTime"].Obj()["clusterTime"].timestamp(), clusterTime.asTimestamp());
    // A signature is always attached for external clients. Client is authed, so it receives a dummy
    // signature.
    ASSERT_TRUE(obj["$clusterTime"].Obj().hasField("signature"));
    ASSERT_EQ(obj["$clusterTime"].Obj()["signature"].Obj()["keyId"].Long(), 0);
    ASSERT_FALSE(obj.hasField("$configTime"));
    ASSERT_FALSE(obj.hasField("$topologyTime"));
}

TEST_F(VectorClockConfigServerTest, GossipInInternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    vc->tickClusterTime(2);  // (1, 2)
    const auto configTime = LogicalTime(Timestamp(1, 1));
    vc->tickConfigTimeTo(configTime);
    const auto topologyTime = LogicalTime(Timestamp(1, 0));
    vc->tickTopologyTimeTo(topologyTime);

    auto dummySignature =
        BSON("hash" << BSONBinData("\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1", 20, BinDataGeneral)
                    << "keyId" << 0);
    auto timepointsObj = BSON(
        "$clusterTime" << BSON("clusterTime" << Timestamp(2, 2) << "signature" << dummySignature)
                       << "$configTime" << Timestamp(2, 2) << "$topologyTime" << Timestamp(2, 2));
    auto timepoints = GossipedVectorClockComponents::parse(
        timepointsObj, IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, false, true);

    // On config servers, gossip in from internal clients should update $clusterTime, $configTime,
    // and $topologyTime.
    auto afterTime = vc->getTime();
    ASSERT_EQ(afterTime.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime.configTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime.topologyTime().asTimestamp(), Timestamp(2, 2));

    // Can gossip in from an internal client without a dummy signature.
    timepointsObj = BSON("$clusterTime" << BSON("clusterTime" << Timestamp(1, 2)) << "$configTime"
                                        << Timestamp(1, 2) << "$topologyTime" << Timestamp(1, 2)),
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, false, true);

    auto afterTime2 = vc->getTime();
    ASSERT_EQ(afterTime2.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime2.configTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime2.topologyTime().asTimestamp(), Timestamp(2, 2));

    // Can gossip in from an internal client with a dummy signature.
    timepointsObj = BSON("$clusterTime" << BSON("clusterTime" << Timestamp(3, 3)) << "$configTime"
                                        << Timestamp(3, 3) << "$topologyTime" << Timestamp(3, 3)),
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, false, true);

    auto afterTime3 = vc->getTime();
    ASSERT_EQ(afterTime3.clusterTime().asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime3.configTime().asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime3.topologyTime().asTimestamp(), Timestamp(3, 3));
}

TEST_F(VectorClockConfigServerTest, GossipInExternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    vc->tickClusterTime(2);  // (1, 2)
    const auto configTime = LogicalTime(Timestamp(1, 1));
    vc->tickConfigTimeTo(configTime);
    const auto topologyTime = LogicalTime(Timestamp(1, 0));
    vc->tickTopologyTimeTo(topologyTime);

    auto dummySignature =
        BSON("hash" << BSONBinData("\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1", 20, BinDataGeneral)
                    << "keyId" << 0);
    auto timepointsObj = BSON(
        "$clusterTime" << BSON("clusterTime" << Timestamp(2, 2) << "signature" << dummySignature)
                       << "$configTime" << Timestamp(2, 2) << "$topologyTime" << Timestamp(2, 2));
    auto timepoints = GossipedVectorClockComponents::parse(
        timepointsObj, IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, false);

    // On config servers, gossip in from external clients should update $clusterTime, but not
    // $configTime or $topologyTime.
    auto afterTime = vc->getTime();
    ASSERT_EQ(afterTime.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime.configTime().asTimestamp(), configTime.asTimestamp());
    ASSERT_EQ(afterTime.topologyTime().asTimestamp(), topologyTime.asTimestamp());

    // Gossiping works without a signature, since it's treated as a dummy signature and the test's
    // client is authorized to advance the clock.
    timepointsObj = BSON("$clusterTime" << BSON("clusterTime" << Timestamp(3, 3)) << "$configTime"
                                        << Timestamp(1, 1) << "$topologyTime" << Timestamp(1, 1)),
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(nullptr, timepoints, false);

    auto afterTime2 = vc->getTime();
    ASSERT_EQ(afterTime2.clusterTime().asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime2.configTime().asTimestamp(), configTime.asTimestamp());
    ASSERT_EQ(afterTime2.topologyTime().asTimestamp(), topologyTime.asTimestamp());
}

}  // namespace
}  // namespace mongo
