// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/operation_context.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

namespace [[MONGO_MOD_PUBLIC]] mongo {

class SessionsCollection;

class RouterSessionCatalog {
public:
    /**
     * Locates session entries from the in-memory catalog  which have not been referenced before
     * 'possiblyExpired' and deletes them.
     *
     * Returns the number of sessions, which were reaped from the in-memory catalog.
     */
    static int reapSessionsOlderThan(OperationContext* opCtx,
                                     SessionsCollection& sessionsCollection,
                                     Date_t possiblyExpired);
};

/**
 * Scoped object, which checks out the session specified in the passed operation context and stores
 * it for later access by the command. The session is installed at construction time and is removed
 * at destruction. This can only be used for multi-statement transactions.
 */
class RouterOperationContextSession {
    RouterOperationContextSession(const RouterOperationContextSession&) = delete;
    RouterOperationContextSession& operator=(const RouterOperationContextSession&) = delete;

public:
    RouterOperationContextSession(OperationContext* opCtx);
    ~RouterOperationContextSession();

    /**
     * These methods take an operation context with a checked-out session and allow it to be
     * temporarily or permanently checked back in, in order to allow other operations to use it.
     *
     * Check-in may only be called if the session has actually been checked out previously and
     * similarly check-out may only be called if the session is not checked out already.
     */
    static void checkIn(OperationContext* opCtx, OperationContextSession::CheckInReason reason);
    static void checkOut(OperationContext* opCtx);

private:
    OperationContext* _opCtx;
    OperationContextSession _operationContextSession;
};

}  // namespace mongo
