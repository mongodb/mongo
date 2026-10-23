// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/grpc/async_client_factory.h"

#include "mongo/client/async_client.h"
#include "mongo/db/connection_health_metrics_parameter_gen.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/async_client_factory.h"
#include "mongo/executor/egress_connection_closer_manager.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/grpc/grpc_session.h"
#include "mongo/transport/grpc_connection_stats_gen.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/cancellation.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/future.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/version.h"

#include <mutex>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork

namespace mongo::transport::grpc {
namespace {
Status shutdownStatus() {
    return Status(ErrorCodes::ShutdownInProgress, "gRPC client factory was shut down");
}
}  // namespace

MONGO_FAIL_POINT_DEFINE(grpcAsyncClientFactoryHangBeforeTimeoutDiagnostics);
MONGO_FAIL_POINT_DEFINE(grpcAsyncClientFactoryHangBeforeHandleRegistration);
MONGO_FAIL_POINT_DEFINE(grpcAsyncClientFactoryHangBeforeConnect);
MONGO_FAIL_POINT_DEFINE(grpcAsyncClientFactoryHangBeforeShutdownWait);

GRPCAsyncClientFactory::GRPCAsyncClientFactory(std::string instanceName)
    : _instanceName(std::move(instanceName)) {}

GRPCAsyncClientFactory::~GRPCAsyncClientFactory() {
    shutdown();
}

void GRPCAsyncClientFactory::startup(ServiceContext* svcCtx,
                                     transport::TransportLayer* tl,
                                     transport::ReactorHandle reactor) {
    invariant(tl->getTransportProtocol() == getTransportProtocol());

    std::lock_guard lk(_mutex);

    if (_state == State::kShutdown) {
        return;
    }

    invariant(_state == State::kNew);
    _state = State::kStarted;
    _svcCtx = svcCtx;
    _tl = checked_cast<GRPCTransportLayer*>(tl);
    _reactor = std::move(reactor);

    executor::EgressConnectionCloserManager::get(_svcCtx).add(this);

    BSONObjBuilder bob;
    auto versionString =
        VersionInfoInterface::instance(VersionInfoInterface::NotEnabledAction::kFallback).version();
    ClientMetadata::serialize(_instanceName, versionString, &bob);

    auto metadataDoc = bob.obj();
    _client = _tl->createGRPCClient(metadataDoc.getObjectField(kMetadataDocumentName).getOwned());
    _client->start();
}

SemiFuture<std::shared_ptr<GRPCAsyncClientFactory::AsyncClientHandle>> GRPCAsyncClientFactory::get(
    const HostAndPort& target,
    transport::ConnectSSLMode sslMode,
    Milliseconds timeout,
    const CancellationToken& token,
    /*purpose=*/executor::ConnectionAcquisitionPurpose) {

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
    std::unique_lock<std::mutex> lk(_mutex);
    if (auto state = std::exchange(_state, State::kShutdown);
        state == State::kShutdown || state == State::kNew) {
        return;
    }

    LOGV2_DEBUG(9936105, kDiagnosticLogLevel, "Shutting down gRPC stream factory");

    _dropConnections(lk);

    auto pendingSetupsSnapshot = _pendingSetups;
    lk.unlock();
    executor::EgressConnectionCloserManager::get(_svcCtx).remove(this);
    for (const auto& setup : pendingSetupsSnapshot) {
        setup->cancellationSource.cancel();
    }
    grpcAsyncClientFactoryHangBeforeShutdownWait.pauseWhileSet();
    lk.lock();

    LOGV2_DEBUG(9936106,
                kDiagnosticLogLevel,
                "Waiting for outstanding streams to terminate",
                "numActiveHandles"_attr = _numActiveHandles,
                "numPendingSetups"_attr = _pendingSetups.size(),
                "numFinishingStreams"_attr = _finishingClientList.size());
    _cv.wait(lk, [&]() { return _shutdownComplete(lk); });

    lk.unlock();
    _client->shutdown();
}

void GRPCAsyncClientFactory::appendStats(BSONObjBuilder& bob) const {
    auto stats = getStats();
    stats.serialize(&bob);
}

GRPCConnectionStats GRPCAsyncClientFactory::getStats() const {
    GRPCConnectionStats stats;
    _client->appendStats(stats);

    stats.setTotalInUseStreams(stats.getTotalActiveStreams() - _numLeasedStreams.get());
    stats.setTotalLeasedStreams(_numLeasedStreams.get());
    stats.setTotalStreamsCreated(_numStreamsCreated.get());
    stats.setTotalStreamUsageTimeMs(_totalStreamUsageTimeMs.get());

    return stats;
}

void GRPCAsyncClientFactory::dropConnections(const Status& status) {
    std::unique_lock lk(_mutex);
    if (_state == State::kShutdown || _state == State::kNew) {
        return;
    }
    _client->dropConnections(status);
    _dropConnections(lk);
}

void GRPCAsyncClientFactory::dropConnections(const HostAndPort& target, const Status& status) {
    std::lock_guard lk(_mutex);
    if (_state == State::kShutdown || _state == State::kNew) {
        return;
    }

    _client->dropConnections(target, status);

    auto it = _endpoints.find(target);
    if (it == _endpoints.end()) {
        return;
    }

    _dropConnections(lk, it->second);
}

void GRPCAsyncClientFactory::setKeepOpen(const HostAndPort& target, bool keepOpen) {
    std::lock_guard lk(_mutex);
    if (_state == State::kShutdown || _state == State::kNew) {
        return;
    }

    _client->setKeepOpen(target, keepOpen);

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

    std::shared_ptr<PendingSetup> setup;
    {
        std::lock_guard lk(_mutex);
        if (_state == State::kShutdown) {
            return Status(ErrorCodes::ShutdownInProgress,
                          "Cannot create new gRPC clients, factory was shut down");
        } else if (_state == State::kNew) {
            return Status(ErrorCodes::NotYetInitialized,
                          "Cannot create new gRPC clients, factory not yet started");
        }
        setup = std::make_shared<PendingSetup>(token);
        setup->iterator = _pendingSetups.insert(_pendingSetups.end(), setup);
    }

    auto cleanup = ScopeGuard([&] {
        std::lock_guard lk(_mutex);
        _unregisterSetup(lk, *setup);
    });

    // In the case of no timeout, set a default timeout to ensure we don't queue streams
    // indefinitely if they cannot connect.
    if (timeout < Milliseconds(0)) {
        timeout = kDefaultStreamEstablishmentTimeout;
    }

    LOGV2_DEBUG(9936101,
                kDiagnosticLogLevel,
                "Requesting new gRPC stream",
                "hostAndPort"_attr = target,
                "timeout"_attr = timeout);

    auto connMetrics = std::make_shared<ConnectionMetrics>(_svcCtx->getFastClockSource());
    grpcAsyncClientFactoryHangBeforeConnect.pauseWhileSet();
    auto result = [&]()
        -> Future<std::shared_ptr<EgressSession>> {
        {
            std::lock_guard lk(_mutex);
            if (_state == State::kShutdown) {
                return shutdownStatus();
            }
        }
        return _client->connect(target,
                                checked_pointer_cast<GRPCReactor>(_reactor),
                                timeout,
                                {boost::none /**authToken */, sslMode},
                                setup->cancellationSource.token(),
                                connMetrics);
    }()
               .then([target, reactor = _reactor, svcCtx = _svcCtx](
                         std::shared_ptr<transport::Session> session) {
                   return AsyncDBClient::create(target, std::move(session), svcCtx, reactor);
               })
               .tapError([target](Status s) {
                   LOGV2_DEBUG(9936103,
                               kDiagnosticLogLevel,
                               "gRPC stream establishment failed",
                               "hostAndPort"_attr = target,
                               "error"_attr = s);
               })
               .onError<ErrorCodes::ExceededTimeLimit>(
                   [this, target, timeout](
                       Status s) mutable -> StatusWith<std::shared_ptr<AsyncDBClient>> {
                       grpcAsyncClientFactoryHangBeforeTimeoutDiagnostics.pauseWhileSet();
                       if (gEnableDetailedConnectionHealthMetricLogLines.load()) {
                           size_t activeCalls;
                           {
                               std::lock_guard lk(_mutex);
                               activeCalls = _endpoints[target].handles.size();
                           }

                           // TODO SERVER-100262: Remove this log line or add diagnostics about how
                           // many calls are on the specific channel used by the stream.
                           LOGV2(10026101,
                                 "Exceeded time limit while starting gRPC call",
                                 "hostAndPort"_attr = target,
                                 "timeout"_attr = timeout,
                                 "error"_attr = s,
                                 "activeStreamEstablishmentAttempts"_attr =
                                     _client->getPendingStreamEstablishments(target),
                                 "activeCalls"_attr = activeCalls);
                       }

                       return s;
                   })
               .then([this, target, lease, connMetrics](std::shared_ptr<AsyncDBClient> client)
                         -> StatusWith<std::shared_ptr<AsyncClientFactory::AsyncClientHandle>> {
                   grpcAsyncClientFactoryHangBeforeHandleRegistration.pauseWhileSet();
                   auto ownedClient = std::move(client);
                   std::unique_lock lk(_mutex);
                   if (_state == State::kShutdown) {
                       lk.unlock();
                       checked_cast<EgressSession&>(ownedClient->getTransportSession())
                           .cancel(shutdownStatus());
                       ownedClient.reset();
                       return shutdownStatus();
                   }
                   LOGV2_DEBUG(9936102,
                               kDiagnosticLogLevel,
                               "gRPC stream establishment succeeded",
                               "hostAndPort"_attr = target,
                               "duration"_attr = connMetrics->total());
                   auto handle =
                       std::make_shared<Handle>(this, _svcCtx, std::move(ownedClient), lease);

                   auto& handlesList = _endpoints[target].handles;
                   handlesList.push_front({&handle->getClient()});
                   handle->_it = handlesList.begin();
                   _numStreamsCreated.increment();
                   _numActiveHandles++;
                   if (lease) {
                       _numLeasedStreams.increment();
                   }

                   return std::shared_ptr<AsyncClientHandle>(std::move(handle));
               })
               .onCompletion(
                   [this, setup, token](StatusWith<std::shared_ptr<AsyncClientHandle>> result) {
                       std::lock_guard lk(_mutex);
                       const Status& tokenCancellation = Client::tokenCancellationStatus();
                       // The conditions here preserve non-Shutdown statuses, e.g. if a timeout or
                       // client cancellation won't be replaced by a shutdown status.
                       if (_state == State::kShutdown && !token.isCanceled() &&
                           result.getStatus().code() == tokenCancellation.code() &&
                           result.getStatus().reason() == tokenCancellation.reason()) {
                           result = shutdownStatus();
                       }
                       _unregisterSetup(lk, *setup);
                       return result;
                   });
    cleanup.dismiss();
    return result;
}

void GRPCAsyncClientFactory::_unregisterSetup(WithLock lk, PendingSetup& setup) {
    _pendingSetups.erase(setup.iterator);
    if (_shutdownComplete(lk)) {
        _cv.notify_all();
    }
}

void GRPCAsyncClientFactory::_destroyHandle(Handle& handle) {
    // If handle was marked as unsuccessful, ensure that the underlying gRPC call was canceled
    // before retrieving the remote status.
    if (!handle._outcome || !handle._outcome->isOK()) {
        handle._client->cancel();
    }

    FinishingClientState* cs;
    {
        std::lock_guard lk(_mutex);
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

    bool isLeased = handle.isLeased();
    auto timer = handle.getAcquiredTimer();

    // gRPC calls must be ended with a call to finish(), which notifies the server that it is done
    // with the RPC call and should be gracefully terminated.
    checked_cast<EgressSession&>(cs->client->getTransportSession())
        .asyncFinish()
        .getAsync([cs, isLeased, acquired = std::move(timer), this](Status s) {
            LOGV2_DEBUG(9859000,
                        kDiagnosticLogLevel,
                        "Completed call to finish() on gRPC stream",
                        "status"_attr = s);

            std::lock_guard lk(_mutex);
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

            _totalStreamUsageTimeMs.increment(acquired->millis());
            if (isLeased) {
                _numLeasedStreams.decrement();
            }

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
    if (target.keepOpen) {
        return;
    }

    for (auto&& client : target.handles) {
        client->cancel();
    }
}

}  // namespace mongo::transport::grpc
