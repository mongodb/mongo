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


#include "mongo/transport/asio/asio_transport_layer.h"

#include <functional>

#include <fmt/format.h>
#include <fstream>

#ifdef __linux__
#include <netinet/tcp.h>
#endif

#include <asio.hpp>
#include <asio/system_timer.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem/operations.hpp>

#include "mongo/config.h"

#include "mongo/base/system_error.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/transport/asio/asio_utils.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/transport_options_gen.h"
#include "mongo/util/clock_source.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/executor_stats.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/socket_utils.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/strong_weak_finish_line.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl.hpp"
#endif

// asio_session.h has some header dependencies that require it to be the last header.

#ifdef __linux__
#include "mongo/transport/asio/asio_networking_baton.h"
#endif

#include "mongo/transport/asio/asio_session.h"
#include "mongo/transport/asio/asio_session_impl.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace transport {

namespace {

using TcpKeepaliveOption = SocketOption<SOL_SOCKET, SO_KEEPALIVE>;
#ifdef __linux__
using TcpInfoOption = SocketOption<IPPROTO_TCP, TCP_INFO, tcp_info>;
using TcpKeepaliveCountOption = SocketOption<IPPROTO_TCP, TCP_KEEPCNT>;
using TcpKeepaliveIdleSecsOption = SocketOption<IPPROTO_TCP, TCP_KEEPIDLE>;
using TcpKeepaliveIntervalSecsOption = SocketOption<IPPROTO_TCP, TCP_KEEPINTVL>;
#ifdef TCP_USER_TIMEOUT
using TcpUserTimeoutMillisOption = SocketOption<IPPROTO_TCP, TCP_USER_TIMEOUT, unsigned>;
#endif
#endif  // __linux__

#ifdef TCP_FASTOPEN
using TcpFastOpenOption = SocketOption<IPPROTO_TCP, TCP_FASTOPEN>;
#endif
/**
 * On systems with TCP_FASTOPEN_CONNECT (linux >= 4.11),
 * we can get TFO "for free" by letting the kernel handle
 * postponing connect() until the first send() call.
 *
 * https://github.com/torvalds/linux/commit/19f6d3f3c8422d65b5e3d2162e30ef07c6e21ea2
 */
#ifdef TCP_FASTOPEN_CONNECT
using TcpFastOpenConnectOption = SocketOption<IPPROTO_TCP, TCP_FASTOPEN_CONNECT>;
#endif

/**
 * Set to `true` if any of the following set parameters were explicitly configured.
 * - tcpFastOpenServer
 * - tcpFastOpenClient
 * - tcpFastOpenQueueSize
 */
bool tcpFastOpenIsConfigured = false;
boost::optional<Status> maybeTcpFastOpenStatus;

bool shouldDiscardSocketDueToLostConnectivity(AsioSession::GenericSocket& peerSocket) {
#ifdef __linux__
    if (gPessimisticConnectivityCheckForAcceptedConnections.load()) {
        auto swEvents = pollASIOSocket(peerSocket, POLLRDHUP | POLLHUP, Milliseconds::zero());
        if (MONGO_unlikely(!swEvents.isOK())) {
            const auto err = swEvents.getStatus();
            if (err.code() != ErrorCodes::NetworkTimeout) {
                LOGV2_DEBUG(10158100, 3, "Error checking socket connectivity", "error"_attr = err);
                return true;
            }
        } else if (MONGO_unlikely(swEvents.getValue() & (POLLRDHUP | POLLHUP))) {
            LOGV2_DEBUG(10158101, 3, "Client has closed the socket before server reading from it");
            return true;
        }
    }
#endif  // __linux__
    return false;
}
}  // namespace

MONGO_FAIL_POINT_DEFINE(asioTransportLayerAsyncConnectTimesOut);
MONGO_FAIL_POINT_DEFINE(asioTransportLayerDelayConnection);
MONGO_FAIL_POINT_DEFINE(asioTransportLayerHangBeforeAcceptCallback);
MONGO_FAIL_POINT_DEFINE(asioTransportLayerHangDuringAcceptCallback);
MONGO_FAIL_POINT_DEFINE(asioTransportLayerAsyncConnectReturnsConnectionError);

#ifdef MONGO_CONFIG_SSL
SSLConnectionContext::~SSLConnectionContext() = default;
#endif

class AsioReactorTimer final : public ReactorTimer {
public:
    using TimerType = synchronized_value<asio::system_timer, RawSynchronizedValueMutexPolicy>;
    explicit AsioReactorTimer(asio::io_context& ctx)
        : _timer(std::make_shared<TimerType>(asio::system_timer(ctx))) {}

    ~AsioReactorTimer() {
        // The underlying timer won't get destroyed until the last promise from _asyncWait
        // has been filled, so cancel the timer so our promises get fulfilled
        cancel();
    }

    void cancel(const BatonHandle& baton = nullptr) override {
        // If we have a baton try to cancel that.
        if (baton && baton->networking() && baton->networking()->cancelTimer(*this)) {
            LOGV2_DEBUG(23010, 2, "Canceled via baton, skipping asio cancel.");
            return;
        }

        // Otherwise there could be a previous timer that was scheduled normally.
        (**_timer)->cancel();
    }


    Future<void> waitUntil(Date_t expiration, const BatonHandle& baton = nullptr) override {
        if (baton && baton->networking()) {
            return _asyncWait([&] { return baton->networking()->waitUntil(*this, expiration); },
                              baton);
        } else {
            return _asyncWait([&] { (**_timer)->expires_at(expiration.toSystemTimePoint()); });
        }
    }

private:
    template <typename ArmTimerCb>
    Future<void> _asyncWait(ArmTimerCb&& armTimer) {
        try {
            cancel();

            armTimer();
            return (**_timer)
                ->async_wait(UseFuture{})
                .tapError([timer = _timer](const Status& status) {
                    if (status != ErrorCodes::CallbackCanceled) {
                        LOGV2_DEBUG(23011,
                                    2,
                                    "Timer received error: {error}",
                                    "Timer received error",
                                    "error"_attr = status);
                    }
                });

        } catch (asio::system_error& ex) {
            return futurize(ex.code());
        }
    }

    template <typename ArmTimerCb>
    Future<void> _asyncWait(ArmTimerCb&& armTimer, const BatonHandle& baton) {
        cancel(baton);

        auto pf = makePromiseFuture<void>();
        armTimer().getAsync([p = std::move(pf.promise)](Status status) mutable {
            if (status.isOK()) {
                p.emplaceValue();
            } else {
                p.setError(status);
            }
        });

        return std::move(pf.future);
    }

    std::shared_ptr<TimerType> _timer;
};

class AsioReactor final : public Reactor {
public:
    AsioReactor() : _clkSource(this), _stats(&_clkSource), _ioContext() {}

    void run() noexcept override {
        ThreadIdGuard threadIdGuard(this);
        asio::io_context::work work(_ioContext);
        _ioContext.run();
    }

    void runFor(Milliseconds time) noexcept override {
        ThreadIdGuard threadIdGuard(this);
        asio::io_context::work work(_ioContext);
        _ioContext.run_for(time.toSystemDuration());
    }

    void stop() override {
        _ioContext.stop();
    }

    void drain() override {
        ThreadIdGuard threadIdGuard(this);
        _ioContext.restart();
        while (_ioContext.poll()) {
            LOGV2_DEBUG(23012, 2, "Draining remaining work in reactor.");
        }
        _ioContext.stop();
    }

    std::unique_ptr<ReactorTimer> makeTimer() override {
        return std::make_unique<AsioReactorTimer>(_ioContext);
    }

