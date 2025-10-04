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

#include "mongo/db/operation_context.h"
#include "mongo/db/session/session_catalog.h"
#include "mongo/util/time_support.h"

namespace mongo {

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
