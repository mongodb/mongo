// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/service_context.h"
#include "mongo/transport/session_manager_common.h"
#include "mongo/util/modules.h"

#include <cstdint>

namespace mongo::transport {

struct [[MONGO_MOD_PUBLIC]] ConnectionsStatsSnapshot {
    int64_t current{0};
    int64_t available{0};
    int64_t totalCreated{0};
    int64_t rejected{0};
    int64_t active{0};
};

/**
 * Finds the AsioSessionManager on `svcCtx` and returns a snapshot of its
 * current connection counts, matching the fields reported by serverStatus.connections.
 */
[[MONGO_MOD_PUBLIC]] ConnectionsStatsSnapshot collectConnectionsStatsSnapshot(
    ServiceContext* svcCtx);

/**
 * ASIO specialization of SessionManagerCommon.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] AsioSessionManager : public SessionManagerCommon {
public:
    using SessionManagerCommon::SessionManagerCommon;

    void startSession(std::shared_ptr<Session> session) override;

    ConnectionsStatsSnapshot getConnectionsStatsSnapshot() const;

    bool shouldIncludeInConnectionsServerStatus() const override {
        return true;
    }

protected:
    std::string getClientThreadName(const Session&) const override;
    void configureServiceExecutorContext(Client& client, bool isPrivilegedSession) const override;
};

}  // namespace mongo::transport
