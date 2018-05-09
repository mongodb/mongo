/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/logical_session_cache_impl.h"

#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/duration.h"
#include "mongo/util/log.h"
#include "mongo/util/periodic_runner.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(
    logicalSessionRefreshMinutes,
    int,
    LogicalSessionCacheImpl::kLogicalSessionDefaultRefresh.count());

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(disableLogicalSessionCacheRefresh, bool, false);

constexpr Minutes LogicalSessionCacheImpl::kLogicalSessionDefaultRefresh;

LogicalSessionCacheImpl::LogicalSessionCacheImpl(
    std::unique_ptr<ServiceLiason> service,
    std::shared_ptr<SessionsCollection> collection,
    std::shared_ptr<TransactionReaper> transactionReaper,
    Options options)
    : _refreshInterval(options.refreshInterval),
      _sessionTimeout(options.sessionTimeout),
      _service(std::move(service)),
      _sessionsColl(std::move(collection)),
      _transactionReaper(std::move(transactionReaper)) {
    if (!disableLogicalSessionCacheRefresh) {
        _service->scheduleJob({"LogicalSessionCacheRefresh",
                               [this](Client* client) { _periodicRefresh(client); },
                               _refreshInterval});
        _service->scheduleJob({"LogicalSessionCacheReap",
                               [this](Client* client) { _periodicReap(client); },
                               _refreshInterval});
    }
    _stats.setLastSessionsCollectionJobTimestamp(now());
    _stats.setLastTransactionReaperJobTimestamp(now());
}

LogicalSessionCacheImpl::~LogicalSessionCacheImpl() {
    try {
        _service->join();
    } catch (...) {
        // If we failed to join we might still be running a background thread,
        // log but swallow the error since there is no good way to recover.
        severe() << "Failed to join background service thread";
    }
}

Status LogicalSessionCacheImpl::promote(LogicalSessionId lsid) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    auto it = _activeSessions.find(lsid);
    if (it == _activeSessions.end()) {
        return {ErrorCodes::NoSuchSession, "no matching session record found in the cache"};
    }

    return Status::OK();
}

void LogicalSessionCacheImpl::startSession(OperationContext* opCtx, LogicalSessionRecord record) {
    // Add the new record to our local cache. We will insert it into the sessions collection
    // the next time _refresh is called. If there is already a record in the cache for this
    // session, we'll just write over it with our newer, more recent one.
    _addToCache(record);
}

Status LogicalSessionCacheImpl::refreshSessions(OperationContext* opCtx,
                                                const RefreshSessionsCmdFromClient& cmd) {
    // Update the timestamps of all these records in our cache.
    auto sessions = makeLogicalSessionIds(cmd.getRefreshSessions(), opCtx);
    for (const auto& lsid : sessions) {
        if (!promote(lsid).isOK()) {
            // This is a new record, insert it.
            _addToCache(makeLogicalSessionRecord(opCtx, lsid, now()));
        }
    }

    return Status::OK();
}

Status LogicalSessionCacheImpl::refreshSessions(OperationContext* opCtx,
                                                const RefreshSessionsCmdFromClusterMember& cmd) {
    // Update the timestamps of all these records in our cache.
    auto records = cmd.getRefreshSessionsInternal();
    for (const auto& record : records) {
        if (!promote(record.getId()).isOK()) {
            // This is a new record, insert it.
            _addToCache(record);
        }
    }

    return Status::OK();
}

void LogicalSessionCacheImpl::vivify(OperationContext* opCtx, const LogicalSessionId& lsid) {
    if (!promote(lsid).isOK()) {
        startSession(opCtx, makeLogicalSessionRecord(opCtx, lsid, now()));
    }
}

Status LogicalSessionCacheImpl::refreshNow(Client* client) {
    try {
        _refresh(client);
    } catch (...) {
        return exceptionToStatus();
    }
    return Status::OK();
}

Status LogicalSessionCacheImpl::reapNow(Client* client) {
    return _reap(client);
}

Date_t LogicalSessionCacheImpl::now() {
    return _service->now();
}

size_t LogicalSessionCacheImpl::size() {
    stdx::lock_guard<stdx::mutex> lock(_cacheMutex);
    return _activeSessions.size();
}

