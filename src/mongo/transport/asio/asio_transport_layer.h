/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include <functional>
#include <memory>
#include <string>

#include "mongo/base/status_with.h"
#include "mongo/config.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/mutex.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/thread.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/net/ssl_types.h"

namespace asio {
class io_context;

template <typename Protocol>
class basic_socket_acceptor;

namespace generic {
class stream_protocol;
}  // namespace generic

namespace ssl {
class context;
}  // namespace ssl
}  // namespace asio

namespace mongo {

class ServiceContext;
class ServiceEntryPoint;

namespace transport {

// Simulates reads and writes that always return 1 byte and fail with EAGAIN
extern FailPoint asioTransportLayerShortOpportunisticReadWrite;

// Cause an asyncConnect to timeout after it's successfully connected to the remote peer
extern FailPoint asioTransportLayerAsyncConnectTimesOut;

extern FailPoint asioTransportLayerHangBeforeAcceptCallback;

extern FailPoint asioTransportLayerHangDuringAcceptCallback;

class AsioNetworkingBaton;
class AsioReactor;
class AsioSession;

/**
 * A TransportLayer implementation based on ASIO networking primitives.
 */
class AsioTransportLayer final : public TransportLayer {
    AsioTransportLayer(const AsioTransportLayer&) = delete;
    AsioTransportLayer& operator=(const AsioTransportLayer&) = delete;

public:
    constexpr static auto kSlowOperationThreshold = Seconds(1);

    struct Options {
        constexpr static auto kIngress = 0x1;
        constexpr static auto kEgress = 0x10;

        explicit Options(const ServerGlobalParams* params) : Options(params, {}) {}
        Options(const ServerGlobalParams* params, boost::optional<int> loadBalancerPort);
        Options() = default;

        int mode = kIngress | kEgress;

        bool isIngress() const {
            return mode & kIngress;
        }

        bool isEgress() const {
            return mode & kEgress;
        }

        int port = ServerGlobalParams::DefaultDBPort;  // port to bind to
        boost::optional<int> loadBalancerPort;         // accepts load balancer connections
        std::vector<std::string> ipList;               // addresses to bind to
#ifndef _WIN32
        bool useUnixSockets = true;  // whether to allow UNIX sockets in ipList
#endif
        bool enableIPv6 = false;             // whether to allow IPv6 sockets in ipList
        size_t maxConns = DEFAULT_MAX_CONN;  // maximum number of active connections
    };

    /**
     * A service, internal to `AsioTransportLayer`, that allows creating timers and running `Future`
     * continuations when a timeout occurs. This allows setting up timeouts for synchronous
     * operations, such as a synchronous SSL handshake. A separate thread is assigned to run these
     * timers to:
     * - Ensure there is always a thread running the timers, regardless of using a synchronous or
     *   asynchronous listener.
     * - Avoid any performance implications on other reactors (e.g., the `egressReactor`).
     * The public visibility is only for testing purposes and this service is not intended to be
     * used outside `AsioTransportLayer`.
     */
    class TimerService {
    public:
        using Spawn = std::function<stdx::thread(std::function<void()>)>;
        struct Options {
            Spawn spawn;
        };
        explicit TimerService(Options opt);
        TimerService() : TimerService(Options{}) {}
        ~TimerService();

        /**
         * Spawns a thread to run the reactor.
         * Immediately returns if the service has already started.
         * May be called more than once, and concurrently.
         */
        void start();

        /**
         * Stops the reactor and joins the thread.
         * Immediately returns if the service is not started, or already stopped.
         * May be called more than once, and concurrently.
         */
        void stop();

        std::unique_ptr<ReactorTimer> makeTimer();

        Date_t now();

    private:
        Reactor* _getReactor();

        const std::shared_ptr<Reactor> _reactor;

        // Serializes invocations of `start()` and `stop()`, and allows updating `_state` and
        // `_thread` as a single atomic operation.
        Mutex _mutex = MONGO_MAKE_LATCH("AsioTransportLayer::TimerService::_mutex");

        // State transitions: `kInitialized` --> `kStarted` --> `kStopped`
        //                          |_______________________________^
        enum class State { kInitialized, kStarted, kStopped };
        AtomicWord<State> _state;

        Spawn _spawn = [](std::function<void()> f) { return stdx::thread{std::move(f)}; };
        stdx::thread _thread;
    };

    AsioTransportLayer(const Options& opts,
                       ServiceEntryPoint* sep,
                       const WireSpec& wireSpec = WireSpec::instance());

    ~AsioTransportLayer() override;

    StatusWith<SessionHandle> connect(HostAndPort peer,
                                      ConnectSSLMode sslMode,
                                      Milliseconds timeout,
                                      boost::optional<TransientSSLParams> transientSSLParams) final;

