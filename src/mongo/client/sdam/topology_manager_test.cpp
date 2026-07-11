// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#include "mongo/client/sdam/topology_manager.h"

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/oid.h"
#include "mongo/client/sdam/sdam_test_base.h"
#include "mongo/client/sdam/server_description.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/rpc/topology_version_gen.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/system_clock_source.h"

#include <memory>
#include <string>
#include <utility>

#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

namespace sdam {

class TopologyManagerTestFixture : public SdamTestFixture {
protected:
    void assertDefaultConfig(const TopologyDescription& topologyDescription);

    static inline const auto kSetName = std::string("mySetName");

    static inline const std::vector<HostAndPort> kOneServer{HostAndPort("foo:1234")};
    static inline const std::vector<HostAndPort> kThreeServers{
        HostAndPort("foo:1234"), HostAndPort("bar:1234"), HostAndPort("baz:1234")};

    static BSONObjBuilder okBuilder() {
        return std::move(BSONObjBuilder().append("ok", 1));
    }

    static inline const auto clockSource = SystemClockSource::get();

    static inline const auto kBsonOk = okBuilder().obj();
    static inline const auto kBsonTopologyVersionLow =
        okBuilder().append("topologyVersion", TopologyVersion(OID::max(), 0).toBSON()).obj();
    static inline const auto kBsonTopologyVersionHigh =
        okBuilder().append("topologyVersion", TopologyVersion(OID::max(), 1).toBSON()).obj();
    static inline const auto kBsonRsPrimary = okBuilder()
                                                  .append("isWritablePrimary", true)
                                                  .append("setName", kSetName)
                                                  .append("minWireVersion", 2)
                                                  .append("maxWireVersion", 10)
                                                  .appendArray("hosts",
                                                               BSON_ARRAY("foo:1234" << "bar:1234"
                                                                                     << "baz:1234"))

                                                  .obj();
};

TEST_F(TopologyManagerTestFixture, ShouldUpdateTopologyVersionOnSuccess) {
    auto config = SdamConfiguration(kOneServer);
    TopologyManagerImpl topologyManager(config, clockSource);

    auto topologyDescription = topologyManager.getTopologyDescription();
    ASSERT_EQUALS(topologyDescription->getServers().size(), 1);
    auto serverDescription = topologyDescription->getServers()[0];
    ASSERT(serverDescription->getTopologyVersion() == boost::none);

    // If previous topologyVersion is boost::none, should update to new topologyVersion
    auto helloOutcome = HelloOutcome(serverDescription->getAddress(),
                                     kBsonTopologyVersionLow,
                                     duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    topologyManager.onServerDescription(helloOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    auto newServerDescription = topologyDescription->getServers()[0];
    ASSERT(newServerDescription->getTopologyVersion());
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionLow.getObjectField("topologyVersion"));

    // If previous topologyVersion is <= new topologyVersion, should update to new topologyVersion
    helloOutcome = HelloOutcome(serverDescription->getAddress(),
                                kBsonTopologyVersionHigh,
                                duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    topologyManager.onServerDescription(helloOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    newServerDescription = topologyDescription->getServers()[0];
    ASSERT(newServerDescription->getTopologyVersion());
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionHigh.getObjectField("topologyVersion"));
}

TEST_F(TopologyManagerTestFixture,
       ShouldUpdateServerDescriptionsTopologyDescriptionPtrWhenTopologyDescriptionIsInstalled) {
    auto checkServerTopologyDescriptionMatches = [](TopologyDescriptionPtr topologyDescription) {
        auto rawTopologyDescPtr = topologyDescription.get();
        for (const auto& server : topologyDescription->getServers()) {
            auto rawServerTopologyDescPtr = (*server->getTopologyDescription()).get();
            ASSERT(server->getTopologyDescription());
            ASSERT(rawServerTopologyDescPtr == rawTopologyDescPtr);
        }
    };

    auto config = SdamConfiguration(kThreeServers);
    TopologyManagerImpl topologyManager(config, clockSource);
    checkServerTopologyDescriptionMatches(topologyManager.getTopologyDescription());

    auto topologyDescription = topologyManager.getTopologyDescription();
    auto firstServer = *topologyDescription->getServers()[0];
    auto host = firstServer.getAddress();
    auto helloOutcome =
        HelloOutcome(host, kBsonRsPrimary, duration_cast<HelloRTT>(mongo::Milliseconds{40}));
    topologyManager.onServerDescription(helloOutcome);
    checkServerTopologyDescriptionMatches(topologyManager.getTopologyDescription());

    topologyManager.onServerRTTUpdated(host, Milliseconds{40});
    checkServerTopologyDescriptionMatches(topologyManager.getTopologyDescription());
}

TEST_F(TopologyManagerTestFixture, ShouldUpdateTopologyVersionOnErrorIfSent) {
    auto config = SdamConfiguration(kOneServer);
    TopologyManagerImpl topologyManager(config, clockSource);

    auto topologyDescription = topologyManager.getTopologyDescription();
    ASSERT_EQUALS(topologyDescription->getServers().size(), 1);
    auto serverDescription = topologyDescription->getServers()[0];
    ASSERT(serverDescription->getTopologyVersion() == boost::none);

    // If previous topologyVersion is boost::none, should update to new topologyVersion
    auto helloOutcome = HelloOutcome(serverDescription->getAddress(),
                                     kBsonTopologyVersionLow,
                                     duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    topologyManager.onServerDescription(helloOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    auto newServerDescription = topologyDescription->getServers()[0];
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionLow.getObjectField("topologyVersion"));

    // If helloOutcome is not successful, should preserve old topologyVersion
    helloOutcome =
        HelloOutcome(serverDescription->getAddress(), kBsonTopologyVersionLow, "an error occurred");
    topologyManager.onServerDescription(helloOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    newServerDescription = topologyDescription->getServers()[0];
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionLow.getObjectField("topologyVersion"));
}

TEST_F(TopologyManagerTestFixture, ShouldNotUpdateServerDescriptionIfNewTopologyVersionOlder) {
    auto config = SdamConfiguration(kOneServer);
    TopologyManagerImpl topologyManager(config, clockSource);

    auto topologyDescription = topologyManager.getTopologyDescription();
    ASSERT_EQUALS(topologyDescription->getServers().size(), 1);
    auto serverDescription = topologyDescription->getServers()[0];
    ASSERT(serverDescription->getTopologyVersion() == boost::none);

    // If previous topologyVersion is boost::none, should update to new topologyVersion
    auto helloOutcome = HelloOutcome(serverDescription->getAddress(),
                                     kBsonTopologyVersionHigh,
                                     duration_cast<HelloRTT>(mongo::Milliseconds(40)));
    topologyManager.onServerDescription(helloOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    auto newServerDescription = topologyDescription->getServers()[0];
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionHigh.getObjectField("topologyVersion"));

    // If helloOutcome is not successful, should preserve old topologyVersion
    helloOutcome = HelloOutcome(serverDescription->getAddress(), kBsonTopologyVersionLow);
    topologyManager.onServerDescription(helloOutcome);
    topologyDescription = topologyManager.getTopologyDescription();
    newServerDescription = topologyDescription->getServers()[0];
    ASSERT_BSONOBJ_EQ(newServerDescription->getTopologyVersion()->toBSON(),
                      kBsonTopologyVersionHigh.getObjectField("topologyVersion"));
}
};  // namespace sdam
};  // namespace mongo
