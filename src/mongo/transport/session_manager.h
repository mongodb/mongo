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

#include <limits>
#include <memory>

#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/transport/hello_metrics.h"
#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_establishment_rate_limiter.h"
#include "mongo/util/duration.h"

namespace mongo {
class BSONObjBuilder;

namespace transport {

/**
 * The SessionManager is responsible for accepting new Sessions from the TransportLayer, creating
 * Client instances from them, and providing them with their own instances of SessionWorkflow. It
 * also provides facilities for terminating sessions and attaching handlers that are invoked during
 * different stages of the Sessions' lifecycles.
 */
class SessionManager {
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
    SessionEstablishmentRateLimiter& getSessionEstablishmentRateLimiter() {
        return _sessionEstablishmentRateLimiter;
    }

    // Stats

    /**
     * Total number of operations created on sessions belonging to this SessionManager.
     */
    std::size_t getTotalOperations() const {
        return _totalOperations.load();
    }

    /**
     * Number of operations on sessions belonging to this SessionManager
     * which have begun but not yet completed.
     */
    std::size_t getActiveOperations() const {
        return _totalOperations.load() - _completedOperations.load();
    }

    /**
     * Total number of completed operations on sessions belonging to this SessionManager.
     */
    std::size_t getCompletedOperations() const {
        return _completedOperations.load();
    }

    HelloMetrics helloMetrics;
    ServiceExecutorStats serviceExecutorStats;

protected:
    friend class Session;
    AtomicWord<std::size_t> _totalOperations{0};
    AtomicWord<std::size_t> _completedOperations{0};

    SessionEstablishmentRateLimiter _sessionEstablishmentRateLimiter;
};

}  // namespace transport
}  // namespace mongo
