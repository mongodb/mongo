/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kNetwork

#include "mongo/platform/basic.h"

#include "mongo/transport/transport_layer_asio.h"

#include <asio.hpp>
#include <asio/system_timer.hpp>
#include <boost/algorithm/string.hpp>

#include "mongo/config.h"

#include "mongo/base/system_error.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/asio_utils.h"
#include "mongo/transport/service_entry_point.h"
#include "mongo/util/log.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/sockaddr.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_options.h"

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

class ASIOReactorTimer final : public ReactorTimer {
public:
    explicit ASIOReactorTimer(asio::io_context& ctx)
        : _timerState(std::make_shared<TimerState>(ctx)) {}

    ~ASIOReactorTimer() {
        // The underlying timer won't get destroyed until the last promise from _asyncWait
        // has been filled, so cancel the timer so call callbacks get run
        cancel();
    }

    void cancel(const BatonHandle& baton = nullptr) override {
        auto promise = [&] {
            stdx::lock_guard<stdx::mutex> lk(_timerState->mutex);
            _timerState->generation++;
            return std::move(_timerState->finalPromise);
        }();

        if (promise) {
            // We're worried that setting the error on the promise without unwinding the stack
            // can lead to a deadlock, so this gets scheduled on the io_context of the timer.
            _timerState->timer.get_io_context().post([promise = promise->share()]() mutable {
                promise.setError({ErrorCodes::CallbackCanceled, "Timer was canceled"});
            });
        }

        if (!(baton && baton->cancelTimer(*this))) {
            _timerState->timer.cancel();
        }
    }

    Future<void> waitFor(Milliseconds timeout, const BatonHandle& baton = nullptr) override {
        if (baton) {
            return _asyncWait([&] { return baton->waitFor(*this, timeout); }, baton);
        } else {
            return _asyncWait(
                [&] { _timerState->timer.expires_after(timeout.toSystemDuration()); });
        }
    }

    Future<void> waitUntil(Date_t expiration, const BatonHandle& baton = nullptr) override {
        if (baton) {
            return _asyncWait([&] { return baton->waitUntil(*this, expiration); }, baton);
        } else {
            return _asyncWait(
                [&] { _timerState->timer.expires_at(expiration.toSystemTimePoint()); });
        }
    }

private:
    std::pair<Future<void>, uint64_t> _getFuture() {
        stdx::lock_guard<stdx::mutex> lk(_timerState->mutex);
        auto id = ++_timerState->generation;
        invariant(!_timerState->finalPromise);
        _timerState->finalPromise = std::make_unique<Promise<void>>();
        auto future = _timerState->finalPromise->getFuture();
        return std::make_pair(std::move(future), id);
    }

    template <typename ArmTimerCb>
    Future<void> _asyncWait(ArmTimerCb&& armTimer) {
        try {
            cancel();

            Future<void> ret;
            uint64_t id;
            std::tie(ret, id) = _getFuture();

            armTimer();
            _timerState->timer.async_wait(
                [ id, state = _timerState ](const std::error_code& ec) mutable {
                    stdx::unique_lock<stdx::mutex> lk(state->mutex);
                    if (id != state->generation) {
                        return;
                    }
                    auto promise = std::move(state->finalPromise);
                    lk.unlock();

                    if (ec) {
                        promise->setError(errorCodeToStatus(ec));
                    } else {
                        promise->emplaceValue();
                    }
                });

            return ret;
        } catch (asio::system_error& ex) {
            return Future<void>::makeReady(errorCodeToStatus(ex.code()));
        }
    }

    template <typename ArmTimerCb>
    Future<void> _asyncWait(ArmTimerCb&& armTimer, const BatonHandle& baton) {
        cancel(baton);

        Future<void> ret;
        uint64_t id;
        std::tie(ret, id) = _getFuture();

        armTimer().getAsync([ id, state = _timerState ](Status status) mutable {
            stdx::unique_lock<stdx::mutex> lk(state->mutex);
            if (id != state->generation) {
                return;
            }
            auto promise = std::move(state->finalPromise);
            lk.unlock();

            if (status.isOK()) {
                promise->emplaceValue();
            } else {
                promise->setError(status);
            }
        });

        return ret;
    }

