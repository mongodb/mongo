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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/transport_layer_asio.h"

#include <fstream>

#include <asio.hpp>
#include <asio/system_timer.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem/operations.hpp>

#include "mongo/config.h"

#include "mongo/base/system_error.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/db/stats/counters.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/transport/transport_options_gen.h"
#include "mongo/util/hierarchical_acquisition.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"
#include "mongo/util/options_parser/startup_options.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl.hpp"
#endif

// session_asio.h has some header dependencies that require it to be the last header.

#ifdef __linux__
#include "mongo/transport/baton_asio_linux.h"
#endif

#include "mongo/transport/session_asio.h"

namespace mongo {
namespace transport {

namespace {
#ifdef TCP_FASTOPEN
using TCPFastOpen = asio::detail::socket_option::integer<IPPROTO_TCP, TCP_FASTOPEN>;
#endif
/**
 * On systems with TCP_FASTOPEN_CONNECT (linux >= 4.11),
 * we can get TFO "for free" by letting the kernel handle
 * postponing connect() until the first send() call.
 *
 * https://github.com/torvalds/linux/commit/19f6d3f3c8422d65b5e3d2162e30ef07c6e21ea2
 */
#ifdef TCP_FASTOPEN_CONNECT
using TCPFastOpenConnect = asio::detail::socket_option::boolean<IPPROTO_TCP, TCP_FASTOPEN_CONNECT>;
#endif

/**
 * Set to `true` if any of the following set parameters were explicitly configured.
 * - tcpFastOpenServer
 * - tcpFastOpenClient
 * - tcpFastOpenQueueSize
 */
bool tcpFastOpenIsConfigured = false;
boost::optional<Status> maybeTcpFastOpenStatus;
}  // namespace

MONGO_FAIL_POINT_DEFINE(transportLayerASIOasyncConnectTimesOut);

class ASIOReactorTimer final : public ReactorTimer {
public:
    explicit ASIOReactorTimer(asio::io_context& ctx)
        : _timer(std::make_shared<asio::system_timer>(ctx)) {}

    ~ASIOReactorTimer() {
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
        _timer->cancel();
    }


