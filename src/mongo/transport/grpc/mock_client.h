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

    Status rotateCertificates(const SSLConfiguration& sslConfig) override {
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
