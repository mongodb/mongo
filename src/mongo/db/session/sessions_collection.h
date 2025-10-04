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

#include "mongo/base/string_data.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/util/modules.h"

#include <functional>
#include <vector>

namespace mongo {

class DBClientBase;
class OperationContext;

/**
 * An abstract interface describing the entrypoint into the sessions collection.
 *
 * Different server deployments (standalone, replica set, sharded cluster) should
 * implement their own classes that fulfill this interface.
 */
class MONGO_MOD_OPEN SessionsCollection {
public:
    static constexpr StringData kSessionsTTLIndex = "lsidTTLIndex"_sd;

    virtual ~SessionsCollection();

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
    virtual void refreshSessions(OperationContext* opCtx,
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

protected:
    SessionsCollection();

    using SendBatchFn = std::function<void(BSONObj batch)>;
    static SendBatchFn makeSendFnForCommand(const NamespaceString& ns, DBClientBase* client);
    static SendBatchFn makeSendFnForBatchWrite(const NamespaceString& ns, DBClientBase* client);

    using FindBatchFn = std::function<BSONObj(BSONObj batch)>;
    static FindBatchFn makeFindFnForCommand(const NamespaceString& ns, DBClientBase* client);

    /**
     * Formats and sends batches of refreshes for the given set of sessions.
     */
    void _doRefresh(const NamespaceString& ns,
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
