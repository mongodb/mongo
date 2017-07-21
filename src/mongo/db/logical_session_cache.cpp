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
#include "mongo/db/operation_context.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/service_context.h"
#include "mongo/util/duration.h"
#include "mongo/util/log.h"
#include "mongo/util/periodic_runner.h"

namespace mongo {

namespace {
const auto getLogicalSessionCache =
    ServiceContext::declareDecoration<std::unique_ptr<LogicalSessionCache>>();
}  // namespace

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(logicalSessionRecordCacheSize,
                                      int,
                                      LogicalSessionCache::kLogicalSessionCacheDefaultCapacity);

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(localLogicalSessionTimeoutMinutes,
                                      int,
                                      LogicalSessionCache::kLogicalSessionDefaultTimeout.count());

MONGO_EXPORT_STARTUP_SERVER_PARAMETER(logicalSessionRefreshMinutes,
                                      int,
                                      LogicalSessionCache::kLogicalSessionDefaultRefresh.count());

constexpr int LogicalSessionCache::kLogicalSessionCacheDefaultCapacity;
constexpr Minutes LogicalSessionCache::kLogicalSessionDefaultTimeout;
constexpr Minutes LogicalSessionCache::kLogicalSessionDefaultRefresh;

LogicalSessionCache* LogicalSessionCache::get(ServiceContext* service) {
    return getLogicalSessionCache(service).get();
}

LogicalSessionCache* LogicalSessionCache::get(OperationContext* ctx) {
    return get(ctx->getClient()->getServiceContext());
}

void LogicalSessionCache::set(ServiceContext* service,
                              std::unique_ptr<LogicalSessionCache> sessionCache) {
    auto& cache = getLogicalSessionCache(service);
    cache = std::move(sessionCache);
}

LogicalSessionCache::LogicalSessionCache(std::unique_ptr<ServiceLiason> service,
                                         std::unique_ptr<SessionsCollection> collection,
                                         Options options)
    : _refreshInterval(options.refreshInterval),
      _sessionTimeout(options.sessionTimeout),
      _service(std::move(service)),
      _sessionsColl(std::move(collection)),
      _cache(options.capacity) {
    PeriodicRunner::PeriodicJob job{[this] { _refresh(); },
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

Status LogicalSessionCache::fetchAndPromote(LogicalSessionId lsid) {
    // Search our local cache first
    auto promoteRes = promote(lsid);
    if (promoteRes.isOK()) {
        return promoteRes;
    }

    // Cache miss, must fetch from the sessions collection.
    auto res = _sessionsColl->fetchRecord(lsid);

    // If we got a valid record, add it to our cache.
    if (res.isOK()) {
        auto& record = res.getValue();
        record.setLastUse(_service->now());

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

    // Do not use records if they have expired.
    auto now = _service->now();
    if (_isDead(it->second, now)) {
        return {ErrorCodes::NoSuchSession, "no matching session record found in the cache"};
    }

    // Update the last use time before returning.
    it->second.setLastUse(now);
    return Status::OK();
}

Status LogicalSessionCache::startSession(LogicalSessionId lsid) {
    LogicalSessionRecord lsr;
    lsr.setId(lsid);
    lsr.setLastUse(_service->now());

    // Attempt to insert into the sessions collection first. This collection enforces
    // unique session ids, so it will act as concurrency control for us.
    auto res = _sessionsColl->insertRecord(lsr);
    if (!res.isOK()) {
        return res;
    }

    // Add the new record to our local cache. If we get a conflict here, and the
    // conflicting record is not dead and is not equal to our record, an interloper
    // may have ended this session and then created a new one with the same id.
    // In this case, return a failure.
    auto oldRecord = _addToCache(lsr);
    if (oldRecord) {
        if (*oldRecord != lsr) {
            if (!_isDead(*oldRecord, _service->now())) {
                return {ErrorCodes::DuplicateSession, "session with this id already exists"};
            }
        }
    }

    return Status::OK();
}

void LogicalSessionCache::_refresh() {
    LogicalSessionIdSet activeSessions;
    LogicalSessionIdSet deadSessions;

    auto now = _service->now();

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
        if (!_isDead(record, now)) {
            activeSessions.insert(LogicalSessionId{record.getId()});
        } else {
            deadSessions.insert(LogicalSessionId{record.getId()});
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
                it->second.setLastUse(now);
            }

            activeSessions.insert(lsid);
        }
    }

    // Query into the sessions collection to do the refresh. If any sessions have
    // failed to refresh, it means their authoritative records were removed, and
    // we should remove such records from our cache as well.
    auto failedToRefresh = _sessionsColl->refreshSessions(std::move(activeSessions));

    // Prune any dead records out of the cache. Dead records are ones that failed to
    // refresh, or ones that have expired locally. We don't make an effort to check
    // if the locally-expired records still have live authoritative records in the
    // sessions collection. We also don't attempt to resurrect our expired records.
    // However, we *do* keep records alive if they are active on the service.
    {
        stdx::unique_lock<stdx::mutex> lk(_cacheMutex);
        for (auto deadId : failedToRefresh) {
            auto it = serviceSessions.find(deadId);
            if (it == serviceSessions.end()) {
                _cache.erase(deadId);
            }
        }
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
