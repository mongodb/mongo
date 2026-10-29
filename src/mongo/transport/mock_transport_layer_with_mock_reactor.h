// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/mock_reactor.h"
#include "mongo/transport/transport_layer_mock.h"
#include "mongo/util/modules.h"

#include <memory>
#include <mutex>
#include <vector>

namespace mongo::transport {

/**
 * An egress-only MongoRPC TransportLayer whose sole capability is handing out MockReactors to
 * code that obtains its reactor through a TransportLayerManager. getReactor(kNewReactor) returns a
 * fresh MockReactor; kIngress and kEgress return nullptr. No connections or sessions are
 * provided, and makeBaton() returns null so OperationContexts get a DefaultBaton.
 */
class [[MONGO_MOD_PUBLIC]] MockTransportLayerWithMockReactor : public TransportLayerMock {
public:
    MockTransportLayerWithMockReactor() = default;

    ReactorHandle getReactor(WhichReactor which) override;

    bool isEgress() const override {
        return true;
    }

    TransportProtocol getTransportProtocol() const override {
        return TransportProtocol::MongoRPC;
    }

    /** The most recently created reactor, or nullptr if none. */
    std::shared_ptr<MockReactor> latestReactor() const;

    /** Every reactor created so far, in creation order. */
    std::vector<std::shared_ptr<MockReactor>> reactors() const;

private:
    mutable std::mutex _mutex;
    std::vector<std::shared_ptr<MockReactor>> _reactors;
};

}  // namespace mongo::transport
