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
#include "mongo/db/keys_collection_client_direct.h"
#include "mongo/db/keys_collection_manager.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/logical_time_validator.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/repl/member_state.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/db/sharding_environment/shard_server_test_fixture.h"
#include "mongo/db/storage/storage_engine.h"
#include "mongo/db/vector_clock/vector_clock.h"
#include "mongo/db/vector_clock/vector_clock_document_gen.h"
#include "mongo/db/vector_clock/vector_clock_mutable.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

#include <memory>
#include <string>
#include <utility>

namespace mongo {
namespace {

class VectorClockShardServerTest : public ShardServerTestFixture {
protected:
    VectorClockShardServerTest()
        : ShardServerTestFixture(Options{}.useMockClock(true), false /* setUpMajorityReads */) {}

    void setUp() override {
        ShardServerTestFixture::setUp();

        auto keysCollectionClient =
            std::make_unique<KeysCollectionClientDirect>(false /*mustUseLocalReads*/);

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

        ShardServerTestFixture::tearDown();
    }

private:
    std::shared_ptr<KeysCollectionManager> _keyManager;
};


TEST_F(VectorClockShardServerTest, TickClusterTime) {
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

TEST_F(VectorClockShardServerTest, TickToClusterTime) {
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

DEATH_TEST_F(VectorClockShardServerTest, CannotTickConfigTime, "invariant") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickConfigTime(1);
}

DEATH_TEST_F(VectorClockShardServerTest, CannotTickToConfigTime, "invariant") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickConfigTimeTo(LogicalTime());
}

DEATH_TEST_F(VectorClockShardServerTest, CannotTickTopologyTime, "invariant") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickTopologyTime(1);
}

DEATH_TEST_F(VectorClockShardServerTest, CannotTickToTopologyTime, "invariant") {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);
    vc->tickTopologyTimeTo(LogicalTime());
}

TEST_F(VectorClockShardServerTest, GossipOutInternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    LogicalTimeValidator::get(getServiceContext())->enableKeyGenerator(operationContext(), true);

    const auto clusterTime = vc->tickClusterTime(1);

    BSONObjBuilder bob;
    vc->gossipOut(operationContext(), &bob, true /*force internal*/);
    auto obj = bob.obj();

    // On shard servers, gossip out to internal clients should have $clusterTime, $configTime, and
    // $topologyTime.
    ASSERT_TRUE(obj.hasField("$clusterTime"));
    ASSERT_EQ(obj["$clusterTime"].Obj()["clusterTime"].timestamp(), clusterTime.asTimestamp());
    // No signature is attached for internal clients.
    ASSERT_FALSE(obj["$clusterTime"].Obj().hasField("signature"));
    ASSERT_TRUE(obj.hasField("$configTime"));
    ASSERT_EQ(obj["$configTime"].timestamp(), VectorClock::kInitialComponentTime.asTimestamp());
    ASSERT_TRUE(obj.hasField("$topologyTime"));
    ASSERT_EQ(obj["$topologyTime"].timestamp(), VectorClock::kInitialComponentTime.asTimestamp());
}

