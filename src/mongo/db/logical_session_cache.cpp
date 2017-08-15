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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/db/logical_session_cache.h"

#include "mongo/db/logical_session_id.h"
#include "mongo/db/logical_session_id_helpers.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/util/duration.h"
#include "mongo/util/log.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

namespace {
const auto getLogicalSessionCache =
    ServiceContext::declareDecoration<std::unique_ptr<LogicalSessionCache>>();

const auto getLogicalSessionCacheIsRegistered = ServiceContext::declareDecoration<AtomicBool>();
}  // namespace

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(logicalSessionRecordCacheSize,
                                      int,
                                      LogicalSessionCache::kLogicalSessionCacheDefaultCapacity);

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(logicalSessionRefreshMinutes,
                                      int,
                                      LogicalSessionCache::kLogicalSessionDefaultRefresh.count());

constexpr int LogicalSessionCache::kLogicalSessionCacheDefaultCapacity;
constexpr Minutes LogicalSessionCache::kLogicalSessionDefaultRefresh;

LogicalSessionCache* LogicalSessionCache::get(ServiceContext* service) {
    if (getLogicalSessionCacheIsRegistered(service).load()) {
        return getLogicalSessionCache(service).get();
    }
    return nullptr;
}

LogicalSessionCache* LogicalSessionCache::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

void LogicalSessionCache::set(ServiceContext* service,
                              std::unique_ptr<LogicalSessionCache> sessionCache) {
    auto& cache = getLogicalSessionCache(service);
    cache = std::move(sessionCache);
    getLogicalSessionCacheIsRegistered(service).store(true);
}

LogicalSessionCache::LogicalSessionCache(std::unique_ptr<ServiceLiason> service,
                                         std::unique_ptr<SessionsCollection> collection,
                                         Options options)
    : _refreshInterval(options.refreshInterval),
      _sessionTimeout(options.sessionTimeout),
      _service(std::move(service)),
      _sessionsColl(std::move(collection)),
      _cache(options.capacity) {
    PeriodicRunner::PeriodicJob job{[this](Client* client) { _refresh(client); },
                                    duration_cast<Milliseconds>(_refreshInterval)};
    _service->scheduleJob(std::move(job));
}

LogicalSessionCache::~LogicalSessionCache() {
    try {
        _service->join();
    } catch (...) {
        // If we failed to join we might still be running a background thread,
        // log but swallow the error since there is no good way to recover.
        severe() << "Failed to join background service thread";
    }
}

// TODO: fetch should attempt to update user info, if it is not in the found record.

Status LogicalSessionCache::fetchAndPromote(OperationContext* opCtx, const LogicalSessionId& lsid) {
    // Search our local cache first
    auto promoteRes = promote(lsid);
    if (promoteRes.isOK()) {
        return promoteRes;
    }

    // Cache miss, must fetch from the sessions collection.
    auto res = _sessionsColl->fetchRecord(opCtx, lsid);

    // If we got a valid record, add it to our cache.
    if (res.isOK()) {
        auto& record = res.getValue();
        record.setLastUse(now());

        // Any duplicate records here are actually the same record with different
        // lastUse times, ignore them.
        auto oldRecord = _addToCache(record);
        return Status::OK();
    }

    // If we could not get a valid record, return the error.
    return res.getStatus();
}

Status LogicalSessionCache::promote(LogicalSessionId lsid) {
    stdx::unique_lock<stdx::mutex> lk(_cacheMutex);
    auto it = _cache.find(lsid);
    if (it == _cache.end()) {
        return {ErrorCodes::NoSuchSession, "no matching session record found in the cache"};
    }

    // Update the last use time before returning.
    it->second.setLastUse(now());
    return Status::OK();
}

Status LogicalSessionCache::startSession(OperationContext* opCtx, LogicalSessionRecord record) {
    // Add the new record to our local cache. We will insert it into the sessions collection
    // the next time _refresh is called. If there is already a record in the cache for this
    // session, we'll just write over it with our newer, more recent one.
    _addToCache(record);
    return Status::OK();
}

Status LogicalSessionCache::refreshSessions(OperationContext* opCtx,
                                            const RefreshSessionsCmdFromClient& cmd) {
    // Update the timestamps of all these records in our cache.
    auto sessions = makeLogicalSessionIds(cmd.getRefreshSessions(), opCtx);
    for (auto& lsid : sessions) {
        if (!promote(lsid).isOK()) {
            // This is a new record, insert it.
            _addToCache(makeLogicalSessionRecord(opCtx, lsid, now()));
        }
    }

    return Status::OK();
}

