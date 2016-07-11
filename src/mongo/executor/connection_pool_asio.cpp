/** *    Copyright (C) 2015 MongoDB Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/executor/connection_pool_asio.h"

#include <asio.hpp>

#include "mongo/executor/async_stream_factory_interface.h"
#include "mongo/executor/network_interface_asio.h"
#include "mongo/rpc/factory.h"
#include "mongo/rpc/legacy_request_builder.h"
#include "mongo/rpc/reply_interface.h"
#include "mongo/stdx/memory.h"

namespace mongo {
namespace executor {
namespace connection_pool_asio {

ASIOTimer::ASIOTimer(asio::io_service::strand* strand)
    : _strand(strand),
      _impl(strand->get_io_service()),
      _callbackSharedState(std::make_shared<CallbackSharedState>()) {}

ASIOTimer::~ASIOTimer() {
    stdx::lock_guard<stdx::mutex> lk(_callbackSharedState->mutex);
    ++_callbackSharedState->id;
}

const auto kMaxTimerDuration = duration_cast<Milliseconds>(ASIOTimer::clock_type::duration::max());

void ASIOTimer::setTimeout(Milliseconds timeout, TimeoutCallback cb) {
    _strand->dispatch([this, timeout, cb] {
        _cb = std::move(cb);

        cancelTimeout();
        _impl.expires_after(std::min(kMaxTimerDuration, timeout).toSystemDuration());

        decltype(_callbackSharedState->id) id;
        decltype(_callbackSharedState) sharedState;

        {
            stdx::lock_guard<stdx::mutex> lk(_callbackSharedState->mutex);
            id = ++_callbackSharedState->id;
            sharedState = _callbackSharedState;
        }

        _impl.async_wait(_strand->wrap([this, id, sharedState](const asio::error_code& error) {
            if (error == asio::error::operation_aborted) {
                return;
            }

            stdx::unique_lock<stdx::mutex> lk(sharedState->mutex);

            // If the id in shared state doesn't match the id in our callback, it
            // means we were cancelled, but still executed. This can occur if we
            // were cancelled just before our timeout. We need a generation, rather
            // than just a bool here, because we could have been cancelled and
            // another callback set, in which case we shouldn't run and the we
            // should let the other callback execute instead.
            if (sharedState->id == id) {
                auto cb = std::move(_cb);
                lk.unlock();
                cb();
            }
        }));
    });
}

void ASIOTimer::cancelTimeout() {
    decltype(_callbackSharedState) sharedState;
    decltype(_callbackSharedState->id) id;
    {
        stdx::lock_guard<stdx::mutex> lk(_callbackSharedState->mutex);
        id = ++_callbackSharedState->id;
        sharedState = _callbackSharedState;
    }
    _strand->dispatch([this, id, sharedState] {
        stdx::lock_guard<stdx::mutex> lk(sharedState->mutex);
        if (sharedState->id != id)
            return;
        _impl.cancel();
    });
}

ASIOConnection::ASIOConnection(const HostAndPort& hostAndPort, size_t generation, ASIOImpl* global)
    : _global(global),
      _hostAndPort(hostAndPort),
      _generation(generation),
      _impl(makeAsyncOp(this)),
      _timer(&_impl->strand()) {}

void ASIOConnection::indicateSuccess() {
    _status = Status::OK();
}

void ASIOConnection::indicateFailure(Status status) {
    invariant(!status.isOK());
    _status = std::move(status);
}

const HostAndPort& ASIOConnection::getHostAndPort() const {
    return _hostAndPort;
}

bool ASIOConnection::isHealthy() {
    // Check if the remote host has closed the connection.
    return _impl->connection().stream().isOpen();
}

void ASIOConnection::indicateUsed() {
    // It is illegal to attempt to use a connection after calling indicateFailure().
    invariant(_status.isOK() || _status == ConnectionPool::kConnectionStateUnknown);
    _lastUsed = _global->now();
}

Date_t ASIOConnection::getLastUsed() const {
    return _lastUsed;
}

const Status& ASIOConnection::getStatus() const {
    return _status;
}

size_t ASIOConnection::getGeneration() const {
    return _generation;
}

std::unique_ptr<NetworkInterfaceASIO::AsyncOp> ASIOConnection::makeAsyncOp(ASIOConnection* conn) {
    return stdx::make_unique<NetworkInterfaceASIO::AsyncOp>(
        conn->_global->_impl,
        TaskExecutor::CallbackHandle(),
        RemoteCommandRequest{
            conn->getHostAndPort(), std::string("admin"), BSON("isMaster" << 1), BSONObj()},
        [conn](const TaskExecutor::ResponseStatus& status) {
            auto cb = std::move(conn->_setupCallback);
            cb(conn, status.isOK() ? Status::OK() : status.getStatus());
        },
        conn->_global->now());
}

Message ASIOConnection::makeIsMasterRequest(ASIOConnection* conn) {
    rpc::LegacyRequestBuilder requestBuilder{};
    requestBuilder.setDatabase("admin");
    requestBuilder.setCommandName("isMaster");
    requestBuilder.setCommandArgs(BSON("isMaster" << 1));
    requestBuilder.setMetadata(rpc::makeEmptyMetadata());

    return requestBuilder.done();
}

void ASIOConnection::setTimeout(Milliseconds timeout, TimeoutCallback cb) {
    _timer.setTimeout(timeout, std::move(cb));
}

void ASIOConnection::cancelTimeout() {
    _timer.cancelTimeout();
}

void ASIOConnection::setup(Milliseconds timeout, SetupCallback cb) {
    _impl->strand().dispatch([this, timeout, cb] {
        _setupCallback = [this, cb](ConnectionInterface* ptr, Status status) {
            cancelTimeout();
            cb(ptr, status);
        };

        std::size_t generation;
        {
            stdx::lock_guard<stdx::mutex> lk(_impl->_access->mutex);
            generation = _impl->_access->id;
        }

        // Actually timeout setup
        setTimeout(timeout, [this, generation] {
            stdx::lock_guard<stdx::mutex> lk(_impl->_access->mutex);
            if (generation != _impl->_access->id) {
                // The operation has been cleaned up, do not access.
                return;
            }
            _impl->timeOut_inlock();
        });

        _global->_impl->_connect(_impl.get());
    });
}

void ASIOConnection::resetToUnknown() {
    _status = ConnectionPool::kConnectionStateUnknown;
}

void ASIOConnection::refresh(Milliseconds timeout, RefreshCallback cb) {
    _impl->strand().dispatch([this, timeout, cb] {
        auto op = _impl.get();

        // We clear state transitions because we're re-running a portion of the asio state machine
        // without entering in startCommand (which would call this for us).
        op->clearStateTransitions();

        _refreshCallback = std::move(cb);

        // Actually timeout refreshes
        setTimeout(timeout, [this] { _impl->connection().stream().cancel(); });

        // Our pings are isMaster's
        auto beginStatus = op->beginCommand(makeIsMasterRequest(this),
                                            NetworkInterfaceASIO::AsyncCommand::CommandType::kRPC,
                                            _hostAndPort);
        if (!beginStatus.isOK()) {
            auto cb = std::move(_refreshCallback);
            cb(this, beginStatus);
            return;
        }

        // If we fail during refresh, the _onFinish function of the AsyncOp will get called. As such
        // we
        // need to intercept those calls so we can capture them. This will get cleared out when we
        // fill
        // in the real onFinish in startCommand.
        op->setOnFinish([this](StatusWith<RemoteCommandResponse> failedResponse) {
            invariant(!failedResponse.isOK());
            auto cb = std::move(_refreshCallback);
            cb(this, failedResponse.getStatus());
        });

        op->_inRefresh = true;

        _global->_impl->_asyncRunCommand(op, [this, op](std::error_code ec, size_t bytes) {
            cancelTimeout();

            auto cb = std::move(_refreshCallback);

            if (ec)
                return cb(this, Status(ErrorCodes::HostUnreachable, ec.message()));

            op->_inRefresh = false;
            cb(this, Status::OK());
        });
    });
}

std::unique_ptr<NetworkInterfaceASIO::AsyncOp> ASIOConnection::releaseAsyncOp() {
    return std::move(_impl);
}

void ASIOConnection::bindAsyncOp(std::unique_ptr<NetworkInterfaceASIO::AsyncOp> op) {
    _impl = std::move(op);
}

ASIOImpl::ASIOImpl(NetworkInterfaceASIO* impl) : _impl(impl) {}

Date_t ASIOImpl::now() {
    return _impl->now();
}

std::unique_ptr<ConnectionPool::TimerInterface> ASIOImpl::makeTimer() {
    return stdx::make_unique<ASIOTimer>(&_impl->_strand);
}

std::unique_ptr<ConnectionPool::ConnectionInterface> ASIOImpl::makeConnection(
    const HostAndPort& hostAndPort, size_t generation) {
    return stdx::make_unique<ASIOConnection>(hostAndPort, generation, this);
}

}  // namespace connection_pool_asio
}  // namespace executor
}  // namespace mongo
