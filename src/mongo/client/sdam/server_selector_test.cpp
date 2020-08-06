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
#include "mongo/client/sdam/server_selector.h"

#include <boost/optional/optional_io.hpp>

#include "mongo/client/sdam/sdam_test_base.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/sdam/topology_manager.h"
#include "mongo/db/wire_version.h"
#include "mongo/util/system_clock_source.h"

namespace mongo::sdam {

class ServerSelectorTestFixture : public SdamTestFixture {
public:
    static inline const auto clockSource = SystemClockSource::get();
    static inline const auto sdamConfiguration = SdamConfiguration({{HostAndPort("s0")}});

    static constexpr auto SET_NAME = "set";
    static constexpr int NUM_ITERATIONS = 1000;

    struct TagSets {
        static inline const auto eastProduction = BSON("dc"
                                                       << "east"
                                                       << "usage"
                                                       << "production");
        static inline const auto westProduction = BSON("dc"
                                                       << "west"
                                                       << "usage"
                                                       << "production");
        static inline const auto northTest = BSON("dc"
                                                  << "north"
                                                  << "usage"
                                                  << "test");
        static inline const auto northProduction = BSON("dc"
                                                        << "north"
                                                        << "usage"
                                                        << "production");
        static inline const auto production = BSON("usage"
                                                   << "production");

        static inline const auto test = BSON("usage"
                                             << "test");

        static inline const auto integration = BSON("usage"
                                                    << "integration");

        static inline const auto primary = BSON("tag"
                                                << "primary");
        static inline const auto secondary = BSON("tag"
                                                  << "secondary");

        static inline const auto emptySet = TagSet{BSONArray(BSONObj())};
        static inline const auto eastOrWestProductionSet =
            TagSet(BSON_ARRAY(eastProduction << westProduction));
        static inline const auto westProductionSet = TagSet(BSON_ARRAY(westProduction));
        static inline const auto productionSet = TagSet(BSON_ARRAY(production));
        static inline const auto testSet = TagSet(BSON_ARRAY(test));
        static inline const auto integrationOrTestSet = TagSet(BSON_ARRAY(integration << test));
        static inline const auto integrationSet = TagSet(BSON_ARRAY(integration));

        static inline const auto primarySet = TagSet(BSON_ARRAY(primary));
        static inline const auto secondarySet = TagSet(BSON_ARRAY(secondary));
    };

    static ServerDescriptionPtr make_with_latency(IsMasterRTT latency,
                                                  HostAndPort address,
                                                  ServerType serverType = ServerType::kRSPrimary,
                                                  std::map<std::string, std::string> tags = {}) {
        auto builder = ServerDescriptionBuilder()
                           .withType(serverType)
                           .withAddress(address)
                           .withSetName(SET_NAME)
                           .withRtt(latency)
                           .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                           .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                           .withLastWriteDate(Date_t::now())
                           .withLastUpdateTime(Date_t::now());

        for (auto it = tags.begin(); it != tags.end(); ++it) {
            builder.withTag(it->first, it->second);
        }

        return builder.instance();
    }

    static auto makeServerDescriptionList() {
        return std::vector<ServerDescriptionPtr>{
            make_with_latency(Milliseconds(1),
                              HostAndPort("s1"),
                              ServerType::kRSSecondary,
                              {{"dc", "east"}, {"usage", "production"}}),
            make_with_latency(Milliseconds(1),
                              HostAndPort("s1-test"),
                              ServerType::kRSSecondary,
                              {{"dc", "east"}, {"usage", "test"}}),
            make_with_latency(Milliseconds(1),
                              HostAndPort("s2"),
                              ServerType::kRSSecondary,
                              {{"dc", "west"}, {"usage", "production"}}),
            make_with_latency(Milliseconds(1),
                              HostAndPort("s2-test"),
                              ServerType::kRSSecondary,
                              {{"dc", "west"}, {"usage", "test"}}),
            make_with_latency(Milliseconds(1),
                              HostAndPort("s3"),
                              ServerType::kRSSecondary,
                              {{"dc", "north"}, {"usage", "production"}})};
    };

