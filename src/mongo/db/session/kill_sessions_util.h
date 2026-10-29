// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/util/modules.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

namespace kill_sessions {

/**
 * Validation callback to check if a given lsid has an associated parent session.
 */
inline Status validateLsid(const LogicalSessionId& lsid) {
    if (isChildSession(lsid)) {
        return {ErrorCodes::InvalidOptions, "Cannot kill a child session"};
    }
    return Status::OK();
}

}  // namespace kill_sessions

}  // namespace mongo