    // The timer itself and its state are stored in this struct managed by a shared_ptr so we can
    // extend the lifetime of the timer until all callbacks to timer.async_wait have run.
    struct TimerState {
        explicit TimerState(asio::io_context& ctx) : timer(ctx) {}

        asio::system_timer timer;
        stdx::mutex mutex;
        uint64_t generation = 0;
        std::unique_ptr<Promise<void>> finalPromise;
    };

    std::shared_ptr<TimerState> _timerState;
};

class TransportLayerASIO::ASIOReactor final : public Reactor {
public:
    ASIOReactor() : _ioContext() {}

    void run() noexcept override {
        ThreadIdGuard threadIdGuard(this);
        asio::io_context::work work(_ioContext);
        try {
            _ioContext.run();
        } catch (...) {
            severe() << "Uncaught exception in reactor: " << exceptionToStatus();
            fassertFailed(40491);
        }
    }

    void runFor(Milliseconds time) noexcept override {
        ThreadIdGuard threadIdGuard(this);
        asio::io_context::work work(_ioContext);

        try {
            _ioContext.run_for(time.toSystemDuration());
        } catch (...) {
            severe() << "Uncaught exception in reactor: " << exceptionToStatus();
            fassertFailed(50473);
        }
    }

    void stop() override {
        _ioContext.stop();
    }

    std::unique_ptr<ReactorTimer> makeTimer() override {
        return std::make_unique<ASIOReactorTimer>(_ioContext);
    }

    Date_t now() override {
        return Date_t(asio::system_timer::clock_type::now());
    }

    void schedule(ScheduleMode mode, Task task) override {
        if (mode == kDispatch) {
            _ioContext.dispatch(std::move(task));
        } else {
            _ioContext.post(std::move(task));
        }
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
            _reactorForThread = reactor;
        }