    Date_t now() override {
        return Date_t(asio::system_timer::clock_type::now());
    }

    void schedule(Task task) override {
        asio::post(_ioContext, [task = _stats.wrapTask(std::move(task))] { task(Status::OK()); });
    }

    void dispatch(Task task) override {
        asio::dispatch(_ioContext,
                       [task = _stats.wrapTask(std::move(task))] { task(Status::OK()); });
    }

    bool onReactorThread() const override {
        return this == _reactorForThread;
    }

    operator asio::io_context&() {
        return _ioContext;
    }

    void appendStats(BSONObjBuilder& bob) const override {
        _stats.serialize(&bob);
    }

private:
    // Provides `ClockSource` API for the reactor's clock source.
    class ReactorClockSource final : public ClockSource {
    public:
        explicit ReactorClockSource(AsioReactor* reactor) : _reactor(reactor) {}
        ~ReactorClockSource() = default;

        Milliseconds getPrecision() override {
            MONGO_UNREACHABLE;
        }

        Date_t now() override {
            return _reactor->now();
        }

    private:
        AsioReactor* const _reactor;
    };

    class ThreadIdGuard {
    public:
        ThreadIdGuard(AsioReactor* reactor) {
            invariant(!_reactorForThread);
            _reactorForThread = reactor;
        }

        ~ThreadIdGuard() {
            invariant(_reactorForThread);
            _reactorForThread = nullptr;
        }
    };

    static thread_local AsioReactor* _reactorForThread;

    ReactorClockSource _clkSource;

    ExecutorStats _stats;

    asio::io_context _ioContext;
};

thread_local AsioReactor* AsioReactor::_reactorForThread = nullptr;

AsioTransportLayer::Options::Options(const ServerGlobalParams* params,
                                     boost::optional<int> loadBalancerPort)
    : port(params->port),
      loadBalancerPort(loadBalancerPort),
      ipList(params->bind_ips),
#ifndef _WIN32
      useUnixSockets(!params->noUnixSocket),
#endif
      enableIPv6(params->enableIPv6),
      maxConns(params->maxConns) {
}

AsioTransportLayer::TimerService::TimerService(Options opt)
    : _reactor(std::make_shared<AsioReactor>()) {
    if (opt.spawn)
        _spawn = std::move(opt.spawn);
}

AsioTransportLayer::TimerService::~TimerService() {
    stop();
}

void AsioTransportLayer::TimerService::start() {
    // Skip the expensive lock acquisition and `compareAndSwap` in the common path.
    if (MONGO_likely(_state.load() != State::kInitialized))
        return;

    // The following ensures only one thread continues to spawn a thread to run the reactor. It also
    // ensures concurrent `start()` and `stop()` invocations are serialized. Holding the lock
    // guarantees that the following runs either before or after running `stop()`. Note that using
    // `compareAndSwap` while holding the lock is for simplicity and not necessary.
    auto lk = stdx::lock_guard(_mutex);
    auto precondition = State::kInitialized;
    if (_state.compareAndSwap(&precondition, State::kStarted)) {
        _thread = _spawn([reactor = _reactor] {
            if (!serverGlobalParams.quiet.load()) {
                LOGV2_INFO(5490002, "Started a new thread for the timer service");
            }

            reactor->run();

            if (!serverGlobalParams.quiet.load()) {
                LOGV2_INFO(5490003, "Returning from the timer service thread");
            }
        });
    }
}

void AsioTransportLayer::TimerService::stop() {
    // It's possible for `stop()` to be called without `start()` having been called (or for them to
    // be called concurrently), so we only proceed with stopping the reactor and joining the thread
    // if we've already transitioned to the `kStarted` state.
    auto lk = stdx::lock_guard(_mutex);
    if (_state.swap(State::kStopped) != State::kStarted)
        return;

    _reactor->stop();
    _thread.join();
}

std::unique_ptr<ReactorTimer> AsioTransportLayer::TimerService::makeTimer() {
    return _getReactor()->makeTimer();
}

Date_t AsioTransportLayer::TimerService::now() {
    return _getReactor()->now();
}

Reactor* AsioTransportLayer::TimerService::_getReactor() {
    // TODO SERVER-57253 We can start this service as part of starting `AsioTransportLayer`.
    // Then, we can remove the following invocation of `start()`.
    start();
    return _reactor.get();
}

AsioTransportLayer::AsioTransportLayer(const AsioTransportLayer::Options& opts,
                                       ServiceEntryPoint* sep,
                                       const WireSpec& wireSpec)
    : TransportLayer(wireSpec),
      _ingressReactor(std::make_shared<AsioReactor>()),
      _egressReactor(std::make_shared<AsioReactor>()),
      _acceptorReactor(std::make_shared<AsioReactor>()),
      _sep(sep),
      _listenerOptions(opts),
      _timerService(std::make_unique<TimerService>()) {}

AsioTransportLayer::~AsioTransportLayer() = default;

struct AsioTransportLayer::AcceptorRecord {
    AcceptorRecord(SockAddr address, GenericAcceptor acceptor)
        : address(std::move(address)), acceptor(std::move(acceptor)) {}

    SockAddr address;
    GenericAcceptor acceptor;
    // Tracks the amount of incoming connections waiting to be accepted by the server on this
    // acceptor.
    AtomicWord<int> backlogQueueDepth{0};
};

class WrappedEndpoint {
public:
    using Endpoint = asio::generic::stream_protocol::endpoint;

    explicit WrappedEndpoint(const asio::ip::basic_resolver_entry<asio::ip::tcp>& source)
        : _str(str::stream() << source.endpoint().address().to_string() << ":"
                             << source.service_name()),
          _endpoint(source.endpoint()) {}

#ifndef _WIN32
    explicit WrappedEndpoint(const asio::local::stream_protocol::endpoint& source)
        : _str(source.path()), _endpoint(source) {}
#endif

    WrappedEndpoint() = default;

    Endpoint* operator->() noexcept {
        return &_endpoint;
    }

    const Endpoint* operator->() const noexcept {
        return &_endpoint;
    }

    Endpoint& operator*() noexcept {
        return _endpoint;
    }

    const Endpoint& operator*() const noexcept {
        return _endpoint;
    }

    bool operator<(const WrappedEndpoint& rhs) const noexcept {
        return _endpoint < rhs._endpoint;
    }

    const std::string& toString() const {
        return _str;
    }

    sa_family_t family() const {
        return _endpoint.data()->sa_family;
    }

private:
    std::string _str;
    Endpoint _endpoint;
};

using Resolver = asio::ip::tcp::resolver;
class WrappedResolver {
public:
    using Flags = Resolver::flags;
    using EndpointVector = std::vector<WrappedEndpoint>;

    explicit WrappedResolver(asio::io_context& ioCtx) : _resolver(ioCtx) {}

    StatusWith<EndpointVector> resolve(const HostAndPort& peer, bool enableIPv6) {
        if (auto unixEp = _checkForUnixSocket(peer)) {
            return *unixEp;
        }

        // We always want to resolve the "service" (port number) as a numeric.
        //
        // We intentionally don't set the Resolver::address_configured flag because it might prevent
        // us from connecting to localhost on hosts with only a loopback interface
        // (see SERVER-1579).
        const auto flags = Resolver::numeric_service;

        // We resolve in two steps, the first step tries to resolve the hostname as an IP address -
        // that way if there's a DNS timeout, we can still connect to IP addresses quickly.
        // (See SERVER-1709)
        //
        // Then, if the numeric (IP address) lookup failed, we fall back to DNS or return the error
        // from the resolver.
        return _resolve(peer, flags | Resolver::numeric_host, enableIPv6)
            .onError([=](Status) { return _resolve(peer, flags, enableIPv6); })
            .getNoThrow();
    }

