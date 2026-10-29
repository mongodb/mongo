// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/db/repl/parsing_utils.h"

#include "mongo/db/repl/replication_coordinator_external_state_mock.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

namespace mongo {
namespace repl {

TEST_F(ServiceContextTest, ParseReplSetSeedList_BadHostName) {
    ReplicationCoordinatorExternalStateMock rses;

    ASSERT_THROWS_WHAT(parseReplSetSeedList(&rses, "name/name:port"),
                       DBException,
                       "bad --replSet seed hostname (could not parse to HostAndPort)");
}

TEST(parseReplSetSeedList, ParseReplSetSeedList_BadPort) {
    ReplicationCoordinatorExternalStateMock rses;

    ASSERT_THROWS_WHAT(parseReplSetSeedList(&rses, "name/host0:abcd"),
                       DBException,
                       "bad --replSet seed hostname (could not parse to HostAndPort)");
}

TEST_F(ServiceContextTest, ParseReplSetSeedList_AllowWhitespaceInHostName) {
    ReplicationCoordinatorExternalStateMock rses;

    const auto [name, seeds] = parseReplSetSeedList(&rses, "name/ host0:1000");
    ASSERT(name == "name");
    ASSERT(seeds.size() == 1);
    ASSERT(seeds[0] == HostAndPort(" host0", 1000));
}

TEST_F(ServiceContextTest, ParseReplSetSeedList_IgnoreSelf) {
    ReplicationCoordinatorExternalStateMock rses;
    rses.addSelf(HostAndPort("host0:1000"));

    const auto [name, seeds] = parseReplSetSeedList(&rses, "name/host0:1000,host1:1001");

    ASSERT(name == "name");
    ASSERT(seeds.size() == 1);
    ASSERT(seeds[0] == HostAndPort("host1", 1001));
}

TEST_F(ServiceContextTest, ParseReplSetSeedList_ReplSetOnly) {
    ReplicationCoordinatorExternalStateMock rses;

    const auto [name, seeds] = parseReplSetSeedList(&rses, "name");
    ASSERT(name == "name");
    ASSERT(seeds.empty());
}

TEST_F(ServiceContextTest, ParseReplSetSeedList_ReplSetOnlyWithSlash) {
    ReplicationCoordinatorExternalStateMock rses;

    const auto [name, seeds] = parseReplSetSeedList(&rses, "name/");
    ASSERT(name == "name");
    ASSERT(seeds.empty());
}

TEST_F(ServiceContextTest, ParseReplSetSeedList_LocalhostNotAllowed) {
    ReplicationCoordinatorExternalStateMock rses;

    ASSERT_THROWS_WHAT(parseReplSetSeedList(&rses, "name/localhost"),
                       DBException,
                       "can't use localhost in replset seed host list");
}

TEST_F(ServiceContextTest, ParseReplSetSeedList_SingleSeed) {
    ReplicationCoordinatorExternalStateMock rses;

    const auto [name, seeds] = parseReplSetSeedList(&rses, "name/host0:1000");
    ASSERT(name == "name");
    ASSERT(seeds.size() == 1);
    ASSERT(seeds[0] == HostAndPort("host0", 1000));
}

TEST_F(ServiceContextTest, ParseReplSetSeedList_EmptyHostName) {
    ReplicationCoordinatorExternalStateMock rses;

    const auto [name, seeds] = parseReplSetSeedList(&rses, "name/host0:1000,");
    ASSERT(name == "name");
    ASSERT(seeds.size() == 1);
    ASSERT(seeds[0] == HostAndPort("host0", 1000));
}

TEST_F(ServiceContextTest, ParseReplSetSeedList_MultipleSeeds) {
    ReplicationCoordinatorExternalStateMock rses;

    const auto [name, seeds] = parseReplSetSeedList(&rses, "name/host0:1000,host1:1001,host2:1002");
    ASSERT(name == "name");
    ASSERT(seeds.size() == 3);
    ASSERT(seeds[0] == HostAndPort("host0", 1000));
    ASSERT(seeds[1] == HostAndPort("host1", 1001));
    ASSERT(seeds[2] == HostAndPort("host2", 1002));
}

TEST_F(ServiceContextTest, ParseReplSetSeedList_DuplicateSeedsNotAllowed) {
    ReplicationCoordinatorExternalStateMock rses;

    ASSERT_THROWS_WHAT(parseReplSetSeedList(&rses, "name/host0:1000,host1:1001,host0:1000"),
                       DBException,
                       "bad --replSet command line config string (has duplicate seeds)");
}

}  // namespace repl
}  // namespace mongo
