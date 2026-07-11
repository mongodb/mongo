// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/session_manager.h"

namespace mongo::transport {

/**
 * A no-op session manager available for use with testing.
 */
class SessionManagerNoop : public SessionManager {
public:
    void startSession(std::shared_ptr<transport::Session> session) override {}
    void endAllSessions(Client::TagMask tags) override {}
    void endSessionByClient(Client* client) override {}
    bool shutdown(Milliseconds timeout) override {
        return true;
    }
    std::size_t numOpenSessions() const override {
        return 0;
    }
    std::size_t maxOpenSessions() const override {
        return 0;
    }
    std::vector<std::pair<SessionId, std::string>> getOpenSessionIDs() const override {
        return {};
    }
    void onLoadBalancerPeerSet(bool) override {}
};
}  // namespace mongo::transport
