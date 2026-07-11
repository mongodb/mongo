// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"

#include <functional>
#include <string_view>
#include <vector>

namespace mongo {
using namespace std::literals::string_view_literals;

class DBClientBase;
class OperationContext;

/**
 * An abstract interface describing the entrypoint into the sessions collection.
 *
 * Different server deployments (standalone, replica set, sharded cluster) should
 * implement their own classes that fulfill this interface.
 */
class [[MONGO_MOD_OPEN]] SessionsCollection {
public:
    static constexpr std::string_view kSessionsTTLIndex = "lsidTTLIndex"sv;

    virtual ~SessionsCollection();

    struct RefreshSessionsResult {
        LogicalSessionRecordSet failedSessions;
        std::vector<Status> errors;  // One per batch that had issues

        bool hasErrors() const {
            return !errors.empty();
        }
    };

    /**
     * Ensures that the sessions collection exists and has the proper indexes. Implementations of
     * this method must support multiple concurrent invocations.
     */
    virtual void setupSessionsCollection(OperationContext* opCtx) = 0;

    /**
     * Checks if the sessions collection exists and has the proper indexes.
     */
    virtual void checkSessionsCollectionExists(OperationContext* opCtx) = 0;

    /**
     * Updates the last-use times on the given sessions to be greater than or equal to the given
     * time. Throws an exception if a networking issue occurred.
     */
    virtual RefreshSessionsResult refreshSessions(OperationContext* opCtx,
                                                  const LogicalSessionRecordSet& sessions) = 0;

    /**
     * Removes the authoritative records for the specified sessions.
     *
     * Implementations should perform authentication checks to ensure that session records may only
     * be removed if their owner is logged in.
     *
     * Throws an exception if the removal fails, for example from a network error.
     */
    virtual void removeRecords(OperationContext* opCtx, const LogicalSessionIdSet& sessions) = 0;

    /**
     * Checks a set of lsids and returns the set that no longer exists.
     *
     * Throws an exception if the fetch cannot occur, for example from a network error.
     */
    virtual LogicalSessionIdSet findRemovedSessions(OperationContext* opCtx,
                                                    const LogicalSessionIdSet& sessions) = 0;

    /**
     * Generates a createIndexes command for the sessions collection TTL index.
     */
    static BSONObj generateCreateIndexesCmd();

    /*
     * Generates a collMod command for the sessions collection TTL index.
     */
    static BSONObj generateCollModCmd();

    /**
     * Wraps a batch-send function to stamp maxTimeMS at 90% of the logical session refresh interval
     * on every outgoing command.  Only applied when logicalSessionCacheJobTimeoutEnabled is
     * enabled, preventing a single slow reap/refresh batch from blocking the next job cycle.
     */
    static std::function<Status(BSONObj)> withRefreshTimeout(std::function<Status(BSONObj)> fn);

protected:
    SessionsCollection();

    using SendBatchFn = std::function<Status(BSONObj batch)>;
    static SendBatchFn makeSendFnForCommand(const NamespaceString& ns, DBClientBase* client);
    static SendBatchFn makeSendFnForBatchWrite(const NamespaceString& ns, DBClientBase* client);

    using FindBatchFn = std::function<BSONObj(BSONObj batch)>;
    static FindBatchFn makeFindFnForCommand(const NamespaceString& ns, DBClientBase* client);

    /**
     * Formats and sends batches of refreshes for the given set of sessions.
     */
    RefreshSessionsResult _doRefresh(const NamespaceString& ns,
                                     const std::vector<LogicalSessionRecord>& sessions,
                                     SendBatchFn send);

    /**
     * Formats and sends batches of deletes for the given set of sessions.
     */
    void _doRemove(const NamespaceString& ns,
                   const std::vector<LogicalSessionId>& sessions,
                   SendBatchFn send);

    /**
     * Returns those lsids from the input 'sessions' array which are not present in the sessions
     * collection. Note that a parent session and its child sessions are tracked as one session in
     * the sessions collection.
     */
    LogicalSessionIdSet _doFindRemoved(const NamespaceString& ns,
                                       const std::vector<LogicalSessionId>& sessions,
                                       FindBatchFn send);
};

}  // namespace mongo