TEST_F(VectorClockShardServerTest, GossipOutExternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    LogicalTimeValidator::get(getServiceContext())->enableKeyGenerator(operationContext(), true);

    const auto clusterTime = vc->tickClusterTime(1);

    BSONObjBuilder bob;
    vc->gossipOut(operationContext(), &bob);
    auto obj = bob.obj();

    // On shard servers, gossip out to external clients should have $clusterTime, but not
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

TEST_F(VectorClockShardServerTest, GossipInInternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    vc->tickClusterTime(1);

    auto dummySignature =
        BSON("hash" << BSONBinData("\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1", 20, BinDataGeneral)
                    << "keyId" << 0);
    auto timepointsObj = BSON(
        "$clusterTime" << BSON("clusterTime" << Timestamp(2, 2) << "signature" << dummySignature)
                       << "$configTime" << Timestamp(2, 2) << "$topologyTime" << Timestamp(2, 2));
    auto timepoints = GossipedVectorClockComponents::parse(
        timepointsObj, IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, false, true);

    // On shard servers, gossip in from internal clients should update $clusterTime, $configTime,
    // and $topologyTime.
    auto afterTime = vc->getTime();
    ASSERT_EQ(afterTime.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime.configTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime.topologyTime().asTimestamp(), Timestamp(2, 2));

    timepointsObj = BSON("$clusterTime"
                         << BSON("clusterTime" << Timestamp(1, 1) << "signature" << dummySignature)
                         << "$configTime" << Timestamp(1, 1) << "$topologyTime" << Timestamp(1, 1));
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, false, true);

    auto afterTime2 = vc->getTime();
    ASSERT_EQ(afterTime2.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime2.configTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime2.topologyTime().asTimestamp(), Timestamp(2, 2));

    // Gossiping works without a signature, since it's treated as a dummy signature.
    timepointsObj = BSON("$clusterTime" << BSON("clusterTime" << Timestamp(3, 3)) << "$configTime"
                                        << Timestamp(3, 3) << "$topologyTime" << Timestamp(3, 3));
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, false, true);

    auto afterTime3 = vc->getTime();
    ASSERT_EQ(afterTime3.clusterTime().asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime3.configTime().asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime3.topologyTime().asTimestamp(), Timestamp(3, 3));
}

TEST_F(VectorClockShardServerTest, GossipInExternal) {
    auto sc = getServiceContext();
    auto vc = VectorClockMutable::get(sc);

    vc->tickClusterTime(1);

    auto dummySignature =
        BSON("hash" << BSONBinData("\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1\1", 20, BinDataGeneral)
                    << "keyId" << 0);
    auto timepointsObj = BSON(
        "$clusterTime" << BSON("clusterTime" << Timestamp(2, 2) << "signature" << dummySignature)
                       << "$configTime" << Timestamp(2, 2) << "$topologyTime" << Timestamp(2, 2));
    auto timepoints = GossipedVectorClockComponents::parse(
        timepointsObj, IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, false);

    // On shard servers, gossip in from external clients should update $clusterTime, but not
    // $configTime or $topologyTime.
    auto afterTime = vc->getTime();
    ASSERT_EQ(afterTime.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime.configTime(), VectorClock::kInitialComponentTime);
    ASSERT_EQ(afterTime.topologyTime(), VectorClock::kInitialComponentTime);

    timepointsObj = BSON("$clusterTime"
                         << BSON("clusterTime" << Timestamp(1, 1) << "signature" << dummySignature)
                         << "$configTime" << Timestamp(1, 1) << "$topologyTime" << Timestamp(1, 1));
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, false);

    auto afterTime2 = vc->getTime();
    ASSERT_EQ(afterTime2.clusterTime().asTimestamp(), Timestamp(2, 2));
    ASSERT_EQ(afterTime2.configTime(), VectorClock::kInitialComponentTime);
    ASSERT_EQ(afterTime2.topologyTime(), VectorClock::kInitialComponentTime);

    // Gossiping works without a signature, since it's treated as a dummy signature and the test's
    // client is authorized to advance the clock.
    timepointsObj = BSON("$clusterTime" << BSON("clusterTime" << Timestamp(3, 3)) << "$configTime"
                                        << Timestamp(3, 3) << "$topologyTime" << Timestamp(3, 3));
    timepoints = GossipedVectorClockComponents::parse(timepointsObj,
                                                      IDLParserContext("VectorClockComponents"));
    vc->gossipIn(operationContext(), timepoints, false);

    auto afterTime3 = vc->getTime();
    ASSERT_EQ(afterTime3.clusterTime().asTimestamp(), Timestamp(3, 3));
    ASSERT_EQ(afterTime3.configTime(), VectorClock::kInitialComponentTime);
    ASSERT_EQ(afterTime3.topologyTime(), VectorClock::kInitialComponentTime);
}

