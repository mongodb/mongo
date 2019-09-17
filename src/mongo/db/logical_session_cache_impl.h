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

#include "mongo/db/logical_session_cache.h"
#include "mongo/db/service_liaison.h"
#include "mongo/db/sessions_collection.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/functional.h"

namespace mongo {

/**
 * A thread-safe cache structure for logical session records.
 *
 * The cache takes ownership of the passed-in ServiceLiaison and SessionsCollection helper types.
 *
 * Uses the following service-wide parameters:
 *  - A timeout value to use for sessions in the cache, in minutes. Defaults to 30 minutes.
 *      --setParameter localLogicalSessionTimeoutMinutes=X
 *
 *  - The interval over which the cache will refresh session records. By default, this is set to
 *    every 5 minutes (300,000). If the caller is setting the sessionTimeout by hand, it is
 *    suggested that they consider also setting the refresh interval accordingly.
 *      --setParameter logicalSessionRefreshMillis=X.
 */
class LogicalSessionCacheImpl final : public LogicalSessionCache {
public:
    using ReapSessionsOlderThanFn =
        unique_function<int(OperationContext*, SessionsCollection&, Date_t)>;

    LogicalSessionCacheImpl(std::unique_ptr<ServiceLiaison> service,
                            std::shared_ptr<SessionsCollection> collection,
                            ReapSessionsOlderThanFn reapSessionsOlderThanFn);

    LogicalSessionCacheImpl(const LogicalSessionCacheImpl&) = delete;
    LogicalSessionCacheImpl& operator=(const LogicalSessionCacheImpl&) = delete;

    ~LogicalSessionCacheImpl();

    void joinOnShutDown() override;

    Status startSession(OperationContext* opCtx, const LogicalSessionRecord& record) override;

    Status vivify(OperationContext* opCtx, const LogicalSessionId& lsid) override;

    Status refreshNow(Client* client) override;

    Status reapNow(Client* client) override;

    size_t size() override;

    std::vector<LogicalSessionId> listIds() const override;

    std::vector<LogicalSessionId> listIds(
        const std::vector<SHA256Block>& userDigest) const override;

    boost::optional<LogicalSessionRecord> peekCached(const LogicalSessionId& id) const override;

    void endSessions(const LogicalSessionIdSet& sessions) override;

    LogicalSessionCacheStats getStats() override;

private:
    void _periodicRefresh(Client* client);
    void _refresh(Client* client);

    void _periodicReap(Client* client);
    Status _reap(Client* client);

    /**
     * Returns true if a record has passed its given expiration.
     */
    bool _isDead(const LogicalSessionRecord& record, Date_t now) const;

    /**
     *
     */
    Status _addToCache(WithLock, LogicalSessionRecord record);

    const std::unique_ptr<ServiceLiaison> _service;
    const std::shared_ptr<SessionsCollection> _sessionsColl;
    const ReapSessionsOlderThanFn _reapSessionsOlderThanFn;

    mutable Mutex _mutex = MONGO_MAKE_LATCH("LogicalSessionCacheImpl::_mutex");

    LogicalSessionIdMap<LogicalSessionRecord> _activeSessions;

    LogicalSessionIdSet _endingSessions;

    Date_t _lastRefreshTime;

    LogicalSessionCacheStats _stats;
};

}  // namespace mongo
