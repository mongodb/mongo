// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/dbmessage.h"
#include "mongo/db/request_execution_context.h"
#include "mongo/util/modules.h"

namespace mongo {

/**
 * Legacy interface for processing client read/write/cmd requests.
 */
class [[MONGO_MOD_PARENT_PRIVATE]] Strategy {
public:
    /**
     * Executes a command from either OP_QUERY or OP_MSG wire protocols.
     *
     * Catches StaleConfig errors and retries the command automatically after refreshing the
     * metadata for the failing namespace.
     */
    static DbResponse clientCommand(RequestExecutionContext* rec);
};

}  // namespace mongo
