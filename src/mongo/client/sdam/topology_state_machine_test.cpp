/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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
#include "mongo/client/sdam/topology_state_machine.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/client/sdam/sdam_test_base.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/client/sdam/topology_description.h"

namespace mongo::sdam {
class TopologyStateMachineTestFixture : public SdamTestFixture {
protected:
    static inline const auto kReplicaSetName = "replica_set";
    static inline const auto kLocalServer = HostAndPort("localhost:123");
    static inline const auto kLocalServer2 = HostAndPort("localhost:456");
    static inline const OID kOidOne{"000000000000000000000001"};
    static inline const OID kOidTwo{"000000000000000000000002"};

    static inline const auto kNotUsedMs = Milliseconds(100);
    static inline const auto kFiveHundredMs = Milliseconds(500);

    static inline const auto kTwoSeedConfig =
        SdamConfiguration(std::vector<HostAndPort>{kLocalServer, kLocalServer2},
                          TopologyType::kUnknown,
                          kFiveHundredMs);

    static inline const auto kTwoSeedReplicaSetNoPrimaryConfig =
        SdamConfiguration(std::vector<HostAndPort>{kLocalServer, kLocalServer2},
                          TopologyType::kReplicaSetNoPrimary,
                          kFiveHundredMs,
                          kNotUsedMs,
                          kNotUsedMs,
                          std::string("setName"));

    static inline const auto kSingleConfig =
        SdamConfiguration(std::vector<HostAndPort>{kLocalServer}, TopologyType::kSingle);

    // Given we are in 'starting' state with initial config 'initialConfig'. We receive a
    // ServerDescription with type 'incoming', and expected the ending topology state to be
    // 'ending'.
    struct TopologyTypeTestCase {
        SdamConfiguration initialConfig;
        TopologyType starting;
        ServerType incoming;
        TopologyType ending;
    };

    // This function sets up the test scenario defined by the given TopologyTypeTestCase. It
    // simulates receiving a ServerDescription, and asserts that the final topology type is in the
    // correct state.
    void assertTopologyTypeTestCase(TopologyTypeTestCase testCase) {
        TopologyStateMachine stateMachine(testCase.initialConfig);

        // setup the initial state
        auto topologyDescription = std::make_shared<TopologyDescription>(testCase.initialConfig);
        topologyDescription->setType(testCase.starting);

        // create new ServerDescription and
        auto serverDescriptionBuilder =
            ServerDescriptionBuilder().withType(testCase.incoming).withAddress(kLocalServer);

        // update the known hosts in the ServerDescription
        if (testCase.initialConfig.getSeedList()) {
            for (auto address : *testCase.initialConfig.getSeedList()) {
                serverDescriptionBuilder.withHost(address);
            }
        }

        // set the primary if we are creating one
        if (testCase.incoming == ServerType::kRSPrimary) {
            serverDescriptionBuilder.withPrimary(kLocalServer);
        }

        // set the replica set name if appropriate
        const std::vector<ServerType>& replicaSetServerTypes = std::vector<ServerType>{
            ServerType::kRSOther, ServerType::kRSSecondary, ServerType::kRSArbiter};
        if (std::find(replicaSetServerTypes.begin(),
                      replicaSetServerTypes.end(),
                      testCase.incoming) != replicaSetServerTypes.end()) {
            serverDescriptionBuilder.withSetName(kReplicaSetName);
        }

        serverDescriptionBuilder.withElectionId(kOidOne).withSetVersion(100);

        const auto serverDescription = serverDescriptionBuilder.instance();

        // simulate the ServerDescription being received
        stateMachine.onServerDescription(*topologyDescription, serverDescription);

        ASSERT_EQUALS(topologyDescription->getType(), testCase.ending);
    }

    std::vector<ServerType> allServerTypesExceptPrimary() {
        auto allExceptPrimary = allServerTypes();
        allExceptPrimary.erase(
            std::remove_if(allExceptPrimary.begin(),
                           allExceptPrimary.end(),
                           [](const ServerType t) { return t == ServerType::kRSPrimary; }),
            allExceptPrimary.end());
        return allExceptPrimary;
    }