class VectorClockPersistenceTest : public ShardServerTestFixture {
protected:
    void setUp() override {
        ShardServerTestFixture::setUp();

        auto replCoord = repl::ReplicationCoordinator::get(operationContext());
        ASSERT_OK(replCoord->setFollowerMode(repl::MemberState::RS_PRIMARY));
    }
};

const BSONObj kVectorClockQuery = BSON("_id" << "vectorClockState");

TEST_F(VectorClockPersistenceTest, PrimaryPersistVectorClockDocument) {
    auto sc = getServiceContext();
    auto opCtx = operationContext();
    auto vc = VectorClockMutable::get(sc);

    // Check that no vectorClockState document is present
    PersistentTaskStore<VectorClockDocument> store(NamespaceString::kVectorClockNamespace);
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 0);

    // Persist and check that the vectorClockState document has been persisted
    vc->advanceConfigTime_forTest(LogicalTime(Timestamp(2, 2)));
    vc->advanceTopologyTime_forTest(LogicalTime(Timestamp(1, 1)));
    vc->waitForDurableConfigTime().get();
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 1);

    // Check that the vectorClockState document is still one after more persist calls
    vc->advanceConfigTime_forTest(LogicalTime(Timestamp(10, 10)));
    vc->advanceTopologyTime_forTest(LogicalTime(Timestamp(1, 1)));
    vc->waitForDurableConfigTime().get();
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 1);
}

TEST_F(VectorClockPersistenceTest, PrimaryRecoverWithoutExistingVectorClockDocument) {
    auto sc = getServiceContext();
    auto opCtx = operationContext();
    auto vc = VectorClockMutable::get(sc);

    // Check that no vectorClockState document is present
    PersistentTaskStore<VectorClockDocument> store(NamespaceString::kVectorClockNamespace);
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 0);

    vc->recoverDirect(opCtx);

    auto time = vc->getTime();
    ASSERT_EQ(VectorClock::kInitialComponentTime, time.configTime());
    ASSERT_EQ(VectorClock::kInitialComponentTime, time.topologyTime());
}

TEST_F(VectorClockPersistenceTest, PrimaryRecoverWithExistingVectorClockDocument) {
    auto sc = getServiceContext();
    auto opCtx = operationContext();
    auto vc = VectorClockMutable::get(sc);

    // Check that no vectorClockState document is present
    PersistentTaskStore<VectorClockDocument> store(NamespaceString::kVectorClockNamespace);
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 0);
    VectorClockDocument vcd;
    vcd.setConfigTime(Timestamp(100));
    vcd.setTopologyTime(Timestamp(50));
    store.add(opCtx, vcd);

    vc->recoverDirect(opCtx);

    auto time = vc->getTime();
    ASSERT_EQ(Timestamp(100), time.configTime().asTimestamp());
    ASSERT_EQ(Timestamp(50), time.topologyTime().asTimestamp());
}

TEST_F(VectorClockPersistenceTest, PrimaryRecoverWithIllegalVectorClockDocument) {
    auto sc = getServiceContext();
    auto opCtx = operationContext();
    auto vc = VectorClockMutable::get(sc);

    // Check that no vectorClockState document is present
    PersistentTaskStore<VectorClockDocument> store(NamespaceString::kVectorClockNamespace);
    ASSERT_EQ(store.count(opCtx, kVectorClockQuery), 0);
    DBDirectClient client(opCtx);
    client.insert(NamespaceString::kVectorClockNamespace,
                  BSON("_id" << "vectorClockState"
                             << "IllegalKey"
                             << "IllegalValue"));

    vc->recoverDirect(opCtx);

    auto time = vc->getTime();
    ASSERT_EQ(VectorClock::kInitialComponentTime, time.configTime());
    ASSERT_EQ(VectorClock::kInitialComponentTime, time.topologyTime());
}

}  // namespace
}  // namespace mongo
