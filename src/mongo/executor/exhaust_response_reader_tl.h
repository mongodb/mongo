/**
 *    Copyright (C) 2024-present MongoDB, Inc.
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

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/baton.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/network_interface.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/future.h"

#include <memory>

namespace mongo::executor {

/**
 * An ExhaustResponseReader implementation based on sessions provided from a TransportLayer.
 * These are created and returned from NetworkInterfaceTL's exhaust path.
 *
 * The underlying session is returned to the pool upon destruction of the ExhaustResponseReader.
 */
class ExhaustResponseReaderTL : public NetworkInterface::ExhaustResponseReader {
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
