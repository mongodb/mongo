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


#include "mongo/transport/transport_layer_manager.h"

#include <algorithm>
#include <memory>
#include <string>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/status.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/asio/asio_transport_layer.h"
#include "mongo/transport/session.h"
#include "mongo/util/assert_util.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kNetwork


namespace mongo {
namespace transport {

template <typename Callable>
void TransportLayerManager::_foreach(Callable&& cb) const {
    {
        stdx::lock_guard<Latch> lk(_tlsMutex);
        for (auto&& tl : _tls) {
            cb(tl.get());
        }
    }
}

TransportLayerManager::TransportLayerManager(std::vector<std::unique_ptr<TransportLayer>> tls,
                                             TransportLayer* egressLayer)
    : _tls(std::move(tls)), _egressLayer(egressLayer) {
    invariant(_egressLayer);
    invariant(find_if(_tls.begin(), _tls.end(), [&](auto& tl) {
                  return tl.get() == _egressLayer;
              }) != _tls.end());
}

StatusWith<std::shared_ptr<Session>> TransportLayerManager::connect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    Milliseconds timeout,
    boost::optional<TransientSSLParams> transientSSLParams) {
    return _egressLayer->connect(peer, sslMode, timeout, transientSSLParams);
}

Future<std::shared_ptr<Session>> TransportLayerManager::asyncConnect(
    HostAndPort peer,
    ConnectSSLMode sslMode,
    const ReactorHandle& reactor,
    Milliseconds timeout,
    std::shared_ptr<ConnectionMetrics> connectionMetrics,
    std::shared_ptr<const SSLConnectionContext> transientSSLContext) {

    return _egressLayer->asyncConnect(
        peer, sslMode, reactor, timeout, connectionMetrics, transientSSLContext);
}

ReactorHandle TransportLayerManager::getReactor(WhichReactor which) {
    return _egressLayer->getReactor(which);
}

// TODO Right now this and setup() leave TLs started if there's an error. In practice the server
// exits with an error and this isn't an issue, but we should make this more robust.
Status TransportLayerManager::start() {
    for (auto&& tl : _tls) {
        auto status = tl->start();
        if (!status.isOK()) {
            _tls.clear();
            return status;
        }
    }

    return Status::OK();
}

void TransportLayerManager::shutdown() {
    _foreach([](TransportLayer* tl) { tl->shutdown(); });
}

// TODO Same comment as start()
Status TransportLayerManager::setup() {
    for (auto&& tl : _tls) {
        auto status = tl->setup();
        if (!status.isOK()) {
            _tls.clear();
            return status;
        }
    }

    return Status::OK();
}

void TransportLayerManager::appendStatsForServerStatus(BSONObjBuilder* bob) const {
    _foreach([&](const TransportLayer* tl) { tl->appendStatsForServerStatus(bob); });
}

void TransportLayerManager::appendStatsForFTDC(BSONObjBuilder& bob) const {
    _foreach([&](const TransportLayer* tl) { tl->appendStatsForFTDC(bob); });
}

Status TransportLayerManager::addAndStartTransportLayer(std::unique_ptr<TransportLayer> tl) {
    auto ptr = tl.get();
    {
        stdx::lock_guard<Latch> lk(_tlsMutex);
        _tls.emplace_back(std::move(tl));
    }
    return ptr->start();
}

std::unique_ptr<TransportLayer> TransportLayerManager::makeAndStartDefaultEgressTransportLayer() {
    transport::AsioTransportLayer::Options opts(&serverGlobalParams);
    opts.mode = transport::AsioTransportLayer::Options::kEgress;
    opts.ipList.clear();

    auto ret = std::make_unique<transport::AsioTransportLayer>(opts, nullptr);
    uassertStatusOK(ret->setup());
    uassertStatusOK(ret->start());
    return std::unique_ptr<TransportLayer>(std::move(ret));
}

std::unique_ptr<TransportLayer> TransportLayerManager::createWithConfig(
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
    return std::make_unique<TransportLayerManager>(std::move(retVector), egress);
}

#ifdef MONGO_CONFIG_SSL
Status TransportLayerManager::rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                                                 bool asyncOCSPStaple) {
    for (auto&& tl : _tls) {
        auto status = tl->rotateCertificates(manager, asyncOCSPStaple);
        if (!status.isOK()) {
            return status;
        }
    }
    return Status::OK();
}

StatusWith<std::shared_ptr<const transport::SSLConnectionContext>>
TransportLayerManager::createTransientSSLContext(const TransientSSLParams& transientSSLParams) {
    return _egressLayer->createTransientSSLContext(transientSSLParams);
}

#endif

}  // namespace transport
}  // namespace mongo
