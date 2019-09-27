
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

#include <memory>

#include "mongo/db/logical_session_id.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/util/time_support.h"

namespace mongo {

class OperationContext;

/**
 * Accesses the sessions collection for mongos and shard servers.
 */
class SessionsCollectionSharded : public SessionsCollection {
public:
    /**
     * Ensures that the sessions collection exists, is sharded,
     * and has the proper indexes.
     */
    Status setupSessionsCollection(OperationContext* opCtx) override;

    /**
     * Checks if the sessions collection exists. Does not check if the index exists in the sharded
     * version of this function.
     */
    virtual Status checkSessionsCollectionExists(OperationContext* opCtx) override;

    /**
     * Updates the last-use times on the given sessions to be greater than
     * or equal to the current time.
     */
    Status refreshSessions(OperationContext* opCtx,
                           const LogicalSessionRecordSet& sessions) override;

    /**
     * Removes the authoritative records for the specified sessions.
     */
    Status removeRecords(OperationContext* opCtx, const LogicalSessionIdSet& sessions) override;

    StatusWith<LogicalSessionIdSet> findRemovedSessions(
        OperationContext* opCtx, const LogicalSessionIdSet& sessions) override;

protected:
    Status _checkCacheForSessionsCollection(OperationContext* opCtx);

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
