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


#include "mongo/executor/connection_pool_tl.h"

#include "mongo/base/checked_cast.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonelement.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/client/authenticate.h"
#include "mongo/client/internal_auth.h"
#include "mongo/client/sasl_client_session.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/db/auth/user.h"
#include "mongo/db/auth/user_name.h"
#include "mongo/db/commands/server_status/server_status_metric.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/executor/egress_networking_parameters_gen.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/stdx/unordered_map.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/functional.h"
#include "mongo/util/future.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/net/hostandport.h"
#include "mongo/util/net/ssl_manager.h"
#include "mongo/util/net/ssl_types.h"
#include "mongo/util/read_through_cache.h"
#include "mongo/util/str.h"

#include <functional>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <vector>

#include <absl/container/node_hash_set.h>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/tuple/tuple.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kConnectionPool


namespace mongo {
namespace executor {
namespace connection_pool_tl {
namespace {
MONGO_FAIL_POINT_DEFINE(triggerConnectionSetupHandshakeTimeout);

const auto kMaxTimerDuration = Milliseconds::max();
struct TimeoutHandler {
    AtomicWord<bool> done;
    Promise<void> promise;

    explicit TimeoutHandler(Promise<void> p) : promise(std::move(p)) {}
};

auto makeSeveritySuppressor() {
    return std::make_unique<logv2::KeyedSeveritySuppressor<HostAndPort>>(
        Seconds{1}, logv2::LogSeverity::Log(), logv2::LogSeverity::Debug(2));
}

bool connHealthMetricsLoggingEnabled() {
    return gEnableDetailedConnectionHealthMetricLogLines.load();
}

void logSlowConnection(const HostAndPort& peer,
                       const ConnectionMetrics& connMetrics,
                       ConnectionPool::ConnectionInterface::PoolConnectionId connectionId) {
    static auto& severitySuppressor = *makeSeveritySuppressor().release();
    LOGV2_DEBUG(6496400,
                severitySuppressor(peer).toInt(),
                "Slow connection establishment",
                "hostAndPort"_attr = peer,
                "dnsResolutionTime"_attr = connMetrics.dnsResolution(),
                "tcpConnectionTime"_attr = connMetrics.tcpConnection(),
                "tlsHandshakeTime"_attr = connMetrics.tlsHandshake(),
                "authTime"_attr = connMetrics.auth(),
                "hookTime"_attr = connMetrics.connectionHook(),
                "totalTime"_attr = connMetrics.total(),
                "poolConnId"_attr = connectionId);
}

auto& totalConnectionEstablishmentTime =
    *MetricBuilder<Counter64>("network.totalEgressConnectionEstablishmentTimeMillis");

}  // namespace

void TLTypeFactory::shutdown() {
    // Stop any attempt to schedule timers in the future
    _inShutdown.store(true);

    stdx::lock_guard<stdx::mutex> lk(_mutex);

    stdx::unordered_map<std::string, int> counts;
    for (const Type* collar : _collars) {
        ++counts[demangleName(typeid(collar))];
    }

    LOGV2(22582,
          "Killing all outstanding egress activity",
          "instanceName"_attr = _instanceName,
          "numKilledByType"_attr = counts);
    for (Type* collar : _collars) {
        collar->kill();
    }
}

void TLTypeFactory::fasten(Type* type) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _collars.insert(type);
}

void TLTypeFactory::release(Type* type) {
    stdx::lock_guard<stdx::mutex> lk(_mutex);
    _collars.erase(type);

    type->_wasReleased = true;
}

TLTypeFactory::Type::Type(const std::shared_ptr<TLTypeFactory>& factory) : _factory{factory} {}

TLTypeFactory::Type::~Type() {
    invariant(_wasReleased);
}

void TLTypeFactory::Type::release() {
    _factory->release(this);
}

bool TLTypeFactory::inShutdown() const {
    return _inShutdown.load();
}

void TLTimer::setTimeout(Milliseconds timeoutVal, TimeoutCallback cb) {
    // We will not wait on a timeout if we are in shutdown.
    // The clients will be canceled as an inevitable consequence of pools shutting down.
    if (inShutdown()) {
        LOGV2_DEBUG(22583,
                    2,
                    "Skipping timeout due to impending shutdown.",
                    "instanceName"_attr = instanceName());
        return;
    }

    // Wait until our timeoutVal then run on the reactor
    _timer->waitUntil(_reactor->now() + timeoutVal)
        .thenRunOn(_reactor)
        .getAsync([cb = std::move(cb)](Status status) {
            // If we get canceled, then we don't worry about the timeout anymore
            if (status == ErrorCodes::CallbackCanceled) {
                return;
            }

            fassert(50475, status);

            cb();
        });
}

void TLTimer::cancelTimeout() {
    _timer->cancel();
}

Date_t TLTimer::now() {
    return _reactor->now();
}

const HostAndPort& TLConnection::getHostAndPort() const {
    return _peer;
}

transport::ConnectSSLMode TLConnection::getSslMode() const {
    return _sslMode;
}

bool TLConnection::isHealthy() {
    return _client->isStillConnected();
}

bool TLConnection::maybeHealthy() {
    // The connection has been successfully used after the last time we checked for its health, so
    // we may assume it's still healthy.
    if (auto lastUsedWithTimeout = getLastUsed() + kIsHealthyCacheTimeout;
        lastUsedWithTimeout > _isHealthyExpiresAt) {
        _isHealthyExpiresAt = lastUsedWithTimeout;
        // We may reset `_isHealthyCache` below if `now()` has already passed `_isHealthyExpiresAt`.
        _isHealthyCache = true;
    }

    if (auto currentTime = now(); !_isHealthyCache || currentTime >= _isHealthyExpiresAt) {
        _isHealthyCache = isHealthy();
        _isHealthyExpiresAt = currentTime + kIsHealthyCacheTimeout;
    }
    return _isHealthyCache;
}

AsyncDBClient* TLConnection::client() {
    return _client.get();
}

void TLConnection::setTimeout(Milliseconds timeout, TimeoutCallback cb) {
    auto anchor = shared_from_this();
    _timer->setTimeout(timeout, [cb = std::move(cb), anchor = std::move(anchor)] { cb(); });
}

void TLConnection::cancelTimeout() {
    _timer->cancelTimeout();
}

namespace {

class TLConnectionSetupHook : public executor::NetworkConnectionHook {
public:
    explicit TLConnectionSetupHook(executor::NetworkConnectionHook* hookToWrap, bool x509AuthOnly)
        : _wrappedHook(hookToWrap), _x509AuthOnly(x509AuthOnly) {}

