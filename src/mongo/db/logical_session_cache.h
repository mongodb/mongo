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

#include <boost/optional.hpp>

#include "mongo/base/status.h"
#include "mongo/db/client.h"
#include "mongo/db/logical_session_cache_gen.h"
#include "mongo/db/logical_session_cache_stats_gen.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context.h"

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
     * Invoked on service shutdown time in order to join the cache's refresher and reaper tasks.
     */
    virtual void joinOnShutDown() = 0;

    /**
     * Inserts a new authoritative session record into the cache.
     *
     * This method will insert the authoritative record into the sessions collection and should only
     * be used when starting new sessions. It should not be used to insert records for existing
     * sessions.
     */
    virtual Status startSession(OperationContext* opCtx, const LogicalSessionRecord& record) = 0;

    /**
     * Vivifies the session in the cache. I.e. creates it if it isn't there, updates last use if it
     * is.
     */
    virtual Status vivify(OperationContext* opCtx, const LogicalSessionId& lsid) = 0;

    /**
     * enqueues LogicalSessionIds for removal during the next _refresh()
     */
    virtual void endSessions(const LogicalSessionIdSet& lsids) = 0;

    /**
     * Refreshes the cache synchronously. This flushes all pending refreshes and inserts to the
     * sessions collection.
     */
    virtual Status refreshNow(Client* client) = 0;

    /**
     * Reaps transaction records synchronously.
     */
    virtual Status reapNow(Client* client) = 0;

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

/**
 * WARNING: This class should only be used for rare operations because it generates a new logical
 * session ID that isn't reaped until the next refresh, which could overwhelm memory if called in a
 * loop.
 */
class AlternativeSessionRegion {
public:
    AlternativeSessionRegion(OperationContext* opCtx)
        : _alternateClient(opCtx->getServiceContext()->makeClient("alternative-session-region")),
          _acr(_alternateClient),
          _newOpCtx(cc().makeOperationContext()),
          _lsid(makeLogicalSessionId(opCtx)) {
        _newOpCtx->setLogicalSessionId(_lsid);
    }

    ~AlternativeSessionRegion() {
        LogicalSessionCache::get(opCtx())->endSessions({_lsid});
    }

    OperationContext* opCtx() {
        return &*_newOpCtx;
    }

private:
    ServiceContext::UniqueClient _alternateClient;
    AlternativeClientRegion _acr;
    ServiceContext::UniqueOperationContext _newOpCtx;
    LogicalSessionId _lsid;
};

}  // namespace mongo
