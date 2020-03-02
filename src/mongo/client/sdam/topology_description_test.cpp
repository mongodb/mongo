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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kDefault

#include "mongo/client/sdam/sdam_test_base.h"
#include "mongo/client/sdam/topology_description.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/db/wire_version.h"
#include "mongo/logv2/log.h"
#include "mongo/unittest/death_test.h"

namespace mongo {
template std::ostream& operator<<(std::ostream& os,
                                  const std::vector<mongo::sdam::ServerAddress>& s);

bool operator==(const TopologyVersion& a, const TopologyVersion& b) {
    return a.getProcessId() == b.getProcessId() && a.getCounter() == b.getCounter();
}

namespace sdam {
using mongo::operator<<;


class TopologyDescriptionTestFixture : public SdamTestFixture {
protected:
    void assertDefaultConfig(const TopologyDescription& topologyDescription);

    static inline const auto kSetName = std::string("mySetName");

    static inline const std::vector<ServerAddress> kOneServer{"foo:1234"};
    static inline const std::vector<ServerAddress> kTwoServersVaryCase{"FoO:1234", "BaR:1234"};
    static inline const std::vector<ServerAddress> kTwoServersNormalCase{"foo:1234", "bar:1234"};
    static inline const std::vector<ServerAddress> kThreeServers{
        "foo:1234", "bar:1234", "baz:1234"};

