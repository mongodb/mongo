// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/client.h"
#include "mongo/db/operation_id.h"
#include "mongo/db/service_context.h"
#include "mongo/util/modules.h"

[[MONGO_MOD_PUBLIC]];

namespace mongo {

/**
 * OperationKiller is a utility class to facilitate interrupting other operations
 *
 * This class is constructed with a pointer to the Client which is attempting to perform the
 * interruption. This Client must have a role with the killop action type or own the targeted
 * OperationContext. The Client must also be a member of the same ServiceContext as the targeted
 * OperationContext.
 * This class should only be used for the killOp command.
 */
class OperationKiller {
public:
    explicit OperationKiller(Client* myClient);

    /**
     * Verify that myClient has permission to kill operations it does not own
     */
    bool isGenerallyAuthorizedToKill() const;

    /**
     * Verify that the target exists and the myClient is allowed to kill it
     */
    bool isAuthorizedToKill(const ClientLock& target) const;

    /**
     * Kill an operation running on this instance of mongod or mongos.
     * This will ignore kill-exempt operations.
     */
    void killOperation(OperationId opId, ErrorCodes::Error killCode = ErrorCodes::Interrupted);
    void killOperation(const OperationKey& opKey,
                       ErrorCodes::Error killCode = ErrorCodes::Interrupted);

private:
    Client* const _myClient;
};

}  // namespace mongo
