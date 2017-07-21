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

namespace mongo {

/**
 * An abstract interface describing the entrypoint into the sessions collection.
 *
 * Different server deployments (standalone, replica set, sharded cluster) should
 * implement their own class that fulfill this interface.
 */
class SessionsCollection {
public:
    virtual ~SessionsCollection();

    /**
     * Returns a LogicalSessionRecord for the given session id. This method
     * may run networking operations on the calling thread.
     */
    virtual StatusWith<LogicalSessionRecord> fetchRecord(LogicalSessionId id) = 0;

    /**
     * Inserts the given record into the sessions collection. This method may run
     * networking operations on the calling thread.
     *
     * Returns a DuplicateSession error if the session already exists in the
     * sessions collection.
     */
    virtual Status insertRecord(LogicalSessionRecord record) = 0;

    /**
     * Updates the last-use times on the given sessions to be greater than
     * or equal to the current time.
     *
     * Returns a list of sessions for which no authoritative record was found,
     * and hence were not refreshed.
     */
    virtual LogicalSessionIdSet refreshSessions(LogicalSessionIdSet sessions) = 0;

    /**
     * Removes the authoritative records for the specified sessions.
     *
     * Implementations should perform authentication checks to ensure that
     * session records may only be removed if their owner is logged in.
     */
    virtual void removeRecords(LogicalSessionIdSet sessions) = 0;
};

}  // namespace mongo
