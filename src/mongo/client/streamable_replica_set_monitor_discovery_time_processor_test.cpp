/**
 *    Copyright (C) 2021-present MongoDB, Inc.
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

#include "mongo/client/streamable_replica_set_monitor_discovery_time_processor.h"

#include "mongo/base/string_data.h"
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
    SdamServerSelector selector = SdamServerSelector(sdamConfiguration);
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