    Future<EndpointVector> asyncResolve(const HostAndPort& peer, bool enableIPv6) {
        if (auto unixEp = _checkForUnixSocket(peer)) {
            return *unixEp;
        }

        // We follow the same numeric -> hostname fallback procedure as the synchronous resolver
        // function for setting resolver flags (see above).
        const auto flags = Resolver::numeric_service;
        return _asyncResolve(peer, flags | Resolver::numeric_host, enableIPv6).onError([=](Status) {
            return _asyncResolve(peer, flags, enableIPv6);
        });
    }

    void cancel() {
        _resolver.cancel();
    }

private:
    boost::optional<EndpointVector> _checkForUnixSocket(const HostAndPort& peer) {
#ifndef _WIN32
        if (str::contains(peer.host(), '/')) {
            asio::local::stream_protocol::endpoint ep(peer.host());
            return EndpointVector{WrappedEndpoint(ep)};
        }
#endif
        return boost::none;
    }

    Future<EndpointVector> _resolve(const HostAndPort& peer, Flags flags, bool enableIPv6) {
        std::error_code ec;
        auto port = std::to_string(peer.port());
        Results results;
        if (enableIPv6) {
            results = _resolver.resolve(peer.host(), port, flags, ec);
        } else {
            results = _resolver.resolve(asio::ip::tcp::v4(), peer.host(), port, flags, ec);
        }

        if (ec) {
            return _makeFuture(errorCodeToStatus(ec, "resolve"), peer);
        } else {
            return _makeFuture(results, peer);
        }
    }

    Future<EndpointVector> _asyncResolve(const HostAndPort& peer, Flags flags, bool enableIPv6) {
        auto port = std::to_string(peer.port());
        Future<Results> ret;
        if (enableIPv6) {
            ret = _resolver.async_resolve(peer.host(), port, flags, UseFuture{});
        } else {
            ret =
                _resolver.async_resolve(asio::ip::tcp::v4(), peer.host(), port, flags, UseFuture{});
        }

        return std::move(ret)
            .onError([this, peer](Status status) { return _checkResults(status, peer); })
            .then([this, peer](Results results) { return _makeFuture(results, peer); });
    }

    using Results = Resolver::results_type;
    StatusWith<Results> _checkResults(StatusWith<Results> results, const HostAndPort& peer) {
        if (!results.isOK()) {
            return Status{ErrorCodes::HostNotFound,
                          str::stream() << "Could not find address for " << peer << ": "
                                        << results.getStatus()};
        } else if (results.getValue().empty()) {
            return Status{ErrorCodes::HostNotFound,
                          str::stream() << "Could not find address for " << peer};
        } else {
            return results;
        }
    }

    Future<EndpointVector> _makeFuture(StatusWith<Results> results, const HostAndPort& peer) {
        results = _checkResults(std::move(results), peer);
        if (!results.isOK()) {
            return results.getStatus();
        } else {
            auto& epl = results.getValue();
            return EndpointVector(epl.begin(), epl.end());
        }
    }

    Resolver _resolver;
};

Status makeConnectError(Status status, const HostAndPort& peer, const WrappedEndpoint& endpoint) {
    std::string errmsg;
    if (peer.toString() != endpoint.toString() && !endpoint.toString().empty()) {
        errmsg = str::stream() << "Error connecting to " << peer << " (" << endpoint.toString()
                               << ")";
    } else {
        errmsg = str::stream() << "Error connecting to " << peer;
    }

    return status.withContext(errmsg);
}


StatusWith<std::shared_ptr<Session>> AsioTransportLayer::connect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    boost::optional<TransientSSLParams> transientSSLParams) {
    if (transientSSLParams) {
        uassert(ErrorCodes::InvalidSSLConfiguration,
                "Specified transient SSL params but connection SSL mode is not set",
                sslMode == kEnableSSL);
        LOGV2_DEBUG(
            5270701, 2, "Connecting to peer using transient SSL connection", "peer"_attr = peer);
    }

    std::error_code ec;
    AsioSession::GenericSocket sock(*_egressReactor);
    WrappedResolver resolver(*_egressReactor);

    Date_t timeBefore = Date_t::now();
    auto swEndpoints = resolver.resolve(peer, _listenerOptions.enableIPv6);
    Date_t timeAfter = Date_t::now();
    if (timeAfter - timeBefore > kSlowOperationThreshold) {
        networkCounter.incrementNumSlowDNSOperations();
    }

    if (!swEndpoints.isOK()) {
        return swEndpoints.getStatus();
    }

    auto endpoints = std::move(swEndpoints.getValue());
    auto sws = _doSyncConnect(endpoints.front(), peer, timeout, transientSSLParams);
    if (!sws.isOK()) {
        return sws.getStatus();
    }

    auto session = std::move(sws.getValue());
    session->ensureSync();

#ifndef _WIN32
    if (endpoints.front().family() == AF_UNIX) {
        return static_cast<std::shared_ptr<Session>>(std::move(session));
    }
#endif

#ifndef MONGO_CONFIG_SSL
    if (sslMode == kEnableSSL) {
        return {ErrorCodes::InvalidSSLConfiguration, "SSL requested but not supported"};
    }
#else
    auto globalSSLMode = this->sslMode();
    if (sslMode == kEnableSSL ||
        (sslMode == kGlobalSSLMode &&
         ((globalSSLMode == SSLParams::SSLMode_preferSSL) ||
          (globalSSLMode == SSLParams::SSLMode_requireSSL)))) {

        if (auto sslStatus = session->buildSSLSocket(peer); !sslStatus.isOK()) {
            return sslStatus;
        }

        // The handshake is complete once either of the following passes the finish line:
        // - The thread running the handshake returns from `handshakeSSLForEgress`.
        // - The thread running `TimerService` cancels the handshake due to a timeout.
        auto finishLine = std::make_shared<StrongWeakFinishLine>(2);

        // Schedules a task to cancel the synchronous handshake if it does not complete before the
        // specified timeout.
        auto timer = _timerService->makeTimer();

#ifndef _WIN32
        // TODO SERVER-62035: enable the following on Windows.
        if (timeout > Milliseconds(0)) {
            timer->waitUntil(_timerService->now() + timeout)
                .getAsync([finishLine, session](Status status) {
                    if (status.isOK() && finishLine->arriveStrongly()) {
                        session->end();
                    }
                });
        }
#endif

        Date_t timeBefore = Date_t::now();
        auto sslStatus = session->handshakeSSLForEgress(peer, nullptr).getNoThrow();
        Date_t timeAfter = Date_t::now();

        if (timeAfter - timeBefore > kSlowOperationThreshold) {
            networkCounter.incrementNumSlowSSLOperations();
        }

        if (finishLine->arriveStrongly()) {
            timer->cancel();
        } else if (!sslStatus.isOK()) {
            // We only take this path if the handshake times out. Overwrite the socket exception
            // with a network timeout.
            auto errMsg = fmt::format("SSL handshake timed out after {}",
                                      (timeAfter - timeBefore).toString());
            sslStatus = Status(ErrorCodes::NetworkTimeout, errMsg);
            LOGV2(5490001,
                  "Timed out while running handshake",
                  "peer"_attr = peer,
                  "timeout"_attr = timeout);
        }

        if (!sslStatus.isOK()) {
            return sslStatus;
        }
    }
#endif

    return static_cast<std::shared_ptr<Session>>(std::move(session));
}

