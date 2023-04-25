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


#include "mongo/platform/basic.h"

#include "mongo/db/session/logical_session_cache_impl.h"

#include "mongo/db/operation_context.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_severity_suppressor.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/duration.h"
#include "mongo/util/scopeguard.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

namespace {

void clearShardingOperationFailedStatus(OperationContext* opCtx) {
    // We do not intend to immediately act upon sharding errors if we receive them during sessions
    // collection operations. We will instead attempt the same operations during the next refresh
    // cycle.
    OperationShardingState::get(opCtx).resetShardingOperationFailedStatus();
}

}  // namespace

LogicalSessionCacheImpl::LogicalSessionCacheImpl(std::unique_ptr<ServiceLiaison> service,
                                                 std::shared_ptr<SessionsCollection> collection,
                                                 ReapSessionsOlderThanFn reapSessionsOlderThanFn)
    : _service(std::move(service)),
      _sessionsColl(std::move(collection)),
      _reapSessionsOlderThanFn(std::move(reapSessionsOlderThanFn)) {
    _stats.setLastSessionsCollectionJobTimestamp(_service->now());
    _stats.setLastTransactionReaperJobTimestamp(_service->now());

    if (!disableLogicalSessionCacheRefresh) {
        _service->scheduleJob({"LogicalSessionCacheRefresh",
                               [this](Client* client) { _periodicRefresh(client); },
                               Milliseconds(logicalSessionRefreshMillis)});

        _service->scheduleJob({"LogicalSessionCacheReap",
                               [this](Client* client) { _periodicReap(client); },
                               Milliseconds(logicalSessionRefreshMillis)});
    }
}

LogicalSessionCacheImpl::~LogicalSessionCacheImpl() {
    joinOnShutDown();
}

void LogicalSessionCacheImpl::joinOnShutDown() {
    _service->join();
}

Status LogicalSessionCacheImpl::startSession(OperationContext* opCtx,
                                             const LogicalSessionRecord& record) {
    stdx::lock_guard lg(_mutex);
    return _addToCacheIfNotFull(lg, record);
}

Status LogicalSessionCacheImpl::vivify(OperationContext* opCtx, const LogicalSessionId& lsid) {
    auto parentLsid = getParentSessionId(lsid);

    stdx::lock_guard lg(_mutex);

    auto it = _activeSessions.find(parentLsid ? *parentLsid : lsid);
    if (it == _activeSessions.end())
        return _addToCacheIfNotFull(lg, makeLogicalSessionRecord(opCtx, lsid, _service->now()));

    auto& cacheEntry = it->second;
    cacheEntry.setLastUse(_service->now());

    return Status::OK();
}

Status LogicalSessionCacheImpl::refreshNow(OperationContext* opCtx) {
    try {
        _refresh(opCtx->getClient());
    } catch (const DBException& ex) {
        LOGV2(
            20714,
            "Failed to refresh session cache, will try again at the next refresh interval {error}",
            "Failed to refresh session cache, will try again at the next refresh interval",
            "error"_attr = redact(ex));
        return exceptionToStatus();
    } catch (...) {
        return exceptionToStatus();
    }
    return Status::OK();
}

void LogicalSessionCacheImpl::reapNow(OperationContext* opCtx) {
    uassertStatusOK(_reap(opCtx->getClient()));
}

size_t LogicalSessionCacheImpl::size() {
    stdx::lock_guard<Latch> lock(_mutex);
    return _activeSessions.size();
}

void LogicalSessionCacheImpl::_periodicRefresh(Client* client) {
    try {
        _refresh(client);
    } catch (const DBException& ex) {
        LOGV2(
            20710,
            "Failed to refresh session cache: {error}, will try again at the next refresh interval",
            "Failed to refresh session cache, will try again at the next refresh interval",
            "error"_attr = redact(ex));
    }
}

