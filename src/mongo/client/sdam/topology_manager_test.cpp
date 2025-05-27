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
#include "mongo/client/sdam/topology_manager.h"

// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/string_data.h"
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
