// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/session/logical_session_cache.h"
#include "mongo/db/session/logical_session_cache_stats_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/service_liaison.h"
#include "mongo/db/session/sessions_collection.h"
#include "mongo/util/concurrency/with_lock.h"
#include "mongo/util/functional.h"
#include "mongo/util/modules.h"
#include "mongo/util/time_support.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include <boost/move/utility_core.hpp>
#include <boost/optional/optional.hpp>

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
class [[MONGO_MOD_NEEDS_REPLACEMENT]] LogicalSessionCacheImpl final : public LogicalSessionCache {
public:
    using ReapSessionsOlderThanFn =
        unique_function<int(OperationContext*, SessionsCollection&, Date_t)>;

    LogicalSessionCacheImpl(std::unique_ptr<ServiceLiaison> service,
                            std::shared_ptr<SessionsCollection> collection,
                            ReapSessionsOlderThanFn reapSessionsOlderThanFn);

    LogicalSessionCacheImpl(const LogicalSessionCacheImpl&) = delete;
    LogicalSessionCacheImpl& operator=(const LogicalSessionCacheImpl&) = delete;

    ~LogicalSessionCacheImpl() override;

    void joinOnShutDown() override;

    Status startSession(OperationContext* opCtx, const LogicalSessionRecord& record) override;

    Status vivify(OperationContext* opCtx, const LogicalSessionId& lsid) override;

    Status refreshNow(OperationContext* opCtx) override;

    void reapNow(OperationContext* opCtx) override;

    size_t size() override;

    std::vector<LogicalSessionId> listIds() const override;

    std::vector<LogicalSessionId> listIds(
        const std::vector<SHA256Block>& userDigest) const override;

    boost::optional<LogicalSessionRecord> peekCached(const LogicalSessionId& id) const override;

    void endSessions(const LogicalSessionIdSet& sessions) override;

    LogicalSessionCacheStats getStats() override;

private:
    void _periodicRefresh(Client* client);
    Status _refresh(Client* client);

    void _periodicReap(Client* client);
    Status _reap(Client* client);

    /**
     * Returns true if a record has passed its given expiration.
     */
    bool _isDead(const LogicalSessionRecord& record, Date_t now) const;

    Status _addToCacheIfNotFull(WithLock, LogicalSessionRecord record);

    const std::unique_ptr<ServiceLiaison> _service;
    const std::shared_ptr<SessionsCollection> _sessionsColl;
    const ReapSessionsOlderThanFn _reapSessionsOlderThanFn;

    // Mutex to protect the stats object and _activeSessions swap operations.
    mutable std::mutex _mutex;
    // Mutex to ensure that only one _refresh operation runs at a time.
    mutable std::mutex _refreshMutex;

    LogicalSessionIdMap<LogicalSessionRecord> _activeSessions;

    LogicalSessionIdSet _endingSessions;

    Date_t _lastRefreshTime;

    LogicalSessionCacheStats _stats;
};

}  // namespace mongo