void LogicalSessionCacheImpl::_periodicReap(Client* client) {
    auto res = _reap(client);
    if (!res.isOK()) {
        LOGV2(20711,
              "Failed to reap transaction table: {error}",
              "Failed to reap transaction table",
              "error"_attr = redact(res));
    }

    return;
}

Status LogicalSessionCacheImpl::_reap(Client* client) {
    boost::optional<ServiceContext::UniqueOperationContext> uniqueCtx;
    auto* const opCtx = [&] {
        if (client->getOperationContext()) {
            return client->getOperationContext();
        }

        uniqueCtx.emplace(client->makeOperationContext());
        return uniqueCtx->get();
    }();

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord && replCoord->isReplEnabled() && replCoord->getMemberState().arbiter()) {
        return Status::OK();
    }

    // Take the lock to update some stats.
    {
        stdx::lock_guard<Latch> lk(_mutex);

        // Clear the last set of stats for our new run.
        _stats.setLastTransactionReaperJobDurationMillis(0);
        _stats.setLastTransactionReaperJobEntriesCleanedUp(0);

        // Start the new run.
        _stats.setLastTransactionReaperJobTimestamp(_service->now());
        _stats.setTransactionReaperJobCount(_stats.getTransactionReaperJobCount() + 1);
    }

    int numReaped = 0;

    try {
        ON_BLOCK_EXIT([&opCtx] { clearShardingOperationFailedStatus(opCtx); });
        try {
            _sessionsColl->checkSessionsCollectionExists(opCtx);
        } catch (const DBException& ex) {
            LOGV2(20712,
                  "Sessions collection is not set up: {error}; waiting until next sessions reap "
                  "interval",
                  "Sessions collection is not set up; waiting until next sessions reap interval",
                  "error"_attr = redact(ex));
            return Status::OK();
        }
        numReaped = _reapSessionsOlderThanFn(opCtx,
                                             *_sessionsColl,
                                             _service->now() -
                                                 Minutes(gTransactionRecordMinimumLifetimeMinutes));
    } catch (const DBException& ex) {
        {
            stdx::lock_guard<Latch> lk(_mutex);
            auto millis = _service->now() - _stats.getLastTransactionReaperJobTimestamp();
            _stats.setLastTransactionReaperJobDurationMillis(millis.count());
        }

        return ex.toStatus();
    }

    {
        stdx::lock_guard<Latch> lk(_mutex);
        auto millis = _service->now() - _stats.getLastTransactionReaperJobTimestamp();
        _stats.setLastTransactionReaperJobDurationMillis(millis.count());
        _stats.setLastTransactionReaperJobEntriesCleanedUp(numReaped);
    }

    return Status::OK();
}

