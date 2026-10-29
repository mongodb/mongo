// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/transport/handoff/handoff_session_manager.h"

#include "mongo/transport/service_executor.h"
#include "mongo/transport/session.h"

#include <fmt/format.h>

namespace mongo::transport {

std::string HandoffSessionManager::getClientThreadName(const Session& session) const {
    return fmt::format("conn{}", session.id());
}

void HandoffSessionManager::configureServiceExecutorContext(Client& client,
                                                            bool isPrivilegedSession) const {
    auto seCtx = std::make_unique<ServiceExecutorContext>();
    seCtx->setThreadModel(ServiceExecutorContext::kSynchronous);
    seCtx->setCanUseReserved(isPrivilegedSession);
    std::lock_guard lk(client);
    ServiceExecutorContext::set(&client, std::move(seCtx));
}

bool HandoffSessionManager::isPrivileged(const Session& session) const {
    return session.isConnectedToPriorityPort();
}

}  // namespace mongo::transport