template <typename Endpoint>
StatusWith<std::shared_ptr<AsioSession>> AsioTransportLayer::_doSyncConnect(
    Endpoint endpoint,
    const HostAndPort& peer,
    const Milliseconds& timeout,
    boost::optional<TransientSSLParams> transientSSLParams) {
    AsioSession::GenericSocket sock(*_egressReactor);
    std::error_code ec;

    const auto protocol = endpoint->protocol();
    sock.open(protocol);

#ifdef TCP_FASTOPEN_CONNECT
    const auto family = protocol.family();
    if ((family == AF_INET) || (family == AF_INET6)) {
        setSocketOption(sock,
                        TcpFastOpenConnectOption(gTCPFastOpenClient),
                        "connect (sync) TCP fast open",
                        logv2::LogSeverity::Info(),
                        ec);
        if (ec && tcpFastOpenIsConfigured) {
            return errorCodeToStatus(ec, "syncConnect tcpFastOpenIsConfigured");
        }
        ec = std::error_code();
    }
#endif

    sock.non_blocking(true);

    auto now = Date_t::now();
    auto expiration = now + timeout;
    do {
        auto curTimeout = expiration - now;
        sock.connect(*endpoint, curTimeout.toSystemDuration(), ec);
        if (ec) {
            now = Date_t::now();
        }
        // We loop below if ec == interrupted to deal with EINTR failures, otherwise we handle
        // the error/timeout below.
    } while (ec == asio::error::interrupted && now < expiration);

    auto status = [&] {
        if (ec) {
            return errorCodeToStatus(ec, "syncConnect connect error");
        } else if (now >= expiration) {
            return Status(ErrorCodes::NetworkTimeout, "Timed out");
        } else {
            return Status::OK();
        }
    }();

    if (!status.isOK()) {
        return makeConnectError(status, peer, endpoint);
    }

    sock.non_blocking(false);
    try {
        std::shared_ptr<const transport::SSLConnectionContext> transientSSLContext;
#ifdef MONGO_CONFIG_SSL
        if (transientSSLParams) {
            auto statusOrContext = createTransientSSLContext(transientSSLParams.value());
            uassertStatusOK(statusOrContext.getStatus());
            transientSSLContext = std::move(statusOrContext.getValue());
        }
#endif
        return std::make_shared<SyncAsioSession>(
            this, std::move(sock), false, *endpoint, transientSSLContext);
    } catch (const asio::system_error& e) {
        return errorCodeToStatus(e.code(), "syncConnect AsioSession constructor");
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

Future<std::shared_ptr<Session>> AsioTransportLayer::asyncConnect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    const ReactorHandle& reactor,
    Milliseconds timeout,
    std::shared_ptr<ConnectionMetrics> connectionMetrics,
    std::shared_ptr<const SSLConnectionContext> transientSSLContext) {
    if (MONGO_unlikely(asioTransportLayerAsyncConnectReturnsConnectionError.shouldFail()))
        return Status{ErrorCodes::ConnectionError, "Failing asyncConnect due to fail-point"};

    invariant(connectionMetrics);
    connectionMetrics->onConnectionStarted();

    if (transientSSLContext) {
        uassert(ErrorCodes::InvalidSSLConfiguration,
                "Specified transient SSL context but connection SSL mode is not set",
                sslMode == kEnableSSL);
        LOGV2_DEBUG(
            5270601, 2, "Connecting to peer using transient SSL connection", "peer"_attr = peer);
    }

    struct AsyncConnectState {
        AsyncConnectState(HostAndPort peer,
                          asio::io_context& context,
                          Promise<std::shared_ptr<Session>> promise_,
                          const ReactorHandle& reactor)
            : promise(std::move(promise_)),
              socket(context),
              timeoutTimer(context),
              resolver(context),
              peer(std::move(peer)),
              reactor(reactor) {}

        AtomicWord<bool> done{false};
        Promise<std::shared_ptr<Session>> promise;

        Mutex mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "AsyncConnectState::mutex");
        AsioSession::GenericSocket socket;
        AsioReactorTimer timeoutTimer;
        WrappedResolver resolver;
        WrappedEndpoint resolvedEndpoint;
        const HostAndPort peer;
        std::shared_ptr<AsioSession> session;
        ReactorHandle reactor;
    };

    auto reactorImpl = checked_cast<AsioReactor*>(reactor.get());
    auto pf = makePromiseFuture<std::shared_ptr<Session>>();
    auto connector = std::make_shared<AsyncConnectState>(
        std::move(peer), *reactorImpl, std::move(pf.promise), reactor);
    Future<std::shared_ptr<Session>> mergedFuture = std::move(pf.future);

    if (connector->peer.host().empty()) {
        return Status{ErrorCodes::HostNotFound, "Hostname or IP address to connect to is empty"};
    }

    if (timeout > Milliseconds{0} && timeout < Milliseconds::max()) {
        connector->timeoutTimer.waitUntil(reactor->now() + timeout)
            .getAsync([connector](Status status) {
                if (status == ErrorCodes::CallbackCanceled || connector->done.swap(true)) {
                    return;
                }

                connector->promise.setError(
                    makeConnectError({ErrorCodes::NetworkTimeout, "Connecting timed out"},
                                     connector->peer,
                                     connector->resolvedEndpoint));

                std::error_code ec;
                stdx::lock_guard<Latch> lk(connector->mutex);
                connector->resolver.cancel();
                if (connector->session) {
                    connector->session->end();
                } else {
                    connector->socket.cancel(ec);
                }
            });
    }

    Date_t timeBefore = Date_t::now();

    auto resolverFuture = [&]() {
        if (auto sfp = asioTransportLayerDelayConnection.scoped(); MONGO_unlikely(sfp.isActive())) {
            Milliseconds delay{sfp.getData()["millis"].safeNumberInt()};
            Date_t deadline = reactor->now() + delay;
            if ((delay > Milliseconds(0)) && (deadline < Date_t::max())) {
                LOGV2(6885900,
                      "delayConnection fail point is active, delaying connection establishment",
                      "delay"_attr = delay);

                // Normally, the unique_ptr returned by makeTimer() is stored somewhere where we can
                // ensure its validity. Here, we have to make it a shared_ptr and capture it so it
                // remains valid until the timer fires.
                std::shared_ptr<ReactorTimer> delayTimer = reactor->makeTimer();
                return delayTimer->waitUntil(deadline).then(
                    [delayTimer, connector, enableIPv6 = _listenerOptions.enableIPv6] {
                        LOGV2(6885901, "finished delaying the connection");
                        return connector->resolver.asyncResolve(connector->peer, enableIPv6);
                    });
            }
        }
        return connector->resolver.asyncResolve(connector->peer, _listenerOptions.enableIPv6);
    }();

    std::move(resolverFuture)
        .then([connector, timeBefore, connectionMetrics](WrappedResolver::EndpointVector results) {
            try {
                connectionMetrics->onDNSResolved();

                Date_t timeAfter = Date_t::now();
                if (timeAfter - timeBefore > kSlowOperationThreshold) {
                    LOGV2_WARNING(23019,
                                  "DNS resolution while connecting to {peer} took {duration}",
                                  "DNS resolution while connecting to peer was slow",
                                  "peer"_attr = connector->peer,
                                  "duration"_attr = timeAfter - timeBefore);
                    networkCounter.incrementNumSlowDNSOperations();
                }

                stdx::lock_guard<Latch> lk(connector->mutex);

                connector->resolvedEndpoint = results.front();
                connector->socket.open(connector->resolvedEndpoint->protocol());
                connector->socket.non_blocking(true);
            } catch (asio::system_error& ex) {
                return futurize(ex.code());
            }

#ifdef TCP_FASTOPEN_CONNECT
            std::error_code ec;
            setSocketOption(connector->socket,
                            TcpFastOpenConnectOption(gTCPFastOpenClient),
                            "connect (async) TCP fast open",
                            logv2::LogSeverity::Info(),
                            ec);
            if (ec && tcpFastOpenIsConfigured) {
                return futurize(ec);
            }
#endif
            return connector->socket.async_connect(*connector->resolvedEndpoint, UseFuture{});
        })
        .then([this, connector, sslMode, transientSSLContext, connectionMetrics]() -> Future<void> {
            connectionMetrics->onTCPConnectionEstablished();

            stdx::unique_lock<Latch> lk(connector->mutex);
            connector->session = [&] {
                try {
                    return std::make_shared<AsyncAsioSession>(this,
                                                              std::move(connector->socket),
                                                              false,
                                                              *connector->resolvedEndpoint,
                                                              transientSSLContext);
                } catch (const asio::system_error& e) {
                    iasserted(errorCodeToStatus(e.code(), "asyncConnect AsioSession constructor"));
                }
            }();
            connector->session->ensureAsync();

#ifndef MONGO_CONFIG_SSL
            if (sslMode == kEnableSSL) {
                uasserted(ErrorCodes::InvalidSSLConfiguration, "SSL requested but not supported");
            }
#else
            auto globalSSLMode = this->sslMode();
            if (sslMode == kEnableSSL ||
                (sslMode == kGlobalSSLMode &&
                 ((globalSSLMode == SSLParams::SSLMode_preferSSL) ||
                  (globalSSLMode == SSLParams::SSLMode_requireSSL)))) {
                if (const auto sslStatus = connector->session->buildSSLSocket(connector->peer);
                    !sslStatus.isOK()) {
                    return sslStatus;
                }
                Date_t timeBefore = Date_t::now();
                return connector->session
                    ->handshakeSSLForEgress(connector->peer, connector->reactor)
                    .then([connector, timeBefore, connectionMetrics] {
                        connectionMetrics->onTLSHandshakeFinished();

                        Date_t timeAfter = Date_t::now();
                        if (timeAfter - timeBefore > kSlowOperationThreshold) {
                            networkCounter.incrementNumSlowSSLOperations();
                        }
                        return Status::OK();
                    });
            }
#endif
            return Status::OK();
        })
        .onError([connector](Status status) -> Future<void> {
            return makeConnectError(status, connector->peer, connector->resolvedEndpoint);
        })
        .getAsync([connector](Status connectResult) {
            if (MONGO_unlikely(asioTransportLayerAsyncConnectTimesOut.shouldFail())) {
                LOGV2(23013, "asyncConnectTimesOut fail point is active. simulating timeout.");
                return;
            }

            if (connector->done.swap(true)) {
                return;
            }

            connector->timeoutTimer.cancel();
            if (connectResult.isOK()) {
                connector->promise.emplaceValue(std::move(connector->session));
            } else {
                connector->promise.setError(connectResult);
            }
        });

    return mergedFuture;
}

