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

#include "mongo/base/counter.h"
#include "mongo/db/client.h"
#include "mongo/transport/client_transport_observer.h"
#include "mongo/transport/session_manager.h"
#include "mongo/util/net/cidr.h"

#include <string>
#include <variant>
#include <vector>

namespace mongo {
class ServiceContext;

namespace transport {

/**
 * Common implementation used by mongod and mongos servers.
 */
class SessionManagerCommon : public SessionManager {
public:
    struct SessionStats {
        int64_t numOpenSessions = 0;
        int64_t maxOpenSessions = 0;
        int64_t numCreatedSessions = 0;
        int64_t numRejectedSessions = 0;
        int64_t numActiveOperations = 0;
        int64_t numLoadBalancedSessions = 0;
        int64_t numPrioritySessions = 0;
    };

    explicit SessionManagerCommon(ServiceContext*);
    SessionManagerCommon(ServiceContext*, std::shared_ptr<ClientTransportObserver> observer);
    SessionManagerCommon(ServiceContext* svcCtx,
                         std::vector<std::shared_ptr<ClientTransportObserver>> observers);
    ~SessionManagerCommon() override;

    void startSession(std::shared_ptr<Session> session) override;
    void endAllSessions(Client::TagMask tags) override;
    void endSessionByClient(Client* client) override;
    void endAllSessionsNoTagMask();

    bool shutdown(Milliseconds timeout) override;
    bool shutdownAndWait(Milliseconds timeout);
    bool waitForNoSessions(Milliseconds timeout);

    std::size_t numOpenSessions() const override;
    std::size_t maxOpenSessions() const override {
        return _maxOpenSessions;
    }

    std::vector<std::pair<SessionId, std::string>> getOpenSessionIDs() const override;

    virtual SessionStats getSessionStats() const;

    void incrementLoadBalancedSessions();
    void decrementLoadBalancedSessions();

    void incrementPrioritySessions();
    void decrementPrioritySessions();

    /**
     * Returns true if this manager's session counts should be included in the "connections"
     * serverStatus section. Defaults to false. Opt in by overriding in subclasses.
     */
    virtual bool shouldIncludeInConnectionsServerStatus() const {
        return false;
    }

protected:
    /** Generate a unique thread name for this session. */
    virtual std::string getClientThreadName(const Session&) const = 0;

    /**
     * Returns whether the specified session ought to be exempt from all of the following:
     * - the maximum number of open sessions
     * - the connection establishment rate limiter
     * - the ingress request rate limiter
     */
    virtual bool isPrivileged(const Session&) const;

    /**
     * Imbues the new Client with a ServiceExecutorContext. `isPrivilegedSession` was obtained from
     * a previous call to `isPrivileged` with the session from which the Client was made.
     */
    virtual void configureServiceExecutorContext(Client& client,
                                                 bool isPrivilegedSession) const = 0;

    /** Called upon client connection. */
    virtual void onClientConnect(Client* client);

    /** Called upon client disconnection. */
    virtual void onClientDisconnect(Client* client);

    /** Total number of sessions created. */
    std::size_t numCreatedSessions() const;

    /** Total number of sessions rejected. */
    std::size_t numRejectedSessions() const;

    // We assume that this SessionManager instance will be owned by a ServiceContext
    // permanently until such time as the ServiceContext is destroyed at which point
    // the SessionManager is destroyed as well, so there is no lingering pointer.
    ServiceContext* _svcCtx;

    const std::size_t _maxOpenSessions;

    Counter64 _loadBalancedSessions;
    Counter64 _prioritySessions;

    class Sessions;
    std::unique_ptr<Sessions> _sessions;

    // External observer which may receive client connect/disconnect events.
    std::vector<std::shared_ptr<ClientTransportObserver>> _observers;
};

/**
 * Returns true if a session with remote/local addresses is part of the exemption list.
 */
bool isExemptedByCIDRList(const std::shared_ptr<Session>& session, const CIDRList& exemptions);

}  // namespace transport
}  // namespace mongo