void LogicalSessionCacheImpl::_periodicRefresh(Client* client) {
    try {
        _refresh(client);
    } catch (...) {
        log() << "Failed to refresh session cache: " << exceptionToStatus();
    }
}

void LogicalSessionCacheImpl::_periodicReap(Client* client) {
    auto res = _reap(client);
    if (!res.isOK()) {
        log() << "Failed to reap transaction table: " << res;
    }

    return;
}

Status LogicalSessionCacheImpl::_reap(Client* client) {
    if (!_transactionReaper) {
        return Status::OK();
    }

    // Take the lock to update some stats.
    {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);

        // Clear the last set of stats for our new run.
        _stats.setLastTransactionReaperJobDurationMillis(0);
        _stats.setLastTransactionReaperJobEntriesCleanedUp(0);

        // Start the new run.
        _stats.setLastTransactionReaperJobTimestamp(now());
        _stats.setTransactionReaperJobCount(_stats.getTransactionReaperJobCount() + 1);
    }

    int numReaped = 0;

    try {
        boost::optional<ServiceContext::UniqueOperationContext> uniqueCtx;
        auto* const opCtx = [&client, &uniqueCtx] {
            if (client->getOperationContext()) {
                return client->getOperationContext();
            }

            uniqueCtx.emplace(client->makeOperationContext());
            return uniqueCtx->get();
        }();

        stdx::lock_guard<stdx::mutex> lk(_reaperMutex);
        numReaped = _transactionReaper->reap(opCtx);
    } catch (...) {
        {
            stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
            auto millis = now() - _stats.getLastTransactionReaperJobTimestamp();
            _stats.setLastTransactionReaperJobDurationMillis(millis.count());
        }

        return exceptionToStatus();
    }

    {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        auto millis = now() - _stats.getLastTransactionReaperJobTimestamp();
        _stats.setLastTransactionReaperJobDurationMillis(millis.count());
        _stats.setLastTransactionReaperJobEntriesCleanedUp(numReaped);
    }

    return Status::OK();
}

