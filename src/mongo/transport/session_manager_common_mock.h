// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/transport/session.h"
#include "mongo/transport/session_manager.h"
#include "mongo/transport/session_manager_common.h"

#include <memory>

#include <fmt/format.h>

namespace mongo::transport {

class MockSessionManagerCommon : public SessionManagerCommon {
public:
    using SessionManagerCommon::onClientConnect;
    using SessionManagerCommon::onClientDisconnect;
    using SessionManagerCommon::SessionManagerCommon;

protected:
    std::string getClientThreadName(const Session& session) const override {
        return fmt::format("mock{}", session.id());
    }

    void configureServiceExecutorContext(Client& client, bool isPrivilegedSession) const override {
        auto seCtx = std::make_unique<ServiceExecutorContext>();
        seCtx->setThreadModel(ServiceExecutorContext::kSynchronous);
        seCtx->setCanUseReserved(isPrivilegedSession);
        std::lock_guard lk(client);
        ServiceExecutorContext::set(&client, std::move(seCtx));
    }
};

}  // namespace mongo::transport