namespace {
#if defined(TCP_FASTOPEN) || defined(TCP_FASTOPEN_CONNECT)
/**
 * Attempt to set an option on a dummy SOCK_STREAM/AF_INET socket
 * and report success/failure.
 */
bool trySetSockOpt(int level, int opt, int val) {
    auto sock = ::socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) {
        auto ec = lastSocketError();
        LOGV2_WARNING(5128700, "socket() failed", "error"_attr = errorMessage(ec));
        return false;
    }

#ifdef _WIN32
    char* pval = reinterpret_cast<char*>(&val);
#else
    void* pval = &val;
#endif

    const auto ret = ::setsockopt(sock, level, opt, pval, sizeof(val));

#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif

    return ret == 0;
}
#endif

Status validateFastOpen() noexcept {
    namespace moe = optionenvironment;
    if (moe::startupOptionsParsed.count("setParameter")) {
        const auto params =
            moe::startupOptionsParsed["setParameter"].as<std::map<std::string, std::string>>();
        tcpFastOpenIsConfigured = (params.find("tcpFastOpenServer") != params.end()) ||
            (params.find("tcpFastOpenClient") != params.end()) ||
            (params.find("tcpFastOpenQueueSize") != params.end());
    }

#ifndef TCP_FASTOPEN
    if (tcpFastOpenIsConfigured && gTCPFastOpenServer) {
        return {ErrorCodes::BadValue,
                "TCP FastOpen server support unavailable in this build of MongoDB"};
    }
#else
    networkCounter.setTFOServerSupport(trySetSockOpt(IPPROTO_TCP, TCP_FASTOPEN, 1));
#endif

#ifndef TCP_FASTOPEN_CONNECT
    if (tcpFastOpenIsConfigured && gTCPFastOpenClient) {
        return {ErrorCodes::BadValue,
                "TCP FastOpen client support unavailable in this build of MongoDB"};
    }
#else
    networkCounter.setTFOClientSupport(trySetSockOpt(IPPROTO_TCP, TCP_FASTOPEN_CONNECT, 1));
#endif

#if defined(TCP_FASTOPEN) && defined(__linux)
    if (!gTCPFastOpenServer && !gTCPFastOpenClient) {
        return Status::OK();
    }

    std::string procfile("/proc/sys/net/ipv4/tcp_fastopen");
    boost::system::error_code ec;
    if (!boost::filesystem::exists(procfile, ec)) {
        return {ErrorCodes::BadValue,
                str::stream() << "Unable to locate " << procfile << ": " << errorCodeToStatus(ec)};
    }

    std::fstream f(procfile, std::ifstream::in);
    if (!f.is_open()) {
        return {ErrorCodes::BadValue, str::stream() << "Unable to read " << procfile};
    }

    std::int64_t val;
    f >> val;
    networkCounter.setTFOKernelSetting(val);

    constexpr std::int64_t kTFOClientBit = (1 << 0);
    constexpr std::int64_t kTFOServerBit = (1 << 1);

    // Future proof this setting by allowing extra bits to stay set in help output.
    std::int64_t wantval = val;
    if (gTCPFastOpenClient) {
        wantval |= kTFOClientBit;
    }
    if (gTCPFastOpenServer) {
        wantval |= kTFOServerBit;
    }

    if (val != wantval) {
        return {ErrorCodes::BadValue,
                str::stream() << "TCP FastOpen disabled in kernel. "
                              << "Set " << procfile << " to " << std::to_string(wantval)};
    }
#endif

    return Status::OK();
}

Status validateFastOpenOnce() noexcept {
    if (!maybeTcpFastOpenStatus) {
        // If we haven't validated the TCP FastOpen situation yet, do so.
        maybeTcpFastOpenStatus = validateFastOpen();

        if (!maybeTcpFastOpenStatus->isOK()) {
            // This has to be a char[] because that's what logv2 understands
            static constexpr char kPrefixString[] = "Unable to enable TCP FastOpen";

            if (tcpFastOpenIsConfigured) {
                // If the user asked for TCP FastOpen and we couldn't provide it, log a startup
                // warning in addition to the hard failure.
                LOGV2_WARNING_OPTIONS(23014,
                                      {logv2::LogTag::kStartupWarnings},
                                      kPrefixString,
                                      "reason"_attr = maybeTcpFastOpenStatus->reason());
            } else {
                LOGV2(4648601,
                      "Implicit TCP FastOpen unavailable. "
                      "If TCP FastOpen is required, set tcpFastOpenServer, tcpFastOpenClient, "
                      "and tcpFastOpenQueueSize.");
            }

            maybeTcpFastOpenStatus->addContext(kPrefixString);
        } else {
            if (!tcpFastOpenIsConfigured) {
                LOGV2(4648602, "Implicit TCP FastOpen in use.");
            }
        }
    }

    if (!tcpFastOpenIsConfigured) {
        // If nobody asked for TCP FastOpen, no one will miss it.
        return Status::OK();
    }

    // TCP FastOpen was requested. It's either there or it's not.
    return *maybeTcpFastOpenStatus;
}
}  // namespace

