// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

namespace mongo {

class DBDirectClient;
class OperationContext;

/**
 * Accesses the sessions collection directly for standalone servers.
 */
class SessionsCollectionStandalone : public SessionsCollection {
public:
    /**
     * Ensures that the sessions collection exists and has the proper indexes.
     */
    void setupSessionsCollection(OperationContext* opCtx) override;

    /**
     * Checks if the sessions collection exists and has the proper indexes.
     */
    void checkSessionsCollectionExists(OperationContext* opCtx) override;

    /**
     * Updates the last-use times on the given sessions to be greater than
     * or equal to the current time.
     */
    RefreshSessionsResult refreshSessions(OperationContext* opCtx,
                                          const LogicalSessionRecordSet& sessions) override;

    /**
     * Removes the authoritative records for the specified sessions.
     */
    void removeRecords(OperationContext* opCtx, const LogicalSessionIdSet& sessions) override;

    LogicalSessionIdSet findRemovedSessions(OperationContext* opCtx,
                                            const LogicalSessionIdSet& sessions) override;
};

}  // namespace mongo
