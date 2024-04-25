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

#include "mongo/transport/session_manager.h"

#include <string>
#include <variant>
#include <vector>

#include "mongo/db/client.h"
#include "mongo/transport/client_transport_observer.h"
#include "mongo/util/net/cidr.h"

namespace mongo {
class ServiceContext;

namespace transport {

/**
 * Common implementation used by mongod and mongos servers.
 */
class SessionManagerCommon : public SessionManager {
public:
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

protected:
    /** Generate a unique thread name for this session. */
    virtual std::string getClientThreadName(const Session&) const = 0;

    /** Imbue the new Client with a ServiceExecutorContext. */
    virtual void configureServiceExecutorContext(Client* client,
                                                 bool isPrivilegedSession) const = 0;

    /** Called upon client connection. Default behavior is to do nothing. */
    virtual void onClientConnect(Client* client) {}

    /** Called upon client disconnection. Default behavior is to do nothing. */
    virtual void onClientDisconnect(Client* client) {}

    /** Total number of sessions created. */
    std::size_t numCreatedSessions() const;

    /** Total number of sessions rejected. */
    std::size_t numRejectedSessions() const;

    // We assume that this SessionManager instance will be owned by a ServiceContext
    // permanently until such time as the ServiceContext is destroyed at which point
    // the SessionManager is destroyed as well, so there is no lingering pointer.
    ServiceContext* _svcCtx;

    const std::size_t _maxOpenSessions;

    class Sessions;
    std::unique_ptr<Sessions> _sessions;

    // External observer which may receive client connect/disconnect events.
    std::vector<std::shared_ptr<ClientTransportObserver>> _observers;
};

/**
 * Returns true if a session with remote/local addresses should be exempted from maxConns.
 */
bool shouldOverrideMaxConns(const std::shared_ptr<Session>& session,
                            const std::vector<std::variant<CIDR, std::string>>& exemptions);

}  // namespace transport
}  // namespace mongo