    SdamServerSelector selector = SdamServerSelector(sdamConfiguration);
};

TEST_F(ServerSelectorTestFixture, ShouldFilterCorrectlyByLatencyWindow) {
    const auto delta = Milliseconds(10);
    const auto windowWidth = Milliseconds(100);
    const auto lowerBound = Milliseconds(100);

    auto window = LatencyWindow(lowerBound, windowWidth);

    std::vector<ServerDescriptionPtr> servers = {
        make_with_latency(window.lower - delta, HostAndPort("less")),
        make_with_latency(window.lower, HostAndPort("boundary-lower")),
        make_with_latency(window.lower + delta, HostAndPort("within")),
        make_with_latency(window.upper, HostAndPort("boundary-upper")),
        make_with_latency(window.upper + delta, HostAndPort("greater"))};

    window.filterServers(&servers);

    ASSERT_EQ(3, servers.size());
    ASSERT_EQ(HostAndPort("boundary-lower"), servers[0]->getAddress());
    ASSERT_EQ(HostAndPort("within"), servers[1]->getAddress());
    ASSERT_EQ(HostAndPort("boundary-upper"), servers[2]->getAddress());
}

TEST_F(ServerSelectorTestFixture, ShouldThrowOnWireError) {
    auto topologyDescription = std::make_shared<TopologyDescription>(sdamConfiguration);
    auto oldServer = ServerDescriptionBuilder()
                         .withAddress(topologyDescription->getServers().back()->getAddress())
                         .withType(ServerType::kRSPrimary)
                         .withMaxWireVersion(WireVersion::RELEASE_2_4_AND_BEFORE)
                         .withMinWireVersion(WireVersion::RELEASE_2_4_AND_BEFORE)
                         .instance();
    topologyDescription->installServerDescription(oldServer);

    ASSERT(!topologyDescription->isWireVersionCompatible());
    ASSERT_THROWS_CODE(selector.selectServers(topologyDescription, ReadPreferenceSetting()),
                       DBException,
                       ErrorCodes::IncompatibleServerVersion);
}

TEST_F(ServerSelectorTestFixture, ShouldReturnNoneIfTopologyUnknown) {
    auto topologyDescription = std::make_shared<TopologyDescription>(sdamConfiguration);
    ASSERT_EQ(TopologyType::kUnknown, topologyDescription->getType());
    ASSERT_EQ(boost::none, selector.selectServers(topologyDescription, ReadPreferenceSetting()));
}

TEST_F(ServerSelectorTestFixture, ShouldBeAbleToSelectWithMaxStalenessFromClonedTopology) {
    TopologyStateMachine stateMachine(sdamConfiguration);
    auto topologyDescription = std::make_shared<TopologyDescription>(sdamConfiguration);
    auto primary = ServerDescriptionBuilder()
                       .withAddress(HostAndPort("s0"))
                       .withType(ServerType::kRSPrimary)
                       .withLastUpdateTime(Date_t::now())
                       .withLastWriteDate(Date_t::now())
                       .withRtt(Milliseconds{1})
                       .withSetName("set")
                       .withHost(HostAndPort("s0"))
                       .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                       .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                       .instance();
    stateMachine.onServerDescription(*topologyDescription, primary);
    topologyDescription = TopologyDescription::clone(topologyDescription);

    const auto ninetySeconds = Seconds(90);
    const auto readPref =
        ReadPreferenceSetting(ReadPreference::Nearest, TagSets::emptySet, ninetySeconds);
    auto result = selector.selectServers(topologyDescription, readPref);
    ASSERT(result);
    ASSERT_EQ(primary->getAddress(), (*result)[0]->getAddress());
}

TEST_F(ServerSelectorTestFixture, ShouldSelectRandomlyWhenMultipleOptionsAreAvailable) {
    TopologyStateMachine stateMachine(sdamConfiguration);
    auto topologyDescription = std::make_shared<TopologyDescription>(sdamConfiguration);

    const auto s0Latency = Milliseconds(1);
    auto primary = ServerDescriptionBuilder()
                       .withAddress(HostAndPort("s0"))
                       .withType(ServerType::kRSPrimary)
                       .withLastUpdateTime(Date_t::now())
                       .withLastWriteDate(Date_t::now())
                       .withRtt(s0Latency)
                       .withSetName("set")
                       .withHost(HostAndPort("s0"))
                       .withHost(HostAndPort("s1"))
                       .withHost(HostAndPort("s2"))
                       .withHost(HostAndPort("s3"))
                       .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                       .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                       .instance();
    stateMachine.onServerDescription(*topologyDescription, primary);

    const auto s1Latency = Milliseconds((s0Latency + sdamConfiguration.getLocalThreshold()) / 2);
    auto secondaryInLatencyWindow =
        make_with_latency(s1Latency, HostAndPort("s1"), ServerType::kRSSecondary);
    stateMachine.onServerDescription(*topologyDescription, secondaryInLatencyWindow);

    // s2 is on the boundary of the latency window
    const auto s2Latency = s0Latency + sdamConfiguration.getLocalThreshold();
    auto secondaryOnBoundaryOfLatencyWindow =
        make_with_latency(s2Latency, HostAndPort("s2"), ServerType::kRSSecondary);
    stateMachine.onServerDescription(*topologyDescription, secondaryOnBoundaryOfLatencyWindow);

    // s3 should not be selected
    const auto s3Latency = s2Latency + Milliseconds(10);
    auto secondaryTooFar =
        make_with_latency(s3Latency, HostAndPort("s3"), ServerType::kRSSecondary);
    stateMachine.onServerDescription(*topologyDescription, secondaryTooFar);

    std::map<HostAndPort, int> frequencyInfo{{HostAndPort("s0"), 0},
                                             {HostAndPort("s1"), 0},
                                             {HostAndPort("s2"), 0},
                                             {HostAndPort("s3"), 0}};
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        auto server = selector.selectServer(topologyDescription,
                                            ReadPreferenceSetting(ReadPreference::Nearest));
        if (server) {
            frequencyInfo[(*server)->getAddress()]++;
        }
    }