Status AsioTransportLayer::setup() {
    std::vector<std::string> listenAddrs;
    if (_listenerOptions.ipList.empty() && _listenerOptions.isIngress()) {
        listenAddrs = {"127.0.0.1"};
        if (_listenerOptions.enableIPv6) {
            listenAddrs.emplace_back("::1");
        }
    } else if (!_listenerOptions.ipList.empty()) {
        listenAddrs = _listenerOptions.ipList;
    }

#ifndef _WIN32
    if (_listenerOptions.useUnixSockets && _listenerOptions.isIngress()) {
        listenAddrs.push_back(makeUnixSockPath(_listenerOptions.port));

        if (_listenerOptions.loadBalancerPort) {
            listenAddrs.push_back(makeUnixSockPath(*_listenerOptions.loadBalancerPort));
        }
    }
#endif

    if (auto foStatus = validateFastOpenOnce(); !foStatus.isOK()) {
        return foStatus;
    }

    if (!(_listenerOptions.isIngress()) && !listenAddrs.empty()) {
        return {ErrorCodes::BadValue,
                "Cannot bind to listening sockets with ingress networking is disabled"};
    }

    _listenerPort = _listenerOptions.port;
    WrappedResolver resolver(*_acceptorReactor);

    std::vector<int> ports = {_listenerPort};
    if (_listenerOptions.loadBalancerPort) {
        ports.push_back(*_listenerOptions.loadBalancerPort);
    }

    // Self-deduplicating list of unique endpoint addresses.
    std::set<WrappedEndpoint> endpoints;
    for (const auto& port : ports) {
        for (const auto& listenAddr : listenAddrs) {
            if (listenAddr.empty()) {
                LOGV2_WARNING(23020, "Skipping empty bind address");
                continue;
            }

            const auto& swAddrs =
                resolver.resolve(HostAndPort(listenAddr, port), _listenerOptions.enableIPv6);
            if (!swAddrs.isOK()) {
                LOGV2_WARNING(23021,
                              "Found no addresses for {peer}",
                              "Found no addresses for peer",
                              "peer"_attr = swAddrs.getStatus());
                continue;
            }
            const auto& addrs = swAddrs.getValue();
            endpoints.insert(addrs.begin(), addrs.end());
        }
    }

    for (const auto& addr : endpoints) {
#ifndef _WIN32
        if (addr.family() == AF_UNIX) {
            if (::unlink(addr.toString().c_str()) == -1) {
                auto ec = lastPosixError();
                if (ec != posixError(ENOENT)) {
                    LOGV2_ERROR(23024,
                                "Failed to unlink socket file {path} {error}",
                                "Failed to unlink socket file",
                                "path"_attr = addr.toString().c_str(),
                                "error"_attr = errorMessage(ec));
                    fassertFailedNoTrace(40486);
                }
            }
        }
#endif
        if (addr.family() == AF_INET6 && !_listenerOptions.enableIPv6) {
            LOGV2_ERROR(23025, "Specified ipv6 bind address, but ipv6 is disabled");
            fassertFailedNoTrace(40488);
        }

        GenericAcceptor acceptor(*_acceptorReactor);
        try {
            acceptor.open(addr->protocol());
        } catch (std::exception&) {
            // Allow the server to start when "ipv6: true" and "bindIpAll: true", but the platform
            // does not support ipv6 (e.g., ipv6 kernel module is not loaded in Linux).
            auto bindAllFmt = [](auto p) {
                return fmt::format(":::{}", p);
            };
            bool addrIsBindAll = addr.toString() == bindAllFmt(_listenerPort);

            if (!addrIsBindAll && _listenerOptions.loadBalancerPort) {
                addrIsBindAll = (addr.toString() == bindAllFmt(*_listenerOptions.loadBalancerPort));
            }

            if (errno == EAFNOSUPPORT && _listenerOptions.enableIPv6 && addr.family() == AF_INET6 &&
                addrIsBindAll) {
                LOGV2_WARNING(4206501,
                              "Failed to bind to address as the platform does not support ipv6",
                              "Failed to bind to {address} as the platform does not support ipv6",
                              "address"_attr = addr.toString());
                continue;
            }

            throw;
        }
        setSocketOption(acceptor,
                        GenericAcceptor::reuse_address(true),
                        "acceptor reuse address",
                        logv2::LogSeverity::Info());

        std::error_code ec;
#ifdef TCP_FASTOPEN
        if (gTCPFastOpenServer && ((addr.family() == AF_INET) || (addr.family() == AF_INET6))) {
            setSocketOption(acceptor,
                            TcpFastOpenOption(gTCPFastOpenQueueSize),
                            "acceptor TCP fast open",
                            logv2::LogSeverity::Info(),
                            ec);
            if (ec && tcpFastOpenIsConfigured) {
                return errorCodeToStatus(ec, "setup tcpFastOpenIsConfigured");
            }
            ec = std::error_code();
        }
#endif
        if (addr.family() == AF_INET6) {
            setSocketOption(
                acceptor, asio::ip::v6_only(true), "acceptor v6 only", logv2::LogSeverity::Info());
        }

        acceptor.non_blocking(true, ec);
        if (ec) {
            return errorCodeToStatus(ec, "setup non_blocking");
        }

        acceptor.bind(*addr, ec);
        if (ec) {
            return errorCodeToStatus(ec, "setup bind");
        }

#ifndef _WIN32
        if (addr.family() == AF_UNIX) {
            if (::chmod(addr.toString().c_str(), serverGlobalParams.unixSocketPermissions) == -1) {
                auto ec = lastPosixError();
                LOGV2_ERROR(23026,
                            "Failed to chmod socket file {path} {error}",
                            "Failed to chmod socket file",
                            "path"_attr = addr.toString().c_str(),
                            "error"_attr = errorMessage(ec));
                fassertFailedNoTrace(40487);
            }
        }
#endif
        auto endpoint = acceptor.local_endpoint(ec);
        if (ec) {
            return errorCodeToStatus(ec);
        }
        auto hostAndPort = endpointToHostAndPort(endpoint);

        auto record = std::make_unique<AcceptorRecord>(SockAddr(addr->data(), addr->size()),
                                                       std::move(acceptor));

        if (_listenerOptions.port == 0 && (addr.family() == AF_INET || addr.family() == AF_INET6)) {
            if (_listenerPort != _listenerOptions.port) {
                return Status(ErrorCodes::BadValue,
                              "Port 0 (ephemeral port) is not allowed when"
                              " listening on multiple IP interfaces");
            }
            _listenerPort = hostAndPort.port();
            record->address.setPort(_listenerPort);
        }

        _acceptorRecords.push_back(std::move(record));
    }

    if (_acceptorRecords.empty() && _listenerOptions.isIngress()) {
        return Status(ErrorCodes::SocketException, "No available addresses/ports to bind to");
    }

#ifdef MONGO_CONFIG_SSL
    std::shared_ptr<SSLManagerInterface> manager = nullptr;
    if (SSLManagerCoordinator::get()) {
        manager = SSLManagerCoordinator::get()->getSSLManager();
    }
    return rotateCertificates(manager, true);
#endif

    return Status::OK();
}

std::vector<std::pair<SockAddr, int>> AsioTransportLayer::getListenerSocketBacklogQueueDepths()
    const {
    std::vector<std::pair<SockAddr, int>> queueDepths;
    for (auto&& record : _acceptorRecords) {
        queueDepths.push_back({SockAddr(record->address), record->backlogQueueDepth.load()});
    }
    return queueDepths;
}