    BSONObj augmentHelloRequest(const HostAndPort& remoteHost, BSONObj cmdObj) override {
        BSONObjBuilder bob(std::move(cmdObj));
        bob.append("hangUpOnStepDown", false);
        auto systemUser = internalSecurity.getUser();
        if (systemUser && *systemUser) {
            bob.append("saslSupportedMechs", (*systemUser)->getName().getUnambiguousName());
        }

        if (_x509AuthOnly) {
            _speculativeAuthType = auth::SpeculativeAuthType::kAuthenticate;
        } else {
            _speculativeAuthType = auth::speculateInternalAuth(remoteHost, &bob, &_session);
        }

        return bob.obj();
    }

    Status validateHost(const HostAndPort& remoteHost,
                        const BSONObj& helloRequest,
                        const RemoteCommandResponse& helloReply) override try {
        const auto& reply = helloReply.data;

        // X.509 auth only means we only want to use a single mechanism regards of what hello says
        if (_x509AuthOnly) {
            _saslMechsForInternalAuth.clear();
            _saslMechsForInternalAuth.push_back("MONGODB-X509");
        } else {
            const auto saslMechsElem = reply.getField("saslSupportedMechs");
            if (saslMechsElem.type() == BSONType::array) {
                auto array = saslMechsElem.Array();
                for (const auto& elem : array) {
                    _saslMechsForInternalAuth.push_back(std::string{elem.checkAndGetStringData()});
                }
            }
        }

        const auto specAuth = reply.getField(auth::kSpeculativeAuthenticate);
        if (specAuth.type() == BSONType::object) {
            _speculativeAuthenticate = specAuth.Obj().getOwned();
        }

        if (!_wrappedHook) {
            return Status::OK();
        } else {
            return _wrappedHook->validateHost(remoteHost, helloRequest, helloReply);
        }
    } catch (const DBException& e) {
        return e.toStatus();
    }

