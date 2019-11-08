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

#include "mongo/client/sdam/topology_state_machine.h"

namespace mongo::sdam {
TopologyManager::TopologyManager(SdamConfiguration config, ClockSource* clockSource)
    : _config(std::move(config)),
      _clockSource(clockSource),
      _topologyDescription(std::make_unique<TopologyDescription>(_config)),
      _topologyStateMachine(std::make_unique<TopologyStateMachine>(_config)) {}

void TopologyManager::onServerDescription(const IsMasterOutcome& isMasterOutcome) {
    stdx::lock_guard<mongo::Mutex> lock(_mutex);

    const auto& lastServerDescription =
        _topologyDescription->findServerByAddress(isMasterOutcome.getServer());
    boost::optional<IsMasterRTT> lastRTT =
        (lastServerDescription) ? (*lastServerDescription)->getRtt() : boost::none;

    auto newServerDescription =
        std::make_shared<ServerDescription>(_clockSource, isMasterOutcome, lastRTT);

    auto newTopologyDescription = std::make_unique<TopologyDescription>(*_topologyDescription);
    _topologyStateMachine->onServerDescription(*newTopologyDescription, newServerDescription);
    _topologyDescription = std::move(newTopologyDescription);
}

const std::shared_ptr<TopologyDescription> TopologyManager::getTopologyDescription() const {
    stdx::lock_guard<mongo::Mutex> lock(_mutex);
    return _topologyDescription;
}
};  // namespace mongo::sdam
