// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/client/async_client.h"
#include "mongo/executor/connection_pool_stats.h"
#include "mongo/executor/egress_connection_closer.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"
#include "mongo/util/modules.h"

namespace mongo::executor {

/**
 * Abstract interface for AsyncDBClient factories to be used by NetworkInterfaceTL.
 * The factory dictates which transport protocol the TransportLayer provided to startup() should
 * communicate over.
 */
class [[MONGO_MOD_PUBLIC]] AsyncClientFactory : public EgressConnectionCloser {
public:
    /**
     * A handle to a client produced from this factory.
     * Implementations MUST clean up the underlying client upon destruction of this handle.
     * What it means to clean up the underlying client is implementation-defined (e.g. the client
     * could be cached for later reuse or destroyed).
     */
    class AsyncClientHandle {
    public:
        AsyncClientHandle() = default;
        AsyncClientHandle(const AsyncClientHandle&) = delete;
        AsyncClientHandle& operator=(const AsyncClientHandle&) = delete;
        virtual ~AsyncClientHandle() {}

        virtual AsyncDBClient& getClient() = 0;
        virtual void startAcquiredTimer() = 0;
        virtual std::shared_ptr<Timer> getAcquiredTimer() = 0;

        /**
         * Indicates that the user is now done with this client. Users MUST call either
         * this method or indicateFailure() before destructing this handle.
         */
        virtual void indicateSuccess() = 0;

        /**
         * Indicates that the underlying client has failed. Users MUST call either this method or
         * indicateSuccess() before destructing this handle.
         */
        virtual void indicateFailure(Status s) = 0;

        /**
         * This method updates a 'liveness' timestamp on the client handle.
         *
         * This method should be invoked whenever an operation is performed using the underlying
         * client (i.e. actual networking was performed). If a client was acquired, then returned
         * back without use, one would expect an indicateSuccess without an indicateUsed.
         */
        virtual void indicateUsed() = 0;
    };

    virtual transport::TransportProtocol getTransportProtocol() const = 0;

    virtual void startup(ServiceContext* svcCtx,
                         transport::TransportLayer* tl,
                         transport::ReactorHandle reactor) = 0;

    /**
     * Implementations of shutdown() MUST trigger the termination of all outstanding clients
     * produced from this factory. Implementations may also wait for all active handles to fully
     * complete termination, but it is not necessary to do so.
     */
    virtual void shutdown() = 0;

    virtual SemiFuture<std::shared_ptr<AsyncClientHandle>> get(
        const HostAndPort& target,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        const CancellationToken& token = CancellationToken::uncancelable()) = 0;

    /**
     * "Lease" a client from this factory. This method behaves similarly
     * to `AsyncClientFactory::get`, except that Clients retrieved via this method are not assumed
     * to be in active use for the duration of their lease and are reported separately in metrics.
     *
     * If a client is intended to be used for multiple operations over a potentially long period of
     * time (e.g. when using `PinnedConnectionTaskExecutor`), `AsyncClientFactory::lease` should be
     * used to retrieve. In all other cases, `AsyncClientFactory::get` should be used instead.
     */
    virtual SemiFuture<std::shared_ptr<AsyncClientHandle>> lease(
        const HostAndPort& target,
        transport::ConnectSSLMode sslMode,
        Milliseconds timeout,
        const CancellationToken& token = CancellationToken::uncancelable()) = 0;

    // TODO SERVER-100677: Remove
    virtual void appendConnectionStats(ConnectionPoolStats* stats) const {};

    virtual void appendStats(BSONObjBuilder& bob) const = 0;

protected:
    AsyncClientFactory() = default;
};
}  // namespace mongo::executor