    Future<SessionHandle> asyncConnect(
        HostAndPort peer,
        ConnectSSLMode sslMode,
        const ReactorHandle& reactor,
        Milliseconds timeout,
        std::shared_ptr<ConnectionMetrics> connectionMetrics,
        std::shared_ptr<const SSLConnectionContext> transientSSLContext = nullptr) final;

    Status setup() final;

    ReactorHandle getReactor(WhichReactor which) final;

    Status start() final;

    void shutdown() final;

    void appendStatsForServerStatus(BSONObjBuilder* bob) const override;

    void appendStatsForFTDC(BSONObjBuilder& bob) const override;

    int listenerPort() const {
        return _listenerPort;
    }

    boost::optional<int> loadBalancerPort() const {
        return _listenerOptions.loadBalancerPort;
    }

    std::vector<std::pair<SockAddr, int>> getListenerSocketBacklogQueueDepths() const;

#ifdef __linux__
    BatonHandle makeBaton(OperationContext* opCtx) const override;
#endif

#ifdef MONGO_CONFIG_SSL
    SSLParams::SSLModes sslMode() const;

    std::shared_ptr<const SSLConnectionContext> sslContext() const {
        return _sslContext.get();
    }

    Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                              bool asyncOCSPStaple) override;

    /**
     * Creates a transient SSL context using targeted (non default) SSL params.
     * @param transientSSLParams overrides any value in stored SSLConnectionContext.
     * @param optionalManager provides an optional SSL manager, otherwise the default one will be
     * used.
     */
    StatusWith<std::shared_ptr<const transport::SSLConnectionContext>> createTransientSSLContext(
        const TransientSSLParams& transientSSLParams) override;
#endif

private:
    using AsioSessionHandle = std::shared_ptr<AsioSession>;
    using ConstAsioSessionHandle = std::shared_ptr<const AsioSession>;
    using GenericAcceptor = asio::basic_socket_acceptor<asio::generic::stream_protocol>;

    void _acceptConnection(GenericAcceptor& acceptor);

    template <typename Endpoint>
    StatusWith<AsioSessionHandle> _doSyncConnect(
        Endpoint endpoint,
        const HostAndPort& peer,
        const Milliseconds& timeout,
        boost::optional<TransientSSLParams> transientSSLParams);

    StatusWith<std::shared_ptr<const transport::SSLConnectionContext>> _createSSLContext(
        std::shared_ptr<SSLManagerInterface>& manager,
        SSLParams::SSLModes sslMode,
        bool asyncOCSPStaple) const;

    void _runListener() noexcept;

    void _trySetListenerSocketBacklogQueueDepth(GenericAcceptor& acceptor) noexcept;

    Mutex _mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "AsioTransportLayer::_mutex");

    // There are three reactors that are used by AsioTransportLayer. The _ingressReactor contains
    // all the accepted sockets and all ingress networking activity. The _acceptorReactor contains
    // all the sockets in _acceptors.  The _egressReactor contains egress connections.
    //
    // AsioTransportLayer should never call run() on the _ingressReactor.
    // In synchronous mode, this will cause a massive performance degradation due to
    // unnecessary wakeups on the asio thread for sockets we don't intend to interact
    // with asynchronously. The additional IO context avoids registering those sockets
    // with the acceptors epoll set, thus avoiding those wakeups.  Calling run will
    // undo that benefit.
    //
    // AsioTransportLayer should run its own thread that calls run() on the _acceptorReactor
    // to process calls to async_accept - this is the equivalent of the "listener" thread in
    // other TransportLayers.
    //
    // The underlying problem that caused this is here:
    // https://github.com/chriskohlhoff/asio/issues/240
    //
    // It is important that the reactors be declared before the vector of acceptors (or any other
    // state that is associated with the reactors), so that we destroy any existing acceptors or
    // other reactor associated state before we drop the refcount on the reactor, which may destroy
    // it.
    std::shared_ptr<AsioReactor> _ingressReactor;
    std::shared_ptr<AsioReactor> _egressReactor;
    std::shared_ptr<AsioReactor> _acceptorReactor;

#ifdef MONGO_CONFIG_SSL
    synchronized_value<std::shared_ptr<const SSLConnectionContext>> _sslContext;
#endif

    struct AcceptorRecord;
    std::vector<std::unique_ptr<AcceptorRecord>> _acceptorRecords;

    // Only used if _listenerOptions.async is false.
    struct Listener {
        stdx::thread thread;
        stdx::condition_variable cv;
        bool active = false;
    };
    Listener _listener;

    ServiceEntryPoint* const _sep = nullptr;

    Options _listenerOptions;
    // The real incoming port in case of _listenerOptions.port==0 (ephemeral).
    int _listenerPort = 0;

    bool _isShutdown = false;

    const std::unique_ptr<TimerService> _timerService;

    // Tracks the cumulative time the listener spends between accepting incoming connections to
    // handing them off to dedicated connection threads.
    AtomicWord<Microseconds> _listenerProcessingTime;
};

}  // namespace transport
}  // namespace mongo
