/**
 *    Copyright (C) 2018 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kASIO

#include "mongo/platform/basic.h"

#include "mongo/executor/connection_pool_tl.h"

#include "mongo/db/auth/internal_user_auth.h"
#include "mongo/util/log.h"

namespace mongo {
namespace executor {
namespace connection_pool_tl {
namespace {
const auto kMaxTimerDuration = Milliseconds::max();

struct TimeoutHandler {
    AtomicBool done;
    Promise<void> promise;
};

}  // namespace

void TLTimer::setTimeout(Milliseconds timeoutVal, TimeoutCallback cb) {
    _timer->waitFor(timeoutVal).getAsync([cb = std::move(cb)](Status status) {
        // TODO: verify why we still get broken promises when expliciting call stop and shutting
        // down NITL's quickly.
        if (status == ErrorCodes::CallbackCanceled || status == ErrorCodes::BrokenPromise) {
            return;
        }

        fassert(50475, status);

        cb();
    });
}

void TLTimer::cancelTimeout() {
    _timer->cancel();
}

void TLConnection::indicateSuccess() {
    _status = Status::OK();
}

void TLConnection::indicateFailure(Status status) {
    _status = std::move(status);
}

const HostAndPort& TLConnection::getHostAndPort() const {
    return _peer;
}

bool TLConnection::isHealthy() {
    return _client->isStillConnected();
}

AsyncDBClient* TLConnection::client() {
    return _client.get();
}

void TLConnection::indicateUsed() {
    // It is illegal to attempt to use a connection after calling indicateFailure().
    invariant(_status.isOK() || _status == ConnectionPool::kConnectionStateUnknown);
    _lastUsed = _reactor->now();
}

Date_t TLConnection::getLastUsed() const {
    return _lastUsed;
}

const Status& TLConnection::getStatus() const {
    return _status;
}

void TLConnection::setTimeout(Milliseconds timeout, TimeoutCallback cb) {
    _timer.setTimeout(timeout, std::move(cb));
}

void TLConnection::cancelTimeout() {
    _timer.cancelTimeout();
}

void TLConnection::setup(Milliseconds timeout, SetupCallback cb) {
    auto anchor = shared_from_this();

    auto handler = std::make_shared<TimeoutHandler>();
    handler->promise.getFuture().getAsync(
        [ this, cb = std::move(cb) ](Status status) { cb(this, std::move(status)); });

    log() << "Connecting to " << _peer;
    setTimeout(timeout, [this, handler, timeout] {
        if (handler->done.swap(true)) {
            return;
        }
        std::string reason = str::stream() << "Timed out connecting to " << _peer << " after "
                                           << timeout;
        handler->promise.setError(
            Status(ErrorCodes::NetworkInterfaceExceededTimeLimit, std::move(reason)));

        if (_client) {
            _client->cancel();
        }
    });

    AsyncDBClient::connect(_peer, transport::kGlobalSSLMode, _serviceContext, _reactor, timeout)
        .onError([](StatusWith<AsyncDBClient::Handle> swc) -> StatusWith<AsyncDBClient::Handle> {
            return Status(ErrorCodes::HostUnreachable, swc.getStatus().reason());
        })
        .then([this](AsyncDBClient::Handle client) {
            _client = std::move(client);
            return _client->initWireVersion("NetworkInterfaceTL", _onConnectHook);
        })
        .then([this] { return _client->authenticate(getInternalUserAuthParams()); })
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
                log() << "Failed to connect to " << _peer << " - " << redact(status);
                handler->promise.setError(status);
            }
        });
}

void TLConnection::resetToUnknown() {
    _status = ConnectionPool::kConnectionStateUnknown;
}

void TLConnection::refresh(Milliseconds timeout, RefreshCallback cb) {
    auto anchor = shared_from_this();

    auto handler = std::make_shared<TimeoutHandler>();
    handler->promise.getFuture().getAsync(
        [ this, cb = std::move(cb) ](Status status) { cb(this, status); });

    setTimeout(timeout, [this, handler] {
        if (handler->done.swap(true)) {
            return;
        }

        _status = {ErrorCodes::HostUnreachable, "Timed out refreshing host"};
        _client->cancel();

        handler->promise.setError(_status);
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

            _status = status;
            if (status.isOK()) {
                handler->promise.emplaceValue();
            } else {
                handler->promise.setError(status);
            }
        });
}

size_t TLConnection::getGeneration() const {
    return _generation;
}

std::shared_ptr<ConnectionPool::ConnectionInterface> TLTypeFactory::makeConnection(
    const HostAndPort& hostAndPort, size_t generation) {
    return std::make_shared<TLConnection>(
        _reactor, getGlobalServiceContext(), hostAndPort, generation, _onConnectHook.get());
}

std::unique_ptr<ConnectionPool::TimerInterface> TLTypeFactory::makeTimer() {
    return std::make_unique<TLTimer>(_reactor);
}

Date_t TLTypeFactory::now() {
    return _reactor->now();
}

}  // namespace connection_pool_tl
}  // namespace executor
}  // namespace