void LogicalSessionCacheImpl::_refresh(Client* client) {
    // get or make an opCtx
    boost::optional<ServiceContext::UniqueOperationContext> uniqueCtx;
    auto* const opCtx = [&client, &uniqueCtx] {
        if (client->getOperationContext()) {
            return client->getOperationContext();
        }

        uniqueCtx.emplace(client->makeOperationContext());
        return uniqueCtx->get();
    }();

    const auto replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord && replCoord->isReplEnabled() && replCoord->getMemberState().arbiter()) {
        stdx::lock_guard<Latch> lk(_mutex);
        _activeSessions.clear();
        return;
    }

    // Stats for serverStatus:
    {
        stdx::lock_guard<Latch> lk(_mutex);

        // Clear the refresh-related stats with the beginning of our run.
        _stats.setLastSessionsCollectionJobDurationMillis(0);
        _stats.setLastSessionsCollectionJobEntriesRefreshed(0);
        _stats.setLastSessionsCollectionJobEntriesEnded(0);
        _stats.setLastSessionsCollectionJobCursorsClosed(0);

        // Start the new run.
        _stats.setLastSessionsCollectionJobTimestamp(_service->now());
        _stats.setSessionsCollectionJobCount(_stats.getSessionsCollectionJobCount() + 1);
    }

    // This will finish timing _refresh for our stats no matter when we return.
    const ScopeGuard timeRefreshJob([this] {
        stdx::lock_guard<Latch> lk(_mutex);
        auto millis = _service->now() - _stats.getLastSessionsCollectionJobTimestamp();
        _stats.setLastSessionsCollectionJobDurationMillis(millis.count());
    });

    ON_BLOCK_EXIT([&opCtx] { clearShardingOperationFailedStatus(opCtx); });

    _sessionsColl->setupSessionsCollection(opCtx);

    LogicalSessionIdSet staleSessions;
    LogicalSessionIdSet explicitlyEndingSessions;
    LogicalSessionIdMap<LogicalSessionRecord> activeSessions;

    {
        using std::swap;
        stdx::lock_guard<Latch> lk(_mutex);
        swap(explicitlyEndingSessions, _endingSessions);
        swap(activeSessions, _activeSessions);
    }

    // Create guards that in the case of a exception replace the ending or active sessions that
    // swapped out of LogicalSessionCache, and merges in any records that had been added since we
    // swapped them out.
    auto backSwap = [this](auto& member, auto& temp) {
        stdx::lock_guard<Latch> lk(_mutex);
        using std::swap;
        swap(member, temp);
        for (const auto& it : temp) {
            member.emplace(it);
        }
    };
    ScopeGuard activeSessionsBackSwapper([&] { backSwap(_activeSessions, activeSessions); });
    auto explicitlyEndingBackSwaper =
        ScopeGuard([&] { backSwap(_endingSessions, explicitlyEndingSessions); });

    // remove all explicitlyEndingSessions from activeSessions
    for (const auto& lsid : explicitlyEndingSessions) {
        activeSessions.erase(lsid);
    }

    // Refresh all recently active sessions as well as for sessions attached to running ops
    LogicalSessionRecordSet activeSessionRecords;

    auto runningOpSessions = _service->getActiveOpSessions();

    for (const auto& it : runningOpSessions) {
        // if a running op is the cause of an upsert, we won't have a user name for the record
        if (explicitlyEndingSessions.count(it) > 0) {
            continue;
        }
        activeSessionRecords.insert(makeLogicalSessionRecord(it, _service->now()));
    }
    for (const auto& it : activeSessions) {
        activeSessionRecords.insert(it.second);
    }

    // Refresh the active sessions in the sessions collection.
    _sessionsColl->refreshSessions(opCtx, activeSessionRecords);
    activeSessionsBackSwapper.dismiss();
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.setLastSessionsCollectionJobEntriesRefreshed(activeSessionRecords.size());
    }

    // Remove the ending sessions from the sessions collection.
    _sessionsColl->removeRecords(opCtx, explicitlyEndingSessions);
    explicitlyEndingBackSwaper.dismiss();
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.setLastSessionsCollectionJobEntriesEnded(explicitlyEndingSessions.size());
    }

    // Find which running, but not recently active sessions, are expired, and add them
    // to the list of sessions to kill cursors for

    KillAllSessionsByPatternSet patterns;

    auto openCursorSessions = _service->getOpenCursorSessions(opCtx);
    // Exclude sessions added to _activeSessions from the openCursorSession to avoid race between
    // killing cursors on the removed sessions and creating sessions.
    {
        stdx::lock_guard<Latch> lk(_mutex);

        for (const auto& it : _activeSessions) {
            auto newSessionIt = openCursorSessions.find(it.first);
            if (newSessionIt != openCursorSessions.end()) {
                openCursorSessions.erase(newSessionIt);
            }
        }
    }

    // think about pruning ending and active out of openCursorSessions
    try {
        auto removedSessions = _sessionsColl->findRemovedSessions(opCtx, openCursorSessions);

        for (const auto& lsid : removedSessions) {
            patterns.emplace(makeKillAllSessionsByPattern(opCtx, lsid));
        }
    } catch (...) {
        // Ignore errors.
    }

    // Add all of the explicitly ended sessions to the list of sessions to kill cursors for.
    for (const auto& lsid : explicitlyEndingSessions) {
        patterns.emplace(makeKillAllSessionsByPattern(opCtx, lsid));
    }

    SessionKiller::Matcher matcher(std::move(patterns));
    auto killRes = _service->killCursorsWithMatchingSessions(opCtx, std::move(matcher));
    {
        stdx::lock_guard<Latch> lk(_mutex);
        _stats.setLastSessionsCollectionJobCursorsClosed(killRes.second);
    }
}