void AsioTransportLayer::appendStatsForServerStatus(BSONObjBuilder* bob) const {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (gFeatureFlagConnHealthMetrics.isEnabledAndIgnoreFCVUnsafe()) {
        bob->append("listenerProcessingTime", _listenerProcessingTime.load().toBSON());
    }
    bob->append("connsRejectedDueToMaxPendingProxyProtocolHeader",
                _discardedDueToMaximumPendingOnProxyHeader.get());
}

void AsioTransportLayer::appendStatsForFTDC(BSONObjBuilder& bob) const {
    // (Ignore FCV check): This feature flag doesn't have any upgrade/downgrade concerns.
    if (gFeatureFlagConnHealthMetrics.isEnabledAndIgnoreFCVUnsafe()) {
        BSONArrayBuilder queueDepthsArrayBuilder(
            bob.subarrayStart("listenerSocketBacklogQueueDepths"));
        for (const auto& record : _acceptorRecords) {
            BSONObjBuilder{queueDepthsArrayBuilder.subobjStart()}.append(
                record->address.toString(), record->backlogQueueDepth.load());
        }
        queueDepthsArrayBuilder.done();
    }
    bob.append("connsDiscardedDueToClientDisconnect", _discardedDueToClientDisconnect.get());
}

void AsioTransportLayer::_runListener() noexcept {
    setThreadName("listener");

    stdx::unique_lock lk(_mutex);
    if (_isShutdown.load()) {
        return;
    }

    for (auto& acceptorRecord : _acceptorRecords) {
        asio::error_code ec;
        acceptorRecord->acceptor.listen(serverGlobalParams.listenBacklog, ec);
        if (ec) {
            LOGV2_FATAL(31339,
                        "Error listening for new connections on {listenAddress}: {error}",
                        "Error listening for new connections on listen address",
                        "listenAddrs"_attr = acceptorRecord->address,
                        "error"_attr = ec.message());
        }

        _acceptConnection(acceptorRecord->acceptor);
        LOGV2(23015, "Listening on", "address"_attr = acceptorRecord->address.getAddr());
    }

    const char* ssl = "off";
#ifdef MONGO_CONFIG_SSL
    if (sslMode() != SSLParams::SSLMode_disabled) {
        ssl = "on";
    }
#endif
    LOGV2(23016, "Waiting for connections", "port"_attr = _listenerPort, "ssl"_attr = ssl);

    _listener.active = true;
    _listener.cv.notify_all();
    ON_BLOCK_EXIT([&] {
        _listener.active = false;
        _listener.cv.notify_all();
    });

    while (!_isShutdown.load()) {
        lk.unlock();
        _acceptorReactor->run();
        lk.lock();
    }

    // Loop through the acceptors and cancel their calls to async_accept. This will prevent new
    // connections from being opened.
    for (auto& acceptorRecord : _acceptorRecords) {
        acceptorRecord->acceptor.cancel();
        auto& addr = acceptorRecord->address;
        if (addr.getType() == AF_UNIX && !addr.isAnonymousUNIXSocket()) {
            auto path = addr.getAddr();
            LOGV2(
                23017, "removing socket file: {path}", "removing socket file", "path"_attr = path);
            if (::unlink(path.c_str()) != 0) {
                auto ec = lastPosixError();
                LOGV2_WARNING(23022,
                              "Unable to remove UNIX socket {path}: {error}",
                              "Unable to remove UNIX socket",
                              "path"_attr = path,
                              "error"_attr = errorMessage(ec));
            }
        }
    }
}

Status AsioTransportLayer::start() {
    stdx::unique_lock lk(_mutex);
    if (_isShutdown.load()) {
        LOGV2(6986801, "Cannot start an already shutdown TransportLayer");
        return ShutdownStatus;
    }

    if (_listenerOptions.isIngress()) {
        _listener.thread = stdx::thread([this] { _runListener(); });
        _listener.cv.wait(lk, [&] { return _isShutdown.load() || _listener.active; });
        return Status::OK();
    }

    invariant(_acceptorRecords.empty());
    return Status::OK();
}

void AsioTransportLayer::shutdown() {
    stdx::unique_lock lk(_mutex);

    if (_isShutdown.swap(true)) {
        // We were already stopped
        return;
    }
    lk.unlock();
    _timerService->stop();
    lk.lock();

    if (!_listenerOptions.isIngress()) {
        // Egress only reactors never start a listener
        return;
    }

    auto thread = std::exchange(_listener.thread, {});
    if (!thread.joinable()) {
        // If the listener never started, then we can return now
        return;
    }

    // Spam stop() on the reactor, it interrupts run()
    while (_listener.active) {
        lk.unlock();
        _acceptorReactor->stop();
        lk.lock();
    }

    // Release the lock and wait for the thread to die
    lk.unlock();
    thread.join();
}

ReactorHandle AsioTransportLayer::getReactor(WhichReactor which) {
    switch (which) {
        case TransportLayer::kIngress:
            return _ingressReactor;
        case TransportLayer::kEgress:
            return _egressReactor;
        case TransportLayer::kNewReactor:
            return std::make_shared<AsioReactor>();
    }

    MONGO_UNREACHABLE;
}

namespace {
bool isConnectionResetError(const std::error_code& ec) {
    // Connection reset errors classically present as asio::error::eof, but can bubble up as
    // asio::error::invalid_argument when calling into socket.set_option().
    return ec == asio::error::eof || ec == asio::error::invalid_argument;
}

/** Tricky: TCP can be represented by IPPROTO_IP or IPPROTO_TCP. */
template <typename Protocol>
bool isTcp(Protocol&& p) {
    auto pf = p.family();
    auto pt = p.type();
    auto pp = p.protocol();
    return (pf == AF_INET || pf == AF_INET6) && (pt == SOCK_STREAM) &&
        (pp == IPPROTO_IP || pp == IPPROTO_TCP);
}
}  // namespace

std::unique_ptr<AsioTransportLayer::TokenType>
AsioTransportLayer::_makeParseProxyTokenIfPossible() {
    stdx::lock_guard lk(_mutex);
    if (_numConnectionsPendingProxyHeader >= gProxyProtocolMaximumPendingConnections.load()) {
        return {};
    }
    ++_numConnectionsPendingProxyHeader;
    return std::make_unique<TokenType>([this] {
        stdx::lock_guard lk(_mutex);
        --_numConnectionsPendingProxyHeader;
    });
}

