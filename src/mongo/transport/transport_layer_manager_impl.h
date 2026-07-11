// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/transport_layer.h"
#include "mongo/transport/transport_layer_manager.h"
#include "mongo/util/modules.h"

#include <memory>
#include <vector>

#include <boost/optional.hpp>

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_manager.h"
#endif

namespace mongo::transport {
class ClientTransportObserver;

class [[MONGO_MOD_NEEDS_REPLACEMENT]] TransportLayerManagerImpl final
    : public TransportLayerManager {
    TransportLayerManagerImpl(const TransportLayerManagerImpl&) = delete;
    TransportLayerManagerImpl& operator=(const TransportLayerManagerImpl&) = delete;

public:
    TransportLayerManagerImpl(std::vector<std::unique_ptr<TransportLayer>> tls,
                              TransportLayer* defaultEgressLayer);
    explicit TransportLayerManagerImpl(std::unique_ptr<TransportLayer> tl);

    ~TransportLayerManagerImpl() override = default;

    Status start() override;
    void shutdown() override;
    Status setup() override;
    void appendStatsForServerStatus(BSONObjBuilder* bob) const override;
    void appendStatsForFTDC(BSONObjBuilder& bob) const override;
    void stopAcceptingSessions() override;

    static std::unique_ptr<TransportLayerManager> make(
        ServiceContext* svcCtx,
        bool isUseGrpc,
        std::shared_ptr<ClientTransportObserver> observer = nullptr);


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
        bool useEgressGRPC = false,
        boost::optional<int> loadBalancerPort = {},
        boost::optional<int> priorityPort = {},
        std::shared_ptr<ClientTransportObserver> observer = nullptr,
        boost::optional<int> secondaryPort = {});

    static std::unique_ptr<TransportLayerManager> makeDefaultEgressTransportLayer();

    TransportLayer* getDefaultEgressLayer() override {
        return _defaultEgressLayer;
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

    TransportLayer* getTransportLayer(TransportProtocol protocol) const override {
        for (auto& tl : _tls) {
            if (tl->getTransportProtocol() == protocol) {
                return tl.get();
            }
        }
        return nullptr;
    }

private:
    /**
     * Expects the following order of state transitions, or terminates the process:
     * kNotInitialized --> kSetup --> kStarted --> kShutdown
     *       |                |                       ^
     *       -----------------------------------------'
     */
    enum class State { kNotInitialized, kSetUp, kStarted, kShutdown };
    State _state{State::kNotInitialized};
    std::mutex _stateMutex;

    std::vector<std::unique_ptr<TransportLayer>> _tls;
    TransportLayer* _defaultEgressLayer;
};

}  // namespace mongo::transport
