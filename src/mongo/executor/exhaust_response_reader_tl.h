// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/baton.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/network_interface.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

#include <memory>

namespace mongo::executor {

/**
 * An ExhaustResponseReader implementation based on sessions provided from a TransportLayer.
 * These are created and returned from NetworkInterfaceTL's exhaust path.
 *
 * The underlying session is returned to the pool upon destruction of the ExhaustResponseReader.
 */
class [[MONGO_MOD_PUBLIC]] ExhaustResponseReaderTL
    : public NetworkInterface::ExhaustResponseReader {
public:
    ExhaustResponseReaderTL(RemoteCommandRequest originalRequest,
                            RemoteCommandResponse initialResponse,
                            std::shared_ptr<AsyncClientFactory::AsyncClientHandle> conn,
                            std::shared_ptr<Baton> baton,
                            std::shared_ptr<transport::Reactor> reactor,
                            const CancellationToken& token = CancellationToken::uncancelable());

    ~ExhaustResponseReaderTL() override;
    SemiFuture<RemoteCommandResponse> next() final;

private:
    void _recordConnectionOutcome(Status outcome);

    Future<RemoteCommandResponse> _read();

    RemoteCommandRequest _originatingRequest;
    boost::optional<RemoteCommandResponse> _initialResponse;

    Atomic<bool> _finished{false};
    std::shared_ptr<AsyncClientFactory::AsyncClientHandle> _client;

    std::shared_ptr<Baton> _baton;
    std::shared_ptr<transport::Reactor> _reactor;
    CancellationSource _cancelSource;
    std::unique_ptr<transport::ReactorTimer> _timer;
};
}  // namespace mongo::executor
