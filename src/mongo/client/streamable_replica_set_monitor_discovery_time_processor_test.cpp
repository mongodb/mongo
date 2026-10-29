// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/client/streamable_replica_set_monitor_discovery_time_processor.h"

#include "mongo/bson/oid.h"
#include "mongo/client/sdam/sdam_configuration.h"
#include "mongo/client/sdam/sdam_test_base.h"
#include "mongo/client/sdam/server_description_builder.h"
#include "mongo/client/sdam/server_selector.h"
#include "mongo/client/sdam/topology_description.h"
#include "mongo/client/sdam/topology_state_machine.h"
#include "mongo/db/wire_version.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <vector>

#include <boost/move/utility_core.hpp>

namespace mongo::sdam {

class PrimaryServerDiscoveryTime : public SdamTestFixture {
public:
    const SdamConfiguration sdamConfiguration;

    PrimaryServerDiscoveryTime() : sdamConfiguration(SdamConfiguration({{HostAndPort("s0")}})) {}
    static inline const OID kOidOne{"000000000000000000000001"};
    ServerSelector selector = ServerSelector(sdamConfiguration);
    StreamableReplicaSetMonitor::StreamableReplicaSetMonitorDiscoveryTimeProcessor
        _rsmTimeProcessor;
};
TEST_F(PrimaryServerDiscoveryTime, ShouldFilterByLastWriteTime2) {
    TopologyStateMachine stateMachine(sdamConfiguration);
    auto topologyDescription = std::make_shared<TopologyDescription>(sdamConfiguration);

    const auto s0 = ServerDescriptionBuilder()
                        .withAddress(HostAndPort("s0"))
                        .withType(ServerType::kRSPrimary)
                        .withRtt(sdamConfiguration.getLocalThreshold())
                        .withSetName("set")
                        .withHost(HostAndPort("s0"))
                        .withHost(HostAndPort("s1"))
                        .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                        .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                        .withElectionId(kOidOne)
                        .withSetVersion(100)
                        .instance();
    stateMachine.onServerDescription(*topologyDescription, s0);

    auto newTopology = TopologyDescription::clone(*topologyDescription);
    const auto s1 = ServerDescriptionBuilder()
                        .withAddress(HostAndPort("s0"))
                        .withType(ServerType::kRSSecondary)
                        .withRtt(sdamConfiguration.getLocalThreshold())
                        .withSetName("set")
                        .withHost(HostAndPort("s0"))
                        .withHost(HostAndPort("s1"))
                        .withMinWireVersion(WireVersion::SUPPORTS_OP_MSG)
                        .withMaxWireVersion(WireVersion::LATEST_WIRE_VERSION)
                        .withElectionId(kOidOne)
                        .withSetVersion(100)
                        .instance();

    stateMachine.onServerDescription(*newTopology, s1);

    // no timer reset because primary server didn't change
    auto beforeElapsedDuration = _rsmTimeProcessor.getPrimaryServerChangeElapsedTime();
    sleepFor(Milliseconds(100));
    _rsmTimeProcessor.onTopologyDescriptionChangedEvent(topologyDescription, topologyDescription);
    auto afterElapsedDuration = _rsmTimeProcessor.getPrimaryServerChangeElapsedTime();
    ASSERT_TRUE(afterElapsedDuration > beforeElapsedDuration);

    // timer reset because of primary server change
    beforeElapsedDuration = _rsmTimeProcessor.getPrimaryServerChangeElapsedTime();
    sleepFor(Milliseconds(100));
    _rsmTimeProcessor.onTopologyDescriptionChangedEvent(topologyDescription, newTopology);
    afterElapsedDuration = _rsmTimeProcessor.getPrimaryServerChangeElapsedTime();
    ASSERT_TRUE(afterElapsedDuration <
                beforeElapsedDuration);  // afterElapsedDuration was just reset
}

}  // namespace mongo::sdam
