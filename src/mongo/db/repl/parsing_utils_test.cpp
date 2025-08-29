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
