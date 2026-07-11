// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/base/status.h"
#include "mongo/crypto/sha256_block.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_cache_gen.h"
#include "mongo/db/session/logical_session_cache_stats_gen.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_gen.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/util/modules.h"

#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace [[MONGO_MOD_PUBLIC]] mongo {

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
    virtual Status refreshNow(OperationContext* opCtx) = 0;

    /**
     * Reaps transaction records synchronously.
     */
    virtual void reapNow(OperationContext* opCtx) = 0;

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
        : _alternateClient(
              opCtx->getServiceContext()->getService()->makeClient("alternative-session-region")),
          _acr(_alternateClient),
          _newOpCtx(cc().makeOperationContext()),
          _lsid(makeLogicalSessionId(opCtx)) {
        auto lk = std::lock_guard(*_newOpCtx->getClient());
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