    static auto getServerDescriptionAddress(const ServerDescriptionPtr& serverDescription) {
        return serverDescription->getAddress();
    };
};

TEST_F(TopologyStateMachineTestFixture, ShouldInstallServerDescriptionInSingleTopology) {
    TopologyStateMachine stateMachine(kSingleConfig);
    auto topologyDescription = std::make_shared<TopologyDescription>(kSingleConfig);

    auto updatedMeAddress = HostAndPort("foo:1234");
    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(kLocalServer)
                                 .withMe(updatedMeAddress)
                                 .withType(ServerType::kStandalone)
                                 .instance();

    stateMachine.onServerDescription(*topologyDescription, serverDescription);
    ASSERT_EQUALS(static_cast<size_t>(1), topologyDescription->getServers().size());

    auto result = topologyDescription->findServerByAddress(kLocalServer);
    ASSERT(result);
    ASSERT_EQUALS(serverDescription, *result);
}

TEST_F(TopologyStateMachineTestFixture, ShouldRemoveServerDescriptionIfNotInHostsList) {
    const auto primary = (*kTwoSeedConfig.getSeedList()).front();
    const auto expectedRemovedServer = (*kTwoSeedConfig.getSeedList()).back();

    TopologyStateMachine stateMachine(kTwoSeedConfig);
    auto topologyDescription = std::make_shared<TopologyDescription>(kTwoSeedConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(primary)
                                 .withType(ServerType::kRSPrimary)
                                 .withPrimary(primary)
                                 .withHost(primary)
                                 .withElectionId(kOidOne)
                                 .withSetVersion(100)
                                 .instance();

    ASSERT_EQUALS(static_cast<size_t>(2), topologyDescription->getServers().size());
    stateMachine.onServerDescription(*topologyDescription, serverDescription);
    ASSERT_EQUALS(static_cast<size_t>(1), topologyDescription->getServers().size());
    ASSERT_EQUALS(serverDescription, topologyDescription->getServers().front());
}

TEST_F(TopologyStateMachineTestFixture,
       ShouldNotRemoveReplicaSetMemberServerWhenTopologyIsReplicaSetNoPrimaryAndMeIsNotPresent) {
    const auto serverAddress = (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).front();

    TopologyStateMachine stateMachine(kTwoSeedReplicaSetNoPrimaryConfig);
    auto topologyDescription =
        std::make_shared<TopologyDescription>(kTwoSeedReplicaSetNoPrimaryConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(serverAddress)
                                 .withType(ServerType::kRSSecondary)
                                 .withSetName(*topologyDescription->getSetName())
                                 .instance();

    ASSERT_EQUALS(static_cast<size_t>(2), topologyDescription->getServers().size());
    auto serversBefore = map(topologyDescription->getServers(), getServerDescriptionAddress);

    stateMachine.onServerDescription(*topologyDescription, serverDescription);

    auto serversAfter = map(topologyDescription->getServers(), getServerDescriptionAddress);
    ASSERT_EQUALS(adaptForAssert(serversBefore), adaptForAssert(serversAfter));
}

TEST_F(TopologyStateMachineTestFixture, ShouldRemoveServerDescriptionIfShardDoesntMatch) {
    const auto expectedRemovedServer = (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).front();
    const auto expectedRemainingServer = (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).back();

    TopologyStateMachine stateMachine(kTwoSeedReplicaSetNoPrimaryConfig);
    auto topologyDescription =
        std::make_shared<TopologyDescription>(kTwoSeedReplicaSetNoPrimaryConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(expectedRemovedServer)
                                 .withType(ServerType::kRSSecondary)
                                 .withSetName(*topologyDescription->getSetName())
                                 .instance();
    auto serverDescriptionWithBadSetName = ServerDescriptionBuilder()
                                               .withAddress(expectedRemovedServer)
                                               .withType(ServerType::kRSSecondary)
                                               .withSetName("wrong_name_should_not_match")
                                               .instance();

    ASSERT_EQUALS(static_cast<size_t>(2), topologyDescription->getServers().size());

    // First tests that description is not removed if set name is correct.
    stateMachine.onServerDescription(*topologyDescription, serverDescription);
    ASSERT_EQUALS(static_cast<size_t>(2), topologyDescription->getServers().size());

    // Tests that description is removed if the set name does not match.
    stateMachine.onServerDescription(*topologyDescription, serverDescriptionWithBadSetName);
    ASSERT_EQUALS(static_cast<size_t>(1), topologyDescription->getServers().size());
    ASSERT_EQUALS(expectedRemainingServer, topologyDescription->getServers().front()->getAddress());
}

TEST_F(TopologyStateMachineTestFixture,
       ShouldChangeStandaloneServerToUnknownAndPreserveTopologyType) {
    const auto primary = (*kTwoSeedConfig.getSeedList()).front();
    const auto otherMember = (*kTwoSeedConfig.getSeedList()).back();

    TopologyStateMachine stateMachine(kTwoSeedConfig);
    auto topologyDescription = std::make_shared<TopologyDescription>(kTwoSeedConfig);

    const auto primaryDescription = ServerDescriptionBuilder()
                                        .withAddress(primary)
                                        .withMe(primary)
                                        .withHost(primary)
                                        .withHost(otherMember)
                                        .withSetName(kReplicaSetName)
                                        .withType(ServerType::kRSPrimary)
                                        .withElectionId(kOidOne)
                                        .withSetVersion(100)
                                        .instance();
    stateMachine.onServerDescription(*topologyDescription, primaryDescription);
    ASSERT_EQUALS(topologyDescription->getType(), TopologyType::kReplicaSetWithPrimary);

    // Primary transforms to a standalone
    const auto standaloneDescription = ServerDescriptionBuilder()
                                           .withType(ServerType::kStandalone)
                                           .withMe(primary)
                                           .withAddress(primary)
                                           .withHost(primary)
                                           .instance();
    stateMachine.onServerDescription(*topologyDescription, standaloneDescription);

    ASSERT_EQUALS(topologyDescription->getType(), TopologyType::kReplicaSetNoPrimary);
    ASSERT_EQUALS(2, topologyDescription->getServers().size());

    const auto finalServerDescription = topologyDescription->findServerByAddress(primary);
    ASSERT(finalServerDescription);
    ASSERT_EQUALS(ServerType::kUnknown, (*finalServerDescription)->getType());
}

TEST_F(TopologyStateMachineTestFixture,
       ShouldNotRemoveNonPrimaryServerWhenTopologyIsReplicaSetWithPrimaryAndMeIsNotPresent) {
    const auto serverAddress = (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).front();
    const auto primaryAddress = (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).back();

    TopologyStateMachine stateMachine(kTwoSeedReplicaSetNoPrimaryConfig);
    auto topologyDescription =
        std::make_shared<TopologyDescription>(kTwoSeedReplicaSetNoPrimaryConfig);

    auto primaryDescription = ServerDescriptionBuilder()
                                  .withAddress(primaryAddress)
                                  .withMe(primaryAddress)
                                  .withHost(primaryAddress)
                                  .withHost(serverAddress)
                                  .withSetName(*topologyDescription->getSetName())
                                  .withType(ServerType::kRSPrimary)
                                  .withElectionId(kOidOne)
                                  .withSetVersion(100)
                                  .instance();

    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(serverAddress)
                                 .withType(ServerType::kRSSecondary)
                                 .withSetName(*topologyDescription->getSetName())
                                 .instance();

    // change topology type to ReplicaSetWithPrimary
    stateMachine.onServerDescription(*topologyDescription, primaryDescription);
    ASSERT_EQUALS(topologyDescription->getType(), TopologyType::kReplicaSetWithPrimary);
    ASSERT_EQUALS(static_cast<size_t>(2), topologyDescription->getServers().size());

    auto serversBefore = map(topologyDescription->getServers(), getServerDescriptionAddress);

    stateMachine.onServerDescription(*topologyDescription, serverDescription);
    ASSERT_EQUALS(adaptForAssert(topologyDescription->getType()),
                  adaptForAssert(TopologyType::kReplicaSetWithPrimary));

    auto serversAfter = map(topologyDescription->getServers(), getServerDescriptionAddress);
    ASSERT_EQUALS(adaptForAssert(serversBefore), adaptForAssert(serversAfter));
}

TEST_F(TopologyStateMachineTestFixture,
       ShouldRemoveNonPrimaryServerWhenTopologyIsReplicaSetNoPrimaryAndMeDoesntMatchAddress) {
    const auto serverAddress = (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).front();
    const auto expectedRemainingHostAndPort =
        (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).back();
    const auto me = HostAndPort(std::string("foo") + serverAddress.toString());

    TopologyStateMachine stateMachine(kTwoSeedReplicaSetNoPrimaryConfig);
    auto topologyDescription =
        std::make_shared<TopologyDescription>(kTwoSeedReplicaSetNoPrimaryConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(serverAddress)
                                 .withMe(me)
                                 .withType(ServerType::kRSSecondary)
                                 .instance();

    ASSERT_EQUALS(static_cast<size_t>(2), topologyDescription->getServers().size());
    stateMachine.onServerDescription(*topologyDescription, serverDescription);
    ASSERT_EQUALS(static_cast<size_t>(1), topologyDescription->getServers().size());
    ASSERT_EQUALS(expectedRemainingHostAndPort,
                  topologyDescription->getServers().front()->getAddress());
}

TEST_F(TopologyStateMachineTestFixture,
       ShouldAddServerDescriptionIfInHostsListButNotInTopologyDescription) {
    const auto primary = (*kTwoSeedConfig.getSeedList()).front();
    const auto secondary = (*kTwoSeedConfig.getSeedList()).back();
    const auto newHost = HostAndPort("newhost:123");

    TopologyStateMachine stateMachine(kTwoSeedConfig);
    auto topologyDescription = std::make_shared<TopologyDescription>(kTwoSeedConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withAddress(primary)
                                 .withType(ServerType::kRSPrimary)
                                 .withPrimary(primary)
                                 .withHost(primary)
                                 .withHost(secondary)
                                 .withHost(newHost)
                                 .withElectionId(kOidOne)
                                 .withSetVersion(100)
                                 .instance();

    ASSERT_EQUALS(static_cast<size_t>(2), topologyDescription->getServers().size());
    stateMachine.onServerDescription(*topologyDescription, serverDescription);
    ASSERT_EQUALS(static_cast<size_t>(3), topologyDescription->getServers().size());

    auto newHostResult = topologyDescription->findServerByAddress(newHost);
    ASSERT(newHostResult);
    ASSERT_EQUALS(newHost, (*newHostResult)->getAddress());
    ASSERT_EQUALS(ServerType::kUnknown, (*newHostResult)->getType());
}

TEST_F(TopologyStateMachineTestFixture, ShouldSaveNewMaxSetVersion) {
    const auto primary = (*kTwoSeedConfig.getSeedList()).front();

    TopologyStateMachine stateMachine(kTwoSeedConfig);
    auto topologyDescription = std::make_shared<TopologyDescription>(kTwoSeedConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withType(ServerType::kRSPrimary)
                                 .withPrimary(primary)
                                 .withMe(primary)
                                 .withAddress(primary)
                                 .withHost(primary)
                                 .withElectionId(kOidOne)
                                 .withSetVersion(100)
                                 .instance();

    stateMachine.onServerDescription(*topologyDescription, serverDescription);
    ASSERT_EQUALS(100, topologyDescription->getMaxElectionIdSetVersionPair().setVersion);

    auto serverDescriptionEvenBiggerSetVersion = ServerDescriptionBuilder()
                                                     .withType(ServerType::kRSPrimary)
                                                     .withPrimary(primary)
                                                     .withMe(primary)
                                                     .withAddress(primary)
                                                     .withHost(primary)
                                                     .withElectionId(kOidOne)
                                                     .withSetVersion(200)
                                                     .instance();

    stateMachine.onServerDescription(*topologyDescription, serverDescriptionEvenBiggerSetVersion);
    ASSERT_EQUALS(200, topologyDescription->getMaxElectionIdSetVersionPair().setVersion);
}

TEST_F(TopologyStateMachineTestFixture, ShouldSaveNewMaxElectionId) {
    const auto primary = (*kTwoSeedConfig.getSeedList()).front();
    auto topologyDescription = std::make_shared<TopologyDescription>(kTwoSeedConfig);
    TopologyStateMachine stateMachine(kTwoSeedConfig);

    auto serverDescription = ServerDescriptionBuilder()
                                 .withType(ServerType::kRSPrimary)
                                 .withPrimary(primary)
                                 .withMe(primary)
                                 .withAddress(primary)
                                 .withHost(primary)
                                 .withSetVersion(1)
                                 .withElectionId(kOidOne)
                                 .instance();

    stateMachine.onServerDescription(*topologyDescription, serverDescription);
    ASSERT_EQUALS(kOidOne, topologyDescription->getMaxElectionIdSetVersionPair().electionId);

    auto serverDescriptionEvenBiggerElectionId = ServerDescriptionBuilder()
                                                     .withType(ServerType::kRSPrimary)
                                                     .withPrimary(primary)
                                                     .withMe(primary)
                                                     .withAddress(primary)
                                                     .withHost(primary)
                                                     .withSetVersion(1)
                                                     .withElectionId(kOidTwo)
                                                     .instance();

    stateMachine.onServerDescription(*topologyDescription, serverDescriptionEvenBiggerElectionId);
    ASSERT_EQUALS(kOidTwo, topologyDescription->getMaxElectionIdSetVersionPair().electionId);
}

// The following two tests (ShouldNotUpdateToplogyType, ShouldUpdateToCorrectToplogyType) assert
// that the topology type is correct given an initial state and a ServerType. Together, they
// cover all the cases specified in the SDAM spec here:
// https://github.com/mongodb/specifications/blob/master/source/server-discovery-and-monitoring/server-discovery-and-monitoring.rst#topologytype-table

TEST_F(TopologyStateMachineTestFixture, ShouldNotUpdateToplogyType) {
    using T = TopologyTypeTestCase;

    // test cases that should not change TopologyType
    std::vector<TopologyTypeTestCase> testCases{
        T{kTwoSeedConfig, TopologyType::kUnknown, ServerType::kUnknown, TopologyType::kUnknown},
        T{kTwoSeedConfig, TopologyType::kUnknown, ServerType::kStandalone, TopologyType::kUnknown},
        T{kTwoSeedConfig, TopologyType::kUnknown, ServerType::kRSGhost, TopologyType::kUnknown},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetNoPrimary,
          ServerType::kUnknown,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetNoPrimary,
          ServerType::kUnknown,
          TopologyType::kReplicaSetNoPrimary},
    };
    for (auto serverType : allServerTypes()) {
        testCases.push_back(
            T{kTwoSeedConfig, TopologyType::kSharded, serverType, TopologyType::kSharded});
    }

    const auto& allExceptPrimary = allServerTypesExceptPrimary();
    for (auto serverType : allExceptPrimary) {
        testCases.push_back(T{kTwoSeedConfig,
                              TopologyType::kReplicaSetNoPrimary,
                              serverType,
                              TopologyType::kReplicaSetNoPrimary});
    }

    int count = 0;
    for (auto testCase : testCases) {
        std::cout << "case " << ++count << " starting TopologyType: " << toString(testCase.starting)
                  << "; incoming ServerType: " << toString(testCase.incoming)
                  << "; expect ending TopologyType: " << toString(testCase.ending) << std::endl;

        assertTopologyTypeTestCase(testCase);
    }
}

TEST_F(TopologyStateMachineTestFixture, ShouldUpdateToCorrectToplogyType) {
    using T = TopologyTypeTestCase;

    // test cases that should change TopologyType
    const std::vector<TopologyTypeTestCase> testCases{
        T{kTwoSeedConfig, TopologyType::kUnknown, ServerType::kMongos, TopologyType::kSharded},
        T{kTwoSeedConfig,
          TopologyType::kUnknown,
          ServerType::kRSPrimary,
          TopologyType::kReplicaSetWithPrimary},
        T{kTwoSeedConfig,
          TopologyType::kUnknown,
          ServerType::kRSSecondary,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kUnknown,
          ServerType::kRSArbiter,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kUnknown,
          ServerType::kRSOther,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetNoPrimary,
          ServerType::kRSPrimary,
          TopologyType::kReplicaSetWithPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kUnknown,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kStandalone,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kMongos,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kRSPrimary,
          TopologyType::kReplicaSetWithPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kRSSecondary,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kRSOther,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kRSArbiter,
          TopologyType::kReplicaSetNoPrimary},
        T{kTwoSeedConfig,
          TopologyType::kReplicaSetWithPrimary,
          ServerType::kRSGhost,
          TopologyType::kReplicaSetNoPrimary}};

    int count = 0;
    for (auto testCase : testCases) {
        std::cout << "case " << ++count << " starting TopologyType: " << toString(testCase.starting)
                  << "; incoming ServerType: " << toString(testCase.incoming)
                  << "; expect ending TopologyType: " << toString(testCase.ending) << std::endl;

        assertTopologyTypeTestCase(testCase);
    }
}

TEST_F(TopologyStateMachineTestFixture, ShouldMarkStalePrimaryAsUnknown) {
    const auto freshPrimary = (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).front();
    const auto stalePrimary = (*kTwoSeedReplicaSetNoPrimaryConfig.getSeedList()).back();

    TopologyStateMachine stateMachine(kTwoSeedReplicaSetNoPrimaryConfig);
    auto topologyDescription =
        std::make_shared<TopologyDescription>(kTwoSeedReplicaSetNoPrimaryConfig);

    auto freshServerDescription = ServerDescriptionBuilder()
                                      .withType(ServerType::kRSPrimary)
                                      .withSetName(*topologyDescription->getSetName())
                                      .withPrimary(freshPrimary)
                                      .withMe(freshPrimary)
                                      .withAddress(freshPrimary)
                                      .withHost(freshPrimary)
                                      .withHost(stalePrimary)
                                      .withSetVersion(1)
                                      .withElectionId(kOidTwo)
                                      .instance();

    auto secondaryServerDescription = ServerDescriptionBuilder()
                                          .withAddress(stalePrimary)
                                          .withType(ServerType::kRSSecondary)
                                          .withSetName(*topologyDescription->getSetName())
                                          .instance();

    stateMachine.onServerDescription(*topologyDescription, freshServerDescription);
    stateMachine.onServerDescription(*topologyDescription, secondaryServerDescription);

    ASSERT_EQUALS(topologyDescription->getType(), TopologyType::kReplicaSetWithPrimary);
    ASSERT_EQUALS((*topologyDescription->getPrimary())->getAddress(), freshPrimary);

    auto secondaryServer = topologyDescription->findServerByAddress(stalePrimary);
    ASSERT_EQUALS((*secondaryServer)->getType(), ServerType::kRSSecondary);

    auto staleServerDescription = ServerDescriptionBuilder()
                                      .withType(ServerType::kRSPrimary)
                                      .withSetName(*topologyDescription->getSetName())
                                      .withPrimary(stalePrimary)
                                      .withMe(stalePrimary)
                                      .withAddress(stalePrimary)
                                      .withHost(stalePrimary)
                                      .withHost(freshPrimary)
                                      .withSetVersion(1)
                                      .withElectionId(kOidOne)
                                      .instance();

    stateMachine.onServerDescription(*topologyDescription, staleServerDescription);

    ASSERT_EQUALS(topologyDescription->getType(), TopologyType::kReplicaSetWithPrimary);
    ASSERT(topologyDescription->getPrimary());
    ASSERT_EQUALS((*topologyDescription->getPrimary())->getAddress(), freshPrimary);

    auto staleServer = topologyDescription->findServerByAddress(stalePrimary);
    ASSERT_EQUALS((*staleServer)->getType(), ServerType::kUnknown);
}
}  // namespace mongo::sdam
