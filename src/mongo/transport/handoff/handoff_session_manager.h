// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/transport/session_manager_common.h"

namespace mongo::transport {

class HandoffSessionManager : public SessionManagerCommon {
public:
    using SessionManagerCommon::SessionManagerCommon;

    bool shouldIncludeInConnectionsServerStatus() const override {
        return true;
    }

protected:
    std::string getClientThreadName(const Session&) const override;
    void configureServiceExecutorContext(Client&, bool isPrivilegedSession) const override;
    bool isPrivileged(const Session&) const override;
};

}  // namespace mongo::transport