void LogicalSessionCacheImpl::_refresh(Client* client) {
    // Stats for serverStatus:
    {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);

        // Clear the refresh-related stats with the beginning of our run.
        _stats.setLastSessionsCollectionJobDurationMillis(0);
        _stats.setLastSessionsCollectionJobEntriesRefreshed(0);
        _stats.setLastSessionsCollectionJobEntriesEnded(0);
        _stats.setLastSessionsCollectionJobCursorsClosed(0);

        // Start the new run.
        _stats.setLastSessionsCollectionJobTimestamp(now());
        _stats.setSessionsCollectionJobCount(_stats.getSessionsCollectionJobCount() + 1);
    }

    // This will finish timing _refresh for our stats no matter when we return.
    const auto timeRefreshJob = MakeGuard([this] {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        auto millis = now() - _stats.getLastSessionsCollectionJobTimestamp();
        _stats.setLastSessionsCollectionJobDurationMillis(millis.count());
    });

    // get or make an opCtx
    boost::optional<ServiceContext::UniqueOperationContext> uniqueCtx;
    auto* const opCtx = [&client, &uniqueCtx] {
        if (client->getOperationContext()) {
            return client->getOperationContext();
        }

        uniqueCtx.emplace(client->makeOperationContext());
        return uniqueCtx->get();
    }();

    auto res = _sessionsColl->setupSessionsCollection(opCtx);
    if (!res.isOK()) {
        log() << "Sessions collection is not set up; "
              << "waiting until next sessions refresh interval: " << res.reason();
        return;
    }

    LogicalSessionIdSet staleSessions;
    LogicalSessionIdSet explicitlyEndingSessions;
    LogicalSessionIdMap<LogicalSessionRecord> activeSessions;

    // backSwapper creates a guard that in the case of a exception
    // replaces the ending or active sessions that swapped out of of LogicalSessionCache,
    // and merges in any records that had been added since we swapped them
    // out.
    auto backSwapper = [this](auto& member, auto& temp) {
        return MakeGuard([this, &member, &temp] {
            stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
            using std::swap;
            swap(member, temp);
            for (const auto& it : temp) {
                member.emplace(it);
            }
        });
    };

    {
        using std::swap;
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        swap(explicitlyEndingSessions, _endingSessions);
        swap(activeSessions, _activeSessions);
    }
    auto activeSessionsBackSwapper = backSwapper(_activeSessions, activeSessions);
    auto explicitlyEndingBackSwaper = backSwapper(_endingSessions, explicitlyEndingSessions);

    // remove all explicitlyEndingSessions from activeSessions
    for (const auto& lsid : explicitlyEndingSessions) {
        activeSessions.erase(lsid);
    }

    // refresh all recently active sessions as well as for sessions attached to running ops

    LogicalSessionRecordSet activeSessionRecords{};

    auto runningOpSessions = _service->getActiveOpSessions();

    for (const auto& it : runningOpSessions) {
        // if a running op is the cause of an upsert, we won't have a user name for the record
        if (explicitlyEndingSessions.count(it) > 0) {
            continue;
        }
        activeSessionRecords.insert(makeLogicalSessionRecord(it, now()));
    }
    for (const auto& it : activeSessions) {
        activeSessionRecords.insert(it.second);
    }

    // Refresh the active sessions in the sessions collection.
    uassertStatusOK(_sessionsColl->refreshSessions(opCtx, activeSessionRecords));
    activeSessionsBackSwapper.Dismiss();
    {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        _stats.setLastSessionsCollectionJobEntriesRefreshed(activeSessionRecords.size());
    }

    // Remove the ending sessions from the sessions collection.
    uassertStatusOK(_sessionsColl->removeRecords(opCtx, explicitlyEndingSessions));
    explicitlyEndingBackSwaper.Dismiss();
    {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        _stats.setLastSessionsCollectionJobEntriesEnded(explicitlyEndingSessions.size());
    }


    // Find which running, but not recently active sessions, are expired, and add them
    // to the list of sessions to kill cursors for

    KillAllSessionsByPatternSet patterns;

    auto openCursorSessions = _service->getOpenCursorSessions();

    // think about pruning ending and active out of openCursorSessions
    auto statusAndRemovedSessions = _sessionsColl->findRemovedSessions(opCtx, openCursorSessions);

    if (statusAndRemovedSessions.isOK()) {
        auto removedSessions = statusAndRemovedSessions.getValue();
        for (const auto& lsid : removedSessions) {
            patterns.emplace(makeKillAllSessionsByPattern(opCtx, lsid));
        }
    } else {
        // Ignore errors.
    }

    // Add all of the explicitly ended sessions to the list of sessions to kill cursors for.
    for (const auto& lsid : explicitlyEndingSessions) {
        patterns.emplace(makeKillAllSessionsByPattern(opCtx, lsid));
    }

    SessionKiller::Matcher matcher(std::move(patterns));
    auto killRes = _service->killCursorsWithMatchingSessions(opCtx, std::move(matcher));
    {
        stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
        _stats.setLastSessionsCollectionJobCursorsClosed(killRes.second);
    }
}

void LogicalSessionCacheImpl::endSessions(const LogicalSessionIdSet& sessions) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    _endingSessions.insert(begin(sessions), end(sessions));
}

LogicalSessionCacheStats LogicalSessionCacheImpl::getStats() {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    _stats.setActiveSessionsCount(_activeSessions.size());
    return _stats;
}

void LogicalSessionCacheImpl::_addToCache(LogicalSessionRecord record) {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    _activeSessions.insert(std::make_pair(record.getId(), record));
}

std::vector<LogicalSessionId> LogicalSessionCacheImpl::listIds() const {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    std::vector<LogicalSessionId> ret;
    ret.reserve(_activeSessions.size());
    for (const auto& id : _activeSessions) {
        ret.push_back(id.first);
    }
    return ret;
}

std::vector<LogicalSessionId> LogicalSessionCacheImpl::listIds(
    const std::vector<SHA256Block>& userDigests) const {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
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
    const LogicalSessionId& id) const {
    stdx::lock_guard<stdx::mutex> lk(_cacheMutex);
    const auto it = _activeSessions.find(id);
    if (it == _activeSessions.end()) {
        return boost::none;
    }
    return it->second;
}
}  // namespace mongo
