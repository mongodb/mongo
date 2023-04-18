/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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

#include <boost/optional.hpp>
#include <memory>

#include "mongo/config.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/grpc/channel_pool.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/periodic_runner.h"

namespace mongo::transport::grpc {

/**
 * Wraps the Server and Client implementations of MongoDB's gRPC service. This abstraction layer
 * aims to hide gRPC-specific details from `SessionWorkflow`, `ServiceEntryPoint`, and the remainder
 * of the command execution path.
 *
 * TODO SERVER-74020: extend this documentation with more details.
 */
class GRPCTransportLayer : public TransportLayer {
public:
    constexpr static auto kDefaultChannelTimeout = Minutes{30};

    // TODO SERVER-74016: replace the empty structs with proper types.
    using ChannelType = struct {};
    using StubType = struct {};

    GRPCTransportLayer(ServiceContext* svcCtx, const WireSpec& wireSpec);

    Status start() override;

    void shutdown() override;

    StatusWith<std::shared_ptr<Session>> connect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        Milliseconds timeout,
        boost::optional<TransientSSLParams> transientSSLParams = boost::none) override {
        // TODO SERVER-74020
        MONGO_UNIMPLEMENTED;
    }

    Future<std::shared_ptr<Session>> asyncConnect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        std::shared_ptr<const SSLConnectionContext> transientSSLContext) override {
        // TODO SERVER-74020
        MONGO_UNIMPLEMENTED;
    }

    Status setup() override {
        // TODO SERVER-74020
        MONGO_UNIMPLEMENTED;
    }

    ReactorHandle getReactor(WhichReactor) override {
        // TODO SERVER-74020
        MONGO_UNIMPLEMENTED;
    }

#ifdef MONGO_CONFIG_SSL
    Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                              bool asyncOCSPStaple) override {
        // TODO SERVER-74020
        MONGO_UNIMPLEMENTED;
    }

    StatusWith<std::shared_ptr<const transport::SSLConnectionContext>> createTransientSSLContext(
        const TransientSSLParams& transientSSLParams) override {
        // TODO SERVER-74020
        MONGO_UNIMPLEMENTED;
    }
#endif

private:
    using ChannelPoolType = ChannelPool<ChannelType, StubType>;
    ServiceContext* const _svcCtx;
    std::shared_ptr<ChannelPoolType> _channelPool;
    boost::optional<PeriodicRunner::JobAnchor> _idleChannelPruner;
};

}  // namespace mongo::transport::grpc