void AsioTransportLayer::_acceptConnection(GenericAcceptor& acceptor) {
    auto acceptCb = [this, &acceptor](const std::error_code& ec,
                                      AsioSession::GenericSocket peerSocket) mutable {
        Timer timer;
        asioTransportLayerHangDuringAcceptCallback.pauseWhileSet();

        if (_isShutdown.load()) {
            LOGV2_DEBUG(9484001, 3, "Unable to accept connection: transport layer in shutdown");
            return;
        }

        if (ec) {
            LOGV2(23018,
                  "Error accepting new connection on {localEndpoint}: {error}",
                  "Error accepting new connection on local endpoint",
                  "localEndpoint"_attr = endpointToHostAndPort(acceptor.local_endpoint()),
                  "error"_attr = ec.message());
            _acceptConnection(acceptor);
            return;
        }

        if (MONGO_unlikely(shouldDiscardSocketDueToLostConnectivity(peerSocket))) {
            _discardedDueToClientDisconnect.increment();
            _acceptConnection(acceptor);
            return;
        }

#ifdef TCPI_OPT_SYN_DATA
        try {
            TcpInfoOption tcpi{};
            peerSocket.get_option(tcpi);
            if (tcpi->tcpi_options & TCPI_OPT_SYN_DATA)
                networkCounter.acceptedTFOIngress();
        } catch (const asio::system_error&) {
        }
#endif

        try {
            std::shared_ptr<AsioSession> session(
                new SyncAsioSession(this, std::move(peerSocket), true));
            if (session->isConnectedToLoadBalancerPort()) {
                // This session is not counted towards the number of accepted connections until the
                // server receives the proxy header and moves the session to `SessionManager`.
                // Therefore, the server may go above the ingress connection limits if it is waiting
                // to receive the proxy header for many accepted connections.
                // Thus, the server limits the number of connections received on the proxy port that
                // are yet to provide the proxy header.
                auto token = _makeParseProxyTokenIfPossible();
                if (MONGO_unlikely(!token)) {
                    static logv2::SeveritySuppressor severitySuppressor{
                        Seconds(10), logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2)};
                    _discardedDueToMaximumPendingOnProxyHeader.increment();
                    LOGV2_DEBUG(
                        11246701,
                        severitySuppressor().toInt(),
                        "Rejecting proxy connection due to reaching the limit on connections "
                        "pending on proxy protocol header",
                        "remote"_attr = session->remote(),
                        "limit"_attr = gProxyProtocolMaximumPendingConnections.loadRelaxed(),
                        "rejected"_attr = _discardedDueToMaximumPendingOnProxyHeader.get());
                    _acceptConnection(acceptor);
                    return;
                }
                session->parseProxyProtocolHeader(_acceptorReactor)
                    .getAsync([this, session = std::move(session), t = std::move(token)](Status s) {
                        if (s.isOK()) {
                            _sep->startSession(std::move(session));
                        }
                        // We will release the token (i.e. `t`) as we leave this function.
                    });
            } else {
                _sep->startSession(std::move(session));
            }
        } catch (const asio::system_error& e) {
            // Swallow connection reset errors.
            if (!isConnectionResetError(e.code())) {
                LOGV2_WARNING(5746600,
                              "Error accepting new connection: {error}",
                              "Error accepting new connection",
                              "error"_attr = e.code().message());
            }
        } catch (const DBException& e) {
            LOGV2_WARNING(23023,
                          "Error accepting new connection: {error}",
                          "Error accepting new connection",
                          "error"_attr = e);
        }

        // _acceptConnection() is accessed by only one thread (i.e. the listener thread), so an
        // atomic increment is not required here
        _listenerProcessingTime.store(_listenerProcessingTime.load() + timer.elapsed());
        _acceptConnection(acceptor);
    };

    asioTransportLayerHangBeforeAcceptCallback.pauseWhileSet();

    _trySetListenerSocketBacklogQueueDepth(acceptor);

    acceptor.async_accept(*_ingressReactor, std::move(acceptCb));
}

void AsioTransportLayer::_trySetListenerSocketBacklogQueueDepth(
    GenericAcceptor& acceptor) noexcept {
#ifdef __linux__
    try {
        if (!isTcp(acceptor.local_endpoint().protocol()))
            return;
        auto matchingRecord =
            std::find_if(begin(_acceptorRecords), end(_acceptorRecords), [&](const auto& record) {
                return acceptor.local_endpoint() == record->acceptor.local_endpoint();
            });
        invariant(matchingRecord != std::end(_acceptorRecords));
        TcpInfoOption tcpi;
        acceptor.get_option(tcpi);
        (*matchingRecord)->backlogQueueDepth.store(tcpi->tcpi_unacked);
    } catch (const asio::system_error& e) {
        // Swallow connection reset errors.
        if (!isConnectionResetError(e.code())) {
            LOGV2_WARNING(7006800,
                          "Error retrieving tcp acceptor socket queue length",
                          "error"_attr = e.code().message());
        }
    }
#endif
}

#ifdef MONGO_CONFIG_SSL
SSLParams::SSLModes AsioTransportLayer::sslMode() const {
    return static_cast<SSLParams::SSLModes>(getSSLGlobalParams().sslMode.load());
}

Status AsioTransportLayer::rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                                              bool asyncOCSPStaple) {
    if (manager && manager->isTransient()) {
        return Status(ErrorCodes::InternalError,
                      "Should not rotate transient SSL manager's certificates");
    }
    auto contextOrStatus = _createSSLContext(manager, sslMode(), asyncOCSPStaple);
    if (!contextOrStatus.isOK()) {
        return contextOrStatus.getStatus();
    }
    _sslContext = std::move(contextOrStatus.getValue());
    return Status::OK();
}

StatusWith<std::shared_ptr<const transport::SSLConnectionContext>>
AsioTransportLayer::_createSSLContext(std::shared_ptr<SSLManagerInterface>& manager,
                                      SSLParams::SSLModes sslMode,
                                      bool asyncOCSPStaple) const {

    std::shared_ptr<SSLConnectionContext> newSSLContext = std::make_shared<SSLConnectionContext>();
    newSSLContext->manager = manager;
    const auto& sslParams = getSSLGlobalParams();

    if (sslMode != SSLParams::SSLMode_disabled && _listenerOptions.isIngress()) {
        newSSLContext->ingress = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);

        Status status = newSSLContext->manager->initSSLContext(
            newSSLContext->ingress->native_handle(),
            sslParams,
            SSLManagerInterface::ConnectionDirection::kIncoming);
        if (!status.isOK()) {
            return status;
        }

        std::weak_ptr<const SSLConnectionContext> weakContextPtr = newSSLContext;
        manager->registerOwnedBySSLContext(weakContextPtr);
        auto resp = newSSLContext->manager->stapleOCSPResponse(
            newSSLContext->ingress->native_handle(), asyncOCSPStaple);

        if (!resp.isOK()) {
            // The stapleOCSPResponse call above may have started a periodic OCSP fetch job
            // on a separate thread which keeps a copy of the manager shared pointer.
            // This stops that thread so that the transient manager can be destructed.
            newSSLContext->manager->stopJobs();
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "Can not staple OCSP Response. Reason: " << resp.reason());
        }
    }

    if (_listenerOptions.isEgress() && newSSLContext->manager) {
        newSSLContext->egress = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
        Status status = newSSLContext->manager->initSSLContext(
            newSSLContext->egress->native_handle(),
            sslParams,
            SSLManagerInterface::ConnectionDirection::kOutgoing);
        if (!status.isOK()) {
            return status;
        }
        if (newSSLContext->manager->isTransient()) {
            newSSLContext->targetClusterURI =
                newSSLContext->manager->getTargetedClusterConnectionString();
        }
    }
    return newSSLContext;
}

StatusWith<std::shared_ptr<const transport::SSLConnectionContext>>
AsioTransportLayer::createTransientSSLContext(const TransientSSLParams& transientSSLParams) {
    try {
        auto coordinator = SSLManagerCoordinator::get();
        if (!coordinator) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          "SSLManagerCoordinator is not initialized");
        }
        auto manager = coordinator->createTransientSSLManager(transientSSLParams);
        invariant(manager);

        return _createSSLContext(manager, sslMode(), true /* asyncOCSPStaple */);
    } catch (...) {
        LOGV2_DEBUG(9079000,
                    1,
                    "Exception in createTransientSSLContext",
                    "error"_attr = describeActiveException());
        throw;
    }
}

#endif

#ifdef __linux__
BatonHandle AsioTransportLayer::makeBaton(OperationContext* opCtx) const {
    return std::dynamic_pointer_cast<Baton>(std::make_shared<AsioNetworkingBaton>(opCtx));
}
#endif

}  // namespace transport
}  // namespace mongo