void LogicalSessionCacheImpl::endSessions(const LogicalSessionIdSet& sessions) {
    for (const auto& lsid : sessions) {
        uassert(ErrorCodes::InvalidOptions,
                str::stream() << "Cannot specify a child session id " << lsid,
                isParentSessionId(lsid));
    }
    stdx::lock_guard<Latch> lk(_mutex);
    _endingSessions.insert(begin(sessions), end(sessions));
}

LogicalSessionCacheStats LogicalSessionCacheImpl::getStats() {
    stdx::lock_guard<Latch> lk(_mutex);
    _stats.setActiveSessionsCount(_activeSessions.size());
    return _stats;
}

Status LogicalSessionCacheImpl::_addToCacheIfNotFull(WithLock, LogicalSessionRecord record) {
    if (_activeSessions.size() >= size_t(maxSessions)) {
        Status status = {ErrorCodes::TooManyLogicalSessions,
                         str::stream()
                             << "Unable to add session ID " << record.getId()
                             << " into the cache because the number of active sessions is too "
                                "high"};
        // Returns Info() unless it was called in the past second.
        // In that case it will return the quieter Debug(2) */
        static auto& bumpedSeverity = *new logv2::SeveritySuppressor{
            Seconds{1}, logv2::LogSeverity::Info(), logv2::LogSeverity::Debug(2)};
        LOGV2_DEBUG(20715,
                    bumpedSeverity().toInt(),
                    "Unable to add session {sessionId} into the cache, too many active sessions: "
                    "{sessionCount}, maximum: {maxSessions}",
                    "Unable to add session into the cache, too many active sessions",
                    "sessionId"_attr = record.getId(),
                    "sessionCount"_attr = _activeSessions.size(),
                    "maxSessions"_attr = maxSessions);
        return status;
    }

    _activeSessions.insert(std::make_pair(record.getId(), std::move(record)));

    return Status::OK();
}

std::vector<LogicalSessionId> LogicalSessionCacheImpl::listIds() const {
    stdx::lock_guard<Latch> lk(_mutex);
    std::vector<LogicalSessionId> ret;
    ret.reserve(_activeSessions.size());
    for (const auto& id : _activeSessions) {
        ret.push_back(id.first);
    }
    return ret;
}

std::vector<LogicalSessionId> LogicalSessionCacheImpl::listIds(
    const std::vector<SHA256Block>& userDigests) const {
    stdx::lock_guard<Latch> lk(_mutex);
    std::vector<LogicalSessionId> ret;
    for (const auto& it : _activeSessions) {
        if (std::find(userDigests.cbegin(), userDigests.cend(), it.first.getUid()) !=
            userDigests.cend()) {
            ret.push_back(it.first);
        }
    }
    return ret;
}

boost::optional<LogicalSessionRecord> LogicalSessionCacheImpl::peekCached(
    const LogicalSessionId& lsid) const {
    uassert(ErrorCodes::InvalidOptions,
            str::stream() << "Cannot specify a child session id " << lsid,
            isParentSessionId(lsid));

    stdx::lock_guard<Latch> lk(_mutex);
    const auto it = _activeSessions.find(lsid);
    if (it == _activeSessions.end()) {
        return boost::none;
    }
    return it->second;
}

}  // namespace mongo