    StatusWith<boost::optional<RemoteCommandRequest>> makeRequest(
        const HostAndPort& remoteHost) final {
        if (_wrappedHook) {
            return _wrappedHook->makeRequest(remoteHost);
        } else {
            return boost::none;
        }
    }

    Status handleReply(const HostAndPort& remoteHost, RemoteCommandResponse&& response) final {
        if (_wrappedHook) {
            return _wrappedHook->handleReply(remoteHost, std::move(response));
        } else {
            return Status::OK();
        }
    }

    const std::vector<std::string>& saslMechsForInternalAuth() const {
        return _saslMechsForInternalAuth;
    }

    std::shared_ptr<SaslClientSession> getSession() {
        return _session;
    }

    auth::SpeculativeAuthType getSpeculativeAuthType() const {
        return _speculativeAuthType;
    }

    BSONObj getSpeculativeAuthenticateReply() {
        return _speculativeAuthenticate;
    }

private:
    std::vector<std::string> _saslMechsForInternalAuth;
    std::shared_ptr<SaslClientSession> _session;
    auth::SpeculativeAuthType _speculativeAuthType;
    BSONObj _speculativeAuthenticate;
    executor::NetworkConnectionHook* const _wrappedHook = nullptr;
    bool _x509AuthOnly;
};

#ifdef MONGO_CONFIG_SSL
class TransientInternalAuthParametersProvider : public auth::InternalAuthParametersProvider {
public:
    TransientInternalAuthParametersProvider(
        const std::shared_ptr<const transport::SSLConnectionContext> transientSSLContext)
        : _transientSSLContext(transientSSLContext) {}

    ~TransientInternalAuthParametersProvider() override = default;