    static inline const auto kDefaultConfig = SdamConfiguration();
    static inline const auto kSingleSeedConfig =
        SdamConfiguration(kOneServer, TopologyType::kSingle);
};

void TopologyDescriptionTestFixture::assertDefaultConfig(
    const TopologyDescription& topologyDescription) {
    ASSERT_EQUALS(boost::none, topologyDescription.getSetName());
    ASSERT_EQUALS(boost::none, topologyDescription.getMaxElectionId());

    auto expectedDefaultServer = ServerDescription("localhost:27017");
    ASSERT_EQUALS(expectedDefaultServer, *topologyDescription.getServers().front());
    ASSERT_EQUALS(static_cast<std::size_t>(1), topologyDescription.getServers().size());

    ASSERT_EQUALS(true, topologyDescription.isWireVersionCompatible());
    ASSERT_EQUALS(boost::none, topologyDescription.getWireVersionCompatibleError());
    ASSERT_EQUALS(boost::none, topologyDescription.getLogicalSessionTimeoutMinutes());
}

TEST_F(TopologyDescriptionTestFixture, ShouldHaveCorrectDefaultValues) {
    assertDefaultConfig(TopologyDescription(kDefaultConfig));
    assertDefaultConfig(TopologyDescription());
}

// Disable this test since this causes failures in jstests running on
// hosts with mixed case hostnames.
// TEST_F(TopologyDescriptionTestFixture, ShouldNormalizeInitialSeedList) {
//    auto config = SdamConfiguration(kTwoServersVaryCase);
//    TopologyDescription topologyDescription(config);
//
//    auto expectedAddresses = kTwoServersNormalCase;
//
//    auto serverAddresses = map<ServerDescriptionPtr, ServerAddress>(
//        topologyDescription.getServers(),
//        [](const ServerDescriptionPtr& description) { return description->getAddress(); });
//
//    ASSERT_EQUALS(expectedAddresses, serverAddresses);
//}

TEST_F(TopologyDescriptionTestFixture, ShouldAllowTypeSingleWithASingleSeed) {
    TopologyDescription topologyDescription(kSingleSeedConfig);

    ASSERT(TopologyType::kSingle == topologyDescription.getType());

    auto servers = map<ServerDescriptionPtr, ServerAddress>(
        topologyDescription.getServers(),
        [](const ServerDescriptionPtr& desc) { return desc->getAddress(); });
    ASSERT_EQUALS(kOneServer, servers);
}

TEST_F(TopologyDescriptionTestFixture, DoesNotAllowMultipleSeedsWithSingle) {
    ASSERT_THROWS_CODE(
        TopologyDescription(SdamConfiguration(kTwoServersNormalCase, TopologyType::kSingle)),
        DBException,
        ErrorCodes::InvalidSeedList);
}

TEST_F(TopologyDescriptionTestFixture, ShouldSetTheReplicaSetName) {
    auto expectedSetName = kSetName;
    auto config = SdamConfiguration(
        kOneServer, TopologyType::kReplicaSetNoPrimary, mongo::Seconds(10), expectedSetName);
    TopologyDescription topologyDescription(config);
    ASSERT_EQUALS(expectedSetName, *topologyDescription.getSetName());
}

TEST_F(TopologyDescriptionTestFixture, ShouldNotAllowSettingTheReplicaSetNameWithWrongType) {
    ASSERT_THROWS_CODE(TopologyDescription(SdamConfiguration(
                           kOneServer, TopologyType::kUnknown, mongo::Seconds(10), kSetName)),
                       DBException,
                       ErrorCodes::InvalidTopologyType);
}

TEST_F(TopologyDescriptionTestFixture, ShouldNotAllowTopologyTypeRSNoPrimaryWithoutSetName) {
    ASSERT_THROWS_CODE(
        SdamConfiguration(
            kOneServer, TopologyType::kReplicaSetNoPrimary, mongo::Seconds(10), boost::none),
        DBException,
        ErrorCodes::TopologySetNameRequired);
}

TEST_F(TopologyDescriptionTestFixture, ShouldOnlyAllowSingleAndRsNoPrimaryWithSetName) {
    auto topologyTypes = allTopologyTypes();
    topologyTypes.erase(std::remove_if(topologyTypes.begin(),
                                       topologyTypes.end(),
                                       [](const TopologyType& topologyType) {
                                           return topologyType == TopologyType::kSingle ||
                                               topologyType == TopologyType::kReplicaSetNoPrimary;
                                       }),
                        topologyTypes.end());

    for (const auto topologyType : topologyTypes) {
        LOGV2(20217,
              "Check TopologyType {topologyType} with setName value.",
              "Check TopologyType with setName value",
              "topologyType"_attr = topologyType);
        ASSERT_THROWS_CODE(
            SdamConfiguration(kOneServer, topologyType, mongo::Seconds(10), kSetName),
            DBException,
            ErrorCodes::InvalidTopologyType);
    }
}

TEST_F(TopologyDescriptionTestFixture, ShouldDefaultHeartbeatToTenSecs) {
    SdamConfiguration config;
    ASSERT_EQUALS(SdamConfiguration::kDefaultHeartbeatFrequencyMs, config.getHeartBeatFrequency());
}

TEST_F(TopologyDescriptionTestFixture, ShouldAllowSettingTheHeartbeatFrequency) {
    const auto expectedHeartbeatFrequency = mongo::Milliseconds(20000);
    SdamConfiguration config(boost::none, TopologyType::kUnknown, expectedHeartbeatFrequency);
    ASSERT_EQUALS(expectedHeartbeatFrequency, config.getHeartBeatFrequency());
}

TEST_F(TopologyDescriptionTestFixture, ShouldNotAllowChangingTheHeartbeatFrequencyBelow500Ms) {
    auto belowThresholdFrequency =
        mongo::Milliseconds(SdamConfiguration::kMinHeartbeatFrequencyMS.count() - 1);
    ASSERT_THROWS_CODE(
        SdamConfiguration(boost::none, TopologyType::kUnknown, belowThresholdFrequency),
        DBException,
        ErrorCodes::InvalidHeartBeatFrequency);
}

TEST_F(TopologyDescriptionTestFixture,
       ShouldSetWireCompatibilityErrorForMinWireVersionWhenMinWireVersionIsGreater) {
    const auto outgoingMaxWireVersion = WireSpec::instance().outgoing.maxWireVersion;
    const auto config = SdamConfiguration(kOneServer, TopologyType::kUnknown, mongo::Seconds(10));
    const auto topologyDescription = std::make_shared<TopologyDescription>(config);
    const auto serverDescriptionMinVersion = ServerDescriptionBuilder()
                                                 .withAddress(kOneServer[0])
                                                 .withMe(kOneServer[0])
                                                 .withType(ServerType::kRSSecondary)
                                                 .withMinWireVersion(outgoingMaxWireVersion + 1)
                                                 .instance();

    ASSERT_EQUALS(boost::none, topologyDescription->getWireVersionCompatibleError());
    topologyDescription->installServerDescription(serverDescriptionMinVersion);
    ASSERT_NOT_EQUALS(boost::none, topologyDescription->getWireVersionCompatibleError());
}

TEST_F(TopologyDescriptionTestFixture,
       ShouldSetWireCompatibilityErrorForMinWireVersionWhenMaxWireVersionIsLess) {
    const auto outgoingMinWireVersion = WireSpec::instance().outgoing.minWireVersion;
    const auto config = SdamConfiguration(kOneServer, TopologyType::kUnknown, mongo::Seconds(10));
    const auto topologyDescription = std::make_shared<TopologyDescription>(config);
    const auto serverDescriptionMaxVersion = ServerDescriptionBuilder()
                                                 .withAddress(kOneServer[0])
                                                 .withMe(kOneServer[0])
                                                 .withType(ServerType::kRSSecondary)
                                                 .withMaxWireVersion(outgoingMinWireVersion - 1)
                                                 .instance();

    ASSERT_EQUALS(boost::none, topologyDescription->getWireVersionCompatibleError());
    topologyDescription->installServerDescription(serverDescriptionMaxVersion);
    ASSERT_NOT_EQUALS(boost::none, topologyDescription->getWireVersionCompatibleError());
}

TEST_F(TopologyDescriptionTestFixture, ShouldNotSetWireCompatibilityErrorWhenServerTypeIsUnknown) {
    const auto outgoingMinWireVersion = WireSpec::instance().outgoing.minWireVersion;
    const auto config = SdamConfiguration(kOneServer, TopologyType::kUnknown, mongo::Seconds(10));
    const auto topologyDescription = std::make_shared<TopologyDescription>(config);
    const auto serverDescriptionMaxVersion =
        ServerDescriptionBuilder().withMaxWireVersion(outgoingMinWireVersion - 1).instance();

    ASSERT_EQUALS(boost::none, topologyDescription->getWireVersionCompatibleError());
    topologyDescription->installServerDescription(serverDescriptionMaxVersion);
    ASSERT_EQUALS(boost::none, topologyDescription->getWireVersionCompatibleError());
}

TEST_F(TopologyDescriptionTestFixture, ShouldSetLogicalSessionTimeoutToMinOfAllServerDescriptions) {
    const auto config = SdamConfiguration(kThreeServers);
    const auto topologyDescription = std::make_shared<TopologyDescription>(config);

    const auto logicalSessionTimeouts = std::vector{300, 100, 200};
    auto timeoutIt = logicalSessionTimeouts.begin();
    const auto serverDescriptionsWithTimeouts = map<ServerDescriptionPtr, ServerDescriptionPtr>(
        topologyDescription->getServers(), [&timeoutIt](const ServerDescriptionPtr& description) {
            auto newInstanceBuilder = ServerDescriptionBuilder()
                                          .withType(ServerType::kRSSecondary)
                                          .withAddress(description->getAddress())
                                          .withMe(description->getAddress())
                                          .withLogicalSessionTimeoutMinutes(*timeoutIt);
            timeoutIt++;
            return newInstanceBuilder.instance();
        });

    for (auto description : serverDescriptionsWithTimeouts) {
        topologyDescription->installServerDescription(description);
    }

    int expectedLogicalSessionTimeout =
        *std::min_element(logicalSessionTimeouts.begin(), logicalSessionTimeouts.end());
    ASSERT_EQUALS(expectedLogicalSessionTimeout,
                  topologyDescription->getLogicalSessionTimeoutMinutes());
}


TEST_F(TopologyDescriptionTestFixture,
       ShouldSetLogicalSessionTimeoutToNoneIfAnyServerDescriptionHasNone) {
    const auto config = SdamConfiguration(kThreeServers);
    const auto topologyDescription = std::make_shared<TopologyDescription>(config);

    const auto logicalSessionTimeouts = std::vector{300, 100, 200};
    auto timeoutIt = logicalSessionTimeouts.begin();

    const auto serverDescriptionsWithTimeouts = map<ServerDescriptionPtr, ServerDescriptionPtr>(
        topologyDescription->getServers(), [&](const ServerDescriptionPtr& description) {
            auto timeoutValue = (timeoutIt == logicalSessionTimeouts.begin())
                ? boost::none
                : boost::make_optional(*timeoutIt);

            auto newInstance = ServerDescriptionBuilder()
                                   .withType(ServerType::kRSSecondary)
                                   .withAddress(description->getAddress())
                                   .withMe(description->getAddress())
                                   .withLogicalSessionTimeoutMinutes(timeoutValue)
                                   .instance();
            ++timeoutIt;
            return newInstance;
        });

    for (auto description : serverDescriptionsWithTimeouts) {
        topologyDescription->installServerDescription(description);
    }

    ASSERT_EQUALS(boost::none, topologyDescription->getLogicalSessionTimeoutMinutes());
}

TEST_F(TopologyDescriptionTestFixture, ShouldUpdateTopologyVersionOnSuccess) {
    const auto config = SdamConfiguration(kThreeServers);
    const auto topologyDescription = std::make_shared<TopologyDescription>(config);

    // Deafult topologyVersion is null
    ASSERT_EQUALS(topologyDescription->getServers().size(), 3);
    auto serverDescription = topologyDescription->getServers()[1];
    ASSERT(serverDescription->getTopologyVersion() == boost::none);

    // Create new serverDescription with topologyVersion, topologyDescription should have the new
    // topologyVersion
    auto processId = OID("000000000000000000000001");
    auto newDescription = ServerDescriptionBuilder()
                              .withType(ServerType::kRSSecondary)
                              .withAddress(serverDescription->getAddress())
                              .withMe(serverDescription->getAddress())
                              .withTopologyVersion(TopologyVersion(processId, 1))
                              .instance();

    topologyDescription->installServerDescription(newDescription);
    ASSERT_EQUALS(topologyDescription->getServers().size(), 3);
    auto topologyVersion = topologyDescription->getServers()[1]->getTopologyVersion();
    ASSERT(topologyVersion == TopologyVersion(processId, 1));
}

TEST_F(TopologyDescriptionTestFixture, ShouldNotUpdateTopologyVersionOnError) {
    const auto config = SdamConfiguration(kThreeServers);
    const auto topologyDescription = std::make_shared<TopologyDescription>(config);

    // Deafult topologyVersion is null
    ASSERT_EQUALS(topologyDescription->getServers().size(), 3);
    auto serverDescription = topologyDescription->getServers()[1];
    ASSERT(serverDescription->getTopologyVersion() == boost::none);

    auto newDescription = ServerDescriptionBuilder()
                              .withAddress(serverDescription->getAddress())
                              .withError("error")
                              .instance();

    topologyDescription->installServerDescription(newDescription);
    ASSERT_EQUALS(topologyDescription->getServers().size(), 3);
    auto topologyVersion = topologyDescription->getServers()[1]->getTopologyVersion();
    ASSERT(topologyVersion == boost::none);
}
};  // namespace sdam
};  // namespace mongo
