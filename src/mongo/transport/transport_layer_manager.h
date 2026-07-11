// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/client.h"
#include "mongo/util/modules.h"

#include <memory>

#ifdef MONGO_CONFIG_SSL
#include "mongo/util/net/ssl_manager.h"
#endif

namespace mongo::transport {

class TransportLayer;
enum class TransportProtocol;

/**
 * This TransportLayerManager holds other TransportLayers, and manages all TransportLayer
 * operations that should touch every TransportLayer. For egress-only functionality, callers should
 * access the egress layer through getDefaultEgressLayer(). Mongod and Mongos can treat this like
 * the "only" TransportLayer and not be concerned with which other TransportLayer implementations it
 * holds underneath.
 *
 * The manager must be provided with an immutable list of TransportLayers that it will manage at
 * construction (preferably through factory functions) to obviate the need for synchronization.
 */
class [[MONGO_MOD_PUBLIC]] TransportLayerManager {
    TransportLayerManager(const TransportLayerManager&) = delete;
    TransportLayerManager& operator=(const TransportLayerManager&) = delete;

public:
    TransportLayerManager() = default;
    virtual ~TransportLayerManager() = default;

    virtual Status start() = 0;
    virtual void shutdown() = 0;
    virtual Status setup() = 0;
    virtual void appendStatsForServerStatus(BSONObjBuilder* bob) const = 0;
    virtual void appendStatsForFTDC(BSONObjBuilder& bob) const = 0;

    virtual TransportLayer* getDefaultEgressLayer() = 0;
    virtual const std::vector<std::unique_ptr<TransportLayer>>& getTransportLayers() const = 0;

    /**
     * Returns the transport layer that matches the TransportProtocol. If none exist this function
     * returns a nullptr.
     */
    virtual TransportLayer* getTransportLayer(TransportProtocol protocol) const = 0;

#ifdef MONGO_CONFIG_SSL
    virtual Status rotateCertificates(std::shared_ptr<SSLManagerInterface> manager,
                                      bool asyncOCSPStaple) = 0;
#endif

    /**
     * Execute a callback on every TransportLayer owned by the TransportLayerManager.
     */
    virtual void forEach(std::function<void(TransportLayer*)> fn) = 0;

    /**
     * True if any of the TransportLayers has any active sessions.
     */
    virtual bool hasActiveSessions() const = 0;

    /**
     * Check that the total number of max open sessions across TransportLayers
     * does not exceed system limits, and log a startup warning if not.
     */
    virtual void checkMaxOpenSessionsAtStartup() const = 0;

    /**
     * End all sessions that do not match the mask in tags.
     */
    virtual void endAllSessions(Client::TagMask tags) = 0;

    /**
     * Instruct transport layers to discontinue accepting new sessions.
     */
    virtual void stopAcceptingSessions() = 0;
};

}  // namespace mongo::transport
