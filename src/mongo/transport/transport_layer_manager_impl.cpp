/**
 *    Copyright (C) 2023-present MongoDB, Inc.
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


#include "mongo/transport/transport_layer_manager_impl.h"

#ifdef __linux__
#include <fstream>
#endif

#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/transport/asio/asio_session_manager.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/util/assert_util.h"

#ifdef MONGO_CONFIG_GRPC
#include "mongo/transport/grpc/grpc_transport_layer_impl.h"
#endif

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_options.h"
#endif

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo::transport {

TransportLayerManagerImpl::TransportLayerManagerImpl(
    std::vector<std::unique_ptr<TransportLayer>> tls, TransportLayer* egressLayer)
    : _tls(std::move(tls)), _egressLayer(egressLayer) {
    invariant(_egressLayer);
    invariant(find_if(_tls.begin(), _tls.end(), [&](auto& tl) {
                  return tl.get() == _egressLayer;
              }) != _tls.end());
}

TransportLayerManagerImpl::TransportLayerManagerImpl(std::unique_ptr<TransportLayer> tl) {
    _tls.push_back(std::move(tl));
    _egressLayer = _tls[0].get();
}

// TODO Right now this and setup() leave TLs started if there's an error. In practice the server
// exits with an error and this isn't an issue, but we should make this more robust.
Status TransportLayerManagerImpl::start() {
    invariant(_state.swap(State::kStarted) == State::kSetUp);
    for (auto&& tl : _tls) {
        auto status = tl->start();
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

void TransportLayerManagerImpl::stopAcceptingSessions() {
    for (auto&& tl : _tls) {
        tl->stopAcceptingSessions();
    }
}

void TransportLayerManagerImpl::shutdown() {
    invariant(_state.swap(State::kShutdown) != State::kShutdown);
    for (auto&& tl : _tls) {
        tl->shutdown();
    }
}

Status TransportLayerManagerImpl::setup() {
    invariant(_state.swap(State::kSetUp) == State::kNotInitialized);
    for (auto&& tl : _tls) {
        auto status = tl->setup();
        if (!status.isOK()) {
            return status;
        }
    }

    return Status::OK();
}

void TransportLayerManagerImpl::forEach(std::function<void(TransportLayer*)> fn) {
    for (auto&& tl : _tls) {
        fn(tl.get());
    }
}

void TransportLayerManagerImpl::appendStatsForServerStatus(BSONObjBuilder* bob) const {
    for (auto&& tl : _tls) {
        tl->appendStatsForServerStatus(bob);
    }
}

void TransportLayerManagerImpl::appendStatsForFTDC(BSONObjBuilder& bob) const {
    for (auto&& tl : _tls) {
        tl->appendStatsForFTDC(bob);
    }
}

std::unique_ptr<TransportLayerManager>
TransportLayerManagerImpl::makeDefaultEgressTransportLayer() {
    transport::AsioTransportLayer::Options opts(&serverGlobalParams);
    opts.mode = transport::AsioTransportLayer::Options::kEgress;
    opts.ipList.clear();

    return std::make_unique<TransportLayerManagerImpl>(
        std::make_unique<transport::AsioTransportLayer>(opts, nullptr));
}

std::unique_ptr<TransportLayerManager> TransportLayerManagerImpl::createWithConfig(
    const ServerGlobalParams* config,
    ServiceContext* svcCtx,
    boost::optional<int> loadBalancerPort,
    boost::optional<int> routerPort,
    std::shared_ptr<ClientTransportObserver> observer) {

    std::vector<std::unique_ptr<TransportLayer>> retVector;
    std::vector<std::shared_ptr<ClientTransportObserver>> observers;
    if (observer) {
        observers.push_back(std::move(observer));
    }

    {
        AsioTransportLayer::Options opts(config);
        opts.loadBalancerPort = std::move(loadBalancerPort);
        opts.routerPort = std::move(routerPort);

        auto sm = std::make_unique<AsioSessionManager>(svcCtx, observers);
        auto tl = std::make_unique<AsioTransportLayer>(opts, std::move(sm));
        retVector.push_back(std::move(tl));
    }

#ifdef MONGO_CONFIG_GRPC
#ifdef MONGO_CONFIG_SSL
    if (!sslGlobalParams.sslPEMKeyFile.empty()) {
        using GRPCTL = grpc::GRPCTransportLayerImpl;
        retVector.push_back(
            GRPCTL::createWithConfig(svcCtx, GRPCTL::Options(*config), std::move(observers)));
    } else {
        LOGV2(8076800, "Unable to start gRPC transport without tlsCertificateKeyFile");
    }
#else
    LOGV2(8076801, "Unable to start gRPC transport in a build without SSL enabled");
#endif
#endif

    auto egress = retVector[0].get();
    return std::make_unique<TransportLayerManagerImpl>(std::move(retVector), egress);
}

#ifdef MONGO_CONFIG_SSL
Status TransportLayerManagerImpl::rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                                                     bool asyncOCSPStaple) {
    std::vector<StringData> successfulRotations;
    for (auto&& tl : _tls) {
        if (auto status = tl->rotateCertificates(manager, asyncOCSPStaple); !status.isOK()) {
            LOGV2_INFO(8074101,
                       "Failed to rotate certificates for transport layer",
                       "transportLayer"_attr = tl->getNameForLogging(),
                       "status"_attr = status);
            StringBuilder failureMessage;
            failureMessage << "Certificate rotation failed. " << status.reason();

            if (successfulRotations.size() > 0) {
                failureMessage << " Before rotation failed for " << tl->getNameForLogging()
                               << ", other transport layer(s) succeeded and are currently "
                                  "using rotated certificates: [ ";
                for (StringData s : successfulRotations) {
                    failureMessage << s << " ";
                }
                failureMessage << "]";
            }

            return status.withContext(failureMessage.str());
        } else {
            successfulRotations.push_back(tl->getNameForLogging());
            LOGV2_INFO(8074102,
                       "Successfully rotated certificates for transport layer",
                       "transportLayer"_attr = tl->getNameForLogging());
        }
    }
    return Status::OK();
}
#endif

bool TransportLayerManagerImpl::hasActiveSessions() const {
    return std::any_of(_tls.cbegin(), _tls.cend(), [](const auto& tl) {
        auto sm = tl->getSessionManager();
        return sm && (sm->numOpenSessions() > 0);
    });
}

void TransportLayerManagerImpl::checkMaxOpenSessionsAtStartup() const {
#ifdef __linux__
    // Check if vm.max_map_count is high enough, as per SERVER-51233
    std::size_t maxConns = std::accumulate(
        _tls.cbegin(), _tls.cend(), std::size_t{0}, [&](std::size_t acc, const auto& tl) {
            if (!tl->getSessionManager())
                return size_t{0};
            return std::clamp(tl->getSessionManager()->maxOpenSessions(),
                              std::size_t{0},
                              std::numeric_limits<std::size_t>::max() - acc);
        });

    std::size_t requiredMapCount = 2 * maxConns;

    std::fstream f("/proc/sys/vm/max_map_count", std::ios_base::in);
    std::size_t val;
    f >> val;

    if (val < requiredMapCount) {
        LOGV2_WARNING_OPTIONS(5123300,
                              {logv2::LogTag::kStartupWarnings},
                              "vm.max_map_count is too low",
                              "currentValue"_attr = val,
                              "recommendedMinimum"_attr = requiredMapCount,
                              "maxConns"_attr = maxConns);
    }
#endif
}

void TransportLayerManagerImpl::endAllSessions(Client::TagMask tags) {
    std::for_each(_tls.cbegin(), _tls.cend(), [&](const auto& tl) {
        if (auto sm = tl->getSessionManager(); sm)
            sm->endAllSessions(tags);
    });
}

}  // namespace mongo::transport
