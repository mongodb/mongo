// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0
#pragma once

#include "mongo/client/sdam/sdam_datatypes.h"
#include "mongo/client/sdam/topology_manager.h"
#include "mongo/util/future.h"
#include "mongo/util/net/hostandport.h"

#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace mongo::sdam {

class MockTopologyManager : public TopologyManager {
public:
    MockTopologyManager();

    bool onServerDescription(const HelloOutcome& helloOutcome) override;
    void onServerRTTUpdated(HostAndPort hostAndPort, HelloRTT rtt) override;

    void setTopologyDescription(std::shared_ptr<TopologyDescription> newDescription);
    std::shared_ptr<TopologyDescription> getTopologyDescription() const override;

private:
    std::shared_ptr<TopologyDescription> _topologyDescription;
    std::shared_ptr<TopologyDescription> _getTopologyDescriptionWithLock(WithLock) const override;
};

}  // namespace mongo::sdam
