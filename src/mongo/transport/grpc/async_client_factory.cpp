/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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

#include "mongo/transport/grpc/async_client_factory.h"

#include "mongo/client/async_client.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/future.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {

GRPCAsyncClientFactory::GRPCAsyncClientFactory() {}

GRPCAsyncClientFactory::~GRPCAsyncClientFactory() {
    shutdown();
}

void GRPCAsyncClientFactory::startup(ServiceContext* svcCtx,
                                     transport::TransportLayer* tl,
                                     transport::ReactorHandle reactor) {
    stdx::lock_guard lk(_mutex);

    if (_state == State::kShutdown) {
        return;
    }

    invariant(_state == State::kNew);
    _state = State::kStarted;
    _svcCtx = svcCtx;
    invariant(tl->getTransportProtocol() == TransportProtocol::GRPC);
    _tl = tl;
    _reactor = std::move(reactor);
}

SemiFuture<std::shared_ptr<GRPCAsyncClientFactory::AsyncClientHandle>> GRPCAsyncClientFactory::get(
    const HostAndPort& target,
    transport::ConnectSSLMode sslMode,
    Milliseconds timeout,
    const CancellationToken& token) {

    return _get(false, target, sslMode, timeout, token).semi();
}

SemiFuture<std::shared_ptr<GRPCAsyncClientFactory::AsyncClientHandle>>
GRPCAsyncClientFactory::lease(const HostAndPort& target,
                              transport::ConnectSSLMode sslMode,
                              Milliseconds timeout,
                              const CancellationToken& token) {

    return _get(true, target, sslMode, timeout, token).semi();
}

void GRPCAsyncClientFactory::shutdown() {
    stdx::unique_lock<stdx::mutex> lk(_mutex);
    if (std::exchange(_state, State::kShutdown) == State::kShutdown) {
        return;
    }

    LOGV2_DEBUG(9936105, kDiagnosticLogLevel, "Shutting down gRPC stream factory");

    _dropConnections(lk);

    LOGV2_DEBUG(9936106,
                kDiagnosticLogLevel,
                "Waiting for outstanding streams to terminate",
                "numActiveStreams"_attr = _numActiveHandles,
                "numFinishingStreams"_attr = _finishingClientList.size());
    _cv.wait(lk, [&]() { return _shutdownComplete(lk); });
}

void GRPCAsyncClientFactory::appendStats(executor::ConnectionPoolStats* stats) {
    // TODO SERVER-99246: properly record "ConnectionPoolStats" for gRPC
    MONGO_UNIMPLEMENTED;
}

void GRPCAsyncClientFactory::dropConnections() {
    stdx::unique_lock lk(_mutex);
    _dropConnections(lk);
}

void GRPCAsyncClientFactory::dropConnections(const HostAndPort& target) {
    stdx::lock_guard lk(_mutex);

    if (_state == State::kShutdown) {
        return;
    }

    auto it = _endpoints.find(target);
    if (it == _endpoints.end()) {
        return;
    }

    _dropConnections(lk, it->second);
}

void GRPCAsyncClientFactory::setKeepOpen(const HostAndPort& target, bool keepOpen) {
    stdx::lock_guard lk(_mutex);
    auto it = _endpoints.find(target);
    if (it == _endpoints.end()) {
        return;
    }
    it->second.keepOpen = keepOpen;
}

Future<std::shared_ptr<GRPCAsyncClientFactory::AsyncClientHandle>> GRPCAsyncClientFactory::_get(
    bool lease,
    const HostAndPort& target,
    transport::ConnectSSLMode sslMode,
    Milliseconds timeout,
    const CancellationToken& token) {

    {
        stdx::lock_guard lk(_mutex);
        if (_state == State::kShutdown) {
            return Status(ErrorCodes::ShutdownInProgress,
                          "Cannot create new gRPC clients, factory was shut down");
        } else if (_state == State::kNew) {
            return Status(ErrorCodes::NotYetInitialized,
                          "Cannot create new gRPC clients, factory not yet started");
        }
    }

    LOGV2_DEBUG(9936101,
                kDiagnosticLogLevel,
                "Requesting new gRPC stream",
                "hostAndPort"_attr = target,
                "timeout"_attr = timeout);

    return AsyncDBClient::connect(target, sslMode, _svcCtx, _tl, _reactor, timeout, nullptr)
        .tapError([target](Status s) {
            LOGV2_DEBUG(9936103,
                        kDiagnosticLogLevel,
                        "gRPC stream establishment failed",
                        "hostAndPort"_attr = target,
                        "error"_attr = s);
        })
        .then([this, target](std::shared_ptr<AsyncDBClient> client)
                  -> std::shared_ptr<AsyncClientFactory::AsyncClientHandle> {
            LOGV2_DEBUG(9936102,
                        kDiagnosticLogLevel,
                        "gRPC stream establishment succeeded",
                        "hostAndPort"_attr = client->remote());
            auto handle = std::make_shared<Handle>(this, _svcCtx, std::move(client));

            stdx::lock_guard lk(_mutex);
            auto& handlesList = _endpoints[handle->getClient().remote()].handles;
            handlesList.push_front({&handle->getClient()});
            handle->_it = handlesList.begin();
            _numActiveHandles++;
            return handle;
        });
}

void GRPCAsyncClientFactory::_destroyHandle(Handle& handle) {
    FinishingClientState* cs;
    {
        stdx::lock_guard lk(_mutex);
        auto remote = handle.getRemote();

        // Preserve the AsyncDBClient until the call to asyncFinish() is complete.
        _finishingClientList.emplace_front(std::move(handle._client));
        cs = &(_finishingClientList.front());
        cs->it = _finishingClientList.begin();

        // Remove the handle from the active handles list.
        auto& handlesList = _endpoints[remote].handles;
        invariant(handle._it);
        handlesList.erase(*handle._it);
        handle._it.reset();
        _numActiveHandles--;
    }

    // gRPC calls must be ended with a call to finish(), which notifies the server that it is done
    // with the RPC call and should be gracefully terminated.
    checked_cast<EgressSession&>(cs->client->getTransportSession())
        .asyncFinish()
        .getAsync([cs, this](Status s) {
            LOGV2_DEBUG(9859000,
                        kDiagnosticLogLevel,
                        "Completed call to finish() on gRPC stream",
                        "status"_attr = s);

            stdx::lock_guard lk(_mutex);
            // Remove the client from the actively destructing list and stop holding onto the client
            // object.
            invariant(cs->it);
            cs->client.reset();
            if (MONGO_unlikely(cs->client.use_count() != 0)) {
                LOGV2_DEBUG(9859001,
                            kDiagnosticLogLevel,
                            "AsyncDBClient must be unowned during cleanup",
                            "clientUseCount"_attr = cs->client.use_count());
            }
            _finishingClientList.erase(*cs->it);

            if (_shutdownComplete(lk)) {
                _cv.notify_all();
            }
        });
}

void GRPCAsyncClientFactory::_dropConnections(WithLock lk) {
    LOGV2_DEBUG(9936107, kDiagnosticLogLevel, "Cancelling outstanding gRPC streams");

    for (auto&& target : _endpoints) {
        _dropConnections(lk, target.second);
    }
}

void GRPCAsyncClientFactory::_dropConnections(WithLock lk, EndpointState& target) {
    // TODO SERVER-99034: drop channel from the transport layer too.

    if (target.keepOpen) {
        return;
    }

    for (auto&& client : target.handles) {
        client->cancel();
    }
}

}  // namespace mongo::transport::grpc
