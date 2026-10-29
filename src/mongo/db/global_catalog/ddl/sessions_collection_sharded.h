// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <memory>
#include <vector>

namespace mongo {

class OperationContext;

/**
 * Accesses the sessions collection for mongos and shard servers.
 */
class [[MONGO_MOD_NEEDS_REPLACEMENT]] SessionsCollectionSharded : public SessionsCollection {
public:
    /**
     * Only ensures that the sessions collection exists, is sharded and has the proper indexes, but
     * doesn't do any configuration on its own. This is left to the config server's implementation
     * in SessionsCollectionConfigServer.
     */
    void setupSessionsCollection(OperationContext* opCtx) override;

    /**
     * Checks if the sessions collection exists. Does not check if the index exists in the sharded
     * version of this function.
     */
    void checkSessionsCollectionExists(OperationContext* opCtx) final;

    SessionsCollection::RefreshSessionsResult refreshSessions(
        OperationContext* opCtx, const LogicalSessionRecordSet& sessions) override;

    void removeRecords(OperationContext* opCtx, const LogicalSessionIdSet& sessions) override;

    LogicalSessionIdSet findRemovedSessions(OperationContext* opCtx,
                                            const LogicalSessionIdSet& sessions) override;

private:
    /**
     * These two methods use the sharding routing metadata to do a best effort attempt at grouping
     * the specified set of sessions by the shards, which have the records for these sessions. This
     * is done as an attempt to avoid broadcast queries.
     *
     * The reason it is 'best effort' is because it makes no attempt at checking whether the routing
     * table is up-to-date and just picks up whatever was most recently fetched from the config
     * server, which could be stale.
     */
    std::vector<LogicalSessionId> _groupSessionIdsByOwningShard(
        OperationContext* opCtx, const LogicalSessionIdSet& sessions);
    std::vector<LogicalSessionRecord> _groupSessionRecordsByOwningShard(
        OperationContext* opCtx, const LogicalSessionRecordSet& sessions);
};

}  // namespace mongo
