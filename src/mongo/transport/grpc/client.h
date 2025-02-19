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

#include "mongo/base/counter.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/db/service_context.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/channel_pool.h"
#include "mongo/transport/grpc/client_stream.h"
#include "mongo/transport/grpc/grpc_client_context.h"
#include "mongo/transport/grpc/grpc_client_stream.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc/reactor.h"
#include "mongo/transport/grpc/serialization.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/duration.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/uuid.h"

namespace mongo::transport::grpc {

constexpr auto kStreamsSubsectionFieldName = "streams"_sd;

class Client : public std::enable_shared_from_this<Client> {
public:
    static constexpr auto kDefaultChannelTimeout = Minutes(30);

    /**
     * State related to an invocation of one of CommandService's methods.
     */
    struct CallContext {
        std::shared_ptr<ClientContext> ctx;
        std::shared_ptr<ClientStream> stream;
        boost::optional<SSLConfiguration> sslConfig;
        UUID channelUUID;
    };

    explicit Client(TransportLayer* tl, ServiceContext* svcCtx, const BSONObj& clientMetadata);

    virtual ~Client() = default;

    UUID id() const {
        return _id;
    }

    virtual void start() = 0;

    /**
     * Cancels all outstanding sessions created from this client and blocks until they all have been
     * terminated. Closes all channels to the server. This client cannot connect sessions again
     * after this method returns.
     */
    virtual void shutdown();

    virtual void appendStats(GRPCConnectionStats& stats) const = 0;

#ifdef MONGO_CONFIG_SSL
    virtual Status rotateCertificates(const SSLConfiguration& sslConfig) = 0;
#endif

    struct ConnectOptions {
        boost::optional<std::string> authToken = {};
        ConnectSSLMode sslMode = ConnectSSLMode::kGlobalSSLMode;
    };

    Future<std::shared_ptr<EgressSession>> connect(
        const HostAndPort& remote,
        const std::shared_ptr<GRPCReactor>& reactor,
        Milliseconds timeout,
        ConnectOptions options,
        const CancellationToken& token = CancellationToken::uncancelable(),
        std::shared_ptr<ConnectionMetrics> connectionMetrics = nullptr);

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

    Counter64 _numActiveStreams;
    Counter64 _numSuccessfulStreams;
    Counter64 _numFailedStreams;

private:
    enum class ClientState { kUninitialized, kStarted, kShutdown };
    class PendingStreamState : public enable_shared_from_this<PendingStreamState> {
    public:
        explicit PendingStreamState(HostAndPort remote,
                                    ConnectSSLMode sslMode,
                                    std::shared_ptr<ConnectionMetrics> connectionMetrics,
                                    const CancellationToken& token)
            : _remote(std::move(remote)),
              _sslMode(std::move(sslMode)),
              _connectionMetrics(std::move(connectionMetrics)),
              _cancelSource(token) {
            if (_connectionMetrics) {
                _connectionMetrics->onConnectionStarted();
            }
        }

        // The WithLock corresponds to the Client's mutex, not the PendingStreamState's.
        void registerWithClient(WithLock, Client& client);

        // The WithLock corresponds to the Client's mutex, not the PendingStreamState's.
        void unregisterFromClient(WithLock, Client& client);

        void cancel(Status reason);

        Status getCancellationReason();

        CancellationToken getCancellationToken() {
            return _cancelSource.token();
        }

        void setTimer(std::shared_ptr<ReactorTimer> timer) {
            _timer = std::move(timer);
        }

        ReactorTimer* getTimer() {
            return _timer.get();
        }

        void cancelTimer() {
            if (_timer) {
                _timer->cancel();
            }
        }

        const HostAndPort& getRemote() {
            return _remote;
        }

        void setDeadline(Date_t deadline) {
            _deadline = deadline;
        }

        const boost::optional<Date_t>& getDeadline() {
            return _deadline;
        }

        const ConnectSSLMode& getSSLMode() {
            return _sslMode;
        }

        const std::shared_ptr<ConnectionMetrics>& getConnectionMetrics() {
            return _connectionMetrics;
        }

    private:
        std::list<std::shared_ptr<PendingStreamState>>::iterator _iter;

        HostAndPort _remote;
        boost::optional<Date_t> _deadline;
        ConnectSSLMode _sslMode;
        std::shared_ptr<ConnectionMetrics> _connectionMetrics;

        stdx::mutex _mutex;
        Status _cancellationReason = Status::OK();
        CancellationSource _cancelSource;

        std::shared_ptr<ReactorTimer> _timer;
    };

    virtual Future<CallContext> _streamFactory(const HostAndPort& remote,
                                               const std::shared_ptr<GRPCReactor>& reactor,
                                               boost::optional<Date_t> deadline,
                                               const ConnectOptions& connectOptions,
                                               const CancellationToken& token) = 0;

    /**
     * Returns whether all outstanding sessions created by this client have been destroyed and this
     * client has halted establishing any new sessions.
     */
    bool _isShutdownComplete_inlock();

    TransportLayer* const _tl;
    ServiceContext* const _svcCtx;
    const UUID _id;
    std::string _clientMetadata;
    std::shared_ptr<EgressSession::SharedState> _sharedState;

    mutable stdx::mutex _mutex;
    stdx::condition_variable _shutdownCV;
    ClientState _state = ClientState::kUninitialized;
    std::list<std::weak_ptr<EgressSession>> _sessions;

    // This list corresponds to ongoing stream establishment attempts, and holds the timeout and
    // cancellation state for those attemps.
    std::list<std::shared_ptr<PendingStreamState>> _pendingStreamStates;
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

    GRPCClient(TransportLayer* tl,
               ServiceContext* svcCtx,
               const BSONObj& clientMetadata,
               Options options);

    void start() override;
    void shutdown() override;
    void appendStats(GRPCConnectionStats& stats) const override;
#ifdef MONGO_CONFIG_SSL
    Status rotateCertificates(const SSLConfiguration& sslConfig) override;
#endif
    void dropAllChannels_forTest();


private:
    Future<CallContext> _streamFactory(const HostAndPort& remote,
                                       const std::shared_ptr<GRPCReactor>& reactor,
                                       boost::optional<Date_t> deadline,
                                       const ConnectOptions& connectOptions,
                                       const CancellationToken& token) override;


    std::unique_ptr<StubFactory> _stubFactory;
};

}  // namespace mongo::transport::grpc
