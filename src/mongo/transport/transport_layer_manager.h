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

#pragma once

#include <memory>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/client.h"

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
class TransportLayerManager {
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
