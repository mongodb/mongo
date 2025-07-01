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

#include "mongo/db/mirror_maestro.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/mirror_maestro_gen.h"
#include "mongo/db/repl/hello/topology_version_observer.h"
#include "mongo/db/repl/replication_coordinator_mock.h"
#include "mongo/db/service_context_d_test_fixture.h"
#include "mongo/idl/server_parameter_test_util.h"
#include "mongo/platform/basic.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

class MirrorMaestroTest : service_context_test::WithSetupTransportLayer,
                          public ServiceContextMongoDTest {
public:
    const BSONObj kEastTag = BSON("dc" << "east");
    const BSONObj kWestTag = BSON("dc" << "west");
    constexpr static auto kHost1 = "host1:27017";
    constexpr static auto kHost2 = "host2:27017";

    void setUp() override {
        ServiceContextMongoDTest::setUp();
        auto service = getServiceContext();

        auto replCoord = std::make_unique<repl::ReplicationCoordinatorMock>(service);
        repl::ReplicationCoordinator::set(service, std::move(replCoord));

        MirrorMaestro::init(service);
    }

    void tearDown() override {
        MirrorMaestro::shutdown(getServiceContext());
        ServiceContextTest::tearDown();
    }

protected:
    repl::ReplSetConfig makeConfig(int version, int term, BSONObj host1Tag, BSONObj host2Tag) {
        BSONObjBuilder configBuilder;
        configBuilder << "_id" << "_rs";
        configBuilder << "version" << version;
        configBuilder << "term" << term;
        configBuilder << "members"
                      << BSON_ARRAY(BSON("_id" << 0 << "host" << kHost1 << "tags" << host1Tag)
                                    << BSON("_id" << 1 << "host" << kHost2 << "tags" << host2Tag));
        configBuilder << "protocolVersion" << 1;
        return repl::ReplSetConfig::parse(configBuilder.obj());
    }

private:
    FailPointEnableBlock _skipRegisteringMirroredReadsTopologyObserverCallback{
        "skipRegisteringMirroredReadsTopologyObserverCallback"};
};

TEST_F(MirrorMaestroTest, BasicInitializationEmptyHostsCache) {
    // Verify hosts cache is initially empty
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT(hosts.empty());
}

TEST_F(MirrorMaestroTest, UpdateCachedHostsOnServerParameterChange) {
    auto serverParam = BSON("targetedMirroring" << BSON("samplingRate" << 0.1 << "maxTimeMS" << 500
                                                                       << "tag" << kEastTag));
    ServerParameterControllerForTest controller("mirrorReads", serverParam);

    // Create test config with tags
    int version = 1;
    int term = 1;
    auto config = makeConfig(version, term, kEastTag, kWestTag);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now update the server parameter to change the tag and call update again
    auto updatedServerParam =
        BSON("targetedMirroring" << BSON("samplingRate" << 0.1 << "maxTimeMS" << 500 << "tag"
                                                        << kWestTag));
    controller = ServerParameterControllerForTest("mirrorReads", updatedServerParam);
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, true /* tagChanged */);

    // Verify hosts were updated
    hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost2);
}

TEST_F(MirrorMaestroTest, UpdateCachedHostsOnTopologyVersionChange) {
    auto serverParam = BSON("targetedMirroring" << BSON("samplingRate" << 0.1 << "maxTimeMS" << 500
                                                                       << "tag" << kEastTag));
    ServerParameterControllerForTest controller("mirrorReads", serverParam);

    int version = 2;
    int term = 1;
    auto config = makeConfig(version, term, kEastTag, kWestTag);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now change the tags for host2 and bump the configVersion
    version++;
    config = makeConfig(version, term, kEastTag, kEastTag);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were updated
    hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 2);
    ASSERT_EQ((hosts)[0].toString(), kHost1);
    ASSERT_EQ((hosts)[1].toString(), kHost2);
}

TEST_F(MirrorMaestroTest, NoUpdateToCachedHostsIfTopologyVersionUnchanged) {
    auto serverParam = BSON("targetedMirroring" << BSON("samplingRate" << 0.1 << "maxTimeMS" << 500
                                                                       << "tag" << kEastTag));
    ServerParameterControllerForTest controller("mirrorReads", serverParam);

    int version = 1;
    int term = 1;
    auto config = makeConfig(version, term, kEastTag, kWestTag);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now call update with an unchanged config version, but changed tags. This is just a sanity
    // check that we don't try to update if the config version hasn't changed - it should never
    // happen in production that tags are changed without a change in version.
    config = makeConfig(version, term, kEastTag, kEastTag);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were not updated
    hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);
}

DEATH_TEST_F(MirrorMaestroTest, InvariantOnDecreasedConfigVersionForSameTerm, "invariant") {
    auto serverParam = BSON("targetedMirroring" << BSON("samplingRate" << 0.1 << "maxTimeMS" << 500
                                                                       << "tag" << kEastTag));
    ServerParameterControllerForTest controller("mirrorReads", serverParam);

    auto version = 2;
    auto term = 1;
    auto config = makeConfig(version, term, kEastTag, kWestTag);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ(((hosts)[0]).toString(), kHost1);

    // Now call update again but with a lower config version and same term, which should crash
    version--;
    config = makeConfig(version, term, kWestTag, kEastTag);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);
}

TEST_F(MirrorMaestroTest, UpdateHostsOnNewTermEvenIfLowerConfigVersion) {
    auto serverParam = BSON("targetedMirroring" << BSON("samplingRate" << 0.1 << "maxTimeMS" << 500
                                                                       << "tag" << kEastTag));
    ServerParameterControllerForTest controller("mirrorReads", serverParam);

    auto version = 2;
    auto term = 1;
    auto config = makeConfig(version, term, kEastTag, kWestTag);

    // Update cached hosts
    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were cached
    auto hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 1);
    ASSERT_EQ((hosts)[0].toString(), kHost1);

    // Now call update again but with a lower config version and higher term. The higher term
    // indicates the more up to date config, so we should update.
    version--;
    term++;
    config = makeConfig(version, term, kEastTag, kEastTag);

    updateCachedHostsForTargetedMirroring_forTest(
        getServiceContext(), config, false /* tagChanged */);

    // Verify hosts were updated
    hosts = getCachedHostsForTargetedMirroring_forTest(getServiceContext());
    ASSERT_EQ(hosts.size(), 2);
    ASSERT_EQ((hosts)[0].toString(), kHost1);
    ASSERT_EQ((hosts)[1].toString(), kHost2);
}

}  // namespace
}  // namespace mongo