Status LogicalSessionCache::refreshSessions(OperationContext* opCtx,
                                            const RefreshSessionsCmdFromClusterMember& cmd) {
    LogicalSessionRecordSet toRefresh{};

    // Update the timestamps of all these records in our cache.
    auto records = cmd.getRefreshSessionsInternal();
    for (auto& record : records) {
        if (!promote(record.getId()).isOK()) {
            // This is a new record, insert it.
            _addToCache(record);
        }
        toRefresh.insert(record);
    }

    // Write to the sessions collection now.
    return _sessionsColl->refreshSessions(opCtx, toRefresh, now());
}

void LogicalSessionCache::refreshNow(Client* client) {
    return _refresh(client);
}

Date_t LogicalSessionCache::now() {
    return _service->now();
}

size_t LogicalSessionCache::size() {
    stdx::lock_guard<stdx::mutex> lock(_cacheMutex);
    return _cache.size();
}

void LogicalSessionCache::_refresh(Client* client) {
    LogicalSessionRecordSet activeSessions;
    LogicalSessionRecordSet deadSessions;

    auto time = now();

    // We should avoid situations where we have records in the cache
    // that have been expired from the sessions collection. If they haven't been
    // used in _sessionTimeout, we should just remove them.

    // Assemble a list of active session records in our cache
    std::vector<decltype(_cache)::ListEntry> cacheCopy;
    {
        stdx::unique_lock<stdx::mutex> lk(_cacheMutex);
        cacheCopy.assign(_cache.begin(), _cache.end());
    }

    for (auto& it : cacheCopy) {
        auto record = it.second;
        if (!_isDead(record, time)) {
            activeSessions.insert(record);
        } else {
            deadSessions.insert(record);
        }
    }

    // Append any active sessions from the service. We should always have
    // cache entries for active sessions. If we don't, then it is a sign that
    // the cache needs to be larger, because active session records are being
    // evicted.

    // Promote our cached entries for all active service sessions to be recently-
    // used, and update their lastUse dates so we don't lose them to eviction. We
    // do not need to do this with records from our own cache, which are being used
    // regularly. Sessions for long-running queries, however, must be kept alive
    // by us here.
    auto serviceSessions = _service->getActiveSessions();
    {
        stdx::unique_lock<stdx::mutex> lk(_cacheMutex);
        for (auto lsid : serviceSessions) {
            auto it = _cache.promote(lsid);
            if (it != _cache.end()) {
                // If we have not found our record, it may have been removed
                // by another thread.
                it->second.setLastUse(time);
                activeSessions.insert(it->second);
            }

            // TODO SERVER-29709: Rethink how active sessions interact with refreshes,
            // and potentially move this block above the block where we separate
            // dead sessions from live sessions, above.
            activeSessions.insert(makeLogicalSessionRecord(lsid, time));
        }
    }

    // Query into the sessions collection to do the refresh. If any sessions have
    // failed to refresh, it means their authoritative records were removed, and
    // we should remove such records from our cache as well.
    {
        boost::optional<ServiceContext::UniqueOperationContext> uniqueCtx;
        auto* const opCtx = [&client, &uniqueCtx] {
            if (client->getOperationContext()) {
                return client->getOperationContext();
            }

            uniqueCtx.emplace(client->makeOperationContext());
            return uniqueCtx->get();
        }();

        auto res = _sessionsColl->refreshSessions(opCtx, std::move(activeSessions), time);
        if (!res.isOK()) {
            // TODO SERVER-29709: handle network errors here.
            return;
        }
    }

    // Prune any dead records out of the cache. Dead records are ones that failed to
    // refresh, or ones that have expired locally. We don't make an effort to check
    // if the locally-expired records still have live authoritative records in the
    // sessions collection. We also don't attempt to resurrect our expired records.
    // However, we *do* keep records alive if they are active on the service.
    {
        // TODO SERVER-29709: handle expiration separately from failure to refresh.
    }
}

bool LogicalSessionCache::_isDead(const LogicalSessionRecord& record, Date_t now) const {
    return record.getLastUse() + _sessionTimeout < now;
}

boost::optional<LogicalSessionRecord> LogicalSessionCache::_addToCache(
    LogicalSessionRecord record) {
    stdx::unique_lock<stdx::mutex> lk(_cacheMutex);
    return _cache.add(record.getId(), std::move(record));
}

}  // namespace mongo
