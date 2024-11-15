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

#pragma once

#include <boost/optional.hpp>
#include <memory>
#include <vector>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_manager.h"
#endif

namespace mongo::transport {
class ClientTransportObserver;

class TransportLayerManagerImpl final : public TransportLayerManager {
    TransportLayerManagerImpl(const TransportLayerManagerImpl&) = delete;
    TransportLayerManagerImpl& operator=(const TransportLayerManagerImpl&) = delete;

public:
    TransportLayerManagerImpl(std::vector<std::unique_ptr<TransportLayer>> tls,
                              TransportLayer* egressLayer);
    explicit TransportLayerManagerImpl(std::unique_ptr<TransportLayer> tl);

    ~TransportLayerManagerImpl() override = default;

    Status start() override;
    void shutdown() override;
    Status setup() override;
    void appendStatsForServerStatus(BSONObjBuilder* bob) const override;
    void appendStatsForFTDC(BSONObjBuilder& bob) const override;
    void stopAcceptingSessions() override;

    /*
     * This initializes a TransportLayerManager with the global configuration of the server.
     *
     * To setup networking in mongod/mongos, create a TransportLayerManager with this function,
     * then call
     * tl->setup();
     * serviceContext->setTransportLayerManager(std::move(tl));
     * serviceContext->getTransportLayerManager()->start();
     */
    static std::unique_ptr<TransportLayerManager> createWithConfig(
        const ServerGlobalParams* config,
        ServiceContext* ctx,
        boost::optional<int> loadBalancerPort = {},
        boost::optional<int> routerPort = {},
        std::shared_ptr<ClientTransportObserver> observer = nullptr);

    static std::unique_ptr<TransportLayerManager> makeDefaultEgressTransportLayer();

    TransportLayer* getEgressLayer() override {
        return _egressLayer;
    }

#ifdef MONGO_CONFIG_SSL
    Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                              bool asyncOCSPStaple) override;
#endif

    void forEach(std::function<void(TransportLayer*)> fn) override;
    bool hasActiveSessions() const override;
    void checkMaxOpenSessionsAtStartup() const override;
    void endAllSessions(Client::TagMask tags) override;
    const std::vector<std::unique_ptr<TransportLayer>>& getTransportLayers() const override {
        return _tls;
    }

private:
    /**
     * Expects the following order of state transitions, or terminates the process:
     * kNotInitialized --> kSetup --> kStarted --> kShutdown
     *       |                |                       ^
     *       -----------------------------------------'
     */
    enum class State { kNotInitialized, kSetUp, kStarted, kShutdown };
    AtomicWord<State> _state{State::kNotInitialized};

    std::vector<std::unique_ptr<TransportLayer>> _tls;
    TransportLayer* _egressLayer;
};

}  // namespace mongo::transport
