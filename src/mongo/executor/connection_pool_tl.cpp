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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include "mongo/executor/connection_pool_tl.h"

#include "mongo/client/authenticate.h"
#include "mongo/db/auth/authorization_manager.h"
#include "mongo/util/log.h"

namespace mongo {
namespace executor {
namespace connection_pool_tl {
namespace {
const auto kMaxTimerDuration = Milliseconds::max();

struct TimeoutHandler {
    AtomicWord<bool> done;
    Promise<void> promise;

    explicit TimeoutHandler(Promise<void> p) : promise(std::move(p)) {}
};

}  // namespace

void TLTypeFactory::shutdown() {
    // Stop any attempt to schedule timers in the future
    _inShutdown.store(true);

    stdx::lock_guard<Latch> lk(_mutex);

    log() << "Killing all outstanding egress activity.";
    for (auto collar : _collars) {
        collar->kill();
    }
}

void TLTypeFactory::fasten(Type* type) {
    stdx::lock_guard<Latch> lk(_mutex);
    _collars.insert(type);
}

void TLTypeFactory::release(Type* type) {
    stdx::lock_guard<Latch> lk(_mutex);
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
        LOG(2) << "Skipping timeout due to impending shutdown.";
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

class TLConnectionSetupHook : public executor::NetworkConnectionHook {
public:
    explicit TLConnectionSetupHook(executor::NetworkConnectionHook* hookToWrap)
        : _wrappedHook(hookToWrap) {}

    BSONObj augmentIsMasterRequest(BSONObj cmdObj) override {
        BSONObjBuilder bob(std::move(cmdObj));
        bob.append("hangUpOnStepDown", false);
        if (internalSecurity.user) {
            bob.append("saslSupportedMechs", internalSecurity.user->getName().getUnambiguousName());
        }

        return bob.obj();
    }

    Status validateHost(const HostAndPort& remoteHost,
                        const BSONObj& isMasterRequest,
                        const RemoteCommandResponse& isMasterReply) override try {
        const auto saslMechsElem = isMasterReply.data.getField("saslSupportedMechs");
        if (saslMechsElem.type() == Array) {
            auto array = saslMechsElem.Array();
            for (const auto& elem : array) {
                _saslMechsForInternalAuth.push_back(elem.checkAndGetStringData().toString());
            }
        }

        if (!_wrappedHook) {
            return Status::OK();
        } else {
            return _wrappedHook->validateHost(remoteHost, isMasterRequest, isMasterReply);
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

private:
    std::vector<std::string> _saslMechsForInternalAuth;
    executor::NetworkConnectionHook* const _wrappedHook = nullptr;
};

void TLConnection::setup(Milliseconds timeout, SetupCallback cb) {
    auto anchor = shared_from_this();

    auto pf = makePromiseFuture<void>();
    auto handler = std::make_shared<TimeoutHandler>(std::move(pf.promise));
    std::move(pf.future).thenRunOn(_reactor).getAsync(
        [this, cb = std::move(cb), anchor](Status status) { cb(this, std::move(status)); });

    setTimeout(timeout, [this, handler, timeout] {
        if (handler->done.swap(true)) {
            return;
        }
        std::string reason = str::stream()
            << "Timed out connecting to " << _peer << " after " << timeout;
        handler->promise.setError(
            Status(ErrorCodes::NetworkInterfaceExceededTimeLimit, std::move(reason)));

        if (_client) {
            _client->cancel();
        }
    });

    auto isMasterHook = std::make_shared<TLConnectionSetupHook>(_onConnectHook);

    AsyncDBClient::connect(_peer, _sslMode, _serviceContext, _reactor, timeout)
        .thenRunOn(_reactor)
        .onError([](StatusWith<AsyncDBClient::Handle> swc) -> StatusWith<AsyncDBClient::Handle> {
            return Status(ErrorCodes::HostUnreachable, swc.getStatus().reason());
        })
        .then([this, isMasterHook](AsyncDBClient::Handle client) {
            _client = std::move(client);
            return _client->initWireVersion("NetworkInterfaceTL", isMasterHook.get());
        })
        .then([this, isMasterHook] {
            if (_skipAuth) {
                return Future<void>::makeReady();
            }

            boost::optional<std::string> mechanism;
            if (!isMasterHook->saslMechsForInternalAuth().empty())
                mechanism = isMasterHook->saslMechsForInternalAuth().front();
            return _client->authenticateInternal(std::move(mechanism));
        })
        .then([this] {
            if (!_onConnectHook) {
                return Future<void>::makeReady();
            }
            auto connectHookRequest = uassertStatusOK(_onConnectHook->makeRequest(_peer));
            if (!connectHookRequest) {
                return Future<void>::makeReady();
            }
            return _client->runCommandRequest(*connectHookRequest)
                .then([this](RemoteCommandResponse response) {
                    return _onConnectHook->handleReply(_peer, std::move(response));
                });
        })
        .getAsync([this, handler, anchor](Status status) {
            if (handler->done.swap(true)) {
                return;
            }

            cancelTimeout();

            if (status.isOK()) {
                handler->promise.emplaceValue();
            } else {
                LOG(2) << "Failed to connect to " << _peer << " - " << redact(status);
                handler->promise.setError(status);
            }
        });
    LOG(2) << "Finished connection setup.";
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
        ->runCommandRequest(
            {_peer, std::string("admin"), BSON("isMaster" << 1), BSONObj(), nullptr})
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

void TLConnection::cancelAsync() {
    if (_client)
        _client->cancel();
}

auto TLTypeFactory::reactor() {
    return checked_pointer_cast<transport::Reactor>(_executor);
}

std::shared_ptr<ConnectionPool::ConnectionInterface> TLTypeFactory::makeConnection(
    const HostAndPort& hostAndPort, transport::ConnectSSLMode sslMode, size_t generation) {
    auto conn = std::make_shared<TLConnection>(shared_from_this(),
                                               reactor(),
                                               getGlobalServiceContext(),
                                               hostAndPort,
                                               sslMode,
                                               generation,
                                               _onConnectHook.get(),
                                               _connPoolOptions.skipAuthentication);
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