    ASSERT(frequencyInfo[HostAndPort("s0")]);
    ASSERT(frequencyInfo[HostAndPort("s1")]);
    ASSERT(frequencyInfo[HostAndPort("s2")]);
    ASSERT_FALSE(frequencyInfo[HostAndPort("s3")]);
}

TEST_F(ServerSelectorTestFixture, ShouldFilterByLastWriteTime) {
    TopologyStateMachine stateMachine(sdamConfiguration);
    auto topologyDescription = std::make_shared<TopologyDescription>(sdamConfiguration);

    const int MAX_STALENESS = 90;
    const auto ninetySeconds = Seconds(MAX_STALENESS);
    const auto now = Date_t::now();


    const auto d0 = now - Milliseconds(1000);
    const auto s0 = ServerDescriptionBuilder()
                        .withAddress(HostAndPort("s0"))
                        .withType(ServerType::kRSPrimary)
                        .withRtt(sdamConfiguration.getLocalThreshold())
                        .withSetName("set")
                        .withHost(HostAndPort("s0"))
                        .withHost(HostAndPort("s1"))
                        .withHost(HostAndPort("s2"))
                        .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                        .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                        .withLastUpdateTime(now)
                        .withLastWriteDate(d0)
                        .instance();
    stateMachine.onServerDescription(*topologyDescription, s0);

    const auto d1 = now - Milliseconds(1000 * 5);
    const auto s1 = ServerDescriptionBuilder()
                        .withAddress(HostAndPort("s1"))
                        .withType(ServerType::kRSSecondary)
                        .withRtt(sdamConfiguration.getLocalThreshold())
                        .withSetName("set")
                        .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                        .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                        .withLastUpdateTime(now)
                        .withLastWriteDate(d1)
                        .instance();
    stateMachine.onServerDescription(*topologyDescription, s1);

    // d2 is stale, so s2 should not be selected.
    const auto d2 = now - ninetySeconds - ninetySeconds;
    const auto s2 = ServerDescriptionBuilder()
                        .withAddress(HostAndPort("s2"))
                        .withType(ServerType::kRSSecondary)
                        .withRtt(sdamConfiguration.getLocalThreshold())
                        .withSetName("set")
                        .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                        .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                        .withLastUpdateTime(now)
                        .withLastWriteDate(d2)
                        .instance();
    stateMachine.onServerDescription(*topologyDescription, s2);

    const auto readPref =
        ReadPreferenceSetting(ReadPreference::Nearest, TagSets::emptySet, ninetySeconds);

    std::map<HostAndPort, int> frequencyInfo{
        {HostAndPort("s0"), 0}, {HostAndPort("s1"), 0}, {HostAndPort("s2"), 0}};
    for (int i = 0; i < NUM_ITERATIONS; i++) {
        auto server = selector.selectServer(topologyDescription, readPref);

        if (server) {
            frequencyInfo[(*server)->getAddress()]++;
        }
    }

    ASSERT(frequencyInfo[HostAndPort("s0")]);
    ASSERT(frequencyInfo[HostAndPort("s1")]);
    ASSERT_FALSE(frequencyInfo[HostAndPort("s2")]);
}

