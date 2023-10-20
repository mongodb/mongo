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

#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace transport {

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
TransportLayerManagerImpl::makeAndStartDefaultEgressTransportLayer() {
    transport::AsioTransportLayer::Options opts(&serverGlobalParams);
    opts.mode = transport::AsioTransportLayer::Options::kEgress;
    opts.ipList.clear();

    std::unique_ptr<TransportLayerManager> ret = std::make_unique<TransportLayerManagerImpl>(
        std::make_unique<transport::AsioTransportLayer>(opts, nullptr));
    uassertStatusOK(ret->setup());
    uassertStatusOK(ret->start());
    return ret;
}

std::unique_ptr<TransportLayerManager> TransportLayerManagerImpl::createWithConfig(
    const ServerGlobalParams* config,
    ServiceContext* svcCtx,
    boost::optional<int> loadBalancerPort,
    boost::optional<int> routerPort) {

    transport::AsioTransportLayer::Options opts(config);
    opts.loadBalancerPort = std::move(loadBalancerPort);
    opts.routerPort = std::move(routerPort);

    std::vector<std::unique_ptr<TransportLayer>> retVector;
    retVector.push_back(
        std::make_unique<transport::AsioTransportLayer>(opts, svcCtx->getSessionManager()));
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

}  // namespace transport
}  // namespace mongo