    BSONObj get(size_t index, StringData mechanism) final {
        if (_transientSSLContext) {
            if (index == 0) {
                return auth::createInternalX509AuthDocument(
                    boost::optional<StringData>{_transientSSLContext->manager->getSSLConfiguration()
                                                    .clientSubjectName.toString()});
            } else {
                return BSONObj();
            }
        }

        return auth::getInternalAuthParams(index, mechanism);
    }

private:
    const std::shared_ptr<const transport::SSLConnectionContext> _transientSSLContext;
};
#endif

}  // namespace

void TLConnection::setup(Milliseconds timeout, SetupCallback cb, std::string instanceName) {
    auto anchor = shared_from_this();

    // Create a shared_ptr to _connMetrics that shares the ownership information with the
    // anchor. We want to keep this TLConnection instance alive as long as the shared_ptr
    // to _connMetrics is in use.
    std::shared_ptr<ConnectionMetrics> connMetricsAnchor{anchor, &_connMetrics};

    auto pf = makePromiseFuture<void>();
    auto handler = std::make_shared<TimeoutHandler>(std::move(pf.promise));
    std::move(pf.future).thenRunOn(_reactor).getAsync(
        [this, cb = std::move(cb), anchor](Status status) { cb(this, std::move(status)); });

    if (MONGO_unlikely(triggerConnectionSetupHandshakeTimeout.shouldFail())) {
        triggerConnectionSetupHandshakeTimeout.executeIf(
            [&](const BSONObj& data) { timeout = Milliseconds(0); },
            [&](const BSONObj& data) {
                std::string nameToTimeout = data["instance"].String();
                return instanceName.substr(0, nameToTimeout.size()) == nameToTimeout;
            });
    }

    setTimeout(timeout, [this, handler, timeout] {
        if (handler->done.swap(true)) {
            return;
        }
        std::string reason = str::stream()
            << "Timed out connecting to " << _peer << " after " << timeout;
        handler->promise.setError(Status(ErrorCodes::HostUnreachable, std::move(reason)));

        cancel();
    });

#ifdef MONGO_CONFIG_SSL
    bool x509AuthOnly =
        _transientSSLContext.get() && _transientSSLContext->targetClusterURI.has_value();
    auto authParametersProvider =
        std::make_shared<TransientInternalAuthParametersProvider>(_transientSSLContext);
#else
    bool x509AuthOnly = false;
    auto authParametersProvider = auth::createDefaultInternalAuthProvider();
#endif

    // For transient connections, only use X.509 auth.
    auto helloHook = std::make_shared<TLConnectionSetupHook>(_onConnectHook, x509AuthOnly);

    AsyncDBClient::connect(_peer,
                           _sslMode,
                           _serviceContext,
                           _tl,
                           _reactor,
                           timeout,
                           connMetricsAnchor,
                           _transientSSLContext)
        .thenRunOn(_reactor)
        .onError([](StatusWith<std::shared_ptr<AsyncDBClient>> swc)
                     -> StatusWith<std::shared_ptr<AsyncDBClient>> {
            if (const Status& status = swc.getStatus();
                status.code() == ErrorCodes::ConnectionError) {
                return status;
            } else {
                return Status(ErrorCodes::HostUnreachable, status.reason());
            }
        })
        .then([this, helloHook, instanceName = std::move(instanceName)](
                  std::shared_ptr<AsyncDBClient> client) {
            {
                stdx::lock_guard lk(_clientMutex);
                _client = std::move(client);
            }
            return _client->initWireVersion(instanceName, helloHook.get());
        })
        .then([this, helloHook]() -> Future<bool> {
            if (_skipAuth) {
                return false;
            }

            return _client->completeSpeculativeAuth(helloHook->getSession(),
                                                    auth::getInternalAuthDB(),
                                                    helloHook->getSpeculativeAuthenticateReply(),
                                                    helloHook->getSpeculativeAuthType());
        })
        .then([this, helloHook, authParametersProvider](bool authenticatedDuringConnect) {
            if (_skipAuth || authenticatedDuringConnect) {
                return Future<void>::makeReady();
            }

            boost::optional<std::string> mechanism;
            if (!helloHook->saslMechsForInternalAuth().empty())
                mechanism = helloHook->saslMechsForInternalAuth().front();
            return _client->authenticateInternal(std::move(mechanism), authParametersProvider);
        })
        .then([this] {
            _connMetrics.onAuthFinished();
            if (!_onConnectHook) {
                return Future<void>::makeReady();
            }
            auto connectHookRequest = uassertStatusOK(_onConnectHook->makeRequest(_peer));
            if (!connectHookRequest) {
                return Future<void>::makeReady();
            }
            return _client->runCommandRequest(*connectHookRequest)
                .then([this](RemoteCommandResponse response) {
                    auto status = _onConnectHook->handleReply(_peer, std::move(response));
                    _connMetrics.onConnectionHookFinished();
                    return status;
                });
        })
        .getAsync([this, handler, anchor](Status status) {
            if (handler->done.swap(true)) {
                return;
            }

            cancelTimeout();

            if (status.isOK()) {
                totalConnectionEstablishmentTime.increment(_connMetrics.total().count());
                if (connHealthMetricsLoggingEnabled() &&
                    _connMetrics.total() >= Milliseconds(gSlowConnectionThresholdMillis.load())) {
                    logSlowConnection(_peer, _connMetrics, _id);
                }
                handler->promise.emplaceValue();
            } else {
                if (ErrorCodes::isNetworkTimeoutError(status) &&
                    connHealthMetricsLoggingEnabled()) {
                    logSlowConnection(_peer, _connMetrics, _id);
                }
                LOGV2_DEBUG(22584,
                            2,
                            "Failed to connect",
                            "hostAndPort"_attr = _peer,
                            "poolConnId"_attr = _id,
                            "error"_attr = redact(status));
                handler->promise.setError(status);
            }
        });
    LOGV2_DEBUG(
        22585, 2, "Finished connection setup", "hostAndPort"_attr = _peer, "poolConnId"_attr = _id);
}

void TLConnection::refresh(Milliseconds timeout, RefreshCallback cb) {
    auto anchor = shared_from_this();

    auto pf = makePromiseFuture<void>();
    auto handler = std::make_shared<TimeoutHandler>(std::move(pf.promise));
    std::move(pf.future).thenRunOn(_reactor).getAsync(
        [this, cb = std::move(cb), anchor](Status status) { cb(this, status); });

    setTimeout(timeout, [this, handler] {
        if (handler->done.swap(true)) {
            return;
        }

        indicateFailure({ErrorCodes::HostUnreachable, "Timed out refreshing host"});
        _client->cancel();

        handler->promise.setError(getStatus());
    });

    _client
        ->runCommandRequest({_peer, DatabaseName::kAdmin, BSON("hello" << 1), BSONObj(), nullptr})
        .then([](executor::RemoteCommandResponse response) {
            return Future<void>::makeReady(response.status);
        })
        .getAsync([this, handler, anchor](Status status) {
            if (handler->done.swap(true)) {
                return;
            }

            cancelTimeout();

            if (status.isOK()) {
                indicateSuccess();
                handler->promise.emplaceValue();
            } else {
                indicateFailure(status);
                handler->promise.setError(status);
            }
        });
}

Date_t TLConnection::now() {
    return _reactor->now();
}

void TLConnection::cancel(bool endClient) {
    auto client = [&] {
        stdx::lock_guard lk(_clientMutex);
        return _client;
    }();

    if (!client) {
        return;
    }

    if (endClient) {
        // This prevents any future operations from running, but in-progress ones may continue until
        // we cancel.
        _reactor->schedule([client](Status s) {
            if (!s.isOK()) {
                return;
            }
            client->end();
        });
    }
    client->cancel();
}

void TLConnection::startConnAcquiredTimer() {
    _connMetrics.startConnAcquiredTimer();
}

std::shared_ptr<Timer> TLConnection::getConnAcquiredTimer() {
    auto anchor = shared_from_this();
    return std::shared_ptr<Timer>{anchor, _connMetrics.getConnAcquiredTimer()};
}

auto TLTypeFactory::reactor() {
    return checked_pointer_cast<transport::Reactor>(_executor);
}

std::shared_ptr<ConnectionPool::ConnectionInterface> TLTypeFactory::makeConnection(
    const HostAndPort& hostAndPort,
    transport::ConnectSSLMode sslMode,
    PoolConnectionId id,
    size_t generation) {
    auto conn = std::make_shared<TLConnection>(shared_from_this(),
                                               reactor(),
                                               getGlobalServiceContext(),
                                               hostAndPort,
                                               sslMode,
                                               id,
                                               generation,
                                               _onConnectHook.get(),
                                               _connPoolOptions.skipAuthentication,
                                               _transientSSLContext);
    fasten(conn.get());
    return conn;
}

std::shared_ptr<ConnectionPool::TimerInterface> TLTypeFactory::makeTimer() {
    auto timer = std::make_shared<TLTimer>(shared_from_this(), reactor());
    fasten(timer.get());
    return timer;
}

Date_t TLTypeFactory::now() {
    return checked_cast<transport::Reactor*>(_executor.get())->now();
}

}  // namespace connection_pool_tl
}  // namespace executor
}  // namespace mongo