TEST_F(ServerSelectorTestFixture, ShouldSelectPreferredIfAvailable) {
    TopologyStateMachine stateMachine(sdamConfiguration);
    auto topologyDescription = std::make_shared<TopologyDescription>(sdamConfiguration);

    const auto now = Date_t::now();


    const auto d0 = now - Milliseconds(1000);
    const auto s0 = ServerDescriptionBuilder()
                        .withAddress(HostAndPort("s0"))
                        .withType(ServerType::kRSPrimary)
                        .withRtt(sdamConfiguration.getLocalThreshold())
                        .withSetName("set")
                        .withHost(HostAndPort("s0"))
                        .withHost(HostAndPort("s1"))
                        .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                        .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                        .withLastWriteDate(d0)
                        .withTag("tag", "primary")
                        .instance();
    stateMachine.onServerDescription(*topologyDescription, s0);

    const auto s1 = ServerDescriptionBuilder()
                        .withAddress(HostAndPort("s1"))
                        .withType(ServerType::kRSSecondary)
                        .withRtt(sdamConfiguration.getLocalThreshold())
                        .withSetName("set")
                        .withHost(HostAndPort("s0"))
                        .withHost(HostAndPort("s1"))
                        .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                        .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                        .withLastWriteDate(d0)
                        .withTag("tag", "secondary")
                        .instance();
    stateMachine.onServerDescription(*topologyDescription, s1);

    const auto primaryPreferredTagSecondary =
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred, TagSets::secondarySet);
    auto result1 = selector.selectServer(topologyDescription, primaryPreferredTagSecondary);
    ASSERT(result1 != boost::none);
    ASSERT_EQ(HostAndPort("s0"), (*result1)->getAddress());

    const auto secondaryPreferredWithTag =
        ReadPreferenceSetting(ReadPreference::SecondaryPreferred, TagSets::secondarySet);
    auto result2 = selector.selectServer(topologyDescription, secondaryPreferredWithTag);
    ASSERT(result2 != boost::none);
    ASSERT_EQ(HostAndPort("s1"), (*result2)->getAddress());

    const auto secondaryPreferredNoTag = ReadPreferenceSetting(ReadPreference::SecondaryPreferred);
    auto result3 = selector.selectServer(topologyDescription, secondaryPreferredNoTag);
    ASSERT(result3 != boost::none);
    ASSERT_EQ(HostAndPort("s1"), (*result2)->getAddress());
}

