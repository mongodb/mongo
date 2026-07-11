// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/transport/grpc/channel_pool.h"
#include "mongo/transport/grpc/client.h"
#include "mongo/transport/grpc/mock_stub.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/net/ssl_types.h"

#include <memory>

namespace mongo::transport::grpc {

class MockClient : public Client {
public:
    using MockChannelPool = ChannelPool<std::shared_ptr<MockChannel>, MockStub>;
    using MockResolver = std::function<MockRPCQueue::Producer(const HostAndPort&)>;

    MockClient(TransportLayer* tl,
               ServiceContext* svcCtx,
               HostAndPort local,
               MockResolver resolver,
               const BSONObj& metadata);

    void start() override {
        Client::start();
    }

    void appendStats(GRPCConnectionStats& stats) const override {
        MONGO_UNIMPLEMENTED;
    }

    Status rotateCertificates(const SSLConfiguration& sslConfig,
                              const SSLManagerInterface& sslManager) override {
        MONGO_UNIMPLEMENTED;
    }

    void dropConnections(const Status& status) override {
        MONGO_UNIMPLEMENTED;
    }

    void dropConnections(const HostAndPort& target, const Status& status) override {
        MONGO_UNIMPLEMENTED;
    }

    void setKeepOpen(const HostAndPort& hostAndPort, bool keepOpen) override {
        MONGO_UNIMPLEMENTED;
    }

private:
    Future<CallContext> _streamFactory(const HostAndPort& remote,
                                       const std::shared_ptr<GRPCReactor>& reactor,
                                       boost::optional<Date_t> deadline,
                                       const ConnectOptions& options,
                                       const CancellationToken& token) override;

    const HostAndPort _local;
    MockResolver _resolver;
    std::shared_ptr<MockChannelPool> _pool;
};

}  // namespace mongo::transport::grpc