        ~ThreadIdGuard() {
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
      ipList(params->bind_ip),
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

using Resolver = asio::ip::tcp::resolver;
class WrappedResolver {
public:
    using Flags = Resolver::flags;
    using Results = Resolver::results_type;

    explicit WrappedResolver(asio::io_context& ioCtx) : _resolver(ioCtx) {}

    Future<Results> resolve(const HostAndPort& peer, Flags flags, bool enableIPv6) {
        Results results;

        std::error_code ec;
        auto port = std::to_string(peer.port());
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

    Future<Results> asyncResolve(const HostAndPort& peer, Flags flags, bool enableIPv6) {
        auto port = std::to_string(peer.port());
        Future<Results> ret;
        if (enableIPv6) {
            ret = _resolver.async_resolve(peer.host(), port, flags, UseFuture{});
        } else {
            ret =
                _resolver.async_resolve(asio::ip::tcp::v4(), peer.host(), port, flags, UseFuture{});
        }

        return std::move(ret)
            .onError([this, peer](Status status) { return _makeFuture(status, peer); })
            .then([this, peer](Results results) { return _makeFuture(results, peer); });
    }

    void cancel() {
        _resolver.cancel();
    }

private:
    Future<Results> _makeFuture(StatusWith<Results> results, const HostAndPort& peer) {
        if (!results.isOK()) {
            return Status{ErrorCodes::HostNotFound,
                          str::stream() << "Could not find address for " << peer << ": "
                                        << results.getStatus()};
        } else if (results.getValue().empty()) {
            return Status{ErrorCodes::HostNotFound,
                          str::stream() << "Could not find address for " << peer};
        } else {
            return std::move(results.getValue());
        }
    }

    Resolver _resolver;
};


StatusWith<SessionHandle> TransportLayerASIO::connect(HostAndPort peer,
                                                      ConnectSSLMode sslMode,
                                                      Milliseconds timeout) {
    std::error_code ec;
    GenericSocket sock(*_egressReactor);
#ifndef _WIN32
    if (mongoutils::str::contains(peer.host(), '/')) {
        invariant(!peer.hasPort());
        auto res =
            _doSyncConnect(asio::local::stream_protocol::endpoint(peer.host()), peer, timeout);
        if (!res.isOK()) {
            return res.getStatus();
        } else {
            return static_cast<SessionHandle>(std::move(res.getValue()));
        }
    }
#endif

    WrappedResolver resolver(*_egressReactor);

    // We always want to resolve the "service" (port number) as a numeric.
    //
    // We intentionally don't set the Resolver::address_configured flag because it might prevent us
    // from connecting to localhost on hosts with only a loopback interface (see SERVER-1579).
    const auto resolverFlags = Resolver::numeric_service;

    // We resolve in two steps, the first step tries to resolve the hostname as an IP address -
    // that way if there's a DNS timeout, we can still connect to IP addresses quickly.
    // (See SERVER-1709)
    //
    // Then, if the numeric (IP address) lookup failed, we fall back to DNS or return the error
    // from the resolver.
    auto swResolverIt =
        resolver.resolve(peer, resolverFlags | Resolver::numeric_host, _listenerOptions.enableIPv6)
            .getNoThrow();
    if (!swResolverIt.isOK()) {
        if (swResolverIt == ErrorCodes::HostNotFound) {
            swResolverIt =
                resolver.resolve(peer, resolverFlags, _listenerOptions.enableIPv6).getNoThrow();
            if (!swResolverIt.isOK()) {
                return swResolverIt.getStatus();
            }
        } else {
            return swResolverIt.getStatus();
        }
    }

    auto& resolverIt = swResolverIt.getValue();
    auto sws = _doSyncConnect(resolverIt->endpoint(), peer, timeout);
    if (!sws.isOK()) {
        return sws.getStatus();
    }

    auto session = std::move(sws.getValue());
    session->ensureSync();

#ifndef MONGO_CONFIG_SSL
    if (sslMode == kEnableSSL) {
        return {ErrorCodes::InvalidSSLConfiguration, "SSL requested but not supported"};
    }
#else
    auto globalSSLMode = _sslMode();
    if (sslMode == kEnableSSL ||
        (sslMode == kGlobalSSLMode && ((globalSSLMode == SSLParams::SSLMode_preferSSL) ||
                                       (globalSSLMode == SSLParams::SSLMode_requireSSL)))) {
        auto sslStatus = session->handshakeSSLForEgress(peer).getNoThrow();
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
    sock.open(endpoint.protocol());
    sock.non_blocking(true);

    auto now = Date_t::now();
    auto expiration = now + timeout;
    do {
        auto curTimeout = expiration - now;
        sock.connect(endpoint, curTimeout.toSystemDuration(), ec);
        if (ec) {
            now = Date_t::now();
        }
        // We loop below if ec == interrupted to deal with EINTR failures, otherwise we handle
        // the error/timeout below.
    } while (ec == asio::error::interrupted && now < expiration);

    if (ec) {
        return errorCodeToStatus(ec);
    } else if (now >= expiration) {
        return {ErrorCodes::NetworkTimeout, str::stream() << "Timed out connecting to " << peer};
    }

    sock.non_blocking(false);
    try {
        return std::make_shared<ASIOSession>(this, std::move(sock), false);
    } catch (const DBException& e) {
        return e.toStatus();
    }
}

Future<SessionHandle> TransportLayerASIO::asyncConnect(HostAndPort peer,
                                                       ConnectSSLMode sslMode,
                                                       const ReactorHandle& reactor) {
    struct AsyncConnectState {
        AsyncConnectState(HostAndPort peer, asio::io_context& context)
            : socket(context), resolver(context), peer(std::move(peer)) {}

        Future<SessionHandle> finish() {
            return SessionHandle(std::move(session));
        }

        GenericSocket socket;
        WrappedResolver resolver;
        const HostAndPort peer;
        TransportLayerASIO::ASIOSessionHandle session;
    };

    auto reactorImpl = checked_cast<ASIOReactor*>(reactor.get());
    auto connector = std::make_shared<AsyncConnectState>(std::move(peer), *reactorImpl);

    if (connector->peer.host().empty()) {
        return Status{ErrorCodes::HostNotFound, "Hostname or IP address to connect to is empty"};
    }

    // We always want to resolve the "service" (port number) as a numeric.
    //
    // We intentionally don't set the Resolver::address_configured flag because it might prevent us
    // from connecting to localhost on hosts with only a loopback interface (see SERVER-1579).
    const auto resolverFlags = Resolver::numeric_service;
    return connector->resolver
        .asyncResolve(
            connector->peer, resolverFlags | Resolver::numeric_host, _listenerOptions.enableIPv6)
        .onError([this, connector, resolverFlags](Status status) {
            return connector->resolver.asyncResolve(
                connector->peer, resolverFlags, _listenerOptions.enableIPv6);
        })
        .then([connector](WrappedResolver::Results results) {
            connector->socket.open(results->endpoint().protocol());
            connector->socket.non_blocking(true);
            return connector->socket.async_connect(results->endpoint(), UseFuture{});
        })
        .then([this, connector, sslMode]() {
            connector->session =
                std::make_shared<ASIOSession>(this, std::move(connector->socket), false);
            connector->session->ensureAsync();
#ifndef MONGO_CONFIG_SSL
            if (sslMode == kEnableSSL) {
                uasserted(ErrorCodes::InvalidSSLConfiguration, "SSL requested but not supported");
            }
#else
            auto globalSSLMode = _sslMode();
            if (sslMode == kEnableSSL ||
                (sslMode == kGlobalSSLMode && ((globalSSLMode == SSLParams::SSLMode_preferSSL) ||
                                               (globalSSLMode == SSLParams::SSLMode_requireSSL)))) {
                return connector->session->handshakeSSLForEgress(connector->peer).then([connector] {
                    return connector->finish();
                });
            }
#endif
            return connector->finish();
        })
        .onError([connector](Status status) -> Future<SessionHandle> {
            return status.withContext(str::stream() << "Error connecting to " << connector->peer);
        });
}

Status TransportLayerASIO::setup() {
    std::vector<std::string> listenAddrs;
    if (_listenerOptions.ipList.empty() && _listenerOptions.isIngress()) {
        listenAddrs = {"127.0.0.1"};
        if (_listenerOptions.enableIPv6) {
            listenAddrs.emplace_back("::1");
        }
    } else if (!_listenerOptions.ipList.empty()) {
        boost::split(
            listenAddrs, _listenerOptions.ipList, boost::is_any_of(","), boost::token_compress_on);
    }

#ifndef _WIN32
    if (_listenerOptions.useUnixSockets && _listenerOptions.isIngress()) {
        listenAddrs.emplace_back(makeUnixSockPath(_listenerOptions.port));
    }
#endif

    if (!(_listenerOptions.isIngress()) && !listenAddrs.empty()) {
        return {ErrorCodes::BadValue,
                "Cannot bind to listening sockets with ingress networking is disabled"};
    }

    _listenerPort = _listenerOptions.port;

    for (auto& ip : listenAddrs) {
        std::error_code ec;
        if (ip.empty()) {
            warning() << "Skipping empty bind address";
            continue;
        }

        const auto addrs = SockAddr::createAll(
            ip, _listenerOptions.port, _listenerOptions.enableIPv6 ? AF_UNSPEC : AF_INET);
        if (addrs.empty()) {
            warning() << "Found no addresses for " << ip;
            continue;
        }

        for (const auto& addr : addrs) {
            asio::generic::stream_protocol::endpoint endpoint(addr.raw(), addr.addressSize);

#ifndef _WIN32
            if (addr.getType() == AF_UNIX) {
                if (::unlink(ip.c_str()) == -1 && errno != ENOENT) {
                    error() << "Failed to unlink socket file " << ip << " "
                            << errnoWithDescription(errno);
                    fassertFailedNoTrace(40486);
                }
            }
#endif
            if (addr.getType() == AF_INET6 && !_listenerOptions.enableIPv6) {
                error() << "Specified ipv6 bind address, but ipv6 is disabled";
                fassertFailedNoTrace(40488);
            }

            GenericAcceptor acceptor(*_acceptorReactor);
            acceptor.open(endpoint.protocol());
            acceptor.set_option(GenericAcceptor::reuse_address(true));
            if (addr.getType() == AF_INET6) {
                acceptor.set_option(asio::ip::v6_only(true));
            }

            acceptor.non_blocking(true, ec);
            if (ec) {
                return errorCodeToStatus(ec);
            }

            acceptor.bind(endpoint, ec);
            if (ec) {
                return errorCodeToStatus(ec);
            }

#ifndef _WIN32
            if (addr.getType() == AF_UNIX) {
                if (::chmod(ip.c_str(), serverGlobalParams.unixSocketPermissions) == -1) {
                    error() << "Failed to chmod socket file " << ip << " "
                            << errnoWithDescription(errno);
                    fassertFailedNoTrace(40487);
                }
            }
#endif
            if (_listenerOptions.port == 0 &&
                (addr.getType() == AF_INET || addr.getType() == AF_INET6)) {
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
            _acceptors.emplace_back(std::move(addr), std::move(acceptor));
        }
    }

    if (_acceptors.empty() && _listenerOptions.isIngress()) {
        return Status(ErrorCodes::SocketException, "No available addresses/ports to bind to");
    }

#ifdef MONGO_CONFIG_SSL
    const auto& sslParams = getSSLGlobalParams();
    auto sslManager = getSSLManager();

    if (_sslMode() != SSLParams::SSLMode_disabled && _listenerOptions.isIngress()) {
        _ingressSSLContext = stdx::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);

        Status status =
            sslManager->initSSLContext(_ingressSSLContext->native_handle(),
                                       sslParams,
                                       SSLManagerInterface::ConnectionDirection::kIncoming);
        if (!status.isOK()) {
            return status;
        }
    }

    if (_listenerOptions.isEgress() && sslManager) {
        _egressSSLContext = stdx::make_unique<asio::ssl::context>(asio::ssl::context::sslv23);
        Status status =
            sslManager->initSSLContext(_egressSSLContext->native_handle(),
                                       sslParams,
                                       SSLManagerInterface::ConnectionDirection::kOutgoing);
        if (!status.isOK()) {
            return status;
        }
    }
#endif

    return Status::OK();
}

Status TransportLayerASIO::start() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _running.store(true);

    if (_listenerOptions.isIngress()) {
        for (auto& acceptor : _acceptors) {
            acceptor.second.listen(serverGlobalParams.listenBacklog);
            _acceptConnection(acceptor.second);
        }

        _listenerThread = stdx::thread([this] {
            setThreadName("listener");
            while (_running.load()) {
                _acceptorReactor->run();
            }
        });

        const char* ssl = "";
#ifdef MONGO_CONFIG_SSL
        if (_sslMode() != SSLParams::SSLMode_disabled) {
            ssl = " ssl";
        }
#endif
        log() << "waiting for connections on port " << _listenerPort << ssl;
    } else {
        invariant(_acceptors.empty());
    }

    return Status::OK();
}

void TransportLayerASIO::shutdown() {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _running.store(false);

    // Loop through the acceptors and cancel their calls to async_accept. This will prevent new
    // connections from being opened.
    for (auto& acceptor : _acceptors) {
        acceptor.second.cancel();
        auto& addr = acceptor.first;
        if (addr.getType() == AF_UNIX && !addr.isAnonymousUNIXSocket()) {
            auto path = addr.getAddr();
            log() << "removing socket file: " << path;
            if (::unlink(path.c_str()) != 0) {
                const auto ewd = errnoWithDescription();
                warning() << "Unable to remove UNIX socket " << path << ": " << ewd;
            }
        }
    }

    // If the listener thread is joinable (that is, we created/started a listener thread), then
    // the io_context is owned exclusively by the TransportLayer and we should stop it and join
    // the listener thread.
    //
    // Otherwise the ServiceExecutor may need to continue running the io_context to drain running
    // connections, so we just cancel the acceptors and return.
    if (_listenerThread.joinable()) {
        _acceptorReactor->stop();
        _listenerThread.join();
    }
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
        if (!_running.load())
            return;

        if (ec) {
            log() << "Error accepting new connection on "
                  << endpointToHostAndPort(acceptor.local_endpoint()) << ": " << ec.message();
            _acceptConnection(acceptor);
            return;
        }

        try {
            std::shared_ptr<ASIOSession> session(
                new ASIOSession(this, std::move(peerSocket), true));
            _sep->startSession(std::move(session));
        } catch (const DBException& e) {
            warning() << "Error accepting new connection " << e;
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

BatonHandle TransportLayerASIO::makeBaton(OperationContext* opCtx) {
#ifdef __linux__
    auto baton = std::make_shared<BatonASIO>(opCtx);

    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        invariant(!opCtx->getBaton());
        opCtx->setBaton(baton);
    }

    return std::move(baton);
#else
    return nullptr;
#endif
}

}  // namespace transport
}  // namespace mongo