TEST_F(ServerSelectorTestFixture, ShouldSelectTaggedSecondaryIfPreferredPrimaryNotAvailable) {
    TopologyStateMachine stateMachine(sdamConfiguration);
    auto topologyDescription = std::make_shared<TopologyDescription>(sdamConfiguration);

    const auto now = Date_t::now();

    const auto d0 = now - Milliseconds(1000);

    const auto s0 = ServerDescriptionBuilder()
                        .withAddress(HostAndPort("s0"))
                        .withType(ServerType::kRSPrimary)
                        .withRtt(sdamConfiguration.getLocalThreshold())
                        .withSetName("set")
                        .withHost(HostAndPort("s0"))
                        .withHost(HostAndPort("s1"))
                        .withHost(HostAndPort("s2"))
                        .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                        .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                        .withLastWriteDate(d0)
                        .withTag("tag", "primary")
                        .instance();
    stateMachine.onServerDescription(*topologyDescription, s0);

    // old primary unavailable
    const auto s0_failed = ServerDescriptionBuilder()
                               .withAddress(HostAndPort("s0"))
                               .withType(ServerType::kUnknown)
                               .withSetName("set")
                               .instance();
    stateMachine.onServerDescription(*topologyDescription, s0_failed);

    const auto s1 = ServerDescriptionBuilder()
                        .withAddress(HostAndPort("s1"))
                        .withType(ServerType::kRSSecondary)
                        .withRtt(sdamConfiguration.getLocalThreshold())
                        .withSetName("set")
                        .withHost(HostAndPort("s0"))
                        .withHost(HostAndPort("s1"))
                        .withHost(HostAndPort("s2"))
                        .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                        .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                        .withLastWriteDate(d0)
                        .withTag("tag", "secondary")
                        .instance();
    stateMachine.onServerDescription(*topologyDescription, s1);

    const auto s2 = ServerDescriptionBuilder()
                        .withAddress(HostAndPort("s2"))
                        .withType(ServerType::kRSSecondary)
                        .withRtt(sdamConfiguration.getLocalThreshold())
                        .withSetName("set")
                        .withHost(HostAndPort("s0"))
                        .withHost(HostAndPort("s1"))
                        .withHost(HostAndPort("s2"))
                        .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                        .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                        .withLastWriteDate(d0)
                        .instance();
    stateMachine.onServerDescription(*topologyDescription, s2);

    const auto primaryPreferredTagSecondary =
        ReadPreferenceSetting(ReadPreference::PrimaryPreferred, TagSets::secondarySet);
    auto result1 = selector.selectServer(topologyDescription, primaryPreferredTagSecondary);
    ASSERT(result1 != boost::none);
    ASSERT_EQ(HostAndPort("s1"), (*result1)->getAddress());
}

TEST_F(ServerSelectorTestFixture, ShouldFilterByTags) {
    auto tags = TagSets::productionSet;
    auto servers = makeServerDescriptionList();
    selector.filterTags(&servers, tags);
    ASSERT_EQ(3, servers.size());

    tags = TagSets::eastOrWestProductionSet;
    servers = makeServerDescriptionList();
    selector.filterTags(&servers, tags);
    ASSERT_EQ(2, servers.size());

    tags = TagSets::testSet;
    servers = makeServerDescriptionList();
    selector.filterTags(&servers, tags);
    ASSERT_EQ(2, servers.size());

    tags = TagSets::integrationOrTestSet;
    servers = makeServerDescriptionList();
    selector.filterTags(&servers, tags);
    ASSERT_EQ(2, servers.size());

    tags = TagSets::westProductionSet;
    servers = makeServerDescriptionList();
    selector.filterTags(&servers, tags);
    ASSERT_EQ(1, servers.size());

    tags = TagSets::integrationSet;
    servers = makeServerDescriptionList();
    selector.filterTags(&servers, tags);
    ASSERT_EQ(0, servers.size());

    tags = TagSets::emptySet;
    servers = makeServerDescriptionList();
    selector.filterTags(&servers, tags);
    ASSERT_EQ(makeServerDescriptionList().size(), servers.size());
}
}  // namespace mongo::sdam
