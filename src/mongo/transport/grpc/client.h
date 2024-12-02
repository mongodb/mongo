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

#include <memory>

#include <boost/optional.hpp>

#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/channel_pool.h"
#include "mongo/transport/grpc/client_stream.h"
#include "mongo/transport/grpc/grpc_client_context.h"
#include "mongo/transport/grpc/grpc_client_stream.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/reactor.h"
#include "mongo/transport/grpc/serialization.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/ssl_util.h"
#include "mongo/util/uuid.h"

namespace mongo::transport::grpc {

class Client : public std::enable_shared_from_this<Client> {
public:
    static constexpr auto kDefaultChannelTimeout = Minutes(30);

    using CtxAndStream = std::pair<std::shared_ptr<ClientContext>, std::shared_ptr<ClientStream>>;

    explicit Client(TransportLayer* tl, const BSONObj& clientMetadata);

    virtual ~Client() = default;

    UUID id() const {
        return _id;
    }

    virtual void start(ServiceContext*) = 0;

    /**
     * Cancels all outstanding sessions created from this client and blocks until they all have been
     * terminated. Closes all channels to the server. This client cannot connect sessions again
     * after this method returns.
     */
    virtual void shutdown();

    struct ConnectOptions {
        boost::optional<std::string> authToken = {};
        ConnectSSLMode sslMode = ConnectSSLMode::kGlobalSSLMode;
    };

    std::shared_ptr<EgressSession> connect(const HostAndPort& remote,
                                           const std::shared_ptr<GRPCReactor>& reactor,
                                           Milliseconds timeout,
                                           ConnectOptions options);

    /**
     * Get this client's current idea of what the cluster's maxWireVersion is. This will be updated
     * based on information received from the cluster via sessions created from this client.
     *
     * The initial value for this is the first wireversion that included gRPC support.
     */
    int getClusterMaxWireVersion() const;

protected:
    /**
     * Adds entries to the provided `ClientContext's` metadata as defined in the MongoDB gRPC
     * Protocol.
     */
    void setMetadataOnClientContext(ClientContext& ctx, const ConnectOptions& options);

private:
    enum class ClientState { kUninitialized, kStarted, kShutdown };

    virtual CtxAndStream _streamFactory(const HostAndPort&,
                                        const std::shared_ptr<GRPCReactor>&,
                                        Milliseconds,
                                        const ConnectOptions&) = 0;

    /**
     * Returns whether all outstanding sessions created by this client have been destroyed and this
     * client has halted establishing any new sessions.
     */
    bool _isShutdownComplete_inlock();

    TransportLayer* const _tl;
    const UUID _id;
    std::string _clientMetadata;
    std::shared_ptr<EgressSession::SharedState> _sharedState;

    mutable stdx::mutex _mutex;
    stdx::condition_variable _shutdownCV;
    ClientState _state = ClientState::kUninitialized;
    size_t _ongoingConnects = 0;
    std::list<std::weak_ptr<EgressSession>> _sessions;
};

class GRPCClient : public Client {
public:
    class StubFactory {
    public:
        virtual ~StubFactory() = default;
    };

    struct Options {
        boost::optional<StringData> tlsCAFile;
        boost::optional<StringData> tlsCertificateKeyFile;
        bool tlsAllowInvalidCertificates = false;
        bool tlsAllowInvalidHostnames = false;
    };

    GRPCClient(TransportLayer* tl, const BSONObj& clientMetadata, Options options);

    void start(ServiceContext* svcCtx) override;
    void shutdown() override;

private:
    CtxAndStream _streamFactory(const HostAndPort&,
                                const std::shared_ptr<GRPCReactor>&,
                                Milliseconds,
                                const ConnectOptions&) override;

    std::unique_ptr<StubFactory> _stubFactory;
};

}  // namespace mongo::transport::grpc
