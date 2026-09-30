// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/mock_transport_layer_with_mock_reactor.h"

namespace mongo::transport {

ReactorHandle MockTransportLayerWithMockReactor::getReactor(WhichReactor which) {
    if (which != kNewReactor) {
        return nullptr;
    }
    auto reactor = std::make_shared<MockReactor>();
    std::lock_guard lk(_mutex);
    _reactors.push_back(reactor);
    return reactor;
}

std::shared_ptr<MockReactor> MockTransportLayerWithMockReactor::latestReactor() const {
    std::lock_guard lk(_mutex);
    return _reactors.empty() ? nullptr : _reactors.back();
}

std::vector<std::shared_ptr<MockReactor>> MockTransportLayerWithMockReactor::reactors() const {
    std::lock_guard lk(_mutex);
    return _reactors;
}

}  // namespace mongo::transport
