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

#include <boost/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/db/commands/end_sessions_gen.h"
#include "mongo/db/logical_session_cache_stats_gen.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/refresh_sessions_gen.h"

namespace mongo {

class Client;
class OperationContext;
class ServiceContext;

/**
 * The interface for the logical session cache
 */
class LogicalSessionCache {
public:
    /**
     * Decorate the ServiceContext with a LogicalSessionCache instance.
     */
    static LogicalSessionCache* get(ServiceContext* service);
    static LogicalSessionCache* get(OperationContext* opCtx);
    static void set(ServiceContext* service, std::unique_ptr<LogicalSessionCache> sessionCache);

    virtual ~LogicalSessionCache() = 0;

    /**
     * If the cache contains a record for this LogicalSessionId, promotes that lsid
     * to be the most recently used and updates its lastUse date to be the current
     * time. Returns an error if the session was not found.
     */
    virtual Status promote(LogicalSessionId lsid) = 0;

    /**
     * Inserts a new authoritative session record into the cache. This method will
     * insert the authoritative record into the sessions collection. This method
     * should only be used when starting new sessions and should not be used to
     * insert records for existing sessions.
     */
    virtual void startSession(OperationContext* opCtx, LogicalSessionRecord record) = 0;

    /**
     * Refresh the given sessions. Updates the timestamps of these records in
     * the local cache.
     */
    virtual Status refreshSessions(OperationContext* opCtx,
                                   const RefreshSessionsCmdFromClient& cmd) = 0;
    virtual Status refreshSessions(OperationContext* opCtx,
                                   const RefreshSessionsCmdFromClusterMember& cmd) = 0;

    /**
     * Vivifies the session in the cache. I.e. creates it if it isn't there, updates last use if it
     * is.
     */
    virtual void vivify(OperationContext* opCtx, const LogicalSessionId& lsid) = 0;

    /**
     * enqueues LogicalSessionIds for removal during the next _refresh()
     */
    virtual void endSessions(const LogicalSessionIdSet& lsids) = 0;

    /**
     * Refreshes the cache synchronously. This flushes all pending refreshes and
     * inserts to the sessions collection.
     */
    virtual Status refreshNow(Client* client) = 0;

    /**
     * Reaps transaction records synchronously.
     */
    virtual Status reapNow(Client* client) = 0;

    /**
     * Returns the current time.
     */
    virtual Date_t now() = 0;

    /**
     * Returns the number of session records currently in the cache.
     */
    virtual size_t size() = 0;

    /**
     * Ennumerate all LogicalSessionId keys currently in the cache.
     */
    virtual std::vector<LogicalSessionId> listIds() const = 0;

    /**
     * Ennumerate all LogicalSessionId keys in the cache for the given UserDigests.
     */
    virtual std::vector<LogicalSessionId> listIds(
        const std::vector<SHA256Block>& userDigest) const = 0;

    /**
     * Retrieve a LogicalSessionRecord by LogicalSessionId, if it exists in the cache.
     */
    virtual boost::optional<LogicalSessionRecord> peekCached(const LogicalSessionId& id) const = 0;

    /**
     * Returns stats about the logical session cache and its recent operations.
     */
    virtual LogicalSessionCacheStats getStats() = 0;
};

}  // namespace mongo
