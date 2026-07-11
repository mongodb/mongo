// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/platform/atomic.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_establishment_rate_limiter.h"
#include "mongo/util/duration.h"
#include "mongo/util/modules.h"

#include <limits>
#include <memory>

namespace mongo {
class BSONObjBuilder;

namespace transport {

/**
 * The SessionManager is responsible for accepting new Sessions from the TransportLayer, creating
 * Client instances from them, and providing them with their own instances of SessionWorkflow. It
 * also provides facilities for terminating sessions and attaching handlers that are invoked during
 * different stages of the Sessions' lifecycles.
 */
class [[MONGO_MOD_PUBLIC]] SessionManager {
private:
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

protected:
    SessionManager() = default;

public:
    virtual ~SessionManager() = default;

    /**
     * Begin running a new Session. This method returns immediately.
     */
    virtual void startSession(std::shared_ptr<Session> session) = 0;

    /**
     * Terminate a session by Client pointer.
     */
    virtual void endSessionByClient(Client* client) = 0;

    /**
     * End all sessions associated with this SessionManager that do not match the mask in tags.
     */
    virtual void endAllSessions(Client::TagMask tags) = 0;

    /**
     * Shuts down the session manager.
     */
    virtual bool shutdown(Milliseconds timeout) = 0;

    /**
     * Returns the number of sessions currently open.
     */
    virtual std::size_t numOpenSessions() const = 0;

    virtual std::vector<std::pair<SessionId, std::string>> getOpenSessionIDs() const = 0;

    /**
     * Returns the maximum number of sessions that can be open.
     */
    virtual std::size_t maxOpenSessions() const {
        return std::numeric_limits<std::size_t>::max();
    }

    /**
     * Returns the rate limiter component used for session establishment. New sessions should call
     * into this component to ensure they are respecting the configured establishment rate limit.
     */
    virtual SessionEstablishmentRateLimiter& getSessionEstablishmentRateLimiter() {
        return _sessionEstablishmentRateLimiter;
    }

    // Stats

    /**
     * Total number of operations created on sessions belonging to this SessionManager.
     */
    std::size_t getTotalOperations() const {
        return _opCounters->total.load();
    }

    /**
     * Number of operations on sessions belonging to this SessionManager
     * which have begun but not yet completed.
     */
    virtual std::size_t getActiveOperations() const {
        return getTotalOperations() - getCompletedOperations();
    }

    /**
     * Total number of completed operations on sessions belonging to this SessionManager.
     */
    std::size_t getCompletedOperations() const {
        return _opCounters->completed.load();
    }

    const auto& getOpCounters() const {
        return _opCounters;
    }

    /**
     * Called when marking a session as a load balancer session or not. Increments or decrements
     * the number of sessions on the loadBalancer port accordingly.
     */
    virtual void onLoadBalancerPeerSet(bool isLoadBalancerPeer) = 0;

    HelloMetrics helloMetrics;
    ServiceExecutorStats serviceExecutorStats;

protected:
    friend class Session;
    std::shared_ptr<SessionManagerOpCounters> _opCounters =
        std::make_shared<SessionManagerOpCounters>();
    SessionEstablishmentRateLimiter _sessionEstablishmentRateLimiter;
};

}  // namespace transport
}  // namespace mongo