    Future<void> waitUntil(Date_t expiration, const BatonHandle& baton = nullptr) override {
        if (baton && baton->networking()) {
            return _asyncWait([&] { return baton->networking()->waitUntil(*this, expiration); },
                              baton);
        } else {
            return _asyncWait([&] { _timer->expires_at(expiration.toSystemTimePoint()); });
        }
    }

private:
    template <typename ArmTimerCb>
    Future<void> _asyncWait(ArmTimerCb&& armTimer) {
        try {
            cancel();

            armTimer();
            return _timer->async_wait(UseFuture{}).tapError([timer = _timer](const Status& status) {
                if (status != ErrorCodes::CallbackCanceled) {
                    LOGV2_DEBUG(23011, 2, "Timer received error: {status}", "status"_attr = status);
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

    std::shared_ptr<asio::system_timer> _timer;
};

class TransportLayerASIO::ASIOReactor final : public Reactor {
public:
    ASIOReactor() : _ioContext() {}

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
        return std::make_unique<ASIOReactorTimer>(_ioContext);
    }

    Date_t now() override {
        return Date_t(asio::system_timer::clock_type::now());
    }

    void schedule(Task task) override {
        asio::post(_ioContext, [task = std::move(task)] { task(Status::OK()); });
    }

    void dispatch(Task task) override {
        asio::dispatch(_ioContext, [task = std::move(task)] { task(Status::OK()); });
    }

    bool onReactorThread() const override {
        return this == _reactorForThread;
    }

    operator asio::io_context&() {
        return _ioContext;
    }

private:
    class ThreadIdGuard {
    public:
        ThreadIdGuard(TransportLayerASIO::ASIOReactor* reactor) {
            invariant(!_reactorForThread);
            _reactorForThread = reactor;
        }

        ~ThreadIdGuard() {
            invariant(_reactorForThread);
            _reactorForThread = nullptr;
        }
    };

    static thread_local ASIOReactor* _reactorForThread;

    asio::io_context _ioContext;
};

thread_local TransportLayerASIO::ASIOReactor* TransportLayerASIO::ASIOReactor::_reactorForThread =
    nullptr;

TransportLayerASIO::Options::Options(const ServerGlobalParams* params)
    : port(params->port),
      ipList(params->bind_ips),
#ifndef _WIN32
      useUnixSockets(!params->noUnixSocket),
#endif
      enableIPv6(params->enableIPv6),
      maxConns(params->maxConns) {
}

TransportLayerASIO::TransportLayerASIO(const TransportLayerASIO::Options& opts,
                                       ServiceEntryPoint* sep)
    : _ingressReactor(std::make_shared<ASIOReactor>()),
      _egressReactor(std::make_shared<ASIOReactor>()),
      _acceptorReactor(std::make_shared<ASIOReactor>()),
#ifdef MONGO_CONFIG_SSL
      _ingressSSLContext(nullptr),
      _egressSSLContext(nullptr),
#endif
      _sep(sep),
      _listenerOptions(opts) {
}

TransportLayerASIO::~TransportLayerASIO() = default;

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
            return _makeFuture(errorCodeToStatus(ec), peer);
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


StatusWith<SessionHandle> TransportLayerASIO::connect(HostAndPort peer,
                                                      ConnectSSLMode sslMode,
                                                      Milliseconds timeout) {
    std::error_code ec;
    GenericSocket sock(*_egressReactor);
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
    auto sws = _doSyncConnect(endpoints.front(), peer, timeout);
    if (!sws.isOK()) {
        return sws.getStatus();
    }

    auto session = std::move(sws.getValue());
    session->ensureSync();

#ifndef _WIN32
    if (endpoints.front().family() == AF_UNIX) {
        return static_cast<SessionHandle>(std::move(session));
    }
#endif

#ifndef MONGO_CONFIG_SSL
    if (sslMode == kEnableSSL) {
        return {ErrorCodes::InvalidSSLConfiguration, "SSL requested but not supported"};
    }
#else
    auto globalSSLMode = _sslMode();
    if (sslMode == kEnableSSL ||
        (sslMode == kGlobalSSLMode &&
         ((globalSSLMode == SSLParams::SSLMode_preferSSL) ||
          (globalSSLMode == SSLParams::SSLMode_requireSSL)))) {
        Date_t timeBefore = Date_t::now();
        auto sslStatus = session->handshakeSSLForEgress(peer).getNoThrow();
        Date_t timeAfter = Date_t::now();
        if (timeAfter - timeBefore > kSlowOperationThreshold) {
            networkCounter.incrementNumSlowSSLOperations();
        }

        if (!sslStatus.isOK()) {
            return sslStatus;
        }
    }
#endif

    return static_cast<SessionHandle>(std::move(session));
}

template <typename Endpoint>
StatusWith<TransportLayerASIO::ASIOSessionHandle> TransportLayerASIO::_doSyncConnect(
    Endpoint endpoint, const HostAndPort& peer, const Milliseconds& timeout) {
    GenericSocket sock(*_egressReactor);
    std::error_code ec;

    const auto protocol = endpoint->protocol();
    sock.open(protocol);

#ifdef TCP_FASTOPEN_CONNECT
    const auto family = protocol.family();
    if ((family == AF_INET) || (family == AF_INET6)) {
        sock.set_option(TCPFastOpenConnect(gTCPFastOpenClient), ec);
        if (tcpFastOpenIsConfigured) {
            return errorCodeToStatus(ec);
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
            return errorCodeToStatus(ec);
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
        return std::make_shared<ASIOSession>(this, std::move(sock), false, *endpoint);
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

Future<SessionHandle> TransportLayerASIO::asyncConnect(HostAndPort peer,
                                                       ConnectSSLMode sslMode,
                                                       const ReactorHandle& reactor,
                                                       Milliseconds timeout) {

    struct AsyncConnectState {
        AsyncConnectState(HostAndPort peer,
                          asio::io_context& context,
                          Promise<SessionHandle> promise_,
                          const ReactorHandle& reactor)
            : promise(std::move(promise_)),
              socket(context),
              timeoutTimer(context),
              resolver(context),
              peer(std::move(peer)),
              reactor(reactor) {}

        AtomicWord<bool> done{false};
        Promise<SessionHandle> promise;

        Mutex mutex = MONGO_MAKE_LATCH(HierarchicalAcquisitionLevel(0), "AsyncConnectState::mutex");
        GenericSocket socket;
        ASIOReactorTimer timeoutTimer;
        WrappedResolver resolver;
        WrappedEndpoint resolvedEndpoint;
        const HostAndPort peer;
        TransportLayerASIO::ASIOSessionHandle session;
        ReactorHandle reactor;
    };

    auto reactorImpl = checked_cast<ASIOReactor*>(reactor.get());
    auto pf = makePromiseFuture<SessionHandle>();
    auto connector = std::make_shared<AsyncConnectState>(
        std::move(peer), *reactorImpl, std::move(pf.promise), reactor);
    Future<SessionHandle> mergedFuture = std::move(pf.future);

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

    connector->resolver.asyncResolve(connector->peer, _listenerOptions.enableIPv6)
        .then([connector, timeBefore](WrappedResolver::EndpointVector results) {
            try {
                Date_t timeAfter = Date_t::now();
                if (timeAfter - timeBefore > kSlowOperationThreshold) {
                    LOGV2_WARNING(23019,
                                  "DNS resolution while connecting to {connector_peer} took "
                                  "{timeAfter_timeBefore}",
                                  "connector_peer"_attr = connector->peer,
                                  "timeAfter_timeBefore"_attr = timeAfter - timeBefore);
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
            connector->socket.set_option(TCPFastOpenConnect(gTCPFastOpenClient), ec);
            if (tcpFastOpenIsConfigured) {
                return futurize(ec);
            }
#endif
            return connector->socket.async_connect(*connector->resolvedEndpoint, UseFuture{});
        })
        .then([this, connector, sslMode]() -> Future<void> {
            stdx::unique_lock<Latch> lk(connector->mutex);
            connector->session = std::make_shared<ASIOSession>(
                this, std::move(connector->socket), false, *connector->resolvedEndpoint);
            connector->session->ensureAsync();

#ifndef MONGO_CONFIG_SSL
            if (sslMode == kEnableSSL) {
                uasserted(ErrorCodes::InvalidSSLConfiguration, "SSL requested but not supported");
            }
#else
            auto globalSSLMode = _sslMode();
            if (sslMode == kEnableSSL ||
                (sslMode == kGlobalSSLMode &&
                 ((globalSSLMode == SSLParams::SSLMode_preferSSL) ||
                  (globalSSLMode == SSLParams::SSLMode_requireSSL)))) {
                Date_t timeBefore = Date_t::now();
                return connector->session
                    ->handshakeSSLForEgressWithLock(
                        std::move(lk), connector->peer, connector->reactor)
                    .then([connector, timeBefore] {
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
            if (MONGO_unlikely(transportLayerASIOasyncConnectTimesOut.shouldFail())) {
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

Status TransportLayerASIO::setup() {
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
        listenAddrs.emplace_back(makeUnixSockPath(_listenerOptions.port));
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

    // Self-deduplicating list of unique endpoint addresses.
    std::set<WrappedEndpoint> endpoints;
    for (auto& ip : listenAddrs) {
        if (ip.empty()) {
            LOGV2_WARNING(23020, "Skipping empty bind address");
            continue;
        }

        auto swAddrs =
            resolver.resolve(HostAndPort(ip, _listenerPort), _listenerOptions.enableIPv6);
        if (!swAddrs.isOK()) {
            LOGV2_WARNING(23021,
                          "Found no addresses for {swAddrs_getStatus}",
                          "swAddrs_getStatus"_attr = swAddrs.getStatus());
            continue;
        }
        auto& addrs = swAddrs.getValue();
        endpoints.insert(addrs.begin(), addrs.end());
    }

    for (auto& addr : endpoints) {
#ifndef _WIN32
        if (addr.family() == AF_UNIX) {
            if (::unlink(addr.toString().c_str()) == -1 && errno != ENOENT) {
                LOGV2_ERROR(
                    23024,
                    "Failed to unlink socket file {addr_c_str} {errnoWithDescription_errno}",
                    "addr_c_str"_attr = addr.toString().c_str(),
                    "errnoWithDescription_errno"_attr = errnoWithDescription(errno));
                fassertFailedNoTrace(40486);
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
            const auto bindAllIPv6Addr = ":::"_sd + std::to_string(_listenerPort);
            if (errno == EAFNOSUPPORT && _listenerOptions.enableIPv6 && addr.family() == AF_INET6 &&
                addr.toString() == bindAllIPv6Addr) {
                LOGV2_WARNING(4206501,
                              "Failed to bind to {bind_addr} as the platform does not support ipv6",
                              "bind_addr"_attr = addr.toString());
                continue;
            }

            throw;
        }
        acceptor.set_option(GenericAcceptor::reuse_address(true));

        std::error_code ec;
#ifdef TCP_FASTOPEN
        if (gTCPFastOpenServer && ((addr.family() == AF_INET) || (addr.family() == AF_INET6))) {
            acceptor.set_option(TCPFastOpen(gTCPFastOpenQueueSize), ec);
            if (tcpFastOpenIsConfigured) {
                return errorCodeToStatus(ec);
            }
            ec = std::error_code();
        }
#endif
        if (addr.family() == AF_INET6) {
            acceptor.set_option(asio::ip::v6_only(true));
        }

        acceptor.non_blocking(true, ec);
        if (ec) {
            return errorCodeToStatus(ec);
        }

        acceptor.bind(*addr, ec);
        if (ec) {
            return errorCodeToStatus(ec);
        }

#ifndef _WIN32
        if (addr.family() == AF_UNIX) {
            if (::chmod(addr.toString().c_str(), serverGlobalParams.unixSocketPermissions) == -1) {
                LOGV2_ERROR(23026,
                            "Failed to chmod socket file {addr_c_str} {errnoWithDescription_errno}",
                            "addr_c_str"_attr = addr.toString().c_str(),
                            "errnoWithDescription_errno"_attr = errnoWithDescription(errno));
                fassertFailedNoTrace(40487);
            }
        }
#endif
        if (_listenerOptions.port == 0 && (addr.family() == AF_INET || addr.family() == AF_INET6)) {
            if (_listenerPort != _listenerOptions.port) {
                return Status(ErrorCodes::BadValue,
                              "Port 0 (ephemeral port) is not allowed when"
                              " listening on multiple IP interfaces");
            }
            std::error_code ec;
            auto endpoint = acceptor.local_endpoint(ec);
            if (ec) {
                return errorCodeToStatus(ec);
            }
            _listenerPort = endpointToHostAndPort(endpoint).port();
        }

        _acceptors.emplace_back(SockAddr(addr->data(), addr->size()), std::move(acceptor));
    }

    if (_acceptors.empty() && _listenerOptions.isIngress()) {
        return Status(ErrorCodes::SocketException, "No available addresses/ports to bind to");
    }

#ifdef MONGO_CONFIG_SSL
    const auto& sslParams = getSSLGlobalParams();

    if (_sslMode() != SSLParams::SSLMode_disabled && _listenerOptions.isIngress()) {
        _ingressSSLContext = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);

        Status status =
            getSSLManager()->initSSLContext(_ingressSSLContext->native_handle(),
                                            sslParams,
                                            SSLManagerInterface::ConnectionDirection::kIncoming);
        if (!status.isOK()) {
            return status;
        }

        auto resp = getSSLManager()->stapleOCSPResponse(_ingressSSLContext->native_handle());
        if (!resp.isOK()) {
            return Status(ErrorCodes::InvalidSSLConfiguration,
                          str::stream()
                              << "Can not staple OCSP Response. Reason: " << resp.reason());
        }
    }

    if (_listenerOptions.isEgress() && getSSLManager()) {
        _egressSSLContext = std::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
        Status status =
            getSSLManager()->initSSLContext(_egressSSLContext->native_handle(),
                                            sslParams,
                                            SSLManagerInterface::ConnectionDirection::kOutgoing);
        if (!status.isOK()) {
            return status;
        }
    }
#endif

    return Status::OK();
}

void TransportLayerASIO::_runListener() noexcept {
    setThreadName("listener");

    stdx::unique_lock lk(_mutex);
    if (_isShutdown) {
        return;
    }

    for (auto& acceptor : _acceptors) {
        asio::error_code ec;
        acceptor.second.listen(serverGlobalParams.listenBacklog, ec);
        if (ec) {
            LOGV2_FATAL(31339,
                        "Error listening for new connections on {acceptor_first}: {ec_message}",
                        "acceptor_first"_attr = acceptor.first,
                        "ec_message"_attr = ec.message());
        }

        _acceptConnection(acceptor.second);
        LOGV2(23015, "Listening on", "address"_attr = acceptor.first.getAddr());
    }

    const char* ssl = "off";
#ifdef MONGO_CONFIG_SSL
    if (_sslMode() != SSLParams::SSLMode_disabled) {
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

    while (!_isShutdown) {
        lk.unlock();
        _acceptorReactor->run();
        lk.lock();
    }

    // Loop through the acceptors and cancel their calls to async_accept. This will prevent new
    // connections from being opened.
    for (auto& acceptor : _acceptors) {
        acceptor.second.cancel();
        auto& addr = acceptor.first;
        if (addr.getType() == AF_UNIX && !addr.isAnonymousUNIXSocket()) {
            auto path = addr.getAddr();
            LOGV2(23017, "removing socket file: {path}", "path"_attr = path);
            if (::unlink(path.c_str()) != 0) {
                const auto ewd = errnoWithDescription();
                LOGV2_WARNING(23022,
                              "Unable to remove UNIX socket {path}: {ewd}",
                              "path"_attr = path,
                              "ewd"_attr = ewd);
            }
        }
    }
}

Status TransportLayerASIO::start() {
    stdx::unique_lock lk(_mutex);

    // Make sure we haven't shutdown already
    invariant(!_isShutdown);

    if (_listenerOptions.isIngress()) {
        _listener.thread = stdx::thread([this] { _runListener(); });
        _listener.cv.wait(lk, [&] { return _isShutdown || _listener.active; });
        return Status::OK();
    }

    invariant(_acceptors.empty());
    return Status::OK();
}

void TransportLayerASIO::shutdown() {
    stdx::unique_lock lk(_mutex);

    if (std::exchange(_isShutdown, true)) {
        // We were already stopped
        return;
    }

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

ReactorHandle TransportLayerASIO::getReactor(WhichReactor which) {
    switch (which) {
        case TransportLayer::kIngress:
            return _ingressReactor;
        case TransportLayer::kEgress:
            return _egressReactor;
        case TransportLayer::kNewReactor:
            return std::make_shared<ASIOReactor>();
    }

    MONGO_UNREACHABLE;
}

void TransportLayerASIO::_acceptConnection(GenericAcceptor& acceptor) {
    auto acceptCb = [this, &acceptor](const std::error_code& ec, GenericSocket peerSocket) mutable {
        if (auto lk = stdx::lock_guard(_mutex); _isShutdown) {
            return;
        }

        if (ec) {
            LOGV2(23018,
                  "Error accepting new connection on "
                  "{endpointToHostAndPort_acceptor_local_endpoint}: {ec_message}",
                  "endpointToHostAndPort_acceptor_local_endpoint"_attr =
                      endpointToHostAndPort(acceptor.local_endpoint()),
                  "ec_message"_attr = ec.message());
            _acceptConnection(acceptor);
            return;
        }

#ifdef TCPI_OPT_SYN_DATA
        struct tcp_info info;
        socklen_t info_len = sizeof(info);
        if (!getsockopt(peerSocket.native_handle(), IPPROTO_TCP, TCP_INFO, &info, &info_len) &&
            (info.tcpi_options & TCPI_OPT_SYN_DATA)) {
            networkCounter.acceptedTFOIngress();
        }
#endif

        try {
            std::shared_ptr<ASIOSession> session(
                new ASIOSession(this, std::move(peerSocket), true));
            _sep->startSession(std::move(session));
        } catch (const DBException& e) {
            LOGV2_WARNING(23023, "Error accepting new connection {e}", "e"_attr = e);
        }

        _acceptConnection(acceptor);
    };

    acceptor.async_accept(*_ingressReactor, std::move(acceptCb));
}

#ifdef MONGO_CONFIG_SSL
SSLParams::SSLModes TransportLayerASIO::_sslMode() const {
    return static_cast<SSLParams::SSLModes>(getSSLGlobalParams().sslMode.load());
}
#endif

#ifdef __linux__
BatonHandle TransportLayerASIO::makeBaton(OperationContext* opCtx) const {
    invariant(!opCtx->getBaton());

    auto baton = std::make_shared<BatonASIO>(opCtx);
    opCtx->setBaton(baton);

    return baton;
}
#endif

}  // namespace transport
}  // namespace mongo
