/**
 *    Copyright (C) 2017 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include "mongo/db/logical_session_id.h"
#include "mongo/stdx/functional.h"

namespace mongo {

class BSONArrayBuilder;
class BSONObjBuilder;
class DBClientBase;
class OperationContext;

/**
 * An abstract interface describing the entrypoint into the sessions collection.
 *
 * Different server deployments (standalone, replica set, sharded cluster) should
 * implement their own classes that fulfill this interface.
 */
class SessionsCollection {

public:
    virtual ~SessionsCollection();

    static constexpr StringData kSessionsDb = "admin"_sd;
    static constexpr StringData kSessionsCollection = "system.sessions"_sd;
    static constexpr StringData kSessionsFullNS = "admin.system.sessions"_sd;

    /**
     * Updates the last-use times on the given sessions to be greater than
     * or equal to the given time. Returns an error if a networking issue occurred.
     */
    virtual Status refreshSessions(OperationContext* opCtx,
                                   const LogicalSessionRecordSet& sessions,
                                   Date_t refreshTime) = 0;

    /**
     * Removes the authoritative records for the specified sessions.
     *
     * Implementations should perform authentication checks to ensure that
     * session records may only be removed if their owner is logged in.
     *
     * Returns an error if the removal fails, for example from a network error.
     */
    virtual Status removeRecords(OperationContext* opCtx, const LogicalSessionIdSet& sessions) = 0;

protected:
    /**
     * Makes a send function for the given client.
     */
    using SendBatchFn = stdx::function<Status(BSONObj batch)>;
    SendBatchFn makeSendFnForCommand(DBClientBase* client);
    SendBatchFn makeSendFnForBatchWrite(DBClientBase* client);

    /**
     * Formats and sends batches of refreshes for the given set of sessions.
     */
    Status doRefresh(const LogicalSessionRecordSet& sessions, Date_t refreshTime, SendBatchFn send);
    Status doRefreshExternal(const LogicalSessionRecordSet& sessions,
                             Date_t refreshTime,
                             SendBatchFn send);

    /**
     * Formats and sends batches of deletes for the given set of sessions.
     */
    Status doRemove(const LogicalSessionIdSet& sessions, SendBatchFn send);
    Status doRemoveExternal(const LogicalSessionIdSet& sessions, SendBatchFn send);
};

}  // namespace mongo
